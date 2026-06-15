#pragma once

#include "halionbridge/halionbridge_export.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace halionbridge
{

namespace detail
{
struct AppOptionsAccess;
}

struct AppOptions
{
    std::optional<std::filesystem::path> buildDirectory;
    std::optional<std::filesystem::path> pluginPathOverride;
    std::optional<std::filesystem::path> executableFile;
    int timeoutSeconds = 3600;
    int buildChunkSize = 15;
    bool showGui = false;
    bool noKill = false;
    bool forceScan = false;
    bool failFast = false;

  private:
    friend struct detail::AppOptionsAccess;

    bool buildWorkerMode = false;
    int buildSliceStart = 0;
    int buildSliceCount = 0;
    int buildSliceTotal = 0;
};

namespace detail
{

struct AppOptionsAccess
{
    static void setBuildWorkerSlice(AppOptions& options, const int start, const int count, const int total) noexcept
    {
        options.buildWorkerMode = true;
        options.buildSliceStart = start;
        options.buildSliceCount = count;
        options.buildSliceTotal = total;
    }

    static bool isBuildWorkerMode(const AppOptions& options) noexcept
    {
        return options.buildWorkerMode;
    }
    static int buildSliceStart(const AppOptions& options) noexcept
    {
        return options.buildSliceStart;
    }
    static int buildSliceCount(const AppOptions& options) noexcept
    {
        return options.buildSliceCount;
    }
    static int buildSliceTotal(const AppOptions& options) noexcept
    {
        return options.buildSliceTotal;
    }
};

} // namespace detail

struct BuildStatusMarkerFiles
{
    std::filesystem::path okFile;
    std::filesystem::path failedFile;
};

struct VstPresetContainerInfo
{
    std::string classId;
    bool hasComponentState = false;
    bool hasControllerState = false;
    bool hasProgramData = false;
    std::optional<int> programOrUnitId;
};

enum class RunResult
{
    success,
    invalidOptions,
    invalidBridge,
    runtimeSetupFailed,
    anotherInstanceRunning,
    pluginNotFound,
    pluginLoadFailed,
    startupStopped,
    presetApplyFailed,
    buildFailed,
    stopped,
    timedOut,
    cleanupFailed
};

HALIONBRIDGE_EXPORT void requestStop() noexcept;
HALIONBRIDGE_EXPORT void resetStopRequest() noexcept;
HALIONBRIDGE_EXPORT bool isStopRequested() noexcept;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class HALIONBRIDGE_EXPORT Bridge
{
  public:
    Bridge();
    ~Bridge();

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    Bridge(Bridge&&) noexcept;
    Bridge& operator=(Bridge&&) noexcept;

    // Defensively parses command line arguments and validates files.
    // Returns std::nullopt and logs errors if parsing fails or files don't exist.
    static std::optional<AppOptions> parseArguments(const std::vector<std::string>& args);

    // Resolves the path to the HALion 7 VST3 plugin.
    // Falls back to standard OS locations if pluginPathOverride is empty.
    // Returns std::nullopt if the plugin cannot be found.
    static std::optional<std::filesystem::path> findHalionPlugin(const std::optional<std::filesystem::path>& pluginPathOverride);
    static std::filesystem::path getDefaultHalionPluginPath();

    static std::optional<VstPresetContainerInfo> inspectVstPresetContainer(std::span<const std::byte> presetData);
    static BuildStatusMarkerFiles getBuildStatusMarkerFilesForDirectory(const std::filesystem::path& directory);

    // Parses the common build file shape `return { "module_a", "module_b" }`.
    // This helper is for source inspection by tooling and is not a full Lua parser.
    static std::vector<std::string> parseBuildFileModuleNames(std::string_view luaText);
    static std::string createRuntimeModuleText(const std::filesystem::path& runtimeRoot);
    static std::string createRuntimeModuleText(const std::filesystem::path& runtimeRoot, int sliceStart, int sliceCount, int totalScripts);

    // Core execution loop. While running, halionbridge temporarily mutates the
    // process working directory, an environment variable, and HALion user script files.
    // Concurrent runs are rejected because those resources are process/global state.
    bool run(const AppOptions& options);
    RunResult runDetailed(const AppOptions& options);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace halionbridge
