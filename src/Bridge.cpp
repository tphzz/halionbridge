#include "halionbridge/Bridge.h"
#include "halionbridge/CrashDiagnostics.h"
#include "halionbridge/BuildInfo.h"
#include "halionbridge_assets.h"
#include "Log.h"
#include "PathUtils.h"
#include "PluginScan.h"
#include "ProgressMarkers.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/base/funknown.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/public.sdk/source/common/memorystream.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/public.sdk/source/vst/vstpresetfile.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <utility>

namespace halionbridge
{
namespace
{
constexpr double kSampleRate = 44100.0;
constexpr int kBlockSize = 512;
constexpr double kPluginInstantiationTimeoutMs = 30000.0;
constexpr int kInitialMessagePumpIterations = 20;
constexpr int kInitialMessagePumpMs = 50;
constexpr int kPrepareMessagePumpMs = 500;
constexpr int kEditorMessagePumpMs = 100;
constexpr int kProcessingDispatchMs = 10;
constexpr int kAsyncInstantiationDispatchMs = 20;
constexpr double kMarkerPollIntervalSeconds = 0.25;
constexpr double kBuildWaitProgressLogIntervalSeconds = 5.0;
constexpr double kNoKillHeartbeatIntervalSeconds = 30.0;
constexpr const char* kBuildStatusOkPresetFileName = "halionbridge_status_ok.vstpreset";
constexpr const char* kBuildStatusFailedPresetFileName = "halionbridge_status_failed.vstpreset";
constexpr const char* kPresetDirEnvironmentVariable = "HALIONBRIDGE_PRESET_DIR";
constexpr const char* kRuntimeModuleFileName = "halionbridge_runtime.lua";
constexpr const char* kBuilderModuleFileName = "halionbridge_builder.lua";
constexpr const char* kBuildFileName = "halionbridge_build.lua";
constexpr const char* kScriptDirectoryLockName = "halionbridge_halion_user_scripts";

std::atomic_bool gStopRequested{false};

std::optional<VstPresetContainerInfo> inspectVstPresetContainerData(const juce::MemoryBlock& presetData);

using detail::makeHalionDescriptionFromClassId;
using detail::normalizeCliPath;
using detail::scanPluginInProcess;
using detail::scanPluginInWorker;
using detail::toJuceFile;
using detail::toJuceString;
using detail::toStdPath;
using detail::toStdString;

juce::MemoryBlock toMemoryBlock(std::span<const std::byte> data)
{
    juce::MemoryBlock block;
    if (!data.empty())
        block.append(data.data(), data.size());
    return block;
}

void pumpMessages(const int milliseconds)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + milliseconds;

