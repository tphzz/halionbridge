#include "halionbridge_converters/dx7/Dx7Converter.h"

#include "dxsyx/Dx7Sysex.h"
#include "halionbridge_converters/BuildDirectoryEmitter.h"
#include "halionbridge_dx7_assets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace halionbridge::converters::dx7
{
namespace
{

constexpr auto kMaximumSyxFileBytes = std::streamoff{64 * 1024 * 1024};
constexpr auto kHelperFileName = std::string_view{"halionbridge-dx7.lua"};
constexpr auto kManifestFileName = std::string_view{"halionbridge_build_manifest.json"};

struct DiscoveredFiles
{
    std::vector<std::filesystem::path> files;
    std::vector<Diagnostic> diagnostics;
};

struct ParsedSource
{
    std::filesystem::path sourceFile;
    std::filesystem::path relativeParent;
    std::string safeSourceStem;
    std::vector<dxsyx::Message> messages;
};

struct VoiceJob
{
    std::filesystem::path sourceFile;
    std::filesystem::path outputFile;
    std::string displayName;
    dxsyx::Voice voice;
};

struct EmbeddedAssets
{
    std::string helperLua;
    std::vector<GeneratedBuildFile> templates;
};

Diagnostic makeDiagnostic(const DiagnosticLevel level, const std::filesystem::path& source, std::string code, std::string message)
{
    return Diagnostic{level, source, 0, std::move(code), std::move(message)};
}

Diagnostic makeError(const std::filesystem::path& source, std::string code, std::string message)
{
    return makeDiagnostic(DiagnosticLevel::error, source, std::move(code), std::move(message));
}

Diagnostic makeInfo(const std::filesystem::path& source, std::string code, std::string message)
{
    return makeDiagnostic(DiagnosticLevel::info, source, std::move(code), std::move(message));
}

std::string lowerCase(std::string text)
{
    std::ranges::transform(text, text.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

bool hasSyxExtension(const std::filesystem::path& path)
{
    return lowerCase(path.extension().string()) == ".syx";
}

bool shouldStop(const ConverterRunContext* context)
{
    return context != nullptr && context->shouldStop();
}

std::string zeroPadded(const std::size_t value, const int width)
{
    auto text = std::ostringstream{};
    text << std::setfill('0') << std::setw(width) << value;
    return text.str();
}

std::string trimVoiceName(std::string name)
{
    const auto isSpace = [](const unsigned char character) { return std::isspace(character) != 0; };
    const auto first = std::ranges::find_if_not(name, isSpace);
    const auto last = std::ranges::find_if_not(name | std::views::reverse, isSpace).base();
    if (first >= last)
        return "Unnamed";
    return std::string(first, last);
}

bool isReservedWindowsStem(const std::string_view text)
{
    const auto lower = lowerCase(std::string(text));
    static constexpr auto fixedNames = std::array<std::string_view, 4>{"con", "prn", "aux", "nul"};
    if (std::ranges::find(fixedNames, lower) != fixedNames.end())
        return true;

    if (lower.size() != 4)
        return false;
    const auto prefix = lower.substr(0, 3);
    return (prefix == "com" || prefix == "lpt") && lower[3] >= '1' && lower[3] <= '9';
}

std::string safePathComponent(const std::string_view text, const std::string_view fallback)
{
    auto result = std::string{};
    result.reserve(text.size());
    auto previousWasUnderscore = false;
    for (const auto rawCharacter : text)
    {
        const auto character = static_cast<unsigned char>(rawCharacter);
        const auto safe = character < 128 && (std::isalnum(character) != 0 || character == '-' || character == '_');
        const auto output = safe ? static_cast<char>(character) : '_';
        if (output == '_')
        {
            if (previousWasUnderscore)
                continue;
            previousWasUnderscore = true;
        }
        else
        {
            previousWasUnderscore = false;
        }
        result.push_back(output);
    }

    while (!result.empty() && (result.front() == '_' || result.front() == '.'))
        result.erase(result.begin());
    while (!result.empty() && (result.back() == '_' || result.back() == '.'))
        result.pop_back();

    if (result.empty())
        result = fallback;
    if (isReservedWindowsStem(result))
        result += "_preset";
    return result;
}

std::string safeVoiceFileStem(const std::string_view rawName, const std::string_view displayName)
{
    const auto isSpace = [](const unsigned char character) { return std::isspace(character) != 0; };
    if (std::ranges::all_of(rawName, isSpace))
        return "unnamed";
    return safePathComponent(displayName, "unnamed");
}

std::filesystem::path safeRelativeParent(const std::filesystem::path& parent)
{
    auto result = std::filesystem::path{};
    for (const auto& component : parent)
    {
        if (component == "." || component.empty())
            continue;
        result /= safePathComponent(component.string(), "unnamed");
    }
    return result;
}

std::string normalizedPathKey(const std::filesystem::path& path)
{
    return lowerCase(path.generic_string());
}

std::filesystem::path makeUniqueOutputPath(std::filesystem::path candidate, std::set<std::string>& usedPaths)
{
    if (usedPaths.insert(normalizedPathKey(candidate)).second)
        return candidate;

    const auto parent = candidate.parent_path();
    const auto stem = candidate.stem().string();
    const auto extension = candidate.extension().string();
    for (auto suffix = std::size_t{2}; suffix < std::numeric_limits<std::size_t>::max(); ++suffix)
    {
        auto disambiguated = parent / (stem + "_" + zeroPadded(suffix, 3) + extension);
        if (usedPaths.insert(normalizedPathKey(disambiguated)).second)
            return disambiguated;
    }
    return candidate;
}

void addInspectionFailure(DiscoveredFiles& result, const std::filesystem::path& path, const std::error_code& error)
{
    result.diagnostics.push_back(
        makeError(path, "source-inspection-failed", "Could not inspect DX7 source path " + path.string() + ": " + error.message()));
}

void considerDirectoryEntry(const std::filesystem::directory_entry& entry, DiscoveredFiles& result)
{
    auto error = std::error_code{};
    const auto status = entry.symlink_status(error);
    if (error)
    {
        addInspectionFailure(result, entry.path(), error);
        return;
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) || !hasSyxExtension(entry.path()))
        return;
    result.files.push_back(entry.path());
}

DiscoveredFiles findSyxFiles(const std::filesystem::path& root, const bool recursive, const ConverterRunContext* context)
{
    auto result = DiscoveredFiles{};
    auto error = std::error_code{};
    if (recursive)
    {
        auto iterator = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::none, error);
        const auto end = std::filesystem::recursive_directory_iterator{};
        if (error)
        {
            addInspectionFailure(result, root, error);
            return result;
        }

        while (iterator != end)
        {
            if (shouldStop(context))
                break;

            const auto status = iterator->symlink_status(error);
            if (error)
            {
                addInspectionFailure(result, iterator->path(), error);
                return result;
            }
            if (std::filesystem::is_symlink(status))
                iterator.disable_recursion_pending();
            considerDirectoryEntry(*iterator, result);
            if (!result.diagnostics.empty())
                return result;
            iterator.increment(error);
            if (error)
            {
                addInspectionFailure(result, root, error);
                return result;
            }
        }
    }
    else
    {
        auto iterator = std::filesystem::directory_iterator(root, std::filesystem::directory_options::none, error);
        const auto end = std::filesystem::directory_iterator{};
        if (error)
        {
            addInspectionFailure(result, root, error);
            return result;
        }

        while (iterator != end)
        {
            if (shouldStop(context))
                break;
            considerDirectoryEntry(*iterator, result);
            if (!result.diagnostics.empty())
                return result;
            iterator.increment(error);
            if (error)
            {
                addInspectionFailure(result, root, error);
                return result;
            }
        }
    }

    std::ranges::sort(result.files,
                      [&root](const auto& first, const auto& second)
                      {
                          const auto firstRelative = first.lexically_relative(root).generic_string();
                          const auto secondRelative = second.lexically_relative(root).generic_string();
                          const auto firstKey = lowerCase(firstRelative);
                          const auto secondKey = lowerCase(secondRelative);
                          return firstKey == secondKey ? firstRelative < secondRelative : firstKey < secondKey;
                      });
    return result;
}

std::optional<std::vector<std::uint8_t>> readSyxFile(const std::filesystem::path& path, std::vector<Diagnostic>& diagnostics)
{
    auto stream = std::ifstream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        diagnostics.push_back(makeError(path, "source-read", "Could not open DX7 SysEx file: " + path.string()));
        return std::nullopt;
    }

    const auto size = stream.tellg();
    if (size <= 0 || size > kMaximumSyxFileBytes)
    {
        diagnostics.push_back(
            makeError(path, "source-size", "DX7 SysEx file is empty or exceeds the 64 MiB safety limit: " + path.string()));
        return std::nullopt;
    }

    auto bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream)
    {
        diagnostics.push_back(makeError(path, "source-read", "Could not read complete DX7 SysEx file: " + path.string()));
        return std::nullopt;
    }
    return bytes;
}

