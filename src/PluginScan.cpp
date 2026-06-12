#include "PluginScan.h"

#include "ChildProcessOutput.h"
#include "Log.h"
#include "PathUtils.h"
#include "halionbridge/Bridge.h"
#include "halionbridge/CrashDiagnostics.h"

#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/base/funknown.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/base/ipluginbase.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/vst/ivstaudioprocessor.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace halionbridge::detail
{
namespace
{
constexpr double kPluginScanTimeoutMs = 120000.0;

// HALion 7's module teardown (ExitDll on Windows) crashes intermittently with
// an access violation or a fast-fail abort when an IComponent was initialized
// and terminated during the same module load. JUCE's findAllTypesForFile does
// exactly that for every audio class in order to fill in channel counts, so a
// JUCE-based scan of HALion dies inside the module unload. The scan below
// therefore reads only factory class metadata and never instantiates the
// plugin; with no component lifecycle in between, module load and unload are
// stable. Channel counts in the resulting descriptions stay 0, which is
// acceptable because they are informational and not required to instantiate
// the plugin afterwards.

template <typename VstInterface> struct ScopedVstInterface
{
    ~ScopedVstInterface()
    {
        if (pointer != nullptr)
            pointer->release();
    }

    VstInterface* pointer = nullptr;
};

#if JUCE_MAC

// VST3 bundles on macOS are loaded through CoreFoundation and use the
// bundleEntry/bundleExit module functions, mirroring JUCE's DLLHandle.
class ScopedVst3Module
{
  public:
    ScopedVst3Module() = default;
    ScopedVst3Module(const ScopedVst3Module&) = delete;
    ScopedVst3Module& operator=(const ScopedVst3Module&) = delete;

    bool open(const juce::File& bundleDirectory)
    {
        const auto path = bundleDirectory.getFullPathName().toRawUTF8();
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path),
                                                               static_cast<CFIndex>(std::strlen(path)), bundleDirectory.isDirectory());
        if (url == nullptr)
            return false;

        bundle = CFBundleCreate(kCFAllocatorDefault, url);
        CFRelease(url);

        if (bundle == nullptr)
            return false;

        if (!CFBundleLoadExecutableAndReturnError(bundle, nullptr))
        {
            log::debug("Could not load VST3 bundle executable: {}", bundleDirectory.getFullPathName().toStdString());
            return false;
        }

        if (auto* bundleEntry = reinterpret_cast<BundleEntryProc>(getFunction("bundleEntry")))
        {
            if (!bundleEntry(bundle))
            {
                log::debug("bundleEntry returned false for {}", bundleDirectory.getFullPathName().toStdString());
                return false;
            }
        }

        initialized = true;
        return true;
    }

    Steinberg::IPluginFactory* getPluginFactory()
    {
        if (factory == nullptr)
            if (auto* getFactory = reinterpret_cast<GetFactoryProc>(getFunction("GetPluginFactory")))
                factory = getFactory();

        return factory;
    }

    ~ScopedVst3Module()
    {
        if (factory != nullptr)
            factory->release();

        if (initialized)
            if (auto* bundleExit = reinterpret_cast<BundleExitProc>(getFunction("bundleExit")))
                bundleExit();

        if (bundle != nullptr)
            CFRelease(bundle);
    }

  private:
    using BundleEntryProc = bool (*)(CFBundleRef);
    using BundleExitProc = bool (*)();
    using GetFactoryProc = Steinberg::IPluginFactory* (*)();

    void* getFunction(const char* functionName)
    {
        if (bundle == nullptr)
            return nullptr;

        CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, functionName, kCFStringEncodingUTF8);
        if (name == nullptr)
            return nullptr;

        auto* function = CFBundleGetFunctionPointerForName(bundle, name);
        CFRelease(name);
        return function;
    }

    CFBundleRef bundle = nullptr;
    Steinberg::IPluginFactory* factory = nullptr;
    bool initialized = false;
};

#else

// VST3 modules on Windows are plain DLLs that use the InitDll/ExitDll module
// functions, mirroring JUCE's DLLHandle.
class ScopedVst3Module
{
  public:
    ScopedVst3Module() = default;
    ScopedVst3Module(const ScopedVst3Module&) = delete;
    ScopedVst3Module& operator=(const ScopedVst3Module&) = delete;

