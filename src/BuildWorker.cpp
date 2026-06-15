#include "BuildWorker.h"

#include "Log.h"
#include "PathUtils.h"

#include <array>
#include <limits>
#include <vector>

namespace halionbridge::detail
{
namespace
{

constexpr auto kRunResultValues = std::array{
    RunResult::success,
    RunResult::invalidOptions,
    RunResult::invalidBridge,
    RunResult::runtimeSetupFailed,
    RunResult::anotherInstanceRunning,
    RunResult::pluginNotFound,
    RunResult::pluginLoadFailed,
    RunResult::startupStopped,
    RunResult::presetApplyFailed,
    RunResult::buildFailed,
    RunResult::stopped,
    RunResult::timedOut,
    RunResult::cleanupFailed,
};

std::optional<int> parsePositiveInt(const std::string& text)
{
    if (text.empty())
        return std::nullopt;

    for (const auto c : text)
        if (c < '0' || c > '9')
            return std::nullopt;

    try
    {
        const auto value = std::stoll(text);
        if (value <= 0 || value > std::numeric_limits<int>::max())
            return std::nullopt;

        return static_cast<int>(value);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

} // namespace

int runResultToBuildWorkerExitCode(const RunResult result) noexcept
{
    return static_cast<int>(result);
}

std::optional<RunResult> buildWorkerExitCodeToRunResult(const int exitCode) noexcept
{
    for (const auto result : kRunResultValues)
        if (exitCode == runResultToBuildWorkerExitCode(result))
            return result;

    return std::nullopt;
}

std::optional<AppOptions> parseBuildWorkerArguments(std::span<const std::string> args)
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

    std::vector<std::string> bridgeArgs;
    bridgeArgs.push_back(args[1]);

    std::optional<int> sliceStart;
    std::optional<int> sliceCount;
    std::optional<int> sliceTotal;

    for (size_t i = 2; i < args.size(); ++i)
    {
        const auto& arg = args[i];

        const auto parseSliceValue = [&](std::optional<int>& target, const char* optionName) -> bool
        {
            if (i + 1 >= args.size())
            {
                log::error("{} requires a value.", optionName);
                return false;
            }

            auto parsed = parsePositiveInt(args[++i]);
            if (!parsed)
            {
                log::error("{} must be a positive integer.", optionName);
                return false;
            }

            target = *parsed;
            return true;
        };

        if (arg == "--build-slice-start")
        {
            if (!parseSliceValue(sliceStart, "--build-slice-start"))
                return std::nullopt;
        }
        else if (arg == "--build-slice-count")
        {
            if (!parseSliceValue(sliceCount, "--build-slice-count"))
                return std::nullopt;
        }
        else if (arg == "--build-slice-total")
        {
            if (!parseSliceValue(sliceTotal, "--build-slice-total"))
                return std::nullopt;
        }
        else if (arg == "--plugin" || arg == "--timeout-seconds")
        {
            if (i + 1 >= args.size())
            {
                log::error("{} requires a value.", arg);
                return std::nullopt;
            }

            bridgeArgs.push_back(arg);
            bridgeArgs.push_back(args[++i]);
        }
        else if (arg == "--force-scan")
        {
            bridgeArgs.push_back(arg);
        }
        else
        {
            log::error("Unknown build-worker argument: {}", arg);
            return std::nullopt;
        }
    }

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

    auto options = Bridge::parseArguments(bridgeArgs);
    if (!options)
        return std::nullopt;

    options->buildWorkerMode = true;
    options->buildSliceStart = *sliceStart;
    options->buildSliceCount = *sliceCount;
    options->buildSliceTotal = *sliceTotal;
    return options;
}

juce::StringArray makeBuildWorkerCommand(const AppOptions& options, const int sliceStart, const int sliceCount, const int totalScripts)
{
    juce::StringArray command;
    if (!options.executableFile || !options.buildDirectory)
        return command;

    command.add(toJuceString(*options.executableFile));
    command.add(kBuildWorkerArgument);
    command.add(toJuceString(*options.buildDirectory));
    command.add("--build-slice-start");
    command.add(juce::String(sliceStart));
    command.add("--build-slice-count");
    command.add(juce::String(sliceCount));
    command.add("--build-slice-total");
    command.add(juce::String(totalScripts));

    if (options.pluginPathOverride)
    {
        command.add("--plugin");
        command.add(toJuceString(*options.pluginPathOverride));
    }

    if (options.timeoutSeconds > 0)
    {
        command.add("--timeout-seconds");
        command.add(juce::String(options.timeoutSeconds));
    }

    if (options.forceScan)
        command.add("--force-scan");

    return command;
}

} // namespace halionbridge::detail