Diagnostic issueDiagnostic(const std::filesystem::path& source, const dxsyx::ParseIssue& issue)
{
    auto message = std::ostringstream{};
    message << issue.message << " File byte offset: " << issue.byteOffset << ", message: " << issue.messageIndex + 1;
    if (issue.voiceIndex)
        message << ", voice: " << *issue.voiceIndex + 1;
    message << ".";
    return makeDiagnostic(issue.severity == dxsyx::IssueSeverity::error ? DiagnosticLevel::error : DiagnosticLevel::warning, source,
                          issue.code, message.str());
}

std::filesystem::path defaultOutputDirectory(const std::filesystem::path& sourcePath)
{
    auto error = std::error_code{};
    return std::filesystem::is_directory(sourcePath, error) && !error ? sourcePath : sourcePath.parent_path();
}

std::vector<VoiceJob> makeVoiceJobs(const std::vector<ParsedSource>& parsedSources)
{
    auto jobs = std::vector<VoiceJob>{};
    auto usedPaths = std::set<std::string>{};
    for (const auto& parsed : parsedSources)
    {
        const auto sourceBase = parsed.relativeParent / parsed.safeSourceStem;
        for (auto messageIndex = std::size_t{0}; messageIndex < parsed.messages.size(); ++messageIndex)
        {
            const auto ordinal = zeroPadded(messageIndex + 1, 3);
            const auto& message = parsed.messages[messageIndex];
            if (const auto* bank = std::get_if<dxsyx::BankMessage>(&message.data))
            {
                for (auto voiceIndex = std::size_t{0}; voiceIndex < bank->voices.size(); ++voiceIndex)
                {
                    const auto displayName = trimVoiceName(bank->voices[voiceIndex].name);
                    const auto fileName =
                        zeroPadded(voiceIndex + 1, 2) + "_" + safeVoiceFileStem(bank->voices[voiceIndex].name, displayName) + ".vstpreset";
                    auto output = sourceBase / ("bank_" + ordinal) / fileName;
                    output = makeUniqueOutputPath(std::move(output), usedPaths);
                    jobs.push_back(VoiceJob{parsed.sourceFile, std::move(output), displayName, bank->voices[voiceIndex]});
                }
            }
            else if (const auto* single = std::get_if<dxsyx::SingleVoiceMessage>(&message.data))
            {
                const auto displayName = trimVoiceName(single->voice.name);
                const auto fileName = "single_" + ordinal + "_" + safeVoiceFileStem(single->voice.name, displayName) + ".vstpreset";
                auto output = makeUniqueOutputPath(sourceBase / fileName, usedPaths);
                jobs.push_back(VoiceJob{parsed.sourceFile, std::move(output), displayName, single->voice});
            }
        }
    }
    return jobs;
}

