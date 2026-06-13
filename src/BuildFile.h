#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace halionbridge::detail
{

struct BuildFileGenerationResult
{
    bool succeeded = false;
    juce::File buildFile;
    std::vector<juce::String> moduleNames;
    juce::String message;
};

struct InitCommandResult
{
    int exitCode = 1;
    juce::String message;
    juce::String warning;
    std::vector<juce::String> moduleNames;
};

std::vector<juce::File> findTopLevelLuaBuildScripts(const juce::File& directory);
bool hasTopLevelLuaBuildScripts(const juce::File& directory);
BuildFileGenerationResult generateBuildFile(const juce::File& directory, bool overwriteExisting);
InitCommandResult runInitCommand(const juce::StringArray& args);

} // namespace halionbridge::detail