    bool open(const juce::File& moduleBinary)
    {
        if (!library.open(moduleBinary.getFullPathName()))
        {
            log::debug("Could not load VST3 module binary: {}", moduleBinary.getFullPathName().toStdString());
            return false;
        }

        // InitDll is optional for VST3 modules; a missing entry point is valid.
        if (auto* initModule = reinterpret_cast<ModuleEntryProc>(library.getFunction("InitDll")))
        {
            if (!initModule())
            {
                log::debug("InitDll returned false for {}", moduleBinary.getFullPathName().toStdString());
                library.close();
                return false;
            }
        }

        initialized = true;
        return true;
    }

    Steinberg::IPluginFactory* getPluginFactory()
    {
        if (factory == nullptr)
            if (auto* getFactory = reinterpret_cast<GetFactoryProc>(library.getFunction("GetPluginFactory")))
                factory = getFactory();

        return factory;
    }

    ~ScopedVst3Module()
    {
        // The factory reference must be released before the module is shut
        // down; ExitDll tears down the allocator the factory lives in.
        if (factory != nullptr)
            factory->release();

        if (initialized)
            if (auto* exitModule = reinterpret_cast<ModuleEntryProc>(library.getFunction("ExitDll")))
                exitModule();

        library.close();
    }

  private:
    using ModuleEntryProc = bool(PLUGIN_API*)();
    using GetFactoryProc = Steinberg::IPluginFactory*(PLUGIN_API*)();

    juce::DynamicLibrary library;
    Steinberg::IPluginFactory* factory = nullptr;
    bool initialized = false;
};

#endif

std::vector<juce::File> findVst3ModuleBinaries(const juce::File& pluginFile)
{
#if JUCE_MAC
    // CoreFoundation loads the bundle directory directly.
    return {pluginFile};
#else
    // Old-style single-file .vst3 module.
    if (pluginFile.existsAsFile())
        return {pluginFile};

    // New-style bundle directory: the loadable module is a .vst3 file below
    // Contents/, e.g. Contents/x86_64-win/<name>.vst3. All candidates are
    // collected; the first one that loads wins, which also skips binaries
    // built for a different architecture.
    juce::Array<juce::File> found;
    pluginFile.findChildFiles(found, juce::File::findFiles, true, "*.vst3");

    std::vector<juce::File> binaries;
    binaries.reserve(static_cast<size_t>(found.size()));
    for (const auto& file : found)
        binaries.push_back(file);

    std::sort(binaries.begin(), binaries.end(),
              [](const juce::File& lhs, const juce::File& rhs) { return lhs.getFullPathName() < rhs.getFullPathName(); });

    return binaries;
#endif
}

// Factory metadata strings are fixed-size fields that a plugin is not
// guaranteed to null-terminate; never read past the end of the field.
juce::String boundedUtf8String(const Steinberg::char8* text, const size_t maxBytes)
{
    const auto* chars = reinterpret_cast<const char*>(text);
    size_t length = 0;
    while (length < maxBytes && chars[length] != 0)
        ++length;

    return juce::String::fromUTF8(chars, static_cast<int>(length));
}

juce::String boundedUtf16String(const Steinberg::char16* text, const size_t maxChars)
{
    const auto* chars = reinterpret_cast<const juce::CharPointer_UTF16::CharType*>(text);
    size_t length = 0;
    while (length < maxChars && chars[length] != 0)
        ++length;

    return juce::String(juce::CharPointer_UTF16(chars), length);
}

bool matchesPreferredDescription(const juce::PluginDescription& description, const juce::PluginDescription& preferredDescription)
{
    return description.uniqueId == preferredDescription.uniqueId || description.deprecatedUid == preferredDescription.deprecatedUid;
}

std::optional<juce::PluginDescription> selectPluginDescription(const std::vector<juce::PluginDescription>& descriptions,
                                                               const juce::File& pluginFile,
                                                               const std::optional<juce::String>& preferredClassId)
{
    if (descriptions.empty())
        return std::nullopt;

    if (preferredClassId)
    {
        if (auto preferredDescription = makeHalionDescriptionFromClassId(pluginFile, *preferredClassId))
        {
            for (const auto& description : descriptions)
            {
                if (matchesPreferredDescription(description, *preferredDescription))
                {
                    log::debug("Selected plugin description matching embedded bootstrap class ID: {}", description.name.toStdString());
                    return description;
                }
            }
        }
    }

    for (const auto& description : descriptions)
    {
        if (description.isInstrument)
        {
            log::debug("Selected first instrument plugin description: {}", description.name.toStdString());
            return description;
        }
    }

    log::debug("Selected first plugin description: {}", descriptions.front().name.toStdString());
    return descriptions.front();
}