    while (juce::Time::getMillisecondCounterHiRes() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(juce::jmin(20, milliseconds));
}

std::optional<int> parseNonNegativeInt(const juce::String& text)
{
    const auto raw = text.trim().toStdString();
    if (raw.empty())
        return std::nullopt;

    for (const auto c : raw)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return std::nullopt;

    try
    {
        const auto value = std::stoll(raw);
        if (value > std::numeric_limits<int>::max())
            return std::nullopt;

        return static_cast<int>(value);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

juce::String toString(const Steinberg::FUID& id)
{
    Steinberg::FUID::String buffer{};
    id.toString(buffer);
    return juce::String(buffer);
}

void logPresetInfo(const VstPresetContainerInfo& info)
{
    log::debug("VST3 preset class ID: {}", info.classId);

    auto chunkText = fmt::format("VST3 preset chunks: Comp={}, Cont={}, Prog={}", info.hasComponentState ? "yes" : "no",
                                 info.hasControllerState ? "yes" : "no", info.hasProgramData ? "yes" : "no");

    if (info.programOrUnitId)
        chunkText += fmt::format(", program/unit id={}", *info.programOrUnitId);

    log::debug("{}", chunkText);
}

bool restoreProgramDataPreset(const juce::MemoryBlock& presetData, Steinberg::Vst::IComponent* component,
                              const VstPresetContainerInfo& info)
{
    if (component == nullptr || !info.hasProgramData)
        return false;

    auto presetDataCopy = presetData;
    Steinberg::MemoryStream stream(presetDataCopy.getData(), static_cast<Steinberg::TSize>(presetDataCopy.getSize()));
    Steinberg::Vst::PresetFile presetFile(&stream);

    if (!presetFile.readChunkList())
        return false;

    const auto targetId = static_cast<Steinberg::int32>(info.programOrUnitId.value_or(0));

    Steinberg::FUnknownPtr<Steinberg::Vst::IProgramListData> programListData(component);
    if (programListData)
    {
        auto programListId = static_cast<Steinberg::Vst::ProgramListID>(targetId);
        log::debug("Attempting VST3 program-list preset restore with list id {}...", programListId);

        if (presetFile.restoreProgramData(programListData, &programListId, 0))
        {
            log::debug("Success: VST3 program-list data accepted the preset.");
            return true;
        }

        log::debug("Diagnostic: IProgramListData rejected the preset.");
    }
    else
    {
        log::debug("Diagnostic: IProgramListData is not exposed by the plugin component.");
    }

    Steinberg::FUnknownPtr<Steinberg::Vst::IUnitData> unitData(component);
    if (unitData)
    {
        auto unitId = static_cast<Steinberg::Vst::UnitID>(targetId);
        log::debug("Attempting VST3 unit preset restore with unit id {}...", unitId);

        if (presetFile.restoreProgramData(unitData, &unitId))
        {
            log::debug("Success: VST3 unit data accepted the preset.");
            return true;
        }

        log::debug("Diagnostic: IUnitData rejected the preset.");
    }
    else
    {
        log::debug("Diagnostic: IUnitData is not exposed by the plugin component.");
    }

    Steinberg::FUnknownPtr<Steinberg::Vst::IUnitInfo> unitInfo(component);
    if (unitInfo)
    {
        log::debug("Attempting VST3 unit-info preset restore with id {}...", targetId);

        if (presetFile.restoreProgramData(unitInfo, targetId, -1))
        {
            log::debug("Success: VST3 unit-info data accepted the preset.");
            return true;
        }

        log::debug("Diagnostic: IUnitInfo rejected the preset.");
    }
    else
    {
        log::debug("Diagnostic: IUnitInfo is not exposed by the plugin component.");
    }

    return false;
}

class PluginWindow final : public juce::DocumentWindow
{
  public:
    PluginWindow(const juce::String& name, bool& closeRequestedRef)
        : juce::DocumentWindow(name, juce::Colours::darkgrey, juce::DocumentWindow::allButtons), closeRequested(closeRequestedRef)
    {
    }

    void closeButtonPressed() override
    {
        closeRequested = true;
        setVisible(false);
    }

  private:
    bool& closeRequested;
};

void holdPluginAliveForInspection(juce::AudioPluginInstance& plugin, const AppOptions& options, PluginWindow* window, bool& closeRequested,
                                  juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    setCrashDiagnosticPhase("runProcessingLoop: --nokill inspection hold");

    if (options.showGui && window != nullptr)
        log::info(
            "--nokill active: HALion remains loaded for inspection. Close the GUI window or press Ctrl+C to stop inspection and clean up.");
    else
        log::info("--nokill active: HALion remains loaded for inspection. Press Ctrl+C to stop inspection and clean up.");

    auto holdStart = juce::Time::getMillisecondCounterHiRes();
    double lastLog = 0.0;

    while (true)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(kAsyncInstantiationDispatchMs);
        buffer.clear();
        plugin.processBlock(buffer, midi);

        const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - holdStart) / 1000.0;
        if (elapsed - lastLog >= kNoKillHeartbeatIntervalSeconds)
        {
            log::info("--nokill: HALion still loaded for inspection ({}s elapsed).", static_cast<int>(elapsed));
            lastLog = elapsed;
        }

        if (isStopRequested())
        {
            log::info("--nokill: stop requested; leaving inspection hold.");
            break;
        }

        if (options.showGui && window != nullptr && closeRequested)
        {
            log::info("--nokill: GUI window closed; leaving inspection hold.");
            break;
        }
    }
}

struct OutputBaseline
{
    juce::File file;
    bool existed = false;
    juce::Time lastModificationTime;
    int64_t size = 0;

