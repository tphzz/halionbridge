#include "halionbridge/Bridge.h"
#include "CliCommand.h"
#include "MacroPageInjection.h"
#include "PathUtils.h"
#include "VstPresetMetadata.h"

#include <juce_core/juce_core.h>

#include <array>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

class MacroPageInjectionTests final : public juce::UnitTest
{
  public:
    MacroPageInjectionTests() : juce::UnitTest("MacroPageInjectionTests", "halionbridge") {}

    void runTest() override
    {
        beginTest("CLI classifies the macro-page injection command");
        {
            const auto args = std::vector<std::string>{"inject-macro-page"};
            expect(halionbridge::detail::classifyCliCommand(args) == halionbridge::detail::CliCommandKind::injectMacroPage);
        }

        beginTest("CLI parses the explicit macro-page injection contract");
        {
            auto root = makeTempRoot("cli");
            const auto input = root.getChildFile("input");
            const auto donor = root.getChildFile("macro.vstpreset");
            expect(input.createDirectory());
            expect(donor.replaceWithText("donor"));

            const auto args = std::vector<std::string>{"--input-directory",
                                                       input.getFullPathName().toStdString(),
                                                       "--output-directory",
                                                       root.getChildFile("output").getFullPathName().toStdString(),
                                                       "--donor-preset",
                                                       donor.getFullPathName().toStdString(),
                                                       "--recursive",
                                                       "--resume",
                                                       "--chunk-size",
                                                       "17",
                                                       "--fail-fast",
                                                       "--no-timeout",
                                                       "--gui",
                                                       "--force-scan"};
            const auto parsed = halionbridge::detail::parseVstPresetMacroPageInjectionOptionsDetailed(args);
            expect(parsed.options.has_value());
            if (parsed.options)
            {
                expect(parsed.options->inputDirectory == halionbridge::detail::toStdPath(input));
                expect(parsed.options->donorPreset == halionbridge::detail::toStdPath(donor));
                expect(parsed.options->recursive);
                expect(parsed.options->resume);
                expectEquals(parsed.options->chunkSize, 17);
                expect(parsed.options->failFast);
                expectEquals(parsed.options->timeoutSeconds, 0);
                expect(parsed.options->showGui);
                expect(parsed.options->forceScan);
            }

            root.deleteRecursively();
        }

        beginTest("CLI rejects missing paths and invalid chunk sizes");
        {
            const auto missing = halionbridge::detail::parseVstPresetMacroPageInjectionOptionsDetailed({});
            expect(!missing.options.has_value());
            expect(missing.errorKind == halionbridge::detail::CliParseErrorKind::syntax);

            const auto noKillArgs = std::vector<std::string>{"--nokill"};
            const auto noKill = halionbridge::detail::parseVstPresetMacroPageInjectionOptionsDetailed(noKillArgs);
            expect(!noKill.options.has_value());
            expect(noKill.errorKind == halionbridge::detail::CliParseErrorKind::syntax);

            auto root = makeTempRoot("invalid_cli");
            const auto input = root.getChildFile("input");
            const auto donor = root.getChildFile("macro.vstpreset");
            expect(input.createDirectory());
            expect(donor.replaceWithText("donor"));
            const auto args = std::vector<std::string>{"--input-directory",  input.getFullPathName().toStdString(),
                                                       "--output-directory", root.getChildFile("output").getFullPathName().toStdString(),
                                                       "--donor-preset",     donor.getFullPathName().toStdString(),
                                                       "--chunk-size",       "0"};
            const auto invalid = halionbridge::detail::parseVstPresetMacroPageInjectionOptionsDetailed(args);
            expect(!invalid.options.has_value());
            expect(invalid.errorKind == halionbridge::detail::CliParseErrorKind::syntax);
            root.deleteRecursively();
        }

        beginTest("Collection is deterministic and recursion is explicit");
        {
            auto root = makeTempRoot("collection");
            const auto nested = root.getChildFile("nested");
            expect(nested.createDirectory());
            expect(root.getChildFile("Zulu.vstpreset").replaceWithText("z"));
            expect(root.getChildFile("alpha.VSTPRESET").replaceWithText("a"));
            expect(nested.getChildFile("Beta.vstpreset").replaceWithText("b"));

            const auto flat = halionbridge::detail::collectMacroPageInjectionFiles(halionbridge::detail::toStdPath(root), false);
            expect(flat.errors.empty());
            expectEquals(static_cast<int>(flat.files.size()), 2);
            const auto recursive = halionbridge::detail::collectMacroPageInjectionFiles(halionbridge::detail::toStdPath(root), true);
            expect(recursive.errors.empty());
            expectEquals(static_cast<int>(recursive.files.size()), 3);
            if (recursive.files.size() == 3)
                expectEquals(recursive.files[1].relativePath.generic_string(), std::string("nested/Beta.vstpreset"));

            root.deleteRecursively();
        }

        beginTest("Runtime module binds one token to an explicit chunk");
        {
            const auto config = halionbridge::detail::MacroPageInjectionRuntimeConfig{
                std::filesystem::path("C:/Presets/macro.vstpreset"),
                std::filesystem::path("C:/Work/presets"),
                std::filesystem::path("C:/Work/receipt.tsv"),
                "a1b2c3",
                false,
                false,
                true,
                {{7, std::filesystem::path("C:/Presets/source.vstpreset"), std::filesystem::path("nested/source.vstpreset")}}};
            const auto text = halionbridge::detail::createMacroPageInjectionRuntimeModuleText(config);
            expect(text.find("HALIONBRIDGE_MACRO_PAGE_TOKEN = \"a1b2c3\"") != std::string::npos);
            expect(text.find("index = 7") != std::string::npos);
            expect(text.find("nested/source.vstpreset") != std::string::npos);
            expect(text.find("HALIONBRIDGE_MACRO_PAGE_FAIL_FAST = true") != std::string::npos);
            expect(text.find("HALIONBRIDGE_MACRO_PAGE_VALIDATE_ONLY = false") != std::string::npos);
            expect(text.find("require(\"halionbridge_macro_page_inject\")") != std::string::npos);

            auto validationConfig = config;
            validationConfig.validateOnly = true;
            const auto validationText = halionbridge::detail::createMacroPageInjectionRuntimeModuleText(validationConfig);
            expect(validationText.find("HALIONBRIDGE_MACRO_PAGE_VALIDATE_ONLY = true") != std::string::npos);
        }

        beginTest("HALion helper retains a Program donor and preserves generic source root state");
        {
            const auto helper = juce::File::getCurrentWorkingDirectory()
                                    .getChildFile("halion-lua")
                                    .getChildFile("macro_page_inject.lua")
                                    .loadFileAsString();
            expect(helper.contains("loadPreset"));
            expect(helper.contains("requireType(donor, \"Program\""));
            expect(helper.contains("parameterDefinitions"));
            expect(helper.contains("getNumQCAssignments"));
            expect(helper.contains("addQCAssignment"));
            expect(helper.contains("removeQCAssignment"));
            expect(helper.contains("removeFromParent"));
            expect(helper.contains("appendChild"));
            expect(helper.contains("ctx.save_preset"));
            expect(helper.contains("\"HS\", \"program\""));
            expect(!helper.contains("appendDonor"));
            expect(!helper.contains("setDonorMarker"));
            expect(helper.contains("HBMPI1\\t"));
            expect(helper.contains("file.flush"));
        }

        beginTest("Receipt accepts only the expected token and indices");
        {
            constexpr auto twoIndices = std::array<std::size_t, 2>{7, 9};
            constexpr auto oneIndex = std::array<std::size_t, 1>{7};
            const auto valid =
                halionbridge::detail::parseMacroPageInjectionReceipt("HBMPI1\ta1b2c3\t7\nHBMPI1\ta1b2c3\t9\n", "a1b2c3", twoIndices);
            expect(valid.errors.empty());
            expectEquals(static_cast<int>(valid.completedIndices.size()), 2);

            const auto stale = halionbridge::detail::parseMacroPageInjectionReceipt("HBMPI1\told\t7\n", "a1b2c3", oneIndex);
            expect(!stale.errors.empty());
            const auto failure = halionbridge::detail::parseMacroPageInjectionReceipt(
                "HBMPE1\ta1b2c3\t7\tSaved Program topology changed.\n", "a1b2c3", oneIndex);
            expect(failure.errors.empty());
            expectEquals(static_cast<int>(failure.reportedFailures.size()), 1);
            if (!failure.reportedFailures.empty())
                expectEquals(failure.reportedFailures.front().second, std::string("Saved Program topology changed."));
            const auto duplicate =
                halionbridge::detail::parseMacroPageInjectionReceipt("HBMPI1\ta1b2c3\t7\nHBMPI1\ta1b2c3\t7\n", "a1b2c3", oneIndex);
            expect(!duplicate.errors.empty());
            const auto truncated = halionbridge::detail::parseMacroPageInjectionReceipt("HBMPI1\ta1b2c3\t7", "a1b2c3", oneIndex);
            expect(truncated.errors.empty());
            expect(truncated.completedIndices.empty());
        }

        beginTest("Info transplant keeps transformed state and exact source metadata bytes");
        {
            auto root = makeTempRoot("info");
            const auto source = root.getChildFile("source.vstpreset");
            const auto transformed = root.getChildFile("transformed.vstpreset");
            const auto sourceInfo = std::string("<MetaInfo data=\"source &amp; exact\"/>\r\n");
            expect(writePreset(source, "source-state", sourceInfo));
            expect(writePreset(transformed, "transformed-state", "<MetaInfo data=\"generated\"/>"));
            const auto staleSidecar = juce::File(halionbridge::detail::toJuceString(
                halionbridge::detail::makeVstPresetInfoReplacementTemporaryPath(halionbridge::detail::toStdPath(transformed))));
            expect(staleSidecar.replaceWithText("interrupted"));

            auto error = std::string{};
            expect(halionbridge::detail::restoreVstPresetInfoChunk(halionbridge::detail::toStdPath(source),
                                                                   halionbridge::detail::toStdPath(transformed), error),
                   error);
            auto info = std::optional<std::string>{};
            expect(halionbridge::detail::readVstPresetInfoChunk(halionbridge::detail::toStdPath(transformed), info, error));
            expect(info.has_value());
            if (info)
                expectEquals(*info, sourceInfo);
            juce::MemoryBlock transformedBytes;
            expect(transformed.loadFileAsData(transformedBytes));
            const auto transformedText = std::string_view(static_cast<const char*>(transformedBytes.getData()), transformedBytes.getSize());
            expect(transformedText.find("transformed-state") != std::string_view::npos);
            expect(transformedText.find("source-state") == std::string_view::npos);
            expect(!staleSidecar.exists());

            root.deleteRecursively();
        }

        beginTest("Info transplant preserves a source preset with no Info chunk");
        {
            auto root = makeTempRoot("no_info");
            const auto source = root.getChildFile("source.vstpreset");
            const auto transformed = root.getChildFile("transformed.vstpreset");
            expect(writePresetWithoutInfo(source, "source-state"));
            expect(writePreset(transformed, "transformed-state", "<MetaInfo data=\"generated\"/>"));

            auto error = std::string{};
            expect(halionbridge::detail::restoreVstPresetInfoChunk(halionbridge::detail::toStdPath(source),
                                                                   halionbridge::detail::toStdPath(transformed), error),
                   error);
            auto info = std::optional<std::string>{"unexpected"};
            expect(halionbridge::detail::readVstPresetInfoChunk(halionbridge::detail::toStdPath(transformed), info, error));
            expect(!info.has_value());

            root.deleteRecursively();
        }

#if JUCE_WINDOWS
        beginTest("Info transplant supports a temporary sidecar beyond MAX_PATH");
        {
            auto root = makeTempRoot("long_info");
            const auto source = root.getChildFile("source.vstpreset");
            constexpr auto destinationLength = 248;
            constexpr auto presetName = std::string_view("voice.vstpreset");
            const auto rootLength = static_cast<std::size_t>(root.getFullPathName().length());
            const auto directoryNameLength = destinationLength - rootLength - 1U - presetName.size() - 1U;
            const auto longDirectory = root.getChildFile(juce::String::repeatedString("x", static_cast<int>(directoryNameLength)));
            const auto transformed = longDirectory.getChildFile("voice.vstpreset");
            const auto transformedPath = halionbridge::detail::toStdPath(transformed);
            const auto sidecar = halionbridge::detail::makeVstPresetInfoReplacementTemporaryPath(transformedPath);

            expect(longDirectory.createDirectory());
            expect(transformedPath.native().size() < 260U);
            expect(sidecar.native().size() >= 260U);
            expect(writePreset(source, "source-state", "<MetaInfo data=\"source\"/>"));
            expect(writePreset(transformed, "transformed-state", "<MetaInfo data=\"generated\"/>"));

            auto error = std::string{};
            expect(halionbridge::detail::restoreVstPresetInfoChunk(halionbridge::detail::toStdPath(source), transformedPath, error), error);

            root.deleteRecursively();
        }

        beginTest("Info transplant supports a transformed preset beyond MAX_PATH");
        {
            auto root = makeTempRoot("long_preset");
            const auto source = root.getChildFile("source.vstpreset");
            constexpr auto destinationLength = 270;
            constexpr auto presetName = std::string_view("voice_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.vstpreset");
            const auto rootLength = static_cast<std::size_t>(root.getFullPathName().length());
            const auto directoryNameLength = destinationLength - rootLength - 1U - presetName.size() - 1U;
            const auto longDirectory = root.getChildFile(juce::String::repeatedString("x", static_cast<int>(directoryNameLength)));
            const auto transformed = longDirectory.getChildFile(halionbridge::detail::toJuceString(presetName));
            const auto transformedPath = halionbridge::detail::toStdPath(transformed);

            expect(longDirectory.createDirectory());
            expect(transformedPath.native().size() >= 260U);
            expect(writePreset(source, "source-state", "<MetaInfo data=\"source\"/>"));
            expect(writePreset(transformed, "transformed-state", "<MetaInfo data=\"generated\"/>"));

            auto error = std::string{};
            expect(halionbridge::detail::restoreVstPresetInfoChunk(halionbridge::detail::toStdPath(source), transformedPath, error), error);
            expect(halionbridge::detail::sha256MacroPageInjectionFile(transformedPath, error).has_value(), error);

            auto filesystemError = std::error_code{};
            std::filesystem::remove(halionbridge::detail::toFilesystemAccessPath(transformedPath), filesystemError);
            expect(!filesystemError, filesystemError.message());
            root.deleteRecursively();
        }
#endif

        beginTest("Manifest and completion journal enforce resumable identity");
        {
            auto root = makeTempRoot("resume");
            const auto paths =
                halionbridge::detail::makeMacroPageInjectionWorkPaths(halionbridge::detail::toStdPath(root.getChildFile("published")));
            expectEquals(paths.root.filename().string(), std::string("published.halionbridge-macro-page-work"));

            const auto manifest = halionbridge::detail::MacroPageInjectionManifest{
                halionbridge::detail::toStdPath(root.getChildFile("input")),
                halionbridge::detail::toStdPath(root.getChildFile("published")),
                halionbridge::detail::toStdPath(root.getChildFile("macro.vstpreset")),
                "donor-hash",
                true,
                {{std::filesystem::path("A.vstpreset"), "source-a"}, {std::filesystem::path("nested/B.vstpreset"), "source-b"}}};
            auto error = std::string{};
            expect(halionbridge::detail::writeMacroPageInjectionManifest(paths.manifest, manifest, error), error);
            expect(juce::File(halionbridge::detail::toJuceString(paths.manifest))
                       .loadFileAsString()
                       .contains("\"transformation_revision\": 2"));
            auto loaded = halionbridge::detail::MacroPageInjectionManifest{};
            expect(halionbridge::detail::readMacroPageInjectionManifest(paths.manifest, loaded, error), error);
            expect(halionbridge::detail::macroPageInjectionManifestsMatch(manifest, loaded, error), error);
            loaded.donorSha256 = "changed";
            expect(!halionbridge::detail::macroPageInjectionManifestsMatch(manifest, loaded, error));

            const auto legacyManifest = root.getChildFile("legacy-manifest.json");
            expect(legacyManifest.replaceWithText(
                R"({"format":"halionbridge-macro-page-injection","format_version":1,"transformation_revision":1,"recursive":true,"input_directory":"input","output_directory":"output","donor_preset":"donor.vstpreset","donor_sha256":"hash","files":[{"path":"voice.vstpreset","source_sha256":"hash"}]})"));
            expect(!halionbridge::detail::readMacroPageInjectionManifest(halionbridge::detail::toStdPath(legacyManifest), loaded, error));
            expect(error.find("revision 1") != std::string::npos);

            const auto malformedManifest = root.getChildFile("malformed-manifest.json");
            expect(malformedManifest.replaceWithText(
                R"({"format":"halionbridge-macro-page-injection","format_version":"1","transformation_revision":1,"recursive":true,"input_directory":"input","output_directory":"output","donor_preset":"donor.vstpreset","donor_sha256":"hash","files":[{"path":"voice.vstpreset","source_sha256":"hash"}]})"));
            expect(
                !halionbridge::detail::readMacroPageInjectionManifest(halionbridge::detail::toStdPath(malformedManifest), loaded, error));

            expect(halionbridge::detail::appendMacroPageInjectionCompletion(paths.completionJournal, {0, "source-a", "output-a"}, error));
            expect(halionbridge::detail::appendMacroPageInjectionCompletion(paths.completionJournal, {1, "source-b", "output-b"}, error));
            auto journal = halionbridge::detail::readMacroPageInjectionCompletions(paths.completionJournal, 2);
            expect(journal.errors.empty());
            expectEquals(static_cast<int>(journal.records.size()), 2);
            expect(journal.records.contains(1));

            expect(juce::File(halionbridge::detail::toJuceString(paths.completionJournal)).appendText("HBMPJ1\t"));
            journal = halionbridge::detail::readMacroPageInjectionCompletions(paths.completionJournal, 2);
            expect(journal.errors.empty());
            expectEquals(static_cast<int>(journal.records.size()), 2);
            root.deleteRecursively();
        }

        beginTest("SHA-256 and replaceable runtime files are deterministic");
        {
            auto root = makeTempRoot("runtime");
            const auto source = halionbridge::detail::toStdPath(root.getChildFile("source.vstpreset"));
            const auto runtime = halionbridge::detail::toStdPath(root.getChildFile("runtime.lua"));
            expect(root.getChildFile("source.vstpreset").replaceWithText("abc"));
            auto error = std::string{};
            const auto hash = halionbridge::detail::sha256MacroPageInjectionFile(source, error);
            expect(hash.has_value(), error);
            if (hash)
                expectEquals(*hash, std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
            expect(halionbridge::detail::writeMacroPageInjectionRuntimeFile(runtime, "first\n", error), error);
            expect(halionbridge::detail::writeMacroPageInjectionRuntimeFile(runtime, "second\n", error), error);
            expectEquals(root.getChildFile("runtime.lua").loadFileAsString().toStdString(), std::string("second\n"));
            const auto directoryDestination = root.getChildFile("runtime-directory");
            expect(directoryDestination.createDirectory());
            expect(!halionbridge::detail::writeMacroPageInjectionRuntimeFile(halionbridge::detail::toStdPath(directoryDestination),
                                                                             "refused\n", error));
            root.deleteRecursively();
        }

        beginTest("Bridge rejects overlapping or existing output paths before HALion starts");
        {
            auto root = makeTempRoot("preflight_paths");
            const auto input = root.getChildFile("input");
            const auto donor = root.getChildFile("donor.vstpreset");
            const auto source = input.getChildFile("source.vstpreset");
            expect(input.createDirectory());
            expect(writePreset(source, "source", "<MetaInfo/>"));
            expect(writePreset(donor, "donor", "<MetaInfo/>"));

            auto options = halionbridge::VstPresetMacroPageInjectionOptions{};
            options.inputDirectory = halionbridge::detail::toStdPath(input);
            options.donorPreset = halionbridge::detail::toStdPath(donor);
            options.outputDirectory = halionbridge::detail::toStdPath(input.getChildFile("nested-output"));
            halionbridge::Bridge bridge;
            expect(bridge.injectMacroPageDetailed(options) == halionbridge::RunResult::invalidOptions);

            const auto existingOutput = root.getChildFile("existing-output");
            expect(existingOutput.createDirectory());
            options.outputDirectory = halionbridge::detail::toStdPath(existingOutput);
            expect(bridge.injectMacroPageDetailed(options) == halionbridge::RunResult::invalidOptions);
            root.deleteRecursively();
        }

        beginTest("Staged output publishes by directory rename without merging");
        {
            auto root = makeTempRoot("publish");
            const auto output = halionbridge::detail::toStdPath(root.getChildFile("published"));
            const auto paths = halionbridge::detail::makeMacroPageInjectionWorkPaths(output);
            expect(juce::File(halionbridge::detail::toJuceString(paths.presets)).getChildFile("nested").createDirectory());
            expect(juce::File(halionbridge::detail::toJuceString(paths.presets))
                       .getChildFile("nested")
                       .getChildFile("voice.vstpreset")
                       .replaceWithText("preset"));
            auto error = std::string{};
            expect(halionbridge::detail::publishMacroPageInjectionPresets(paths, output, error), error);
            expect(root.getChildFile("published").getChildFile("nested").getChildFile("voice.vstpreset").existsAsFile());
            expect(!halionbridge::detail::publishMacroPageInjectionPresets(paths, output, error));
            expect(halionbridge::detail::cleanupMacroPageInjectionWorkDirectory(paths, output, error), error);
            root.deleteRecursively();
        }

        beginTest("Incomplete staged preset cleanup removes metadata sidecars");
        {
            auto root = makeTempRoot("sidecar_cleanup");
            const auto output = halionbridge::detail::toStdPath(root.getChildFile("published"));
            const auto paths = halionbridge::detail::makeMacroPageInjectionWorkPaths(output);
            const auto relative = std::filesystem::path("nested/voice.vstpreset");
            const auto preset = paths.presets / relative;
            const auto sidecar = halionbridge::detail::makeVstPresetInfoReplacementTemporaryPath(preset);
            expect(juce::File(halionbridge::detail::toJuceString(preset)).getParentDirectory().createDirectory());
            expect(juce::File(halionbridge::detail::toJuceString(preset)).replaceWithText("partial preset"));
            expect(juce::File(halionbridge::detail::toJuceString(sidecar)).replaceWithText("partial metadata"));
            auto error = std::string{};
            expect(halionbridge::detail::removeMacroPageInjectionStagedPreset(paths, relative, error), error);
            expect(!juce::File(halionbridge::detail::toJuceString(preset)).exists());
            expect(!juce::File(halionbridge::detail::toJuceString(sidecar)).exists());
            root.deleteRecursively();
        }
    }

  private:
    static juce::File makeTempRoot(const char* suffix)
    {
        auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getNonexistentChildFile(juce::String("halionbridge_macro_page_") + suffix, {}, false);
        root.createDirectory();
        return root;
    }

    static void appendU32(std::vector<unsigned char>& bytes, const std::uint32_t value)
    {
        for (auto index = 0U; index < 4U; ++index)
            bytes.push_back(static_cast<unsigned char>((value >> (index * 8U)) & 0xffU));
    }

    static void appendU64(std::vector<unsigned char>& bytes, const std::uint64_t value)
    {
        for (auto index = 0U; index < 8U; ++index)
            bytes.push_back(static_cast<unsigned char>((value >> (index * 8U)) & 0xffU));
    }

    static void appendText(std::vector<unsigned char>& bytes, const std::string_view text)
    {
        for (const auto character : text)
            bytes.push_back(static_cast<unsigned char>(character));
    }

    static bool writePreset(const juce::File& file, const std::string_view state, const std::string_view info)
    {
        auto bytes = std::vector<unsigned char>{};
        appendText(bytes, "VST3");
        appendU32(bytes, 1);
        appendText(bytes, "3B63D74130B34AE397AF92A9659137D5");
        appendU64(bytes, 0);
        const auto programOffset = bytes.size();
        appendText(bytes, state);
        const auto infoOffset = bytes.size();
        appendText(bytes, info);
        const auto listOffset = bytes.size();
        appendText(bytes, "List");
        appendU32(bytes, 2);
        appendText(bytes, "Prog");
        appendU64(bytes, programOffset);
        appendU64(bytes, state.size());
        appendText(bytes, "Info");
        appendU64(bytes, infoOffset);
        appendU64(bytes, info.size());
        for (auto index = 0U; index < 8U; ++index)
            bytes[40 + index] = static_cast<unsigned char>((static_cast<std::uint64_t>(listOffset) >> (index * 8U)) & 0xffU);

        auto stream = std::ofstream(halionbridge::detail::toFilesystemAccessPath(halionbridge::detail::toStdPath(file)),
                                    std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return stream.good();
    }

    static bool writePresetWithoutInfo(const juce::File& file, const std::string_view state)
    {
        auto bytes = std::vector<unsigned char>{};
        appendText(bytes, "VST3");
        appendU32(bytes, 1);
        appendText(bytes, "3B63D74130B34AE397AF92A9659137D5");
        appendU64(bytes, 0);
        const auto programOffset = bytes.size();
        appendText(bytes, state);
        const auto listOffset = bytes.size();
        appendText(bytes, "List");
        appendU32(bytes, 1);
        appendText(bytes, "Prog");
        appendU64(bytes, programOffset);
        appendU64(bytes, state.size());
        for (auto index = 0U; index < 8U; ++index)
            bytes[40 + index] = static_cast<unsigned char>((static_cast<std::uint64_t>(listOffset) >> (index * 8U)) & 0xffU);

        auto stream = std::ofstream(halionbridge::detail::toFilesystemAccessPath(halionbridge::detail::toStdPath(file)),
                                    std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return stream.good();
    }
};

MacroPageInjectionTests macroPageInjectionTests;

} // namespace