std::optional<juce::PluginDescription> loadPluginDescriptionFromXmlFile(const juce::File& xmlFile, const juce::File& pluginFile,
                                                                        const std::optional<juce::String>& preferredClassId)
{
    if (!xmlFile.existsAsFile())
        return std::nullopt;

    auto xml = juce::XmlDocument::parse(xmlFile);
    if (xml == nullptr || !xml->hasTagName("LIST"))
        return std::nullopt;

    std::vector<juce::PluginDescription> descriptions;
    for (auto* child : xml->getChildIterator())
    {
        if (child == nullptr || !child->hasTagName("PLUGIN"))
            continue;

        juce::PluginDescription description;
        if (description.loadFromXml(*child))
            descriptions.push_back(std::move(description));
    }

    return selectPluginDescription(descriptions, pluginFile, preferredClassId);
}

template <typename Range> int getVst3HashForRange(Range&& range) noexcept
{
    // This mirrors the UID hash used by JUCE's VST3 plugin format when it turns
    // a Steinberg class TUID into PluginDescription uniqueId/deprecatedUid values.
    // Keeping the synthesized description aligned with JUCE lets halionbridge
    // instantiate HALion from the class ID embedded in the bootstrap preset
    // without scanning the plugin bundle on the common path.
    juce::uint32 value = 0;

    for (const auto& item : range)
        value = (value * 31) + static_cast<juce::uint32>(item);

    return static_cast<int>(value);
}

std::array<juce::uint32, 4> getNormalisedTuid(const Steinberg::TUID& tuid) noexcept
{
    const Steinberg::FUID fuid{tuid};
    return {{fuid.getLong1(), fuid.getLong2(), fuid.getLong3(), fuid.getLong4()}};
}

void appendFactoryClassDescriptions(Steinberg::IPluginFactory& factory, const juce::File& pluginFile,
                                    std::vector<juce::PluginDescription>& descriptions)
{
    auto factoryVendor = juce::String();
    Steinberg::PFactoryInfo factoryInfo{};
    if (factory.getFactoryInfo(&factoryInfo) == Steinberg::kResultOk)
        factoryVendor = boundedUtf8String(factoryInfo.vendor, Steinberg::PFactoryInfo::kNameSize).trim();

    ScopedVstInterface<Steinberg::IPluginFactory2> factory2;
    factory.queryInterface(Steinberg::IPluginFactory2_iid, reinterpret_cast<void**>(&factory2.pointer));

    ScopedVstInterface<Steinberg::IPluginFactory3> factory3;
    factory.queryInterface(Steinberg::IPluginFactory3_iid, reinterpret_cast<void**>(&factory3.pointer));

    const auto numClasses = factory.countClasses();
    log::debug("VST3 factory: vendor='{}', classes={}", factoryVendor.toStdString(), numClasses);

    juce::StringArray collectedNames;

    for (Steinberg::int32 i = 0; i < numClasses; ++i)
    {
        Steinberg::PClassInfo info{};
        if (factory.getClassInfo(i, &info) != Steinberg::kResultOk)
            continue;

        if (std::strcmp(info.category, kVstAudioEffectClass) != 0)
            continue;

        const auto name = boundedUtf8String(info.name, Steinberg::PClassInfo::kNameSize).trim();
        if (name.isEmpty() || collectedNames.contains(name, true))
            continue;

        collectedNames.add(name);

        auto classVendor = juce::String();
        auto classVersion = juce::String();
        auto subCategories = juce::String();

        if (factory3.pointer != nullptr)
        {
            Steinberg::PClassInfoW infoW{};
            if (factory3.pointer->getClassInfoUnicode(i, &infoW) == Steinberg::kResultOk)
            {
                classVendor = boundedUtf16String(infoW.vendor, Steinberg::PClassInfoW::kVendorSize).trim();
                classVersion = boundedUtf16String(infoW.version, Steinberg::PClassInfoW::kVersionSize).trim();
                subCategories = boundedUtf8String(infoW.subCategories, Steinberg::PClassInfoW::kSubCategoriesSize).trim();
            }
        }

        if (classVersion.isEmpty() && subCategories.isEmpty() && factory2.pointer != nullptr)
        {
            Steinberg::PClassInfo2 info2{};
            if (factory2.pointer->getClassInfo2(i, &info2) == Steinberg::kResultOk)
            {
                classVendor = boundedUtf8String(info2.vendor, Steinberg::PClassInfo2::kVendorSize).trim();
                classVersion = boundedUtf8String(info2.version, Steinberg::PClassInfo2::kVersionSize).trim();
                subCategories = boundedUtf8String(info2.subCategories, Steinberg::PClassInfo2::kSubCategoriesSize).trim();
            }
        }

        juce::PluginDescription description;
        description.fileOrIdentifier = pluginFile.getFullPathName();
        description.lastFileModTime = pluginFile.getLastModificationTime();
        description.lastInfoUpdateTime = juce::Time::getCurrentTime();
        description.name = name;
        description.descriptiveName = name;
        description.pluginFormatName = "VST3";
        // JUCE prefers the factory-level vendor and uses the class vendor only
        // as a fallback; the same precedence keeps descriptions comparable.
        description.manufacturerName = factoryVendor.isNotEmpty() ? factoryVendor : classVendor;
        description.version = classVersion;
        description.category =
            subCategories.isNotEmpty() ? subCategories : boundedUtf8String(info.category, Steinberg::PClassInfo::kCategorySize).trim();
        description.isInstrument = description.category.containsIgnoreCase("Instrument");
        description.deprecatedUid = getVst3HashForRange(info.cid);
        description.uniqueId = getVst3HashForRange(getNormalisedTuid(info.cid));

        log::debug("VST3 class: name='{}', category='{}', version='{}'", name.toStdString(), description.category.toStdString(),
                   classVersion.toStdString());

        descriptions.push_back(std::move(description));
    }
}