    bool hasChanged() const
    {
        if (!file.existsAsFile())
            return false;

        if (!existed)
            return true;

        return file.getLastModificationTime() != lastModificationTime || file.getSize() != size;
    }
};

OutputBaseline makeOutputBaseline(const juce::File& file)
{
    OutputBaseline baseline;
    baseline.file = file;
    baseline.existed = file.existsAsFile();
    baseline.lastModificationTime = baseline.existed ? file.getLastModificationTime() : juce::Time();
    baseline.size = baseline.existed ? file.getSize() : 0;
    return baseline;
}

bool deleteFileIfExists(const juce::File& file, const char* description)
{
    if (!file.existsAsFile())
        return true;

    log::debug("Deleting {}: {}", description, file.getFullPathName().toStdString());
    if (file.deleteFile())
        return true;

    log::error("Failed to delete {}.", description);
    return false;
}

std::optional<juce::String> getEnvironmentVariableIfSet(const char* name)
{
    constexpr const char* kUnsetSentinel = "\x1fhalionbridge_ENV_UNSET\x1f";
    const auto value = juce::SystemStats::getEnvironmentVariable(name, kUnsetSentinel);
    if (value == kUnsetSentinel)
        return std::nullopt;

    return value;
}

bool setEnvironmentVariable(const char* name, const juce::String& value)
{
#if JUCE_WINDOWS
    const auto nameString = juce::String(name);
    return _wputenv_s(nameString.toWideCharPointer(), value.toWideCharPointer()) == 0;
#else
    return setenv(name, value.toRawUTF8(), 1) == 0;
#endif
}

bool clearEnvironmentVariable(const char* name)
{
#if JUCE_WINDOWS
    const auto nameString = juce::String(name);
    return _wputenv_s(nameString.toWideCharPointer(), L"") == 0;
#else
    return unsetenv(name) == 0;
#endif
}

juce::File getHalionUserScriptDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Steinberg")
        .getChildFile("HALion")
        .getChildFile("Library")
        .getChildFile("scripts");
}

juce::String luaQuotedString(juce::String value)
{
    value = value.replace("\\", "\\\\");
    value = value.replace("\"", "\\\"");
    value = value.replace("\r", "\\r");
    value = value.replace("\n", "\\n");
    return "\"" + value + "\"";
}

juce::MemoryBlock makeEmbeddedBootstrapPresetData()
{
    return juce::MemoryBlock(halionbridge_assets::builder_bootstrap_vstpreset,
                             static_cast<size_t>(halionbridge_assets::builder_bootstrap_vstpresetSize));
}

juce::String getEmbeddedBuilderModuleText()
{
    return juce::String::fromUTF8(halionbridge_assets::builder_lua, halionbridge_assets::builder_luaSize);
}

class ScopedTemporaryTextFile
{
  public:
    bool write(const juce::File& targetFile, const juce::String& text, const char* description)
    {
        file = targetFile;
        existed = file.existsAsFile();
        if (existed)
            previousText = file.loadFileAsString();

        const auto parent = file.getParentDirectory();
        if (!parent.createDirectory())
        {
            log::error("Failed to create {} directory: {}", description, parent.getFullPathName().toStdString());
            return false;
        }

        if (!file.replaceWithText(text, false, false, "\n"))
        {
            log::error("Failed to write {}: {}", description, file.getFullPathName().toStdString());
            return false;
        }

        changed = true;
        return true;
    }

    ~ScopedTemporaryTextFile()
    {
        if (!changed)
            return;

        if (existed)
        {
            if (!file.replaceWithText(previousText, false, false, "\n"))
                log::warn("Failed to restore temporary HALion script file: {}", file.getFullPathName().toStdString());
        }
        else if (file.existsAsFile() && !file.deleteFile())
        {
            log::warn("Failed to delete temporary HALion script file: {}", file.getFullPathName().toStdString());
        }
    }

  private:
    juce::File file;
    juce::String previousText;
    bool existed = false;
    bool changed = false;
};

juce::String createRuntimeModuleTextForFile(const juce::File& runtimeRoot)
{
    auto root = runtimeRoot.getFullPathName().replace("\\", "/");

    return juce::String("-- Generated by halionbridge.exe for the embedded bootstrap preset that is currently being loaded.\n"
                        "--\n"
                        "-- HALion resolves require() through its script library paths before the copied\n"
                        "-- builder folder is known to Lua. This runtime module points require() at the\n"
                        "-- build directory passed to halionbridge.exe, then loads the embedded builder\n"
                        "-- module that was written temporarily beside this runtime module.\n\n"
                        "local runtimeRoot = ") +
           luaQuotedString(root) +
           "\n"
           "runtimeRoot = runtimeRoot:gsub(\"\\\\\", \"/\")\n"
           "if runtimeRoot:sub(-1) ~= \"/\" then\n"
           "    runtimeRoot = runtimeRoot .. \"/\"\n"
           "end\n"
           "HALIONBRIDGE_RUNTIME_ROOT = runtimeRoot\n\n"
           "local runtimePathPrefix = runtimeRoot .. \"?.lua;\" .. runtimeRoot .. \"?/init.lua;\"\n"
           "if not package.path:find(runtimePathPrefix, 1, true) then\n"
           "    package.path = runtimePathPrefix .. package.path\n"
           "end\n\n"
           "local ok, result = pcall(require, \"halionbridge_builder\")\n"
           "if not ok then\n"
           "    error(\n"
           "        \"halionbridge runtime failed while loading halionbridge_builder.lua.\\n\" ..\n"
           "        \"Runtime root: \" .. runtimeRoot .. \"\\n\" ..\n"
           "        \"package.path:\\n\" .. tostring(package.path) .. \"\\n\" ..\n"
           "        \"Original error:\\n\" .. tostring(result)\n"
           "    )\n"
           "end\n";
}

class ScopedPresetRuntimeRoot
{
  public:
    explicit ScopedPresetRuntimeRoot(const std::optional<juce::File>& runtimeRootIn)
    {
        if (!runtimeRootIn)
        {
            ready = true;
            return;
        }

        runtimeRoot = *runtimeRootIn;

        if (!scriptDirectoryLock.enter(0))
        {
            log::error("Another halionbridge instance is already using HALion's user script directory. Wait for it to finish and retry.");
            failureResult = RunResult::anotherInstanceRunning;
            return;
        }

        lockAcquired = true;
        previousWorkingDirectory = juce::File::getCurrentWorkingDirectory();
        previousEnvironmentValue = getEnvironmentVariableIfSet(kPresetDirEnvironmentVariable);

        if (!setEnvironmentVariable(kPresetDirEnvironmentVariable, runtimeRoot.getFullPathName()))
        {
            log::error("Failed to set {} for HALion Lua bootstrap.", kPresetDirEnvironmentVariable);
            return;
        }

        environmentChanged = true;

        if (!runtimeRoot.setAsCurrentWorkingDirectory())
        {
            log::error("Failed to set working directory to {}", runtimeRoot.getFullPathName().toStdString());
            return;
        }

        workingDirectoryChanged = true;

        const auto scriptDirectory = getHalionUserScriptDirectory();
        const auto runtimeModuleFile = scriptDirectory.getChildFile(kRuntimeModuleFileName);
        const auto builderModuleFile = scriptDirectory.getChildFile(kBuilderModuleFileName);

        if (!runtimeModule.write(runtimeModuleFile, createRuntimeModuleTextForFile(runtimeRoot), "halionbridge runtime module"))
            return;

        if (!builderModule.write(builderModuleFile, getEmbeddedBuilderModuleText(), "halionbridge builder module"))
            return;

        log::debug("HALion Lua runtime module written temporarily: {}", runtimeModuleFile.getFullPathName().toStdString());
        log::debug("HALion Lua builder module written temporarily: {}", builderModuleFile.getFullPathName().toStdString());

        ready = true;
    }

