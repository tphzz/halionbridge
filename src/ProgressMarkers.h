#pragma once

#include <string>
#include <string_view>
#include <set>

#include <juce_core/juce_core.h>

namespace halionbridge::detail
{

struct ProgressMarkerCleanupResult
{
    int found = 0;
    int deleted = 0;
    int failed = 0;
    std::set<std::string> remainingNames;
};

std::string decodeProgressMarkerText(std::string_view text);
void logNewProgressMarkers(const juce::File& directory, std::set<std::string>& seenMarkers);
ProgressMarkerCleanupResult deleteProgressMarkers(const juce::File& directory, const char* description);

} // namespace halionbridge::detail
