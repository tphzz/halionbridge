#pragma once

#include "halionbridge/Bridge.h"

#include <optional>
#include <span>
#include <string>

#include <juce_core/juce_core.h>

namespace halionbridge::detail
{

constexpr const char* kBuildWorkerArgument = "--halionbridge-build-worker";

std::optional<AppOptions> parseBuildWorkerArguments(std::span<const std::string> args);
juce::StringArray makeBuildWorkerCommand(const AppOptions& options, int sliceStart, int sliceCount, int totalScripts);

int runResultToBuildWorkerExitCode(RunResult result) noexcept;
std::optional<RunResult> buildWorkerExitCodeToRunResult(int exitCode) noexcept;

} // namespace halionbridge::detail