    ~ScopedPresetRuntimeRoot()
    {
        if (workingDirectoryChanged)
            previousWorkingDirectory.setAsCurrentWorkingDirectory();

        if (environmentChanged)
        {
            if (previousEnvironmentValue)
                setEnvironmentVariable(kPresetDirEnvironmentVariable, *previousEnvironmentValue);
            else
                clearEnvironmentVariable(kPresetDirEnvironmentVariable);
        }

        if (lockAcquired)
            scriptDirectoryLock.exit();
    }

    bool isReady() const noexcept
    {
        return ready;
    }

    RunResult getFailureResult() const noexcept
    {
        return failureResult;
    }

  private:
    juce::InterProcessLock scriptDirectoryLock{kScriptDirectoryLockName};
    juce::File runtimeRoot;
    juce::File previousWorkingDirectory;
    std::optional<juce::String> previousEnvironmentValue;
    ScopedTemporaryTextFile builderModule;
    ScopedTemporaryTextFile runtimeModule;
    RunResult failureResult = RunResult::runtimeSetupFailed;
    bool lockAcquired = false;
    bool environmentChanged = false;
    bool workingDirectoryChanged = false;
    bool ready = false;
};

struct BuildMarkerSet
{
    juce::File builderRoot;
    juce::File okMarkerFile;
    juce::File failedMarkerFile;
    OutputBaseline okMarkerBaseline;
    OutputBaseline failedMarkerBaseline;
    std::set<std::string> staleProgressMarkerNames;
};

struct BuildWaitResult
{
    bool succeeded = false;
    bool completionReceived = false;
    RunResult failureResult = RunResult::buildFailed;
};

std::optional<BuildMarkerSet> prepareBuildMarkers(const juce::File& builderRoot)
{
    BuildMarkerSet markers;
    markers.builderRoot = builderRoot;
    markers.okMarkerFile = builderRoot.getChildFile(kBuildStatusOkPresetFileName);
    markers.failedMarkerFile = builderRoot.getChildFile(kBuildStatusFailedPresetFileName);

    if (!deleteFileIfExists(markers.okMarkerFile, "stale OK build status marker") ||
        !deleteFileIfExists(markers.failedMarkerFile, "stale failed build status marker"))
        return std::nullopt;

    auto progressCleanup = detail::deleteProgressMarkers(builderRoot, "stale HALion Lua progress marker");
    markers.staleProgressMarkerNames = std::move(progressCleanup.remainingNames);

    markers.okMarkerBaseline = makeOutputBaseline(markers.okMarkerFile);
    markers.failedMarkerBaseline = makeOutputBaseline(markers.failedMarkerFile);
    return markers;
}

void logBuildWaitConfiguration(const BuildMarkerSet& markers, const AppOptions& options)
{
    log::debug("Entering offline processing phase...");
    log::debug("Waiting for HALion build status markers:");
    log::debug("  OK: {}", markers.okMarkerFile.getFullPathName().toStdString());
    log::debug("  Failed: {}", markers.failedMarkerFile.getFullPathName().toStdString());

    if (options.timeoutSeconds == 0)
        log::debug("Build timeout disabled. Waiting indefinitely.");
    else
        log::debug("Build timeout: {} seconds.", options.timeoutSeconds);
}

BuildWaitResult waitForBuildCompletion(juce::AudioPluginInstance& plugin, const AppOptions& options, const BuildMarkerSet& markers,
                                       PluginWindow* window, bool& closeRequested, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    auto startTime = juce::Time::getMillisecondCounterHiRes();
    double lastProgressLog = 0.0;
    double lastMarkerPoll = -kMarkerPollIntervalSeconds;
    auto seenProgressMarkers = markers.staleProgressMarkerNames;

    while (true)
    {
        setCrashDiagnosticPhase("runProcessingLoop: processBlock");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(kProcessingDispatchMs);
        buffer.clear();
        plugin.processBlock(buffer, midi);

        auto elapsed = (juce::Time::getMillisecondCounterHiRes() - startTime) / 1000.0;
        if (elapsed - lastMarkerPoll >= kMarkerPollIntervalSeconds)
        {
            detail::logNewProgressMarkers(markers.builderRoot, seenProgressMarkers);
            lastMarkerPoll = elapsed;

            if (markers.failedMarkerBaseline.hasChanged())
            {
                detail::logNewProgressMarkers(markers.builderRoot, seenProgressMarkers);
                log::error("HALion build failure marker written: {}", markers.failedMarkerFile.getFullPathName().toStdString());
                return {.succeeded = false, .completionReceived = true, .failureResult = RunResult::buildFailed};
            }

            if (markers.okMarkerBaseline.hasChanged())
            {
                detail::logNewProgressMarkers(markers.builderRoot, seenProgressMarkers);
                log::debug("HALion build completion marker written: {}", markers.okMarkerFile.getFullPathName().toStdString());
                log::info("HALion Lua build completed.");
                return {.succeeded = true, .completionReceived = true, .failureResult = RunResult::success};
            }
        }

        if (elapsed - lastProgressLog >= kBuildWaitProgressLogIntervalSeconds)
        {
            log::debug("Waiting for HALion build completion... {}s elapsed", static_cast<int>(elapsed));
            lastProgressLog = elapsed;
        }

        if (isStopRequested())
        {
            log::warn("HALion Lua build stopped by user request.");
            return {.failureResult = RunResult::stopped};
        }

        if (options.timeoutSeconds > 0 && elapsed >= static_cast<double>(options.timeoutSeconds))
        {
            log::error("Timed out waiting for HALion build completion after {} seconds.", options.timeoutSeconds);
            return {.failureResult = RunResult::timedOut};
        }

        if (options.showGui && window != nullptr && closeRequested)
        {
            log::error("GUI window closed before HALion build completion.");
            return {.failureResult = RunResult::stopped};
        }
    }
}

bool cleanupSuccessfulBuildMarkers(const BuildMarkerSet& markers)
{
    if (!deleteFileIfExists(markers.okMarkerFile, "OK build status marker") ||
        !deleteFileIfExists(markers.failedMarkerFile, "failed build status marker"))
        return false;

    detail::deleteProgressMarkers(markers.builderRoot, "HALion Lua progress marker");
    return true;
}

std::optional<juce::String> readVstPresetClassId(const juce::MemoryBlock& presetData)
{
    auto presetInfo = inspectVstPresetContainerData(presetData);
    if (!presetInfo)
        return std::nullopt;

    return juce::String(presetInfo->classId);
}
} // namespace