void appendNumberArray(std::ostringstream& lua, const std::string_view name, const std::span<const std::uint8_t> values, const int indent)
{
    lua << std::string(static_cast<std::size_t>(indent), ' ') << name << " = { ";
    for (auto index = std::size_t{0}; index < values.size(); ++index)
    {
        if (index != 0)
            lua << ", ";
        lua << static_cast<int>(values[index]);
    }
    lua << " },\n";
}

std::string buildLuaSource(const VoiceJob& job)
{
    const auto& voice = job.voice;
    const auto algorithm = static_cast<std::size_t>(voice.algorithm) + 1;
    auto lua = std::ostringstream{};
    lua << "-- Generated by halionbridge from Yamaha DX7 SysEx.\n"
        << "-- The algorithm-specific template is loaded before the remaining DX7 voice parameters are applied.\n"
        << "local dx7 = require(\"halionbridge-dx7\")\n\n"
        << "return function(ctx)\n"
        << "    return dx7.build_voice(ctx, {\n"
        << "        name = " << luaQuotedString(job.displayName) << ",\n"
        << "        algorithm = " << algorithm << ",\n"
        << "        template_file = " << luaQuotedString(".halionbridge/dx7/templates/dx7_" + zeroPadded(algorithm, 2) + ".vstpreset")
        << ",\n"
        << "        output_file = " << luaQuotedString(job.outputFile.generic_string()) << ",\n"
        << "        feedback = " << static_cast<int>(voice.feedback) << ",\n"
        << "        oscillator_sync = " << (voice.oscillatorSync ? "true" : "false") << ",\n"
        << "        transpose = " << static_cast<int>(voice.transpose) << ",\n";
    appendNumberArray(lua, "pitch_envelope_rates", voice.pitchEnvelopeRates, 8);
    appendNumberArray(lua, "pitch_envelope_levels", voice.pitchEnvelopeLevels, 8);
    lua << "        lfo = {\n"
        << "            speed = " << static_cast<int>(voice.lfoSpeed) << ",\n"
        << "            delay = " << static_cast<int>(voice.lfoDelay) << ",\n"
        << "            pitch_modulation_depth = " << static_cast<int>(voice.lfoPitchModulationDepth) << ",\n"
        << "            amplitude_modulation_depth = " << static_cast<int>(voice.lfoAmplitudeModulationDepth) << ",\n"
        << "            pitch_modulation_sensitivity = " << static_cast<int>(voice.lfoPitchModulationSensitivity) << ",\n"
        << "            waveform = " << static_cast<int>(voice.lfoWaveform) << ",\n"
        << "            sync = " << (voice.lfoSync ? "true" : "false") << ",\n"
        << "        },\n"
        << "        operators = {\n";

    for (auto operatorIndex = std::size_t{0}; operatorIndex < voice.operators.size(); ++operatorIndex)
    {
        const auto& op = voice.operators[operatorIndex];
        lua << "            { -- Operator " << operatorIndex + 1 << "\n";
        appendNumberArray(lua, "envelope_rates", op.envelopeRates, 16);
        appendNumberArray(lua, "envelope_levels", op.envelopeLevels, 16);
        lua << "                key_level_breakpoint = " << static_cast<int>(op.keyLevelBreakpoint) << ",\n"
            << "                key_level_left_depth = " << static_cast<int>(op.keyLevelLeftDepth) << ",\n"
            << "                key_level_right_depth = " << static_cast<int>(op.keyLevelRightDepth) << ",\n"
            << "                left_curve = " << static_cast<int>(op.leftCurve) << ",\n"
            << "                right_curve = " << static_cast<int>(op.rightCurve) << ",\n"
            << "                rate_scaling = " << static_cast<int>(op.rateScaling) << ",\n"
            << "                amplitude_modulation_sensitivity = " << static_cast<int>(op.amplitudeModulationSensitivity) << ",\n"
            << "                key_velocity_sensitivity = " << static_cast<int>(op.keyVelocitySensitivity) << ",\n"
            << "                output_level = " << static_cast<int>(op.outputLevel) << ",\n"
            << "                oscillator_mode = " << (op.oscillatorMode == dxsyx::OscillatorMode::ratio ? "\"ratio\"" : "\"fixed\"")
            << ",\n"
            << "                frequency_coarse = " << static_cast<int>(op.frequencyCoarse) << ",\n"
            << "                frequency_fine = " << static_cast<int>(op.frequencyFine) << ",\n"
            << "                detune = " << static_cast<int>(op.detune) << ",\n"
            << "            },\n";
    }

    lua << "        },\n"
        << "    })\n"
        << "end\n";
    return lua.str();
}

