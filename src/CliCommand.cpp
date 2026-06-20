#include "CliCommand.h"

#include "BuildFile.h"
#include "Log.h"
#include "PathUtils.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace halionbridge::detail
{
namespace
{

constexpr const char* kBuildFileName = "halionbridge_build.lua";
constexpr const char* kBuildWorkerArgument = "--halionbridge-build-worker";

std::vector<std::string> toMutableArgs(std::span<const std::string> args)
{
    return {args.rbegin(), args.rend()};
}

std::string trim(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};

    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string toUpperAscii(std::string text)
{
    std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

std::optional<int> parseNonNegativeInt(std::string_view text)
{
    const auto raw = trim(std::string(text));
    if (raw.empty())
        return std::nullopt;

    for (const auto c : raw)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return std::nullopt;

    try
    {
        const auto value = std::stoll(raw);
        if (value > std::numeric_limits<int>::max())
            return std::nullopt;

        return static_cast<int>(value);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<int> parsePositiveInt(std::string_view text)
{
    const auto parsed = parseNonNegativeInt(text);
    if (!parsed || *parsed <= 0)
        return std::nullopt;

    return parsed;
}

void addDiagnostic(std::vector<CliDiagnostic>& diagnostics, const CliDiagnosticLevel level, std::string message)
{
    diagnostics.push_back(CliDiagnostic{level, std::move(message)});
}

void addError(std::vector<CliDiagnostic>& diagnostics, std::string message)
{
    addDiagnostic(diagnostics, CliDiagnosticLevel::error, std::move(message));
}

void addWarning(std::vector<CliDiagnostic>& diagnostics, std::string message)
{
    addDiagnostic(diagnostics, CliDiagnosticLevel::warning, std::move(message));
}

void logDiagnostics(const std::vector<CliDiagnostic>& diagnostics)
{
    for (const auto& diagnostic : diagnostics)
    {
        if (diagnostic.level == CliDiagnosticLevel::warning)
            log::warn("{}", diagnostic.message);
        else
            log::error("{}", diagnostic.message);
    }
}

bool applyTimeoutOption(AppOptions& options, const std::vector<std::string>& values, const bool noTimeoutRequested,
                        std::vector<CliDiagnostic>& diagnostics)
{
    auto noTimeoutSeen = noTimeoutRequested;
    auto positiveTimeoutSeen = false;

    if (noTimeoutSeen)
        options.timeoutSeconds = 0;

    for (const auto& value : values)
    {
        const auto parsed = parseNonNegativeInt(value);
        if (!parsed)
        {
            addError(diagnostics, "--timeout-seconds must be a non-negative integer.");
            return false;
        }

        if (*parsed == 0)
        {
            if (positiveTimeoutSeen)
            {
                addError(diagnostics, "--timeout-seconds 0 cannot be combined with a positive --timeout-seconds value.");
                return false;
            }

            noTimeoutSeen = true;
        }
        else
        {
            if (noTimeoutSeen)
            {
                addError(diagnostics, "--timeout-seconds cannot be combined with --no-timeout or --timeout-seconds 0.");
                return false;
            }

            positiveTimeoutSeen = true;
        }

        options.timeoutSeconds = *parsed;
    }

    return true;
}

bool applyTimeoutOption(VstPresetRemapOptions& options, const std::vector<std::string>& values, const bool noTimeoutRequested,
                        std::vector<CliDiagnostic>& diagnostics)
{
    auto noTimeoutSeen = noTimeoutRequested;
    auto positiveTimeoutSeen = false;

    if (noTimeoutSeen)
        options.timeoutSeconds = 0;

    for (const auto& value : values)
    {
        const auto parsed = parseNonNegativeInt(value);
        if (!parsed)
        {
            addError(diagnostics, "--timeout-seconds must be a non-negative integer.");
            return false;
        }

        if (*parsed == 0)
        {
            if (positiveTimeoutSeen)
            {
                addError(diagnostics, "--timeout-seconds 0 cannot be combined with a positive --timeout-seconds value.");
                return false;
            }

            noTimeoutSeen = true;
        }
        else
        {
            if (noTimeoutSeen)
            {
                addError(diagnostics, "--timeout-seconds cannot be combined with --no-timeout or --timeout-seconds 0.");
                return false;
            }

            positiveTimeoutSeen = true;
        }

        options.timeoutSeconds = *parsed;
    }

    return true;
}

bool parseCli11App(CLI::App& app, std::span<const std::string> args, std::vector<CliDiagnostic>& diagnostics)
{
    auto mutableArgs = toMutableArgs(args);

    try
    {
        app.parse(mutableArgs);
        return true;
    }
    catch (const CLI::ParseError& error)
    {
        addError(diagnostics, error.what());
        return false;
    }
}

} // namespace

CliCommandKind classifyCliCommand(std::span<const std::string> args) noexcept
{
    if (args.empty())
        return CliCommandKind::help;

    const auto& command = args.front();
    if (command == "--halionbridge-build-worker")
        return CliCommandKind::buildWorker;
    if (command == "--halionbridge-scan-plugin")
        return CliCommandKind::scanPluginWorker;
    if (command == "--help" || command == "-h")
        return CliCommandKind::help;
    if (command == "--version")
        return CliCommandKind::version;
    if (command == "build")
        return CliCommandKind::build;
    if (command == "init")
        return CliCommandKind::init;
    if (command == "convert")
        return CliCommandKind::convert;
    if (command == "remap-vstpresets")
        return CliCommandKind::remapVstPresets;
    if (command == "vstpreset-metadata")
        return CliCommandKind::vstPresetMetadata;

    return CliCommandKind::unknown;
}

BuildOptionsParseResult parseBuildOptionsDetailed(std::span<const std::string> args)
{
    BuildOptionsParseResult result;
    AppOptions options;
    std::string buildDirectoryText;
    std::string pluginText;
    std::string outputDirectoryText;
    std::vector<std::string> timeoutValues;
    std::string buildChunkSizeText;
    auto noTimeoutRequested = false;

    CLI::App app{"halionbridge build"};
    app.set_help_flag();
    app.add_option("build-directory", buildDirectoryText);
    app.add_option("--plugin", pluginText);
    app.add_option("--output-directory", outputDirectoryText);
    app.add_flag("--gui", options.showGui);
    app.add_flag("--force-scan", options.forceScan);
    app.add_flag("--nokill", options.noKill);
    app.add_flag("--fail-fast", options.failFast);
    app.add_flag("--no-timeout", noTimeoutRequested);
    app.add_option("--timeout-seconds", timeoutValues)->expected(1);
    app.add_option("--build-chunk-size", buildChunkSizeText);

    if (!parseCli11App(app, args, result.diagnostics))
    {
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    if (pluginText.empty() && app.count("--plugin") > 0)
    {
        addError(result.diagnostics, "--plugin requires a value.");
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    if (!pluginText.empty())
    {
        juce::File file(normalizeCliPath(toJuceString(std::string_view(pluginText))));
        if (!file.existsAsFile() && !file.isDirectory())
        {
            addError(result.diagnostics, "Override plugin path does not exist at " + file.getFullPathName().toStdString());
            result.errorKind = CliParseErrorKind::validation;
            return result;
        }
        options.pluginPathOverride = toStdPath(file);
    }

    if (!outputDirectoryText.empty())
        options.outputDirectory = toStdPath(normalizeCliPath(toJuceString(std::string_view(outputDirectoryText))));

    if (!applyTimeoutOption(options, timeoutValues, noTimeoutRequested, result.diagnostics))
    {
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    if (!buildChunkSizeText.empty())
    {
        const auto parsed = parsePositiveInt(buildChunkSizeText);
        if (!parsed)
        {
            addError(result.diagnostics, "--build-chunk-size must be a positive integer.");
            result.errorKind = CliParseErrorKind::syntax;
            return result;
        }

        options.buildChunkSize = *parsed;
    }

    if (buildDirectoryText.empty())
    {
        addError(result.diagnostics, std::string("You must provide a build directory containing ") + kBuildFileName + ".");
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    const auto buildDirectory = normalizeCliPath(toJuceString(std::string_view(buildDirectoryText)));
    if (!buildDirectory.isDirectory())
    {
        addError(result.diagnostics, "Build directory does not exist at " + buildDirectory.getFullPathName().toStdString());
        result.errorKind = CliParseErrorKind::validation;
        return result;
    }

    const auto buildFile = buildDirectory.getChildFile(kBuildFileName);
    if (!buildFile.existsAsFile())
    {
        if (hasTopLevelLuaBuildScripts(buildDirectory))
        {
            addWarning(result.diagnostics, std::string("No ") + kBuildFileName +
                                               " was found, but Lua files exist in this directory. Run \"halionbridge init " +
                                               buildDirectory.getFullPathName().toStdString() + "\" to generate one.");
        }

        addError(result.diagnostics,
                 std::string("Build directory must contain ") + kBuildFileName + " at " + buildFile.getFullPathName().toStdString());
        result.errorKind = CliParseErrorKind::validation;
        return result;
    }

    options.buildDirectory = toStdPath(buildDirectory);
    result.options = std::move(options);
    return result;
}

std::optional<AppOptions> parseBuildOptions(std::span<const std::string> args)
{
    return parseBuildOptionsDetailed(args).options;
}

VstPresetRemapOptionsParseResult parseVstPresetRemapOptionsDetailed(std::span<const std::string> args)
{
    VstPresetRemapOptionsParseResult result;
    VstPresetRemapOptions options;
    std::string inputDirectoryText;
    std::string outputDirectoryText;
    std::string oldRootText;
    std::string newRootText;
    std::string presetPluginCodeText;
    std::string pluginText;
    std::vector<std::string> timeoutValues;
    auto noTimeoutRequested = false;

    CLI::App app{"halionbridge remap-vstpresets"};
    app.set_help_flag();
    app.add_option("--input-directory", inputDirectoryText);
    app.add_option("--output-directory", outputDirectoryText);
    app.add_option("--old-root", oldRootText);
    app.add_option("--new-root", newRootText);
    app.add_option("--preset-plugin-code", presetPluginCodeText);
    app.add_option("--plugin", pluginText);
    app.add_flag("--gui", options.showGui);
    app.add_flag("--force-scan", options.forceScan);
    app.add_flag("--nokill", options.noKill);
    app.add_flag("--no-timeout", noTimeoutRequested);
    app.add_option("--timeout-seconds", timeoutValues)->expected(1);

    if (!parseCli11App(app, args, result.diagnostics))
    {
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    if (app.remaining_size() > 0)
    {
        const auto unexpected = app.remaining().empty() ? std::string() : app.remaining().front();
        addError(result.diagnostics, "remap-vstpresets uses named options. Unexpected positional argument: " + unexpected);
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    if (!inputDirectoryText.empty())
    {
        const auto directory = normalizeCliPath(toJuceString(std::string_view(inputDirectoryText)));
        if (!directory.isDirectory())
        {
            addError(result.diagnostics, "Input directory does not exist at " + directory.getFullPathName().toStdString());
            result.errorKind = CliParseErrorKind::validation;
            return result;
        }

        options.inputDirectory = toStdPath(directory);
    }

    if (!outputDirectoryText.empty())
        options.outputDirectory = toStdPath(normalizeCliPath(toJuceString(std::string_view(outputDirectoryText))));

    options.oldRoot = trim(oldRootText);
    options.newRoot = trim(newRootText);

    if (!presetPluginCodeText.empty())
    {
        options.presetPluginCode = toUpperAscii(trim(presetPluginCodeText));
        if (options.presetPluginCode != "H7" && options.presetPluginCode != "HS")
        {
            addError(result.diagnostics, "--preset-plugin-code must be H7 or HS.");
            result.errorKind = CliParseErrorKind::syntax;
            return result;
        }
    }

    if (!pluginText.empty())
    {
        juce::File file(normalizeCliPath(toJuceString(std::string_view(pluginText))));
        if (!file.existsAsFile() && !file.isDirectory())
        {
            addError(result.diagnostics, "Override plugin path does not exist at " + file.getFullPathName().toStdString());
            result.errorKind = CliParseErrorKind::validation;
            return result;
        }
        options.pluginPathOverride = toStdPath(file);
    }

    if (!applyTimeoutOption(options, timeoutValues, noTimeoutRequested, result.diagnostics))
    {
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    if (inputDirectoryText.empty() || outputDirectoryText.empty() || options.oldRoot.empty() || options.newRoot.empty())
    {
        addError(result.diagnostics, "remap-vstpresets requires --input-directory, --output-directory, --old-root, and --new-root.");
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    result.options = std::move(options);
    return result;
}

std::optional<VstPresetRemapOptions> parseVstPresetRemapOptions(std::span<const std::string> args)
{
    return parseVstPresetRemapOptionsDetailed(args).options;
}

std::optional<AppOptions> parseBuildWorkerOptions(std::span<const std::string> args)
{
    if (args.empty() || args.front() != kBuildWorkerArgument)
    {
        log::error("{} must be the first build-worker argument.", kBuildWorkerArgument);
        return std::nullopt;
    }

    if (args.size() < 2)
    {
        log::error("{} requires a build directory.", kBuildWorkerArgument);
        return std::nullopt;
    }

    std::string buildDirectoryText;
    std::string pluginText;
    std::string outputDirectoryText;
    std::string timeoutValue;
    std::string resultFileText;
    std::string sliceStartText;
    std::string sliceCountText;
    std::string sliceTotalText;
    auto noTimeoutRequested = false;
    auto forceScan = false;

    CLI::App app{"halionbridge build worker"};
    app.set_help_flag();
    app.add_option("build-directory", buildDirectoryText);
    app.add_option("--build-slice-start", sliceStartText);
    app.add_option("--build-slice-count", sliceCountText);
    app.add_option("--build-slice-total", sliceTotalText);
    app.add_option("--worker-result-file", resultFileText);
    app.add_option("--plugin", pluginText);
    app.add_option("--timeout-seconds", timeoutValue);
    app.add_option("--output-directory", outputDirectoryText);
    app.add_flag("--no-timeout", noTimeoutRequested);
    app.add_flag("--force-scan", forceScan);

    auto diagnostics = std::vector<CliDiagnostic>{};
    const auto workerArgs = std::span<const std::string>(args.begin() + 1, args.end());
    if (!parseCli11App(app, workerArgs, diagnostics))
    {
        logDiagnostics(diagnostics);
        return std::nullopt;
    }

    if (buildDirectoryText.empty())
    {
        log::error("{} requires a build directory.", kBuildWorkerArgument);
        return std::nullopt;
    }

    const auto sliceStart = parsePositiveInt(sliceStartText);
    const auto sliceCount = parsePositiveInt(sliceCountText);
    const auto sliceTotal = parsePositiveInt(sliceTotalText);
    if (!sliceStart || !sliceCount || !sliceTotal)
    {
        log::error("{} requires --build-slice-start, --build-slice-count, and --build-slice-total.", kBuildWorkerArgument);
        return std::nullopt;
    }

    if (*sliceStart > *sliceTotal || *sliceCount > (*sliceTotal - *sliceStart + 1))
    {
        log::error("Build-worker slice {}-{} is outside the total script count {}.", *sliceStart, *sliceStart + *sliceCount - 1,
                   *sliceTotal);
        return std::nullopt;
    }

    std::vector<std::string> bridgeArgs;
    bridgeArgs.push_back(buildDirectoryText);

    if (!pluginText.empty())
    {
        bridgeArgs.push_back("--plugin");
        bridgeArgs.push_back(pluginText);
    }

    if (!outputDirectoryText.empty())
    {
        bridgeArgs.push_back("--output-directory");
        bridgeArgs.push_back(outputDirectoryText);
    }

    if (!timeoutValue.empty())
    {
        bridgeArgs.push_back("--timeout-seconds");
        bridgeArgs.push_back(timeoutValue);
    }

    if (noTimeoutRequested)
        bridgeArgs.push_back("--no-timeout");

    if (forceScan)
        bridgeArgs.push_back("--force-scan");

    auto parseResult = parseBuildOptionsDetailed(bridgeArgs);
    if (!parseResult.options)
    {
        logDiagnostics(parseResult.diagnostics);
        return std::nullopt;
    }

    AppOptionsAccess::setBuildWorkerSlice(*parseResult.options, *sliceStart, *sliceCount, *sliceTotal);
    if (!resultFileText.empty())
        AppOptionsAccess::setBuildWorkerResultFile(*parseResult.options, std::filesystem::path(resultFileText));

    return parseResult.options;
}

} // namespace halionbridge::detail