void requestStop() noexcept
{
    gStopRequested.store(true, std::memory_order_release);
}

void resetStopRequest() noexcept
{
    gStopRequested.store(false, std::memory_order_release);
}

bool isStopRequested() noexcept
{
    return gStopRequested.load(std::memory_order_acquire);
}

struct Bridge::Impl
{
    RunResult runDetailed(const AppOptions& options);
    bool loadPlugin(const juce::File& pluginFile, const AppOptions& options);
    bool applyVstPresetData(const juce::MemoryBlock& presetData);
    RunResult runProcessingLoop(const AppOptions& options, const juce::File& builderRoot);

    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> pluginInstance;
};

Bridge::Bridge() : impl(std::make_unique<Impl>()) {}

Bridge::~Bridge() = default;

Bridge::Bridge(Bridge&& other) noexcept = default;

Bridge& Bridge::operator=(Bridge&& other) noexcept = default;

std::optional<AppOptions> Bridge::parseArguments(const std::vector<std::string>& args)
{
    AppOptions options;
    std::optional<juce::File> positionalBuildDirectory;

    for (int i = 0; i < static_cast<int>(args.size()); ++i)
    {
        const auto arg = toJuceString(std::string_view(args[static_cast<size_t>(i)]));

        if (arg == "--plugin" && i + 1 < static_cast<int>(args.size()))
        {
            juce::File file(normalizeCliPath(toJuceString(std::string_view(args[static_cast<size_t>(++i)]))));
            if (!file.existsAsFile() && !file.isDirectory())
            {
                log::error("Override plugin path does not exist at {}", file.getFullPathName().toStdString());
                return std::nullopt;
            }
            options.pluginPathOverride = toStdPath(file);
        }
        else if (arg == "--gui")
        {
            options.showGui = true;
        }
        else if (arg == "--force-scan")
        {
            options.forceScan = true;
        }
        else if (arg == "--nokill")
        {
            options.noKill = true;
        }
        else if (arg == "--timeout-seconds" && i + 1 < static_cast<int>(args.size()))
        {
            auto parsed = parseNonNegativeInt(toJuceString(std::string_view(args[static_cast<size_t>(++i)])));
            if (!parsed)
            {
                log::error("--timeout-seconds must be a non-negative integer.");
                return std::nullopt;
            }

            options.timeoutSeconds = *parsed;
        }
        else if (arg == "--timeout-seconds")
        {
            log::error("--timeout-seconds requires a value.");
            return std::nullopt;
        }
        else if (arg == "--plugin")
        {
            log::error("{} requires a value.", arg.toStdString());
            return std::nullopt;
        }
        else if (arg.startsWith("-"))
        {
            log::error("Unknown argument: {}", arg.toStdString());
            log::error("Run halionbridge --help to see available options.");
            return std::nullopt;
        }
        else
        {
            if (positionalBuildDirectory)
            {
                log::error("Provide exactly one build directory.");
                return std::nullopt;
            }

            positionalBuildDirectory = normalizeCliPath(arg);
        }
    }

    if (!positionalBuildDirectory)
    {
        log::error("You must provide a build directory containing {}.", kBuildFileName);
        return std::nullopt;
    }

    if (!positionalBuildDirectory->isDirectory())
    {
        log::error("Build directory does not exist at {}", positionalBuildDirectory->getFullPathName().toStdString());
        return std::nullopt;
    }

    const auto buildFile = positionalBuildDirectory->getChildFile(kBuildFileName);
    if (!buildFile.existsAsFile())
    {
        log::error("Build directory must contain {} at {}", kBuildFileName, buildFile.getFullPathName().toStdString());
        return std::nullopt;
    }

    options.buildDirectory = toStdPath(*positionalBuildDirectory);
    return options;
}

