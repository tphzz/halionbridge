#include "halionbridge/BuildInfo.h"
#include "halionbridge/Bridge.h"
#include "halionbridge/CrashDiagnostics.h"
#if HALIONBRIDGE_ENABLE_CONVERTERS
#include "halionbridge_converters/BuildDirectoryEmitter.h"
#include "halionbridge_converters/Converter.h"
#if HALIONBRIDGE_ENABLE_CONVERTER_DX7
#include "halionbridge_converters/dx7/Dx7Converter.h"
#endif
#include "halionbridge_converters/sfz/SfzConverter.h"
#endif

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
    auto inspectionOptions = halionbridge::VstPresetInspectionOptions{};
    inspectionOptions.recursive = true;

    const auto markers = halionbridge::Bridge::getBuildStatusMarkerFilesForDirectory(std::filesystem::path("example-build-dir"));

    const auto buildInfo = halionbridge::getBuildInfo();
    const auto result = halionbridge::RunResult::success;
    const auto macroPageOptions = halionbridge::VstPresetMacroPageInjectionOptions{};

#if HALIONBRIDGE_ENABLE_CONVERTERS
    auto registry = halionbridge::converters::ConverterRegistry{};
    auto buildDirectoryRequest = halionbridge::converters::BuildDirectoryRequest{};
#if HALIONBRIDGE_ENABLE_CONVERTER_DX7
    auto dx7Options = halionbridge::converters::dx7::ConversionOptions{};
#endif
    auto sfzOptions = halionbridge::converters::sfz::ConversionOptions{};
#endif

    return static_cast<int>(options.has_value()) + static_cast<int>(!markers.okFile.empty()) +
           static_cast<int>(buildInfo.versionString != nullptr) + static_cast<int>(directOptions.forceScan) +
           static_cast<int>(inspectionOptions.recursive) + static_cast<int>(macroPageOptions.recursive) +
           static_cast<int>(result == halionbridge::RunResult::success)
#if HALIONBRIDGE_ENABLE_CONVERTERS
           + static_cast<int>(registry.list().empty()) + static_cast<int>(buildDirectoryRequest.scripts.empty()) +
#if HALIONBRIDGE_ENABLE_CONVERTER_DX7
           static_cast<int>(dx7Options.sourcePath.empty()) +
#endif
           static_cast<int>(sfzOptions.sourcePath.empty()) + static_cast<int>(sfzOptions.sourceDirectory.empty())
#endif
        ;
}
