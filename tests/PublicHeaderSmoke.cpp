#include "halionbridge/BuildInfo.h"
#include "halionbridge/Bridge.h"
#include "halionbridge/CrashDiagnostics.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

int halionbridge_public_headers_compile_without_juce()
{
    auto args = std::vector<std::string>{"example-build-dir", "--timeout-seconds", "0"};
    auto options = halionbridge::Bridge::parseArguments(args);
    auto directOptions = halionbridge::AppOptions{};
    directOptions.forceScan = true;

    const auto markers = halionbridge::Bridge::getBuildStatusMarkerFilesForDirectory(std::filesystem::path("example-build-dir"));

    const auto buildInfo = halionbridge::getBuildInfo();
    const auto result = halionbridge::RunResult::success;

    return static_cast<int>(options.has_value()) + static_cast<int>(!markers.okFile.empty()) +
           static_cast<int>(buildInfo.versionString != nullptr) + static_cast<int>(directOptions.forceScan) +
           static_cast<int>(result == halionbridge::RunResult::success);
}