std::optional<std::filesystem::path> Bridge::findHalionPlugin(const std::optional<std::filesystem::path>& pluginPathOverride)
{
    if (pluginPathOverride)
    {
        return *pluginPathOverride;
    }

    // Default search paths for HALion 7.vst3
#if JUCE_WINDOWS
    juce::File standardPath(R"(C:\Program Files\Common Files\VST3\Steinberg\HALion 7.vst3)");
#elif JUCE_MAC
    juce::File standardPath("/Library/Audio/Plug-Ins/VST3/HALion 7.vst3");
#else
    juce::File standardPath("");
#endif

    if (standardPath.exists())
    {
        return toStdPath(standardPath);
    }

    log::error("HALion 7.vst3 could not be found at standard location: {}", standardPath.getFullPathName().toStdString());
    return std::nullopt;
}

BuildStatusMarkerFiles Bridge::getBuildStatusMarkerFilesForDirectory(const std::filesystem::path& directory)
{
    return {directory / kBuildStatusOkPresetFileName, directory / kBuildStatusFailedPresetFileName};
}

namespace
{

std::optional<VstPresetContainerInfo> inspectVstPresetContainerData(const juce::MemoryBlock& presetData)
{
    auto presetDataCopy = presetData;
    Steinberg::MemoryStream stream(presetDataCopy.getData(), static_cast<Steinberg::TSize>(presetDataCopy.getSize()));
    Steinberg::Vst::PresetFile presetFile(&stream);

    if (!presetFile.readChunkList())
        return std::nullopt;

    VstPresetContainerInfo info;
    info.classId = toStdString(toString(presetFile.getClassID()));
    info.hasComponentState = presetFile.contains(Steinberg::Vst::kComponentState);
    info.hasControllerState = presetFile.contains(Steinberg::Vst::kControllerState);
    info.hasProgramData = presetFile.contains(Steinberg::Vst::kProgramData);

    Steinberg::int32 programOrUnitId = 0;
    if (presetFile.getUnitProgramListID(programOrUnitId))
        info.programOrUnitId = static_cast<int>(programOrUnitId);

    return info;
}

} // namespace

std::optional<VstPresetContainerInfo> Bridge::inspectVstPresetContainer(std::span<const std::byte> presetData)
{
    return inspectVstPresetContainerData(toMemoryBlock(presetData));
}

std::string Bridge::createRuntimeModuleText(const std::filesystem::path& runtimeRoot)
{
    return toStdString(createRuntimeModuleTextForFile(toJuceFile(runtimeRoot)));
}

