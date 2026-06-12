#pragma once

#include <optional>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

namespace halionbridge::detail
{

std::optional<juce::PluginDescription> makeHalionDescriptionFromClassId(const juce::File& pluginFile, const juce::String& classId);

// Both scan variants read VST3 factory class metadata without instantiating
// the plugin. HALion 7 crashes in its module teardown when a component was
// initialized and terminated during the same module load, so a conventional
// instantiating scan (JUCE findAllTypesForFile) must not be used for it.
std::optional<juce::PluginDescription> scanPluginInWorker(const juce::File& executableFile, const juce::File& pluginFile,
                                                          const std::optional<juce::String>& preferredClassId);
std::optional<juce::PluginDescription> scanPluginInProcess(const juce::File& pluginFile,
                                                           const std::optional<juce::String>& preferredClassId);
int runPluginScanWorker(const juce::StringArray& args);

} // namespace halionbridge::detail
