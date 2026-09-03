#include "halionbridge/Bridge.h"
#include "halionbridge/CrashDiagnostics.h"
#include "halionbridge/BuildInfo.h"
#include "halionbridge_assets.h"
#include "BuildFile.h"
#include "BuildManifest.h"
#include "BuildWorker.h"
#include "ChildProcessOutput.h"
#include "CliCommand.h"
#include "Log.h"
#include "MacroPageInjection.h"
#include "PathUtils.h"
#include "PluginScan.h"
#include "PresetInspection.h"
#include "PresetRemap.h"
#include "ProgressMarkers.h"
#include "VstPresetMetadata.h"
#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/base/funknown.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/public.sdk/source/common/memorystream.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/public.sdk/source/vst/vstpresetfile.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <thread>
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
constexpr int kTerminalProgressDrainMaxMs = 1500;
constexpr int kTerminalProgressDrainQuietMs = 300;
constexpr double kBuildWaitProgressLogIntervalSeconds = 5.0;
constexpr double kNoKillHeartbeatIntervalSeconds = 30.0;
constexpr double kBuildWorkerStopGraceMs = 5000.0;
constexpr double kBuildWorkerProgressPollMs = 100.0;
constexpr double kBuildWorkerHeartbeatIntervalMs = 5000.0;
constexpr int kDefaultBuildChunkSize = 1000;
constexpr const char* kBuildStatusOkPresetFileName = "halionbridge_status_ok.vstpreset";
constexpr const char* kBuildStatusFailedPresetFileName = "halionbridge_status_failed.vstpreset";
constexpr const char* kPresetDirEnvironmentVariable = "HALIONBRIDGE_PRESET_DIR";
constexpr const char* kForbidPluginInstantiationEnvironmentVariable = "HALIONBRIDGE_FORBID_PLUGIN_INSTANTIATION";
constexpr const char* kRuntimeModuleFileName = "halionbridge_runtime.lua";
constexpr const char* kBuilderModuleFileName = "halionbridge_builder.lua";
constexpr const char* kPresetRemapModuleFileName = "halionbridge_preset_remap.lua";
constexpr const char* kPresetInspectionModuleFileName = "halionbridge_preset_inspect.lua";
constexpr const char* kMacroPageInjectionJobFileName = "macro_page_job.lua";
constexpr const char* kMacroPageInjectionHelperFileName = "halionbridge_macro_page_inject.lua";
constexpr const char* kMacroPageInjectionReceiptPrefix = "halionbridge-macro-page-receipt-";
constexpr const char* kPresetRemapTemporaryDirectoryPrefix = "halionbridge-remap-";
constexpr const char* kPresetInspectionTemporaryDirectoryPrefix = "halionbridge-inspect-";
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

struct BuildSlice
{
    int start = 1;
    int count = 0;
    int total = 0;

    bool enabled() const noexcept
    {
        return count > 0 && total > 0;
    }

