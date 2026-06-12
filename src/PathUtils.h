#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <juce_core/juce_core.h>

namespace halionbridge::detail
{

juce::String toJuceString(const std::filesystem::path& path);
juce::String toJuceString(std::string_view text);
juce::File toJuceFile(const std::filesystem::path& path);
std::filesystem::path toStdPath(const juce::File& file);
std::string toStdString(const juce::String& text);
juce::File normalizeCliPath(juce::String path);

} // namespace halionbridge::detail