std::vector<std::byte> byteVector(const char* data, const int size)
{
    auto result = std::vector<std::byte>{};
    if (data == nullptr || size <= 0)
        return result;
    result.reserve(static_cast<std::size_t>(size));
    for (auto index = 0; index < size; ++index)
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(data[index])));
    return result;
}

std::optional<EmbeddedAssets> loadEmbeddedAssets(std::vector<Diagnostic>& diagnostics)
{
    auto result = EmbeddedAssets{};
    auto templateNames = std::set<std::string>{};
    for (auto index = 0; index < halionbridge_dx7_assets::namedResourceListSize; ++index)
    {
        auto size = 0;
        const auto* data = halionbridge_dx7_assets::getNamedResource(halionbridge_dx7_assets::namedResourceList[index], size);
        const auto original = std::filesystem::path(halionbridge_dx7_assets::originalFilenames[index]).filename().string();
        if (data == nullptr || size <= 0)
        {
            diagnostics.push_back(makeError({}, "embedded-resource", "Embedded DX7 converter resource is empty: " + original));
            return std::nullopt;
        }

        if (original == kHelperFileName)
        {
            result.helperLua.assign(data, static_cast<std::size_t>(size));
            continue;
        }

        if (hasSyxExtension(original) || lowerCase(std::filesystem::path(original).extension().string()) != ".vstpreset" ||
            !std::string_view(original).starts_with("dx7_"))
        {
            diagnostics.push_back(makeError({}, "embedded-resource", "Unexpected embedded DX7 converter resource: " + original));
            return std::nullopt;
        }
        if (!templateNames.insert(lowerCase(original)).second)
        {
            diagnostics.push_back(makeError({}, "embedded-resource", "Duplicate embedded DX7 algorithm template: " + original));
            return std::nullopt;
        }
        result.templates.push_back(
            GeneratedBuildFile{std::filesystem::path(".halionbridge/dx7/templates") / original, byteVector(data, size)});
    }

    if (result.helperLua.empty() || result.templates.size() != 32)
    {
        diagnostics.push_back(makeError({}, "embedded-resource", "DX7 converter must contain one Lua helper and 32 algorithm templates."));
        return std::nullopt;
    }

    std::ranges::sort(result.templates, {}, [](const auto& file) { return file.relativePath.generic_string(); });
    for (auto algorithm = std::size_t{1}; algorithm <= 32; ++algorithm)
    {
        const auto expected = ".halionbridge/dx7/templates/dx7_" + zeroPadded(algorithm, 2) + ".vstpreset";
        if (result.templates[algorithm - 1].relativePath.generic_string() != expected)
        {
            diagnostics.push_back(makeError({}, "embedded-resource", "Missing embedded DX7 algorithm template: " + expected));
            return std::nullopt;
        }
    }
    return result;
}

