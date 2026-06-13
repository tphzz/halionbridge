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

std::vector<juce::File> findTopLevelLuaBuildScripts(const juce::File& directory);
bool hasTopLevelLuaBuildScripts(const juce::File& directory);
BuildFileGenerationResult generateBuildFile(const juce::File& directory, bool overwriteExisting);

} // namespace halionbridge::detail