std::vector<juce::PluginDescription> enumerateVst3FactoryClassDescriptions(const juce::File& pluginFile)
{
    std::vector<juce::PluginDescription> descriptions;

    for (const auto& binary : findVst3ModuleBinaries(pluginFile))
    {
        log::debug("Reading VST3 factory metadata from {}", binary.getFullPathName().toStdString());

        ScopedVst3Module module;
        if (!module.open(binary))
            continue;

        auto* factory = module.getPluginFactory();
        if (factory == nullptr)
        {
            log::debug("VST3 module exposes no plugin factory: {}", binary.getFullPathName().toStdString());
            continue;
        }

        appendFactoryClassDescriptions(*factory, pluginFile, descriptions);

        // The first loadable binary that exposes audio classes wins; remaining
        // candidates are usually the same module built for other architectures.
        if (!descriptions.empty())
            break;
    }

    return descriptions;
}

std::optional<juce::String> normaliseVst3ClassId(juce::String classId)
{
    classId = classId.trim().unquoted().removeCharacters("{}- ").toUpperCase();

    if (classId.length() != 32)
        return std::nullopt;

    for (auto i = 0; i < classId.length(); ++i)
    {
        const auto c = static_cast<char>(classId[i]);
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return std::nullopt;
    }

    return classId;
}

juce::String formatExitCode(const juce::uint32 exitCode)
{
    std::ostringstream stream;
    stream << static_cast<std::int32_t>(exitCode) << " (0x" << std::hex << std::uppercase << exitCode << ")";
    return stream.str();
}

} // namespace

std::optional<juce::PluginDescription> makeHalionDescriptionFromClassId(const juce::File& pluginFile, const juce::String& classId)
{
    const auto normalisedClassId = normaliseVst3ClassId(classId);
    if (!normalisedClassId)
        return std::nullopt;

    Steinberg::FUID fuid;
    if (!fuid.fromString(normalisedClassId->toRawUTF8()))
        return std::nullopt;

    Steinberg::TUID tuid{};
    fuid.toTUID(tuid);

    juce::PluginDescription description;
    description.fileOrIdentifier = pluginFile.getFullPathName();
    description.lastFileModTime = pluginFile.getLastModificationTime();
    description.lastInfoUpdateTime = juce::Time::getCurrentTime();
    description.manufacturerName = "Steinberg Media Technologies";
    description.name = "HALion 7";
    description.descriptiveName = "HALion 7";
    description.pluginFormatName = "VST3";
    description.category = "Instrument";
    description.isInstrument = true;
    description.deprecatedUid = getVst3HashForRange(tuid);
    description.uniqueId = getVst3HashForRange(getNormalisedTuid(tuid));

    return description;
}