std::string buildManifest(const std::vector<VoiceJob>& jobs)
{
    auto directories = std::set<std::string>{};
    for (const auto& job : jobs)
        directories.insert(job.outputFile.parent_path().generic_string());

    auto json = std::ostringstream{};
    json << "{\n  \"schema_version\": 1,\n  \"output_directories\": [\n";
    auto index = std::size_t{0};
    for (const auto& directory : directories)
    {
        json << "    \"" << directory << "\"";
        if (++index != directories.size())
            json << ',';
        json << '\n';
    }
    json << "  ]\n}\n";
    return json.str();
}

std::vector<std::byte> textBytes(const std::string_view text)
{
    auto result = std::vector<std::byte>{};
    result.reserve(text.size());
    for (const auto character : text)
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    return result;
}

std::string helpText()
{
    return "Usage:\n"
           "  halionbridge convert dx7 <source-path> [options]\n"
           "  halionbridge convert dx7 <source-path> <output-directory> [options]\n\n"
           "<source-path> may be a Yamaha DX7 .syx file or a directory containing .syx files. Each DX7 voice becomes one HALion "
           ".vstpreset.\n"
           "When output-directory is omitted, generated Lua/build files are written beside the source file or into the source "
           "directory.\n\n"
           "Options:\n"
           "  --recursive             Include .syx files below a source directory recursively.\n"
           "  --overwrite             Replace existing generated build files.\n"
           "  --strict-parameters     Reject out-of-range DX7 parameter values instead of normalizing them.\n"
           "  --help, -h              Show this help and exit.\n";
}