    int end() const noexcept
    {
        return start + count - 1;
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

bool isEnvironmentFlagEnabled(const char* name)
{
    const auto value = getEnvironmentVariableIfSet(name);
    if (!value || value->isEmpty())
        return false;

    const auto normalized = value->trim().toLowerCase();
    return normalized != "0" && normalized != "false" && normalized != "off" && normalized != "no";
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

juce::String getEmbeddedPresetRemapModuleText()
{
    return juce::String::fromUTF8(halionbridge_assets::preset_remap_lua, halionbridge_assets::preset_remap_luaSize);
}

juce::String getEmbeddedPresetInspectionModuleText()
{
    return juce::String::fromUTF8(halionbridge_assets::preset_inspect_lua, halionbridge_assets::preset_inspect_luaSize);
}

std::string getEmbeddedMacroPageInjectionModuleText()
{
    return juce::String::fromUTF8(halionbridge_assets::macro_page_inject_lua, halionbridge_assets::macro_page_inject_luaSize).toStdString();
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

juce::String createRuntimeModuleTextForFile(const juce::File& runtimeRoot, const std::optional<juce::File>& outputRoot = std::nullopt,
                                            const BuildSlice& slice = {})
{
    auto root = runtimeRoot.getFullPathName().replace("\\", "/");
    auto output = juce::String();
    if (outputRoot)
        output = outputRoot->getFullPathName().replace("\\", "/");

    auto sliceText = juce::String();
    if (slice.enabled())
    {
        sliceText = juce::String("HALIONBRIDGE_BUILD_SLICE_START = ") + juce::String(slice.start) +
                    "\n"
                    "HALIONBRIDGE_BUILD_SLICE_COUNT = " +
                    juce::String(slice.count) +
                    "\n"
                    "HALIONBRIDGE_BUILD_TOTAL = " +
                    juce::String(slice.total) + "\n\n";
    }
    else
    {
        sliceText = "HALIONBRIDGE_BUILD_SLICE_START = nil\n"
                    "HALIONBRIDGE_BUILD_SLICE_COUNT = nil\n"
                    "HALIONBRIDGE_BUILD_TOTAL = nil\n\n";
    }

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
           "local outputRoot = " +
           luaQuotedString(output) +
           "\n"
           "outputRoot = outputRoot:gsub(\"\\\\\", \"/\")\n"
           "if outputRoot ~= \"\" and outputRoot:sub(-1) ~= \"/\" then\n"
           "    outputRoot = outputRoot .. \"/\"\n"
           "end\n"
           "HALIONBRIDGE_OUTPUT_ROOT = outputRoot ~= \"\" and outputRoot or nil\n\n" +
           sliceText +
           "local runtimePathPrefix = runtimeRoot .. \"?.lua;\" .. runtimeRoot .. \"?/init.lua;\"\n"
           "if not package.path:find(runtimePathPrefix, 1, true) then\n"
           "    package.path = runtimePathPrefix .. package.path\n"
           "end\n\n"
           "package.loaded[\"halionbridge_builder\"] = nil\n"
           "local ok, result = pcall(require, \"halionbridge_builder\")\n"
           "package.loaded[\"halionbridge_runtime\"] = nil\n"
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
    explicit ScopedPresetRuntimeRoot(const std::optional<juce::File>& runtimeRootIn,
                                     const std::optional<juce::File>& outputRootIn = std::nullopt, const BuildSlice& slice = {})
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

        if (!runtimeModule.write(runtimeModuleFile, createRuntimeModuleTextForFile(runtimeRoot, outputRootIn, slice),
                                 "halionbridge runtime module"))
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

class ScopedPresetRemapRuntimeRoot
{
  public:
    explicit ScopedPresetRemapRuntimeRoot(const detail::PresetRemapRuntimeConfig& config)
    {
        if (!scriptDirectoryLock.enter(0))
        {
            log::error("Another halionbridge instance is already using HALion's user script directory. Wait for it to finish and retry.");
            failureResult = RunResult::anotherInstanceRunning;
            return;
        }

        lockAcquired = true;

        const auto scriptDirectory = getHalionUserScriptDirectory();
        const auto runtimeModuleFile = scriptDirectory.getChildFile(kRuntimeModuleFileName);
        const auto remapModuleFile = scriptDirectory.getChildFile(kPresetRemapModuleFileName);
        const auto runtimeTextSource = detail::createPresetRemapRuntimeModuleText(config);
        const auto runtimeText = juce::String::fromUTF8(runtimeTextSource.c_str());

        if (!runtimeModule.write(runtimeModuleFile, runtimeText, "halionbridge preset-remap runtime module"))
            return;

        if (!remapModule.write(remapModuleFile, getEmbeddedPresetRemapModuleText(), "halionbridge preset-remap module"))
            return;

        log::debug("HALion Lua runtime module written temporarily: {}", runtimeModuleFile.getFullPathName().toStdString());
        log::debug("HALion Lua preset-remap module written temporarily: {}", remapModuleFile.getFullPathName().toStdString());

        ready = true;
    }

    ~ScopedPresetRemapRuntimeRoot()
    {
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
    ScopedTemporaryTextFile remapModule;
    ScopedTemporaryTextFile runtimeModule;
    RunResult failureResult = RunResult::runtimeSetupFailed;
    bool lockAcquired = false;
    bool ready = false;
};

class ScopedPresetInspectionRuntimeRoot
{
  public:
    explicit ScopedPresetInspectionRuntimeRoot(const detail::VstPresetInspectionRuntimeConfig& config)
    {
        if (!scriptDirectoryLock.enter(0))
        {
            log::error("Another halionbridge instance is already using HALion's user script directory. Wait for it to finish and retry.");
            failureResult = RunResult::anotherInstanceRunning;
            return;
        }

        lockAcquired = true;

        const auto scriptDirectory = getHalionUserScriptDirectory();
        const auto runtimeModuleFile = scriptDirectory.getChildFile(kRuntimeModuleFileName);
        const auto inspectionModuleFile = scriptDirectory.getChildFile(kPresetInspectionModuleFileName);
        const auto runtimeTextSource = detail::createVstPresetInspectionRuntimeModuleText(config);
        const auto runtimeText = juce::String::fromUTF8(runtimeTextSource.c_str());

        if (!runtimeModule.write(runtimeModuleFile, runtimeText, "halionbridge preset-inspection runtime module"))
            return;

        if (!inspectionModule.write(inspectionModuleFile, getEmbeddedPresetInspectionModuleText(), "halionbridge preset-inspection module"))
            return;

        log::debug("HALion Lua runtime module written temporarily: {}", runtimeModuleFile.getFullPathName().toStdString());
        log::debug("HALion Lua preset-inspection module written temporarily: {}", inspectionModuleFile.getFullPathName().toStdString());

        ready = true;
    }

    ~ScopedPresetInspectionRuntimeRoot()
    {
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
    ScopedTemporaryTextFile inspectionModule;
    ScopedTemporaryTextFile runtimeModule;
    RunResult failureResult = RunResult::runtimeSetupFailed;
    bool lockAcquired = false;
    bool ready = false;
};

bool cleanupPresetRemapTemporaryDirectory(const std::filesystem::path& directory, const std::filesystem::path& userPresetRoot,
                                          const bool warnOnFailure)
{
    if (directory.empty())
        return true;

    auto error = std::string();
    if (detail::cleanupPresetRemapStageDirectory(directory, userPresetRoot, error))
        return true;

    if (warnOnFailure)
    {
        log::warn("{} If remapped presets were already copied, this temporary directory can be deleted later.", error);
    }
    else
    {
        log::debug("Temporary preset-remap directory cleanup did not complete: {}", error);
    }

    return false;
}

void cleanupStalePresetRemapTemporaryDirectories(const std::filesystem::path& userPresetRoot)
{
    std::error_code error;
    auto iterator = std::filesystem::directory_iterator(userPresetRoot, error);
    if (error)
    {
        log::debug("Could not scan HALion user preset root for stale preset-remap directories {}: {}", userPresetRoot.string(),
                   error.message());
        return;
    }

    for (; iterator != std::filesystem::directory_iterator(); iterator.increment(error))
    {
        if (error)
        {
            log::debug("Could not continue stale preset-remap directory scan below {}: {}", userPresetRoot.string(), error.message());
            return;
        }

        const auto path = iterator->path();
        std::error_code statusError;
        if (!iterator->is_directory(statusError))
            continue;

        const auto name = path.filename().string();
        if (name.rfind(kPresetRemapTemporaryDirectoryPrefix, 0) != 0)
            continue;

        log::debug("Attempting cleanup of stale temporary preset-remap directory {}.", path.string());
        cleanupPresetRemapTemporaryDirectory(path, userPresetRoot, false);
    }
}

class ScopedTemporaryDirectory
{
  public:
    ScopedTemporaryDirectory(std::filesystem::path directoryIn, std::filesystem::path userPresetRootIn)
        : directory(std::move(directoryIn)), userPresetRoot(std::move(userPresetRootIn))
    {
    }

    ~ScopedTemporaryDirectory()
    {
        cleanup();
    }

    const std::filesystem::path& get() const noexcept
    {
        return directory;
    }

    bool cleanup(const bool warnOnFailure = false)
    {
        return cleanupPresetRemapTemporaryDirectory(directory, userPresetRoot, warnOnFailure);
    }

    void dismiss() noexcept
    {
        directory.clear();
    }

  private:
    std::filesystem::path directory;
    std::filesystem::path userPresetRoot;
};

class ScopedInspectionDirectory
{
  public:
    ScopedInspectionDirectory(std::filesystem::path directoryIn, std::filesystem::path inspectionRootIn)
        : directory(std::move(directoryIn)), inspectionRoot(std::move(inspectionRootIn))
    {
    }

    ~ScopedInspectionDirectory()
    {
        cleanup();
    }

    bool cleanup(const bool warnOnFailure = false)
    {
        if (directory.empty())
            return true;

        auto error = std::string{};
        if (detail::cleanupVstPresetInspectionDirectory(directory, inspectionRoot, error))
            return true;

        if (warnOnFailure)
            log::warn("{} The temporary inspection directory can be deleted later.", error);
        else
            log::debug("Temporary preset-inspection directory cleanup did not complete: {}", error);
        return false;
    }

    void dismiss() noexcept
    {
        directory.clear();
    }

  private:
    std::filesystem::path directory;
    std::filesystem::path inspectionRoot;
};

void cleanupStalePresetInspectionDirectories(const std::filesystem::path& inspectionRoot)
{
    auto filesystemError = std::error_code{};
    auto iterator = std::filesystem::directory_iterator(inspectionRoot, filesystemError);
    if (filesystemError)
    {
        log::debug("Could not scan HALion inspection root for stale directories {}: {}", inspectionRoot.string(),
                   filesystemError.message());
        return;
    }

    for (; iterator != std::filesystem::directory_iterator(); iterator.increment(filesystemError))
    {
        if (filesystemError)
        {
            log::debug("Could not continue stale preset-inspection directory scan below {}: {}", inspectionRoot.string(),
                       filesystemError.message());
            return;
        }

        auto statusError = std::error_code{};
        if (!iterator->is_directory(statusError))
            continue;

        const auto path = iterator->path();
        if (path.filename().string().rfind(kPresetInspectionTemporaryDirectoryPrefix, 0) != 0)
            continue;

        auto cleanupError = std::string{};
        if (!detail::cleanupVstPresetInspectionDirectory(path, inspectionRoot, cleanupError))
            log::debug("Could not clean stale preset-inspection directory: {}", cleanupError);
    }
}

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
        if (isStopRequested())
        {
            detail::logNewProgressMarkers(markers.builderRoot, seenProgressMarkers);
            log::warn("HALion Lua build stopped by user request.");
            return {.failureResult = RunResult::stopped};
        }

        if (elapsed - lastMarkerPoll >= kMarkerPollIntervalSeconds)
        {
            detail::logNewProgressMarkers(markers.builderRoot, seenProgressMarkers);
            lastMarkerPoll = elapsed;

            if (markers.failedMarkerBaseline.hasChanged())
            {
                detail::logNewProgressMarkers(markers.builderRoot, seenProgressMarkers);
                log::error("HALion build failure marker written: {}", markers.failedMarkerFile.getFullPathName().toStdString());
                log::debug("Draining HALion Lua progress markers after terminal failure marker...");
                auto drained = detail::drainProgressMarkers(markers.builderRoot, seenProgressMarkers, kTerminalProgressDrainMaxMs,
                                                            kTerminalProgressDrainQuietMs);
                if (drained.failed > 0)
                    log::warn("HALion Lua progress marker drain left {} marker(s) behind.", drained.failed);
                return {.succeeded = false, .completionReceived = true, .failureResult = RunResult::buildFailed};
            }

            if (markers.okMarkerBaseline.hasChanged())
            {
                detail::logNewProgressMarkers(markers.builderRoot, seenProgressMarkers);
                log::debug("HALion build completion marker written: {}", markers.okMarkerFile.getFullPathName().toStdString());
                log::debug("Draining HALion Lua progress markers after terminal success marker...");
                auto drained = detail::drainProgressMarkers(markers.builderRoot, seenProgressMarkers, kTerminalProgressDrainMaxMs,
                                                            kTerminalProgressDrainQuietMs);
                if (drained.failed > 0)
                    log::warn("HALion Lua progress marker drain left {} marker(s) behind.", drained.failed);
                log::info("HALion Lua build completed.");
                return {.succeeded = true, .completionReceived = true, .failureResult = RunResult::success};
            }
        }

        if (elapsed - lastProgressLog >= kBuildWaitProgressLogIntervalSeconds)
        {
            log::debug("Waiting for HALion build completion... {}s elapsed", static_cast<int>(elapsed));
            lastProgressLog = elapsed;
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

    return true;
}

bool cleanupPostReleaseMarkers(const BuildMarkerSet& markers, const RunResult result)
{
    auto cleanupOk = true;
    const auto progressCleanup = detail::deleteProgressMarkers(markers.builderRoot, "post-release HALion Lua progress marker");
    if (progressCleanup.failed > 0)
    {
        cleanupOk = false;
        log::warn("Post-release cleanup left {} HALion Lua progress marker(s) behind.", progressCleanup.failed);
    }

    if (result == RunResult::success && !cleanupSuccessfulBuildMarkers(markers))
        cleanupOk = false;
    else if (result == RunResult::stopped && !deleteFileIfExists(markers.okMarkerFile, "OK build status marker from stopped run"))
        cleanupOk = false;

    return cleanupOk;
}

std::optional<juce::String> readVstPresetClassId(const juce::MemoryBlock& presetData)
{
    auto presetInfo = inspectVstPresetContainerData(presetData);
    if (!presetInfo)
        return std::nullopt;

    return juce::String(presetInfo->classId);
}

std::vector<BuildSlice> makeBuildSlices(const int totalScripts, const int chunkSize)
{
    std::vector<BuildSlice> slices;
    if (totalScripts <= 0 || chunkSize <= 0)
        return slices;

    for (int start = 1; start <= totalScripts; start += chunkSize)
    {
        const auto count = std::min(chunkSize, totalScripts - start + 1);
        slices.push_back(BuildSlice{start, count, totalScripts});
    }

    return slices;
}

bool isRecoverableChunkFailure(const RunResult result) noexcept
{
    return result == RunResult::buildFailed || result == RunResult::timedOut;
}

bool isInfrastructureChunkFailure(const RunResult result) noexcept
{
    return !isRecoverableChunkFailure(result) && result != RunResult::success;
}

AppOptions toRuntimeOptions(const VstPresetRemapOptions& options)
{
    AppOptions runtimeOptions;
    runtimeOptions.pluginPathOverride = options.pluginPathOverride;
    runtimeOptions.executableFile = options.executableFile;
    runtimeOptions.timeoutSeconds = options.timeoutSeconds;
    runtimeOptions.showGui = options.showGui;
    runtimeOptions.noKill = options.noKill;
    runtimeOptions.forceScan = options.forceScan;
    return runtimeOptions;
}

AppOptions toRuntimeOptions(const VstPresetInspectionOptions& options)
{
    AppOptions runtimeOptions;
    runtimeOptions.pluginPathOverride = options.pluginPathOverride;
    runtimeOptions.executableFile = options.executableFile;
    runtimeOptions.timeoutSeconds = options.timeoutSeconds;
    runtimeOptions.showGui = options.showGui;
    runtimeOptions.noKill = options.noKill;
    runtimeOptions.forceScan = options.forceScan;
    return runtimeOptions;
}

AppOptions toRuntimeOptions(const VstPresetMacroPageInjectionOptions& options, const detail::MacroPageInjectionWorkPaths& paths)
{
    AppOptions runtimeOptions;
    runtimeOptions.buildDirectory = paths.runtime;
    runtimeOptions.outputDirectory = paths.presets;
    runtimeOptions.pluginPathOverride = options.pluginPathOverride;
    runtimeOptions.executableFile = options.executableFile;
    runtimeOptions.timeoutSeconds = options.timeoutSeconds;
    runtimeOptions.buildChunkSize = 1;
    runtimeOptions.showGui = options.showGui;
    runtimeOptions.forceScan = options.forceScan;
    runtimeOptions.failFast = true;
    return runtimeOptions;
}

bool pathExistsNoFollow(const std::filesystem::path& path, std::error_code& error)
{
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found || error == std::make_error_code(std::errc::no_such_file_or_directory))
    {
        error.clear();
        return false;
    }
    return !error;
}

std::string comparablePathComponent(const std::filesystem::path& component)
{
    auto text = component.generic_u8string();
    auto result = std::string(reinterpret_cast<const char*>(text.data()), text.size());
#if JUCE_WINDOWS
    std::ranges::transform(result, result.begin(),
                           [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
#endif
    return result;
}

bool hasVstPresetExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension == ".vstpreset";
}

bool pathIsSameOrDescendant(const std::filesystem::path& candidate, const std::filesystem::path& root)
{
    const auto normalizedCandidate = candidate.lexically_normal();
    const auto normalizedRoot = root.lexically_normal();
    auto candidatePart = normalizedCandidate.begin();
    for (auto rootPart = normalizedRoot.begin(); rootPart != normalizedRoot.end(); ++rootPart, ++candidatePart)
    {
        if (candidatePart == normalizedCandidate.end() || comparablePathComponent(*candidatePart) != comparablePathComponent(*rootPart))
            return false;
    }
    return true;
}

std::optional<std::filesystem::path> canonicalExistingPath(const std::filesystem::path& path, const bool requireDirectory,
                                                           const std::string_view description, std::string& error)
{
    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, filesystemError);
    const auto correctType = requireDirectory ? std::filesystem::is_directory(status) : std::filesystem::is_regular_file(status);
    if (filesystemError || std::filesystem::is_symlink(status) || !correctType)
    {
        error =
            std::string(description) + " must be an existing non-symlink " + (requireDirectory ? "directory: " : "file: ") + path.string();
        return std::nullopt;
    }
    auto result = std::filesystem::canonical(path, filesystemError);
    if (filesystemError)
    {
        error = "Could not resolve " + std::string(description) + " " + path.string() + ": " + filesystemError.message();
        return std::nullopt;
    }
    return result;
}

std::optional<std::filesystem::path> canonicalOutputPath(const std::filesystem::path& path, std::string& error)
{
    auto filesystemError = std::error_code{};
    auto absolute = std::filesystem::absolute(path, filesystemError).lexically_normal();
    if (filesystemError || absolute.filename().empty())
    {
        error = "Could not resolve macro-page output directory: " + path.string();
        return std::nullopt;
    }
    const auto parent = canonicalExistingPath(absolute.parent_path(), true, "Macro-page output parent", error);
    if (!parent)
        return std::nullopt;
    return (*parent / absolute.filename()).lexically_normal();
}

bool validateVstPresetFile(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    auto data = juce::MemoryBlock{};
#if JUCE_WINDOWS
    const auto file = juce::File(juce::String(path.c_str()));
#else
    const auto file = juce::File(juce::String::fromUTF8(path.c_str()));
#endif
    if (!file.loadFileAsData(data))
    {
        error = "Could not read VSTPreset file: " + path.string();
        return false;
    }
    if (!inspectVstPresetContainerData(data))
    {
        error = "File is not a valid VST3 preset container: " + path.string();
        return false;
    }
    return true;
}

bool requireNonSymlinkDirectory(const std::filesystem::path& path, const std::string_view description, std::string& error)
{
    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, filesystemError);
    if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
    {
        error = std::string(description) + " is missing, not a directory, or symlinked: " + path.string();
        return false;
    }
    return true;
}

bool ensureSafeRelativeDirectory(const std::filesystem::path& root, const std::filesystem::path& relative, std::string& error)
{
    if (!requireNonSymlinkDirectory(root, "Macro-page staging root", error))
        return false;

    auto current = root;
    for (const auto& component : relative)
    {
        if (component.empty() || component == "." || component == "..")
        {
            error = "Refusing unsafe macro-page staging directory: " + relative.string();
            return false;
        }
        current /= component;
        auto filesystemError = std::error_code{};
        if (!pathExistsNoFollow(current, filesystemError))
        {
            if (filesystemError || !std::filesystem::create_directory(current, filesystemError) || filesystemError)
            {
                error = "Could not create macro-page staging directory " + current.string() + ": " + filesystemError.message();
                return false;
            }
            continue;
        }
        const auto status = std::filesystem::symlink_status(current, filesystemError);
        if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
        {
            error = "Macro-page staging path is not a usable directory: " + current.string();
            return false;
        }
    }
    return true;
}

bool removeRegularFileIfPresent(const std::filesystem::path& path, const std::string_view description, std::string& error)
{
    auto filesystemError = std::error_code{};
    if (!pathExistsNoFollow(path, filesystemError))
    {
        if (filesystemError)
        {
            error = "Could not inspect " + std::string(description) + " " + path.string() + ": " + filesystemError.message();
            return false;
        }
        return true;
    }
    const auto status = std::filesystem::symlink_status(path, filesystemError);
    if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) ||
        !std::filesystem::remove(path, filesystemError) || filesystemError)
    {
        error = "Could not safely remove " + std::string(description) + ": " + path.string();
        return false;
    }
    return true;
}

bool writeMacroPageRuntime(const detail::MacroPageInjectionRuntimeConfig& config, const detail::MacroPageInjectionWorkPaths& paths,
                           std::string& error)
{
    constexpr auto buildFile = "return { \"macro_page_job.lua\" }\n";
    return detail::writeMacroPageInjectionRuntimeFile(paths.runtime / kBuildFileName, buildFile, error) &&
           detail::writeMacroPageInjectionRuntimeFile(paths.runtime / kMacroPageInjectionJobFileName,
                                                      detail::createMacroPageInjectionRuntimeModuleText(config), error) &&
           detail::writeMacroPageInjectionRuntimeFile(paths.runtime / kMacroPageInjectionHelperFileName,
                                                      getEmbeddedMacroPageInjectionModuleText(), error);
}

class ScopedMacroPageReceiptFile
{
  public:
    explicit ScopedMacroPageReceiptFile(std::filesystem::path pathIn) : path(std::move(pathIn)) {}

    ~ScopedMacroPageReceiptFile()
    {
        auto error = std::error_code{};
        const auto exists = pathExistsNoFollow(path, error);
        if (error)
        {
            log::warn("Could not inspect temporary macro-page receipt during cleanup: {}", path.string());
            return;
        }
        if (!exists)
            return;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) ||
            !std::filesystem::remove(path, error) || error)
            log::warn("Could not clean temporary macro-page receipt: {}", path.string());
    }

  private:
    std::filesystem::path path;
};

std::optional<int> readBuildWorkerResultFile(const juce::File& resultFile)
{
    if (!resultFile.existsAsFile())
        return std::nullopt;

    const auto text = resultFile.loadFileAsString().trim();
    if (text.isEmpty())
        return std::nullopt;

    for (const auto c : text)
        if (c < '0' || c > '9')
            return std::nullopt;

    const auto value = text.getLargeIntValue();
    if (value < 0 || value > std::numeric_limits<int>::max())
        return std::nullopt;

    return static_cast<int>(value);
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
    RunResult runSingleInvocation(const AppOptions& options, const juce::File& runtimeRoot, const BuildSlice& slice);
    RunResult runChunkedInProcess(const AppOptions& options, const juce::File& runtimeRoot, std::span<const BuildSlice> slices);
    RunResult runChunkedInWorkers(const AppOptions& options, std::span<const BuildSlice> slices);
    RunResult runWorkerInvocation(const AppOptions& options, const BuildSlice& slice);
    RunResult remapVstPresetsDetailed(const VstPresetRemapOptions& options);
    RunResult runPresetRemapInvocation(const VstPresetRemapOptions& options, const detail::PresetRemapRuntimeConfig& config);
    RunResult inspectVstPresetsDetailed(const VstPresetInspectionOptions& options);
    RunResult runPresetInspectionInvocation(const VstPresetInspectionOptions& options,
                                            const detail::VstPresetInspectionRuntimeConfig& config);
    RunResult injectMacroPageDetailed(const VstPresetMacroPageInjectionOptions& options);
    bool loadPlugin(const juce::File& pluginFile, const AppOptions& options);
    bool loadPlugin(const juce::File& pluginFile, const VstPresetRemapOptions& options);
    bool applyVstPresetData(const juce::MemoryBlock& presetData);
    RunResult runProcessingLoop(const AppOptions& options, const juce::File& builderRoot);

    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<juce::AudioPluginInstance> pluginInstance;
    bool pluginFormatsRegistered = false;
};

Bridge::Bridge() : impl(std::make_unique<Impl>()) {}

Bridge::~Bridge() = default;

Bridge::Bridge(Bridge&& other) noexcept = default;

Bridge& Bridge::operator=(Bridge&& other) noexcept = default;

std::optional<AppOptions> Bridge::parseArguments(const std::vector<std::string>& args)
{
    return detail::parseBuildOptions(args);
}

std::optional<VstPresetRemapOptions> Bridge::parseVstPresetRemapArguments(const std::vector<std::string>& args)
{
    return detail::parseVstPresetRemapOptions(args);
}

std::optional<VstPresetInspectionOptions> Bridge::parseVstPresetInspectionArguments(const std::vector<std::string>& args)
{
    return detail::parseVstPresetInspectionOptions(args);
}

std::optional<VstPresetMacroPageInjectionOptions> Bridge::parseVstPresetMacroPageInjectionArguments(const std::vector<std::string>& args)
{
    return detail::parseVstPresetMacroPageInjectionOptions(args);
}

std::optional<std::filesystem::path> Bridge::findHalionPlugin(const std::optional<std::filesystem::path>& pluginPathOverride)
{
    if (pluginPathOverride)
    {
        return *pluginPathOverride;
    }

    auto standardPath = toJuceFile(getDefaultHalionPluginPath());
    if (standardPath.exists())
    {
        return toStdPath(standardPath);
    }

    log::error("HALion 7.vst3 could not be found at standard location: {}", standardPath.getFullPathName().toStdString());
    return std::nullopt;
}

std::filesystem::path Bridge::getDefaultHalionPluginPath()
{
#if JUCE_WINDOWS
    return std::filesystem::path(R"(C:\Program Files\Common Files\VST3\Steinberg\HALion 7.vst3)");
#elif JUCE_MAC
    return std::filesystem::path("/Library/Audio/Plug-Ins/VST3/Steinberg/HALion 7.vst3");
#else
    return {};
#endif
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

std::string Bridge::createRuntimeModuleText(const std::filesystem::path& runtimeRoot, const std::filesystem::path& outputRoot)
{
    return toStdString(createRuntimeModuleTextForFile(toJuceFile(runtimeRoot), toJuceFile(outputRoot)));
}

std::string Bridge::createRuntimeModuleText(const std::filesystem::path& runtimeRoot, const int sliceStart, const int sliceCount,
                                            const int totalScripts)
{
    return toStdString(
        createRuntimeModuleTextForFile(toJuceFile(runtimeRoot), std::nullopt, BuildSlice{sliceStart, sliceCount, totalScripts}));
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
        if (detail::hasTopLevelLuaBuildScripts(runtimeRoot))
        {
            log::warn("No {} was found, but Lua files exist in this directory. Run \"halionbridge init {}\" to generate one.",
                      kBuildFileName, runtimeRoot.getFullPathName().toStdString());
        }

        log::error("Build directory must contain {} at {}", kBuildFileName, buildFile.getFullPathName().toStdString());
        return RunResult::invalidOptions;
    }

    if (options.outputDirectory)
    {
        const auto outputRoot = toJuceFile(*options.outputDirectory);
        if (outputRoot.exists() && !outputRoot.isDirectory())
        {
            log::error("Output directory path exists but is not a directory: {}", outputRoot.getFullPathName().toStdString());
            return RunResult::invalidOptions;
        }

        if (!outputRoot.createDirectory())
        {
            log::error("Could not create output directory: {}", outputRoot.getFullPathName().toStdString());
            return RunResult::invalidOptions;
        }

        log::info("Build output directory: {}", outputRoot.getFullPathName().toStdString());
    }

    const auto effectiveOutputDirectory = options.outputDirectory.value_or(*options.buildDirectory);
    if (const auto manifestError = detail::prepareBuildManifestOutputDirectories(*options.buildDirectory, effectiveOutputDirectory))
    {
        log::error("{}: {}", manifestError->code, manifestError->message);
        return RunResult::invalidOptions;
    }

    if (options.timeoutSeconds == 0)
        log::warn("No build timeout is configured; halionbridge will wait indefinitely for HALion Lua status markers.");

    const auto buildFileText = buildFile.loadFileAsString().toStdString();
    const auto moduleNames = Bridge::parseBuildFileModuleNames(buildFileText);
    const auto chunkSize = options.buildChunkSize > 0 ? options.buildChunkSize : kDefaultBuildChunkSize;
    const auto slices = makeBuildSlices(static_cast<int>(moduleNames.size()), chunkSize);

    if (detail::AppOptionsAccess::isBuildWorkerMode(options))
    {
        const auto sliceStart = detail::AppOptionsAccess::buildSliceStart(options);
        const auto sliceCount = detail::AppOptionsAccess::buildSliceCount(options);
        const auto sliceTotal = detail::AppOptionsAccess::buildSliceTotal(options);
        if (sliceStart <= 0 || sliceCount <= 0 || sliceTotal <= 0 || sliceStart > sliceTotal || sliceCount > (sliceTotal - sliceStart + 1))
        {
            log::error("Invalid build-worker slice configuration.");
            return RunResult::invalidOptions;
        }

        if (sliceTotal != static_cast<int>(moduleNames.size()))
        {
            log::error("Build-worker slice total {} does not match {} entries parsed from {}.", sliceTotal,
                       static_cast<int>(moduleNames.size()), kBuildFileName);
            return RunResult::invalidOptions;
        }

        return runSingleInvocation(options, runtimeRoot, BuildSlice{sliceStart, sliceCount, sliceTotal});
    }

    if (slices.empty())
    {
        log::warn("Could not statically split {} into build chunks; running it as one HALion Lua invocation.", kBuildFileName);
        return runSingleInvocation(options, runtimeRoot, {});
    }

    log::info("Running {} Lua build script file(s) in {} chunk(s) of up to {}.", static_cast<int>(moduleNames.size()),
              static_cast<int>(slices.size()), chunkSize);

    if (!options.showGui && !options.noKill)
    {
        if (options.executableFile)
            return runChunkedInWorkers(options, slices);

        log::warn("No executable path is available; running HALion chunks in-process without hard Ctrl+C isolation.");
    }

    return runChunkedInProcess(options, runtimeRoot, slices);
}

RunResult Bridge::Impl::runChunkedInProcess(const AppOptions& options, const juce::File& runtimeRoot, std::span<const BuildSlice> slices)
{
    auto failedChunks = 0;
    auto lastFailure = RunResult::success;

    for (size_t i = 0; i < slices.size(); ++i)
    {
        if (isStopRequested())
        {
            log::warn("HALion Lua build stopped by user request before starting build chunk {}/{}.", static_cast<int>(i + 1),
                      static_cast<int>(slices.size()));
            return RunResult::stopped;
        }

        const auto& slice = slices[i];
        log::info("Starting build chunk {}/{}: entries {}-{} of {}.", static_cast<int>(i + 1), static_cast<int>(slices.size()), slice.start,
                  slice.end(), slice.total);

        const auto result = runSingleInvocation(options, runtimeRoot, slice);
        if (result == RunResult::success)
        {
            if (isStopRequested())
            {
                log::warn("HALion Lua build stopped by user request after build chunk {}/{}.", static_cast<int>(i + 1),
                          static_cast<int>(slices.size()));
                return RunResult::stopped;
            }

            log::info("Build chunk {}/{} completed.", static_cast<int>(i + 1), static_cast<int>(slices.size()));
            continue;
        }

        ++failedChunks;
        lastFailure = result;
        log::error("Build chunk {}/{} failed; entries {}-{} were not completed successfully.", static_cast<int>(i + 1),
                   static_cast<int>(slices.size()), slice.start, slice.end());

        if (options.failFast || !isRecoverableChunkFailure(result))
        {
            log::error("Stopping after failed build chunk.");
            return result;
        }
    }

    if (failedChunks > 0)
    {
        log::error("HALion Lua build completed with {} failed chunk(s).", failedChunks);
        return lastFailure == RunResult::success ? RunResult::buildFailed : lastFailure;
    }

    log::info("HALion Lua build completed.");
    return RunResult::success;
}

RunResult Bridge::Impl::runChunkedInWorkers(const AppOptions& options, std::span<const BuildSlice> slices)
{
    auto failedChunks = 0;
    auto lastFailure = RunResult::success;

    for (size_t i = 0; i < slices.size(); ++i)
    {
        if (isStopRequested())
        {
            log::warn("HALion Lua build stopped by user request before starting build chunk {}/{}.", static_cast<int>(i + 1),
                      static_cast<int>(slices.size()));
            return RunResult::stopped;
        }

        const auto& slice = slices[i];
        log::info("Starting build chunk {}/{}: entries {}-{} of {}.", static_cast<int>(i + 1), static_cast<int>(slices.size()), slice.start,
                  slice.end(), slice.total);

        const auto result = runWorkerInvocation(options, slice);
        if (result == RunResult::success)
        {
            if (isStopRequested())
            {
                log::warn("HALion Lua build stopped by user request after build chunk {}/{}.", static_cast<int>(i + 1),
                          static_cast<int>(slices.size()));
                return RunResult::stopped;
            }

            log::info("Build chunk {}/{} completed.", static_cast<int>(i + 1), static_cast<int>(slices.size()));
            continue;
        }

        if (result == RunResult::stopped)
            return RunResult::stopped;

        ++failedChunks;
        lastFailure = result;
        log::error("Build chunk {}/{} failed; entries {}-{} were not completed successfully.", static_cast<int>(i + 1),
                   static_cast<int>(slices.size()), slice.start, slice.end());

        if (options.failFast || isInfrastructureChunkFailure(result))
        {
            log::error("Stopping after failed build chunk.");
            return result;
        }
    }

    if (failedChunks > 0)
    {
        log::error("HALion Lua build completed with {} failed chunk(s).", failedChunks);
        return lastFailure == RunResult::success ? RunResult::buildFailed : lastFailure;
    }

    log::info("HALion Lua build completed.");
    return RunResult::success;
}

RunResult Bridge::Impl::runWorkerInvocation(const AppOptions& options, const BuildSlice& slice)
{
    auto command = detail::makeBuildWorkerCommand(options, slice.start, slice.count, slice.total);
    if (command.isEmpty())
    {
        log::error("Could not create HALion build worker command.");
        return RunResult::runtimeSetupFailed;
    }

    const auto resultFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getNonexistentChildFile("halionbridge_build_worker_result", ".txt", false);
    command.add("--worker-result-file");
    command.add(resultFile.getFullPathName());

    auto seenProgressMarkers = std::set<std::string>();
    if (options.buildDirectory)
    {
        const auto builderRoot = toJuceFile(*options.buildDirectory);
        const auto staleMarkers = detail::deleteProgressMarkers(builderRoot, "stale HALion Lua progress marker before worker run");
        seenProgressMarkers = std::move(staleMarkers.remainingNames);
    }

    auto process = std::make_shared<juce::ChildProcess>();
    if (!process->start(command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        log::error("Failed to launch HALion build worker.");
        return RunResult::runtimeSetupFailed;
    }

    auto stopLogged = false;
    auto stopDeadline = 0.0;
    auto nextProgressPoll = 0.0;
    const auto workerStartTime = juce::Time::getMillisecondCounterHiRes();
    auto nextHeartbeat = workerStartTime + kBuildWorkerHeartbeatIntervalMs;
    auto childOutput = std::make_shared<detail::ChildProcessOutputBuffer>();
    auto outputThread = std::thread([process, childOutput] { detail::forwardChildOutputToConsole(*process, *childOutput); });

    while (process->isRunning())
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (options.buildDirectory && now >= nextProgressPoll)
        {
            detail::logNewProgressMarkers(toJuceFile(*options.buildDirectory), seenProgressMarkers);
            nextProgressPoll = now + kBuildWorkerProgressPollMs;
        }

        if (now >= nextHeartbeat)
        {
            const auto elapsedSeconds = static_cast<int>((now - workerStartTime) / 1000.0);
            log::info("Build worker still running for chunk entries {}-{} of {} ({}s elapsed).", slice.start, slice.end(), slice.total,
                      elapsedSeconds);
            nextHeartbeat = now + kBuildWorkerHeartbeatIntervalMs;
        }

        if (isStopRequested())
        {
            if (!stopLogged)
            {
                stopLogged = true;
                stopDeadline = now + kBuildWorkerStopGraceMs;
                log::warn("Stop requested; waiting up to {} seconds for current HALion worker to exit.",
                          static_cast<int>(kBuildWorkerStopGraceMs / 1000.0));
            }

            if (now >= stopDeadline)
            {
                const auto killed = process->kill();
                if (!killed && process->isRunning())
                {
                    log::error("Failed to terminate HALion build worker after Ctrl+C grace period; leaving worker output reader detached.");
                    if (outputThread.joinable())
                        outputThread.detach();

                    return RunResult::stopped;
                }

                if (!process->waitForProcessToFinish(2000))
                {
                    log::error(
                        "HALion build worker did not exit within 2 seconds after termination request; leaving output reader detached.");
                    if (outputThread.joinable())
                        outputThread.detach();

                    return RunResult::stopped;
                }

                if (outputThread.joinable())
                    outputThread.join();

                detail::flushChildOutputToConsole(*childOutput);
                log::warn("HALion worker killed after Ctrl+C grace period.");
                return RunResult::stopped;
            }
        }

        juce::Thread::sleep(10);
    }

    if (outputThread.joinable())
        outputThread.join();

    detail::flushChildOutputToConsole(*childOutput);
    if (options.buildDirectory)
        detail::logNewProgressMarkers(toJuceFile(*options.buildDirectory), seenProgressMarkers);

    if (!process->waitForProcessToFinish(2000))
    {
        log::error("HALion build worker stopped running but its exit status could not be collected.");
        return RunResult::buildFailed;
    }

    const auto processExitCode = static_cast<int>(process->getExitCode());
    auto exitCode = processExitCode;
    if (const auto resultFileExitCode = readBuildWorkerResultFile(resultFile))
    {
        exitCode = *resultFileExitCode;
        log::debug("HALion build worker result file reported code {}.", exitCode);
    }
    else if (processExitCode != 0)
    {
        log::warn("HALion build worker result file was not available; using nonzero process exit code {}.", processExitCode);
    }
    else
    {
        log::error("HALion build worker did not write its result file.");
        return RunResult::buildFailed;
    }

    if (resultFile.existsAsFile() && !resultFile.deleteFile())
        log::warn("Failed to delete HALion build worker result file: {}", resultFile.getFullPathName().toStdString());

    const auto result = detail::buildWorkerExitCodeToRunResult(exitCode);
    if (!result)
    {
        log::error("HALion build worker exited with unexpected code {}.", exitCode);
        return RunResult::buildFailed;
    }

    return *result;
}

RunResult Bridge::Impl::runSingleInvocation(const AppOptions& options, const juce::File& runtimeRoot, const BuildSlice& slice)
{
    pluginInstance = nullptr;

    const auto outputRoot = options.outputDirectory ? std::optional<juce::File>(toJuceFile(*options.outputDirectory)) : std::nullopt;
    ScopedPresetRuntimeRoot presetRuntimeRoot{std::optional<juce::File>(runtimeRoot), outputRoot, slice};
    if (!presetRuntimeRoot.isReady())
        return presetRuntimeRoot.getFailureResult();

    if (!pluginFormatsRegistered && options.showGui)
    {
        log::debug("Registering GUI-capable plugin formats...");
        juce::addDefaultFormatsToManager(formatManager);
        pluginFormatsRegistered = true;
    }
    else if (!pluginFormatsRegistered)
    {
        log::debug("Registering headless plugin formats...");
        juce::addHeadlessDefaultFormatsToManager(formatManager);
        pluginFormatsRegistered = true;
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

RunResult Bridge::remapVstPresetsDetailed(const VstPresetRemapOptions& options)
{
    if (impl == nullptr)
        return RunResult::invalidBridge;

    return impl->remapVstPresetsDetailed(options);
}

RunResult Bridge::inspectVstPresetsDetailed(const VstPresetInspectionOptions& options)
{
    if (impl == nullptr)
        return RunResult::invalidBridge;

    return impl->inspectVstPresetsDetailed(options);
}

RunResult Bridge::injectMacroPageDetailed(const VstPresetMacroPageInjectionOptions& options)
{
    if (impl == nullptr)
        return RunResult::invalidBridge;

    return impl->injectMacroPageDetailed(options);
}

RunResult Bridge::Impl::remapVstPresetsDetailed(const VstPresetRemapOptions& options)
{
    setCrashDiagnosticPhase("halionbridge::remapVstPresets startup");
    log::info("Starting halionbridge preset remap {}...", getBuildInfo().versionString);

    if (options.oldRoot.empty() || options.newRoot.empty())
    {
        log::error("Both --old-root and --new-root are required.");
        return RunResult::invalidOptions;
    }

    if (options.presetPluginCode != "H7" && options.presetPluginCode != "HS")
    {
        log::error("Preset plugin code must be H7 or HS.");
        return RunResult::invalidOptions;
    }

    std::string outputError;
    if (!detail::isDirectoryEmpty(options.outputDirectory, outputError))
    {
        log::error("{}", outputError);
        return RunResult::invalidOptions;
    }

    auto collection = detail::collectPresetRemapFiles(options.inputDirectory);
    if (!collection.errors.empty())
    {
        for (const auto& error : collection.errors)
            log::error("{}", error);
        return RunResult::invalidOptions;
    }

    const auto userPresetRoot = detail::getDefaultHalionUserPresetDirectory();
    cleanupStalePresetRemapTemporaryDirectories(userPresetRoot);

    const auto stageDirectory =
        userPresetRoot / (std::string{kPresetRemapTemporaryDirectoryPrefix} + juce::Uuid().toString().toStdString());
    ScopedTemporaryDirectory temporaryDirectory(stageDirectory, userPresetRoot);

    std::error_code ec;
    std::filesystem::create_directories(stageDirectory, ec);
    if (ec)
    {
        log::error("Could not create temporary preset-remap directory {}: {}", stageDirectory.string(), ec.message());
        return RunResult::runtimeSetupFailed;
    }

    log::info("Staging {} .vstpreset file(s) for HALion remap.", static_cast<int>(collection.files.size()));

    std::vector<std::string> copyErrors;
    if (!detail::copyPresetRemapFilesToStage(collection.files, stageDirectory, copyErrors))
    {
        for (const auto& error : copyErrors)
            log::error("{}", error);
        return RunResult::runtimeSetupFailed;
    }

    auto relativePresetPaths = std::vector<std::string>();
    relativePresetPaths.reserve(collection.files.size());
    for (const auto& file : collection.files)
        relativePresetPaths.push_back(file.relativePath.generic_string());

    auto runtimeConfig =
        detail::PresetRemapRuntimeConfig{stageDirectory, std::move(relativePresetPaths), detail::normalizePresetRemapRoot(options.oldRoot),
                                         detail::normalizePresetRemapRoot(options.newRoot), options.presetPluginCode};

    const auto remapResult = runPresetRemapInvocation(options, runtimeConfig);
    if (remapResult != RunResult::success)
        return remapResult;

    std::string outputCreatedError;
    if (!detail::isDirectoryEmpty(options.outputDirectory, outputCreatedError))
    {
        log::error("{}", outputCreatedError);
        return RunResult::cleanupFailed;
    }

    copyErrors.clear();
    if (!detail::copyPresetRemapFilesFromStage(collection.files, stageDirectory, options.outputDirectory, copyErrors))
    {
        for (const auto& error : copyErrors)
            log::error("{}", error);
        return RunResult::cleanupFailed;
    }

    log::info("Copied remapped .vstpreset files to {}.", options.outputDirectory.string());
    temporaryDirectory.cleanup(true);
    temporaryDirectory.dismiss();
    return RunResult::success;
}

RunResult Bridge::Impl::inspectVstPresetsDetailed(const VstPresetInspectionOptions& options)
{
    setCrashDiagnosticPhase("halionbridge::inspectVstPresets startup");
    log::info("Starting halionbridge VSTPreset inspection {}...", getBuildInfo().versionString);

    if (options.inputPath.empty() || options.outputJson.empty() || options.timeoutSeconds < 0)
    {
        log::error("Inspection requires a valid input path, output JSON path, and non-negative timeout.");
        return RunResult::invalidOptions;
    }

    auto error = std::string{};
    if (!detail::validateVstPresetInspectionOutput(options.outputJson, options.overwrite, error))
    {
        log::error("{}", error);
        return RunResult::invalidOptions;
    }

    auto collection = detail::collectVstPresetsForInspection(options.inputPath, options.recursive);
    if (!collection.errors.empty())
    {
        for (const auto& collectionError : collection.errors)
            log::error("{}", collectionError);
        return RunResult::invalidOptions;
    }

    for (auto& file : collection.files)
    {
        auto absoluteError = std::error_code{};
        file.sourcePath = std::filesystem::absolute(file.sourcePath, absoluteError).lexically_normal();
        if (absoluteError)
        {
            log::error("Could not resolve inspection source path {}: {}", file.sourcePath.string(), absoluteError.message());
            return RunResult::invalidOptions;
        }
    }

    const auto inspectionRoot = detail::getDefaultHalionInspectionDirectory();
    auto filesystemError = std::error_code{};
    std::filesystem::create_directories(inspectionRoot, filesystemError);
    if (filesystemError)
    {
        log::error("Could not create HALion inspection root {}: {}", inspectionRoot.string(), filesystemError.message());
        return RunResult::runtimeSetupFailed;
    }

    cleanupStalePresetInspectionDirectories(inspectionRoot);

    const auto runtimeRoot =
        inspectionRoot / (std::string{kPresetInspectionTemporaryDirectoryPrefix} + juce::Uuid().toString().toStdString());
    ScopedInspectionDirectory temporaryDirectory(runtimeRoot, inspectionRoot);
    std::filesystem::create_directory(runtimeRoot, filesystemError);
    if (filesystemError)
    {
        log::error("Could not create temporary preset-inspection directory {}: {}", runtimeRoot.string(), filesystemError.message());
        return RunResult::runtimeSetupFailed;
    }

    const auto reportPath = runtimeRoot / "inspection-report.json";
    const auto config = detail::VstPresetInspectionRuntimeConfig{runtimeRoot, reportPath, collection.files};
    log::info("Inspecting {} .vstpreset file(s) through HALion.", static_cast<int>(collection.files.size()));

    const auto invocationResult = runPresetInspectionInvocation(options, config);
    auto reportJson = std::string{};
    if (!detail::readVstPresetInspectionReport(reportPath, reportJson, error))
    {
        log::error("{}", error);
        if (invocationResult != RunResult::success && invocationResult != RunResult::buildFailed)
            return invocationResult;
        return RunResult::inspectionFailed;
    }

    auto summary = detail::VstPresetInspectionReportSummary{};
    if (!detail::validateVstPresetInspectionReport(reportJson, static_cast<int>(collection.files.size()), summary, error))
    {
        log::error("{}", error);
        return RunResult::inspectionFailed;
    }

    if (!detail::publishVstPresetInspectionReport(reportPath, options.outputJson, options.overwrite, error))
    {
        log::error("{}", error);
        return RunResult::cleanupFailed;
    }

    log::info("Wrote VSTPreset inspection report to {}.", options.outputJson.string());
    log::info("Inspection summary: {} inspected, {} failed, {} total.", summary.inspected, summary.failed, summary.total);

    const auto cleanupOk = temporaryDirectory.cleanup(true);
    temporaryDirectory.dismiss();
    if (!cleanupOk && summary.failed == 0 && invocationResult == RunResult::success)
        return RunResult::cleanupFailed;

    if (summary.failed > 0)
        return RunResult::inspectionFailed;
    if (invocationResult != RunResult::success)
        return invocationResult;
    return RunResult::success;
}

RunResult Bridge::Impl::injectMacroPageDetailed(const VstPresetMacroPageInjectionOptions& originalOptions)
{
    setCrashDiagnosticPhase("halionbridge::injectMacroPage startup");
    log::info("Starting halionbridge macro-page injection {}...", getBuildInfo().versionString);

    if (originalOptions.inputDirectory.empty() || originalOptions.outputDirectory.empty() || originalOptions.donorPreset.empty() ||
        originalOptions.chunkSize <= 0 || originalOptions.timeoutSeconds < 0)
    {
        log::error("Macro-page injection requires input, output, and donor paths, a positive chunk size, and a non-negative timeout.");
        return RunResult::invalidOptions;
    }

    auto error = std::string{};
    const auto inputDirectory = canonicalExistingPath(originalOptions.inputDirectory, true, "Macro-page input directory", error);
    if (!inputDirectory)
    {
        log::error("{}", error);
        return RunResult::invalidOptions;
    }
    const auto donorPreset = canonicalExistingPath(originalOptions.donorPreset, false, "Macro-page donor preset", error);
    if (!donorPreset || !hasVstPresetExtension(*donorPreset))
    {
        log::error("{}", error.empty() ? "Macro-page donor path is invalid." : error);
        return RunResult::invalidOptions;
    }
    const auto outputDirectory = canonicalOutputPath(originalOptions.outputDirectory, error);
    if (!outputDirectory)
    {
        log::error("{}", error);
        return RunResult::invalidOptions;
    }

    auto filesystemError = std::error_code{};
    if (pathExistsNoFollow(*outputDirectory, filesystemError) || filesystemError)
    {
        log::error("Macro-page output directory must not already exist: {}", outputDirectory->string());
        return RunResult::invalidOptions;
    }

    const auto paths = detail::makeMacroPageInjectionWorkPaths(*outputDirectory);
    if (pathIsSameOrDescendant(*outputDirectory, *inputDirectory) || pathIsSameOrDescendant(*inputDirectory, *outputDirectory) ||
        pathIsSameOrDescendant(paths.root, *inputDirectory) || pathIsSameOrDescendant(*inputDirectory, paths.root))
    {
        log::error("Macro-page input, output, and deterministic work directories must not overlap.");
        return RunResult::invalidOptions;
    }

    auto collection = detail::collectMacroPageInjectionFiles(*inputDirectory, originalOptions.recursive);
    if (!collection.errors.empty())
    {
        for (const auto& message : collection.errors)
            log::error("{}", message);
        return RunResult::invalidOptions;
    }

    if (!validateVstPresetFile(*donorPreset, error))
    {
        log::error("{}", error);
        return RunResult::invalidOptions;
    }
    const auto donorHash = detail::sha256MacroPageInjectionFile(*donorPreset, error);
    if (!donorHash)
    {
        log::error("{}", error);
        return RunResult::invalidOptions;
    }

    auto manifest =
        detail::MacroPageInjectionManifest{*inputDirectory, *outputDirectory, *donorPreset, *donorHash, originalOptions.recursive, {}};
    manifest.files.reserve(collection.files.size());
    for (const auto& file : collection.files)
    {
        const auto canonicalSource = canonicalExistingPath(file.sourcePath, false, "Macro-page source preset", error);
        if (!canonicalSource ||
            (pathIsSameOrDescendant(*canonicalSource, *donorPreset) && pathIsSameOrDescendant(*donorPreset, *canonicalSource)))
        {
            log::error("{}", canonicalSource ? "The macro-page donor must not also be an input preset." : error);
            return RunResult::invalidOptions;
        }
        if (!validateVstPresetFile(*canonicalSource, error))
        {
            log::error("{}", error);
            return RunResult::invalidOptions;
        }
        const auto hash = detail::sha256MacroPageInjectionFile(*canonicalSource, error);
        if (!hash)
        {
            log::error("{}", error);
            return RunResult::invalidOptions;
        }
        manifest.files.push_back({file.relativePath, *hash});
    }

    const auto workExists = pathExistsNoFollow(paths.root, filesystemError);
    if (filesystemError || (originalOptions.resume && !workExists) || (!originalOptions.resume && workExists))
    {
        if (filesystemError)
            log::error("Could not inspect macro-page work directory {}: {}", paths.root.string(), filesystemError.message());
        else if (originalOptions.resume)
            log::error("No resumable macro-page work directory exists at {}.", paths.root.string());
        else
            log::error("Macro-page work directory already exists at {}. Use --resume only for the matching interrupted run.",
                       paths.root.string());
        return RunResult::invalidOptions;
    }

    if (originalOptions.resume)
    {
        if (!requireNonSymlinkDirectory(paths.root, "Macro-page work root", error) ||
            !requireNonSymlinkDirectory(paths.runtime, "Macro-page runtime directory", error) ||
            !requireNonSymlinkDirectory(paths.presets, "Macro-page staging directory", error))
        {
            log::error("{}", error);
            return RunResult::invalidOptions;
        }
        auto previousManifest = detail::MacroPageInjectionManifest{};
        if (!detail::readMacroPageInjectionManifest(paths.manifest, previousManifest, error) ||
            !detail::macroPageInjectionManifestsMatch(manifest, previousManifest, error))
        {
            log::error("Cannot resume macro-page injection: {}", error);
            return RunResult::invalidOptions;
        }
    }
    else
    {
        std::filesystem::create_directories(paths.runtime, filesystemError);
        if (!filesystemError)
            std::filesystem::create_directories(paths.presets, filesystemError);
        if (filesystemError || !detail::writeMacroPageInjectionManifest(paths.manifest, manifest, error))
        {
            log::error("Could not initialize macro-page work directory: {}", filesystemError ? filesystemError.message() : error);
            auto cleanupError = std::string{};
            detail::cleanupMacroPageInjectionWorkDirectory(paths, *outputDirectory, cleanupError);
            return RunResult::runtimeSetupFailed;
        }
    }

    const auto resumeCommand = [&]
    {
        auto command = fmt::format("halionbridge inject-macro-page --input-directory \"{}\" --output-directory \"{}\" "
                                   "--donor-preset \"{}\" --chunk-size {}",
                                   inputDirectory->string(), outputDirectory->string(), donorPreset->string(), originalOptions.chunkSize);
        command += originalOptions.recursive ? " --recursive" : "";
        command += originalOptions.failFast ? " --fail-fast" : "";
        command +=
            originalOptions.timeoutSeconds == 0 ? " --no-timeout" : fmt::format(" --timeout-seconds {}", originalOptions.timeoutSeconds);
        command += originalOptions.showGui ? " --gui" : "";
        command += originalOptions.forceScan ? " --force-scan" : "";
        if (originalOptions.pluginPathOverride)
            command += fmt::format(" --plugin \"{}\"", originalOptions.pluginPathOverride->string());
        command += " --resume";
        return command;
    };
    const auto retainWorkAndReturn = [&](const RunResult result)
    {
        log::warn("Macro-page work was retained at {}.", paths.root.string());
        log::warn("Resume with: {}", resumeCommand());
        return result;
    };

    auto completed = detail::readMacroPageInjectionCompletions(paths.completionJournal, manifest.files.size());
    if (!completed.errors.empty())
    {
        for (const auto& message : completed.errors)
            log::error("{}", message);
        return retainWorkAndReturn(RunResult::invalidOptions);
    }
    for (const auto& [index, record] : completed.records)
    {
        const auto& source = manifest.files[index];
        const auto stagedPreset = paths.presets / source.relativePath;
        if (record.sourceSha256 != source.sourceSha256 || !validateVstPresetFile(stagedPreset, error))
        {
            log::error("Resumable macro-page output {} is inconsistent: {}", stagedPreset.string(), error);
            return retainWorkAndReturn(RunResult::invalidOptions);
        }
        const auto outputHash = detail::sha256MacroPageInjectionFile(stagedPreset, error);
        if (!outputHash || *outputHash != record.outputSha256)
        {
            log::error("Resumable macro-page output hash does not match its completion record: {}", stagedPreset.string());
            return retainWorkAndReturn(RunResult::invalidOptions);
        }
    }

    auto pending = std::vector<std::size_t>{};
    pending.reserve(manifest.files.size() - completed.records.size());
    for (std::size_t index = 0; index < manifest.files.size(); ++index)
        if (!completed.records.contains(index))
            pending.push_back(index);

    auto runtimeOptions = toRuntimeOptions(originalOptions, paths);
    const auto receiptPath = toStdPath(getHalionUserScriptDirectory()) /
                             (std::string{kMacroPageInjectionReceiptPrefix} + juce::Uuid().toString().toStdString() + ".log");
    ScopedMacroPageReceiptFile receiptFile{receiptPath};
    if (!removeRegularFileIfPresent(receiptPath, "macro-page receipt", error))
    {
        log::error("{}", error);
        return retainWorkAndReturn(RunResult::runtimeSetupFailed);
    }
    auto preflight = detail::MacroPageInjectionRuntimeConfig{
        *donorPreset, paths.presets, receiptPath, juce::Uuid().toString().toStdString(), true, false, true, {}};
    if (!writeMacroPageRuntime(preflight, paths, error))
    {
        log::error("{}", error);
        return retainWorkAndReturn(RunResult::runtimeSetupFailed);
    }
    log::info("Validating macro donor in HALion before processing {} pending preset(s).", pending.size());
    const auto preflightResult = runDetailed(runtimeOptions);
    if (preflightResult != RunResult::success)
    {
        log::error("Macro donor preflight failed in HALion.");
        return retainWorkAndReturn(isInfrastructureChunkFailure(preflightResult) ? preflightResult : RunResult::macroPageInjectionFailed);
    }

    auto hadProcessingFailure = false;
    const auto chunkCount = pending.empty() ? std::size_t{}
                                            : (pending.size() + static_cast<std::size_t>(originalOptions.chunkSize) - 1) /
                                                  static_cast<std::size_t>(originalOptions.chunkSize);
    for (std::size_t chunk = 0; chunk < chunkCount; ++chunk)
    {
        if (isStopRequested())
            return retainWorkAndReturn(RunResult::stopped);

        const auto begin = chunk * static_cast<std::size_t>(originalOptions.chunkSize);
        const auto end = std::min(pending.size(), begin + static_cast<std::size_t>(originalOptions.chunkSize));
        auto config = detail::MacroPageInjectionRuntimeConfig{
            *donorPreset, paths.presets, receiptPath, juce::Uuid().toString().toStdString(), false, false, originalOptions.failFast, {}};
        auto expectedIndices = std::vector<std::size_t>{};
        expectedIndices.reserve(end - begin);
        config.entries.reserve(end - begin);
        for (auto position = begin; position < end; ++position)
        {
            const auto index = pending[position];
            const auto& source = collection.files[index];
            if (!ensureSafeRelativeDirectory(paths.presets, source.relativePath.parent_path(), error) ||
                !detail::removeMacroPageInjectionStagedPreset(paths, source.relativePath, error))
            {
                log::error("{}", error);
                return retainWorkAndReturn(RunResult::runtimeSetupFailed);
            }
            expectedIndices.push_back(index);
            config.entries.push_back({index, source.sourcePath, source.relativePath});
        }

        if (!removeRegularFileIfPresent(receiptPath, "macro-page receipt", error) || !writeMacroPageRuntime(config, paths, error))
        {
            log::error("{}", error);
            return retainWorkAndReturn(RunResult::runtimeSetupFailed);
        }

        log::info("Starting macro-page save phase {}/{} with {} preset(s).", chunk + 1, chunkCount, expectedIndices.size());
        const auto saveResult = runDetailed(runtimeOptions);
        auto saveReceiptText = std::string{};
        if (!detail::readMacroPageInjectionReceiptFile(receiptPath, saveReceiptText, error))
        {
            log::error("{}", error);
            return retainWorkAndReturn(RunResult::runtimeSetupFailed);
        }
        const auto saveReceipt = detail::parseMacroPageInjectionReceipt(saveReceiptText, config.token, expectedIndices);
        for (const auto& [index, message] : saveReceipt.reportedFailures)
            log::error("HALion could not save macro-page input {} ({}): {}", index, manifest.files[index].relativePath.string(), message);
        if (!saveReceipt.reportedFailures.empty())
            hadProcessingFailure = true;
        if (!saveReceipt.errors.empty())
        {
            for (const auto& message : saveReceipt.errors)
                log::error("{}", message);
            hadProcessingFailure = true;
        }
        if (saveResult == RunResult::stopped || saveResult == RunResult::startupStopped)
            return retainWorkAndReturn(RunResult::stopped);
        if (isInfrastructureChunkFailure(saveResult))
            return retainWorkAndReturn(saveResult);

        config.validateOnly = true;
        if (!removeRegularFileIfPresent(receiptPath, "macro-page receipt", error) || !writeMacroPageRuntime(config, paths, error))
        {
            log::error("{}", error);
            return retainWorkAndReturn(RunResult::runtimeSetupFailed);
        }
        log::info("Starting macro-page validation phase {}/{}.", chunk + 1, chunkCount);
        const auto validationResult = runDetailed(runtimeOptions);
        auto validationReceiptText = std::string{};
        if (!detail::readMacroPageInjectionReceiptFile(receiptPath, validationReceiptText, error))
        {
            log::error("{}", error);
            return retainWorkAndReturn(RunResult::runtimeSetupFailed);
        }
        const auto receipt = detail::parseMacroPageInjectionReceipt(validationReceiptText, config.token, expectedIndices);
        auto accepted = std::set<std::size_t>{};
        for (const auto& [index, message] : receipt.reportedFailures)
            log::error("HALion rejected macro-page input {} ({}): {}", index, manifest.files[index].relativePath.string(), message);
        if (!receipt.reportedFailures.empty())
            hadProcessingFailure = true;
        if (!receipt.errors.empty())
        {
            for (const auto& message : receipt.errors)
                log::error("{}", message);
            hadProcessingFailure = true;
        }
        else
        {
            for (const auto index : receipt.completedIndices)
            {
                const auto& manifestFile = manifest.files[index];
                const auto stagedPreset = paths.presets / manifestFile.relativePath;
                if (!validateVstPresetFile(stagedPreset, error) ||
                    !detail::restoreVstPresetInfoChunk(collection.files[index].sourcePath, stagedPreset, error) ||
                    !validateVstPresetFile(stagedPreset, error))
                {
                    log::error("Macro-page post-processing failed for {}: {}", manifestFile.relativePath.string(), error);
                    hadProcessingFailure = true;
                    continue;
                }
                const auto outputHash = detail::sha256MacroPageInjectionFile(stagedPreset, error);
                if (!outputHash || !detail::appendMacroPageInjectionCompletion(
                                       paths.completionJournal, {index, manifestFile.sourceSha256, outputHash.value_or("")}, error))
                {
                    log::error("Could not commit macro-page completion for {}: {}", manifestFile.relativePath.string(), error);
                    return retainWorkAndReturn(RunResult::runtimeSetupFailed);
                }
                completed.records.emplace(index, detail::MacroPageInjectionCompletion{index, manifestFile.sourceSha256, *outputHash});
                accepted.insert(index);
                log::info("Completed macro-page injection {}/{}: {}", completed.records.size(), manifest.files.size(),
                          manifestFile.relativePath.string());
            }
        }

        for (const auto index : expectedIndices)
        {
            if (accepted.contains(index))
                continue;
            if (!detail::removeMacroPageInjectionStagedPreset(paths, manifest.files[index].relativePath, error))
            {
                log::error("{}", error);
                return retainWorkAndReturn(RunResult::cleanupFailed);
            }
        }

        if (saveResult != RunResult::success || validationResult != RunResult::success || accepted.size() != expectedIndices.size())
        {
            hadProcessingFailure = true;
            log::error("Macro-page chunk {}/{} completed with {} of {} presets accepted.", chunk + 1, chunkCount, accepted.size(),
                       expectedIndices.size());
            if (validationResult == RunResult::stopped || validationResult == RunResult::startupStopped)
                return retainWorkAndReturn(RunResult::stopped);
            if (isInfrastructureChunkFailure(validationResult))
                return retainWorkAndReturn(validationResult);
            if (originalOptions.failFast)
                break;
        }
    }

    if (hadProcessingFailure || completed.records.size() != manifest.files.size())
    {
        log::error("Macro-page injection is incomplete: {} of {} presets are durably staged.", completed.records.size(),
                   manifest.files.size());
        return retainWorkAndReturn(RunResult::macroPageInjectionFailed);
    }

    for (const auto& [index, record] : completed.records)
    {
        const auto stagedPreset = paths.presets / manifest.files[index].relativePath;
        const auto outputHash = detail::sha256MacroPageInjectionFile(stagedPreset, error);
        if (!outputHash || *outputHash != record.outputSha256 || !validateVstPresetFile(stagedPreset, error))
        {
            log::error("Final macro-page staging validation failed for {}: {}", stagedPreset.string(), error);
            return retainWorkAndReturn(RunResult::macroPageInjectionFailed);
        }
    }

    if (!detail::publishMacroPageInjectionPresets(paths, *outputDirectory, error))
    {
        log::error("{}", error);
        return retainWorkAndReturn(RunResult::cleanupFailed);
    }
    log::info("Published {} HALion Sonic Program preset(s) to {}.", manifest.files.size(), outputDirectory->string());

    if (!detail::cleanupMacroPageInjectionWorkDirectory(paths, *outputDirectory, error))
        log::warn("Macro-page presets were published successfully, but work-directory cleanup failed: {}", error);

    return RunResult::success;
}

RunResult Bridge::Impl::runPresetRemapInvocation(const VstPresetRemapOptions& options, const detail::PresetRemapRuntimeConfig& config)
{
    pluginInstance = nullptr;

    ScopedPresetRemapRuntimeRoot presetRuntimeRoot{config};
    if (!presetRuntimeRoot.isReady())
        return presetRuntimeRoot.getFailureResult();

    const auto runtimeOptions = toRuntimeOptions(options);
    if (!pluginFormatsRegistered && options.showGui)
    {
        log::debug("Registering GUI-capable plugin formats...");
        juce::addDefaultFormatsToManager(formatManager);
        pluginFormatsRegistered = true;
    }
    else if (!pluginFormatsRegistered)
    {
        log::debug("Registering headless plugin formats...");
        juce::addHeadlessDefaultFormatsToManager(formatManager);
        pluginFormatsRegistered = true;
    }

    auto pluginFile = Bridge::findHalionPlugin(options.pluginPathOverride);
    if (!pluginFile)
        return RunResult::pluginNotFound;

    if (!loadPlugin(toJuceFile(*pluginFile), options))
        return RunResult::pluginLoadFailed;

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

    return runProcessingLoop(runtimeOptions, toJuceFile(config.runtimeRoot));
}

RunResult Bridge::Impl::runPresetInspectionInvocation(const VstPresetInspectionOptions& options,
                                                      const detail::VstPresetInspectionRuntimeConfig& config)
{
    pluginInstance = nullptr;

    ScopedPresetInspectionRuntimeRoot presetRuntimeRoot{config};
    if (!presetRuntimeRoot.isReady())
        return presetRuntimeRoot.getFailureResult();

    const auto runtimeOptions = toRuntimeOptions(options);
    if (!pluginFormatsRegistered && options.showGui)
    {
        log::debug("Registering GUI-capable plugin formats...");
        juce::addDefaultFormatsToManager(formatManager);
        pluginFormatsRegistered = true;
    }
    else if (!pluginFormatsRegistered)
    {
        log::debug("Registering headless plugin formats...");
        juce::addHeadlessDefaultFormatsToManager(formatManager);
        pluginFormatsRegistered = true;
    }

    const auto pluginFile = Bridge::findHalionPlugin(options.pluginPathOverride);
    if (!pluginFile)
        return RunResult::pluginNotFound;

    if (!loadPlugin(toJuceFile(*pluginFile), runtimeOptions))
        return RunResult::pluginLoadFailed;

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

    return runProcessingLoop(runtimeOptions, toJuceFile(config.runtimeRoot));
}

bool Bridge::Impl::loadPlugin(const juce::File& pluginFile, const AppOptions& options)
{
    setCrashDiagnosticPhase("loadPlugin: preparing plugin description");

    if (isEnvironmentFlagEnabled(kForbidPluginInstantiationEnvironmentVariable))
    {
        log::error("HALion plugin instantiation is disabled by {}.", kForbidPluginInstantiationEnvironmentVariable);
        return false;
    }

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

bool Bridge::Impl::loadPlugin(const juce::File& pluginFile, const VstPresetRemapOptions& options)
{
    return loadPlugin(pluginFile, toRuntimeOptions(options));
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

    auto finishProcessing = [&](const RunResult result, const BuildMarkerSet* markersToClean = nullptr)
    {
        if (window)
        {
            window->setVisible(false);
            window = nullptr;
            editor = nullptr;
        }

        log::debug("Releasing plugin resources...");
        pluginInstance->releaseResources();

        log::debug("Unloading plugin instance...");
        pluginInstance = nullptr;
        pumpMessages(kPrepareMessagePumpMs);

        if (markersToClean != nullptr && !cleanupPostReleaseMarkers(*markersToClean, result) && result == RunResult::success)
            return RunResult::cleanupFailed;

        return result;
    };

    auto markers = prepareBuildMarkers(builderRoot);
    if (!markers)
        return finishProcessing(RunResult::runtimeSetupFailed);

    const auto stateApplied = applyVstPresetData(makeEmbeddedBootstrapPresetData());

    if (stateApplied)
        log::info("State/Preset applied successfully.");
    else
        return finishProcessing(RunResult::presetApplyFailed, &*markers);

    log::info("Running HALion Lua build...");
    logBuildWaitConfiguration(*markers, options);

    juce::AudioBuffer<float> buffer(juce::jmax(2, pluginInstance->getTotalNumInputChannels(), pluginInstance->getTotalNumOutputChannels()),
                                    kBlockSize);
    juce::MidiBuffer midi;

    auto waitResult = waitForBuildCompletion(*pluginInstance, options, *markers, window.get(), closeRequested, buffer, midi);

    if (options.noKill)
        holdPluginAliveForInspection(*pluginInstance, options, window.get(), closeRequested, buffer, midi);

    return finishProcessing(waitResult.succeeded ? RunResult::success : waitResult.failureResult, &*markers);
}

} // namespace halionbridge