std::optional<juce::PluginDescription> scanPluginInWorker(const juce::File& executableFile, const juce::File& pluginFile,
                                                          const std::optional<juce::String>& preferredClassId)
{
    auto outputFile =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("halionbridge_plugin_scan", ".xml", false);

    juce::StringArray command;
    command.add(executableFile.getFullPathName());
    command.add("--halionbridge-scan-plugin");
    command.add(pluginFile.getFullPathName());
    command.add(outputFile.getFullPathName());

    log::debug("Scanning plugin in isolated worker process...");

    juce::ChildProcess process;
    if (!process.start(command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        log::error("Failed to launch plugin scan worker.");
        return std::nullopt;
    }

    const auto startTime = juce::Time::getMillisecondCounterHiRes();
    ChildProcessOutputBuffer childOutput;

    while (process.isRunning())
    {
        forwardChildOutput(process, childOutput);

        if (isStopRequested())
        {
            process.kill();
            forwardChildOutput(process, childOutput);
            flushChildOutput(childOutput);
            log::warn("Plugin scan worker stopped by user request.");
            outputFile.deleteFile();
            return std::nullopt;
        }

        if (juce::Time::getMillisecondCounterHiRes() - startTime >= kPluginScanTimeoutMs)
        {
            process.kill();
            forwardChildOutput(process, childOutput);
            flushChildOutput(childOutput);
            log::error("Plugin scan worker timed out after {} seconds.", static_cast<int>(kPluginScanTimeoutMs / 1000.0));
            outputFile.deleteFile();
            return std::nullopt;
        }

        juce::Thread::sleep(10);
    }

    forwardChildOutput(process, childOutput);
    flushChildOutput(childOutput);

    const auto exitCode = process.getExitCode();
    if (exitCode != 0)
    {
        log::error("Plugin scan worker exited with {}.", formatExitCode(exitCode).toStdString());
        outputFile.deleteFile();
        return std::nullopt;
    }

    auto description = loadPluginDescriptionFromXmlFile(outputFile, pluginFile, preferredClassId);
    outputFile.deleteFile();

    if (!description)
        log::error("Plugin scan worker did not write a valid plugin description.");

    return description;
}

std::optional<juce::PluginDescription> scanPluginInProcess(const juce::File& pluginFile,
                                                           const std::optional<juce::String>& preferredClassId)
{
    log::debug("Scanning plugin at: {}", pluginFile.getFullPathName().toStdString());

    setCrashDiagnosticPhase("loadPlugin: VST3 factory metadata scan");
    const auto descriptions = enumerateVst3FactoryClassDescriptions(pluginFile);
    setCrashDiagnosticPhase("loadPlugin: VST3 factory metadata scan completed");

    log::debug("Plugin scan completed. Descriptions found: {}", descriptions.size());

    if (descriptions.empty())
        log::error("No VST3 audio classes found in {}", pluginFile.getFullPathName().toStdString());

    return selectPluginDescription(descriptions, pluginFile, preferredClassId);
}

int runPluginScanWorker(const juce::StringArray& args)
{
    if (args.size() != 3)
    {
        log::error("--halionbridge-scan-plugin requires <plugin-path> <output-xml>.");
        return 1;
    }

    const auto pluginFile = normalizeCliPath(args[1]);
    const auto outputFile = normalizeCliPath(args[2]);

    if (!pluginFile.exists())
    {
        log::error("Plugin scan worker cannot find plugin at {}", pluginFile.getFullPathName().toStdString());
        return 1;
    }

    log::debug("Worker scanning plugin at: {}", pluginFile.getFullPathName().toStdString());

    setCrashDiagnosticPhase("scan worker: VST3 factory metadata scan");
    const auto descriptions = enumerateVst3FactoryClassDescriptions(pluginFile);
    setCrashDiagnosticPhase("scan worker: scan completed");

    log::debug("Worker scan completed. Descriptions found: {}", descriptions.size());

    if (descriptions.empty())
    {
        log::error("Plugin scan worker found no plugin descriptions.");
        return 1;
    }

    juce::XmlElement list("LIST");
    for (const auto& description : descriptions)
        list.addChildElement(description.createXml().release());

    if (!outputFile.replaceWithText(list.toString()))
    {
        log::error("Plugin scan worker failed to write {}", outputFile.getFullPathName().toStdString());
        return 1;
    }

    return 0;
}

} // namespace halionbridge::detail