ConverterArgumentParseResult validateArguments(const std::span<const std::string> args)
{
    auto result = ConverterArgumentParseResult{};
    auto positional = std::vector<std::string>{};
    auto recursive = false;
    const auto fail = [&result](const ConverterArgumentErrorKind kind, Diagnostic diagnostic)
    {
        result.exitCode = 1;
        result.errorKind = kind;
        result.diagnostics.push_back(std::move(diagnostic));
    };

    for (const auto& argument : args)
    {
        if (argument == "--help" || argument == "-h")
            return result;
        if (argument == "--recursive")
        {
            recursive = true;
            continue;
        }
        if (argument == "--overwrite" || argument == "--strict-parameters")
            continue;
        if (!argument.empty() && argument.front() == '-')
        {
            fail(ConverterArgumentErrorKind::syntax, makeError({}, "argument", "Unknown dx7 converter argument: " + argument));
            return result;
        }
        positional.push_back(argument);
    }

    if (positional.empty() || positional.size() > 2)
    {
        fail(ConverterArgumentErrorKind::syntax,
             makeError({}, "argument", "halionbridge convert dx7 requires a source path and optional output directory."));
        return result;
    }

    const auto source = std::filesystem::path(positional.front());
    auto error = std::error_code{};
    const auto status = std::filesystem::symlink_status(source, error);
    if (error || status.type() == std::filesystem::file_type::not_found)
    {
        fail(ConverterArgumentErrorKind::validation,
             makeError(source, "source-missing", "DX7 source path does not exist: " + source.string()));
        return result;
    }
    if (std::filesystem::is_symlink(status))
    {
        fail(ConverterArgumentErrorKind::validation,
             makeError(source, "source-symlink", "DX7 source path must not be a symbolic link: " + source.string()));
        return result;
    }

    const auto isFile = std::filesystem::is_regular_file(status);
    const auto isDirectory = std::filesystem::is_directory(status);
    if (!isFile && !isDirectory)
    {
        fail(ConverterArgumentErrorKind::validation,
             makeError(source, "source-not-file-or-directory", "DX7 source path is not a file or directory: " + source.string()));
        return result;
    }
    if (isFile && !hasSyxExtension(source))
    {
        fail(ConverterArgumentErrorKind::validation,
             makeError(source, "source-not-syx", "DX7 source file must have a .syx extension: " + source.string()));
        return result;
    }
    if (isFile && recursive)
    {
        fail(ConverterArgumentErrorKind::validation,
             makeError(source, "recursive-with-file", "--recursive can only be used when the DX7 source path is a directory."));
    }
    return result;
}

ConverterResult runConverterWithContext(const std::span<const std::string> args, const ConverterRunContext& context)
{
    auto result = ConverterResult{};
    auto options = ConversionOptions{};
    options.context = &context;
    auto positional = std::vector<std::string>{};
    for (const auto& argument : args)
    {
        if (argument == "--help" || argument == "-h")
        {
            result.exitCode = 0;
            result.diagnostics.push_back(makeInfo({}, "help", helpText()));
            context.report(result.diagnostics.back());
            return result;
        }
        if (argument == "--recursive")
            options.recursive = true;
        else if (argument == "--overwrite")
            options.overwrite = true;
        else if (argument == "--strict-parameters")
            options.strictParameters = true;
        else if (!argument.empty() && argument.front() == '-')
        {
            result.diagnostics.push_back(makeError({}, "argument", "Unknown dx7 converter argument: " + argument));
            context.report(result.diagnostics.back());
            return result;
        }
        else
            positional.push_back(argument);
    }

    if (positional.empty() || positional.size() > 2)
    {
        result.diagnostics.push_back(
            makeError({}, "argument", "halionbridge convert dx7 requires a source path and optional output directory."));
        context.report(result.diagnostics.back());
        return result;
    }

    options.sourcePath = positional.front();
    options.outputDirectory = positional.size() == 2 ? std::filesystem::path(positional[1]) : defaultOutputDirectory(options.sourcePath);
    const auto conversion = convertSource(options);
    result.diagnostics = conversion.diagnostics;
    result.exitCode = conversion.succeeded ? 0 : 1;
    if (conversion.succeeded)
    {
        result.diagnostics.push_back(makeInfo(conversion.buildFile, "generated",
                                              "Generated " + std::to_string(conversion.voicesConverted) +
                                                  " DX7 preset build script(s) from " + std::to_string(conversion.syxFilesConverted) +
                                                  " SysEx file(s). Run halionbridge build to create the .vstpreset files."));
        context.report(result.diagnostics.back());
    }
    return result;
}

