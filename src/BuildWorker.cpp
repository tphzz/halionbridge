#include "BuildWorker.h"

#include "CliCommand.h"
#include "PathUtils.h"

#include <array>

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
    return parseBuildWorkerOptions(args);
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

    if (options.outputDirectory)
    {
        command.add("--output-directory");
        command.add(toJuceString(*options.outputDirectory));
    }

    if (options.timeoutSeconds > 0)
    {
        command.add("--timeout-seconds");
        command.add(juce::String(options.timeoutSeconds));
    }
    else
    {
        command.add("--no-timeout");
    }

    if (options.forceScan)
        command.add("--force-scan");

    return command;
}

} // namespace halionbridge::detail