RunResult Bridge::Impl::runDetailed(const AppOptions& options)
{
    setCrashDiagnosticPhase("halionbridge::run startup");
    log::info("Starting halionbridge {}...", getBuildInfo().versionString);

    if (!options.buildDirectory)
    {
        log::error("AppOptions::buildDirectory is not set. A build directory containing {} is required.", kBuildFileName);
        return RunResult::invalidOptions;
    }

    const auto runtimeRoot = toJuceFile(*options.buildDirectory);
    if (!runtimeRoot.isDirectory())
    {
        log::error("Build directory does not exist at {}", runtimeRoot.getFullPathName().toStdString());
        return RunResult::invalidOptions;
    }

    const auto buildFile = runtimeRoot.getChildFile(kBuildFileName);
    if (!buildFile.existsAsFile())
    {
        log::error("Build directory must contain {} at {}", kBuildFileName, buildFile.getFullPathName().toStdString());
        return RunResult::invalidOptions;
    }

    if (options.timeoutSeconds == 0)
        log::warn("No build timeout is configured; halionbridge will wait indefinitely for HALion Lua status markers.");

    ScopedPresetRuntimeRoot presetRuntimeRoot{std::optional<juce::File>(runtimeRoot)};
    if (!presetRuntimeRoot.isReady())
        return presetRuntimeRoot.getFailureResult();

    if (options.showGui)
    {
        log::debug("Registering GUI-capable plugin formats...");
        juce::addDefaultFormatsToManager(formatManager);
    }
    else
    {
        log::debug("Registering headless plugin formats...");
        juce::addHeadlessDefaultFormatsToManager(formatManager);
    }

    auto pluginFile = Bridge::findHalionPlugin(options.pluginPathOverride);
    if (!pluginFile)
    {
        return RunResult::pluginNotFound;
    }

    if (!loadPlugin(toJuceFile(*pluginFile), options))
    {
        return RunResult::pluginLoadFailed;
    }

    log::info("Plugin loaded.");
    log::debug("Initializing message loops...");
    for (int i = 0; i < kInitialMessagePumpIterations; ++i)
    {
        if (isStopRequested())
        {
            log::warn("Startup stopped by user request.");
            return RunResult::startupStopped;
        }

        juce::MessageManager::getInstance()->runDispatchLoopUntil(kInitialMessagePumpMs);
    }

    return runProcessingLoop(options, runtimeRoot);
}

bool Bridge::run(const AppOptions& options)
{
    return runDetailed(options) == RunResult::success;
}

RunResult Bridge::runDetailed(const AppOptions& options)
{
    if (impl == nullptr)
        return RunResult::invalidBridge;

    return impl->runDetailed(options);
}