ConverterResult runConverter(const std::span<const std::string> args)
{
    return runConverterWithContext(args, ConverterRunContext{});
}

} // namespace

ConversionResult convertSource(const ConversionOptions& options)
{
    auto result = ConversionResult{};
    auto reportedDiagnostics = std::size_t{0};
    const auto reportPending = [&]()
    {
        if (options.context == nullptr)
        {
            reportedDiagnostics = result.diagnostics.size();
            return;
        }
        for (; reportedDiagnostics < result.diagnostics.size(); ++reportedDiagnostics)
            options.context->report(result.diagnostics[reportedDiagnostics]);
    };
    const auto addDiagnostic = [&](Diagnostic diagnostic)
    {
        result.diagnostics.push_back(std::move(diagnostic));
        reportPending();
    };

    if (options.sourcePath.empty())
    {
        addDiagnostic(makeError({}, "source-missing", "DX7 source path is not set."));
        return result;
    }

    const auto sourcePath = options.sourcePath;
    const auto outputDirectory = options.outputDirectory.empty() ? defaultOutputDirectory(sourcePath) : options.outputDirectory;
    auto error = std::error_code{};
    const auto sourceStatus = std::filesystem::symlink_status(sourcePath, error);
    if (error || sourceStatus.type() == std::filesystem::file_type::not_found)
    {
        addDiagnostic(makeError(sourcePath, "source-missing", "DX7 source path does not exist: " + sourcePath.string()));
        return result;
    }
    if (std::filesystem::is_symlink(sourceStatus))
    {
        addDiagnostic(makeError(sourcePath, "source-symlink", "DX7 source path must not be a symbolic link: " + sourcePath.string()));
        return result;
    }

    const auto sourceIsDirectory = std::filesystem::is_directory(sourceStatus);
    const auto sourceIsFile = std::filesystem::is_regular_file(sourceStatus);
    if (!sourceIsDirectory && !sourceIsFile)
    {
        addDiagnostic(
            makeError(sourcePath, "source-not-file-or-directory", "DX7 source path is not a file or directory: " + sourcePath.string()));
        return result;
    }

    auto syxFiles = std::vector<std::filesystem::path>{};
    if (sourceIsFile)
    {
        if (!hasSyxExtension(sourcePath))
        {
            addDiagnostic(makeError(sourcePath, "source-not-syx", "DX7 source file must have a .syx extension: " + sourcePath.string()));
            return result;
        }
        if (options.recursive)
        {
            addDiagnostic(
                makeError(sourcePath, "recursive-with-file", "--recursive can only be used when the DX7 source path is a directory."));
            return result;
        }
        syxFiles.push_back(sourcePath);
    }
    else
    {
        addDiagnostic(makeInfo(sourcePath, "scan-started", "Scanning " + sourcePath.string() + " for DX7 .syx files."));
        auto discovered = findSyxFiles(sourcePath, options.recursive, options.context);
        syxFiles = std::move(discovered.files);
        result.diagnostics.insert(result.diagnostics.end(), std::make_move_iterator(discovered.diagnostics.begin()),
                                  std::make_move_iterator(discovered.diagnostics.end()));
        reportPending();
        if (!result.diagnostics.empty() &&
            std::ranges::any_of(result.diagnostics, [](const auto& diagnostic) { return diagnostic.level == DiagnosticLevel::error; }))
            return result;
    }

    if (shouldStop(options.context))
    {
        addDiagnostic(makeError(sourcePath, "stopped", "DX7 conversion stopped by user request."));
        return result;
    }
    if (syxFiles.empty())
    {
        addDiagnostic(makeError(sourcePath, "no-syx",
                                "No .syx files were found in " + sourcePath.string() +
                                    (options.recursive ? "." : ". Use --recursive to include nested directories.")));
        return result;
    }
    addDiagnostic(makeInfo(sourcePath, "scan-complete", "Found " + std::to_string(syxFiles.size()) + " DX7 .syx file(s)."));

    auto parsedSources = std::vector<ParsedSource>{};
    parsedSources.reserve(syxFiles.size());
    auto parseFailed = false;
    for (const auto& syxFile : syxFiles)
    {
        if (shouldStop(options.context))
        {
            addDiagnostic(makeError(syxFile, "stopped", "DX7 conversion stopped by user request."));
            return result;
        }

        auto bytes = readSyxFile(syxFile, result.diagnostics);
        reportPending();
        if (!bytes)
        {
            parseFailed = true;
            continue;
        }
        auto parsed = dxsyx::parse(*bytes, dxsyx::ParseOptions{options.strictParameters});
        for (const auto& issue : parsed.issues)
            result.diagnostics.push_back(issueDiagnostic(syxFile, issue));
        reportPending();
        if (!parsed.succeeded())
        {
            parseFailed = true;
            continue;
        }

        auto messageCount = parsed.messages.size();
        auto source = ParsedSource{syxFile, {}, safePathComponent(syxFile.stem().string(), "unnamed"), std::move(parsed.messages)};
        if (sourceIsDirectory)
            source.relativeParent = safeRelativeParent(syxFile.parent_path().lexically_relative(sourcePath));
        parsedSources.push_back(std::move(source));
        ++result.syxFilesConverted;
        result.messagesConverted += static_cast<int>(messageCount);
    }

    if (parseFailed)
    {
        addDiagnostic(makeError(sourcePath, "batch-aborted",
                                "No build directory was written because one or more DX7 SysEx files failed validation."));
        return result;
    }

    auto jobs = makeVoiceJobs(parsedSources);
    result.voicesConverted = static_cast<int>(jobs.size());
    if (jobs.empty())
    {
        addDiagnostic(makeError(sourcePath, "no-voices", "No DX7 voices were found in the validated SysEx input."));
        return result;
    }

    auto embedded = loadEmbeddedAssets(result.diagnostics);
    reportPending();
    if (!embedded)
        return result;

    auto scripts = std::vector<GeneratedLuaScript>{};
    scripts.reserve(jobs.size() + 1);
    scripts.push_back(
        GeneratedLuaScript{"", std::string(kHelperFileName), std::move(embedded->helperLua), GeneratedLuaFileRole::helperModule});
    for (auto index = std::size_t{0}; index < jobs.size(); ++index)
    {
        const auto moduleName = zeroPadded(index + 1, 6) + "_" + safePathComponent(jobs[index].displayName, "Unnamed") + ".lua";
        scripts.push_back(GeneratedLuaScript{moduleName, moduleName, buildLuaSource(jobs[index])});
    }

    auto generatedFiles = std::move(embedded->templates);
    generatedFiles.push_back(GeneratedBuildFile{std::filesystem::path(kManifestFileName), textBytes(buildManifest(jobs))});
    if (shouldStop(options.context))
    {
        addDiagnostic(makeError(outputDirectory, "stopped", "DX7 conversion stopped before writing generated files."));
        return result;
    }

    addDiagnostic(makeInfo(outputDirectory, "write-started",
                           "Writing " + std::to_string(jobs.size()) + " DX7 voice build script(s) to " + outputDirectory.string() + "."));
    auto emitted =
        writeBuildDirectory(BuildDirectoryRequest{outputDirectory, options.overwrite, std::move(scripts), std::move(generatedFiles)});
    result.buildFile = emitted.buildFile;
    result.generatedLuaFiles = std::move(emitted.generatedLuaFiles);
    result.generatedFiles = std::move(emitted.generatedFiles);
    result.diagnostics.insert(result.diagnostics.end(), std::make_move_iterator(emitted.diagnostics.begin()),
                              std::make_move_iterator(emitted.diagnostics.end()));
    reportPending();
    result.succeeded = emitted.succeeded;
    return result;
}

void registerConverter(ConverterRegistry& registry)
{
    auto definition = ConverterDefinition{"dx7",
                                          "Yamaha DX7 SysEx",
                                          "Generate one HALion preset build script per Yamaha DX7 SysEx voice.",
                                          runConverter,
                                          runConverterWithContext,
                                          helpText,
                                          validateArguments};
    definition.sourcePathKind = ConverterSourcePathKind::fileOrDirectory;
    registry.registerConverter(std::move(definition));
}

} // namespace halionbridge::converters::dx7