bool Bridge::Impl::loadPlugin(const juce::File& pluginFile, const AppOptions& options)
{
    setCrashDiagnosticPhase("loadPlugin: preparing plugin description");
    auto description = std::optional<juce::PluginDescription>();
    const auto preferredClassId = readVstPresetClassId(makeEmbeddedBootstrapPresetData());

    if (options.forceScan)
    {
        log::debug("Forced plugin scan enabled; embedded VST3 class ID shortcut is disabled.");
    }
    else if (preferredClassId)
    {
        log::debug("Using VST3 class ID from embedded bootstrap preset; plugin scan is skipped.");
        description = makeHalionDescriptionFromClassId(pluginFile, *preferredClassId);
    }

    if (!description)
    {
        setCrashDiagnosticPhase("loadPlugin: preparing VST3 scan");
        description = options.executableFile.has_value()
                          ? scanPluginInWorker(toJuceFile(*options.executableFile), pluginFile, preferredClassId)
                          : scanPluginInProcess(pluginFile, preferredClassId);
    }

    if (!description)
    {
        log::error("No valid VST3 plugin description found in {}", pluginFile.getFullPathName().toStdString());
        return false;
    }

    auto pluginDescription = description->name.toStdString() + " (" + description->manufacturerName.toStdString() + ")";
    if (description->version.isNotEmpty())
        pluginDescription += " version " + description->version.toStdString();
    log::info("Plugin identified: {}", pluginDescription);

    struct AsyncCreation
    {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        juce::String error;
        std::atomic_bool finished{false};
    };

    auto creation = std::make_shared<AsyncCreation>();

    log::debug("Instantiating plugin asynchronously...");
    setCrashDiagnosticPhase("loadPlugin: posting async plugin instantiation");
    formatManager.createPluginInstanceAsync(*description, kSampleRate, kBlockSize,
                                            [creation](std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
                                            {
                                                setCrashDiagnosticPhase("loadPlugin: async plugin callback");
                                                creation->instance = std::move(instance);
                                                creation->error = error;
                                                creation->finished.store(true, std::memory_order_release);
                                            });

    const auto creationStart = juce::Time::getMillisecondCounterHiRes();
    auto timedOut = false;
    while (!creation->finished.load(std::memory_order_acquire))
    {
        setCrashDiagnosticPhase("loadPlugin: pumping messages during async instantiation");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(kAsyncInstantiationDispatchMs);

        if (isStopRequested())
        {
            log::warn("Plugin instantiation stopped by user request.");
            return false;
        }

        if ((juce::Time::getMillisecondCounterHiRes() - creationStart) > kPluginInstantiationTimeoutMs)
        {
            timedOut = true;
            break;
        }
    }

    if (timedOut)
    {
        log::error("Failed to instantiate plugin. Timed out after {} ms.", static_cast<long long>(kPluginInstantiationTimeoutMs));
        return false;
    }

    pluginInstance = std::move(creation->instance);
    if (!pluginInstance)
    {
        log::error("Failed to instantiate plugin. {}", creation->error.toStdString());
        return false;
    }

    // Explicitly enable all buses for HALion
    setCrashDiagnosticPhase("loadPlugin: enableAllBuses");
    pluginInstance->enableAllBuses();

    setCrashDiagnosticPhase("loadPlugin: completed");
    return true;
}

bool Bridge::Impl::applyVstPresetData(const juce::MemoryBlock& presetData)
{
    if (!pluginInstance)
        return false;

    auto* vst3Client = pluginInstance->getVST3Client();
    if (vst3Client != nullptr)
    {
        setCrashDiagnosticPhase("applyVstPreset: inspect VST3 preset container");
        auto presetInfo = inspectVstPresetContainerData(presetData);
        if (presetInfo)
            logPresetInfo(*presetInfo);
        else
            log::debug("Diagnostic: File is not a readable VST3 preset container.");

        log::debug("VST3 client interface found. Attempting component-state setPreset...");
        setCrashDiagnosticPhase("applyVstPreset: VST3 client setPreset");
        if (vst3Client->setPreset(presetData))
        {
            log::debug("Success: VST3 component-state setPreset accepted the container.");
            return true;
        }

        log::debug("Diagnostic: VST3 component-state setPreset returned false.");

        if (presetInfo && presetInfo->hasProgramData)
        {
            setCrashDiagnosticPhase("applyVstPreset: restore HALion program data");
            if (restoreProgramDataPreset(presetData, vst3Client->getIComponentPtr(), *presetInfo))
                return true;

            log::error("VST3 preset contains program data, but the plugin did not accept it through program/unit restore interfaces.");
            return false;
        }

        log::error("VST3 preset was not accepted and does not contain HALion-style program data.");
        return false;
    }

    log::error("VST3 client interface was not found; cannot apply .vstpreset.");
    return false;
}

RunResult Bridge::Impl::runProcessingLoop(const AppOptions& options, const juce::File& builderRoot)
{
    if (!pluginInstance)
        return RunResult::pluginLoadFailed;

    setCrashDiagnosticPhase("runProcessingLoop: prepareToPlay");
    log::debug("Preparing offline processing without opening an audio device...");
    pluginInstance->setNonRealtime(true);
    pluginInstance->prepareToPlay(kSampleRate, kBlockSize);
    pumpMessages(kPrepareMessagePumpMs);

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<PluginWindow> window;
    bool closeRequested = false;

    if (options.showGui)
    {
        log::debug("Attempting to create editor...");

        if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
            log::warn("Not on message thread during editor creation.");

        pumpMessages(kEditorMessagePumpMs);

        try
        {
            auto* rawEditor = pluginInstance->createEditorAndMakeActive();
            if (rawEditor != nullptr)
            {
                log::debug("Editor instance returned. Wrapping in DocumentWindow...");
                editor.reset(rawEditor);

                window = std::make_unique<PluginWindow>(pluginInstance->getName(), closeRequested);
                window->setContentNonOwned(editor.get(), true);
                window->setResizable(editor->isResizable(), false);
                window->setUsingNativeTitleBar(true);
                window->centreWithSize(editor->getWidth(), editor->getHeight());
                window->setVisible(true);
                window->toFront(true);
                log::info("GUI window should now be visible.");
            }
            else
            {
                log::error("HALion 7 returned a null editor.");
            }
        }
        catch (const std::exception& e)
        {
            log::error("Exception during editor creation: {}", e.what());
        }
        catch (...)
        {
            log::error("Unknown exception during editor creation.");
        }
    }

    auto finishProcessing = [&](const RunResult result)
    {
        if (window)
        {
            window->setVisible(false);
            window = nullptr;
            editor = nullptr;
        }

        log::debug("Releasing plugin resources...");
        pluginInstance->releaseResources();
        return result;
    };

    auto markers = prepareBuildMarkers(builderRoot);
    if (!markers)
        return finishProcessing(RunResult::runtimeSetupFailed);

    const auto stateApplied = applyVstPresetData(makeEmbeddedBootstrapPresetData());

    if (stateApplied)
        log::info("State/Preset applied successfully.");
    else
        return finishProcessing(RunResult::presetApplyFailed);

    log::info("Running HALion Lua build...");
    logBuildWaitConfiguration(*markers, options);

    juce::AudioBuffer<float> buffer(juce::jmax(2, pluginInstance->getTotalNumInputChannels(), pluginInstance->getTotalNumOutputChannels()),
                                    kBlockSize);
    juce::MidiBuffer midi;

    auto waitResult = waitForBuildCompletion(*pluginInstance, options, *markers, window.get(), closeRequested, buffer, midi);

    if (options.noKill)
        holdPluginAliveForInspection(*pluginInstance, options, window.get(), closeRequested, buffer, midi);

    if (waitResult.completionReceived && waitResult.succeeded && !cleanupSuccessfulBuildMarkers(*markers))
        return finishProcessing(RunResult::cleanupFailed);

    return finishProcessing(waitResult.succeeded ? RunResult::success : waitResult.failureResult);
}

} // namespace halionbridge
