#include "halionbridge/Bridge.h"
#include "CliCommand.h"
#include "PathUtils.h"
#include "PresetInspection.h"

#include <juce_core/juce_core.h>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

class VstPresetInspectionTests final : public juce::UnitTest
{
  public:
    VstPresetInspectionTests() : juce::UnitTest("VstPresetInspectionTests", "halionbridge") {}

    void runTest() override
    {
        beginTest("Inspection CLI accepts one preset and explicit JSON output");
        {
            auto root = makeTempRoot("cli_file");
            const auto input = root.getChildFile("Voice.vstpreset");
            const auto output = root.getChildFile("report.json");
            expect(input.replaceWithText("preset"));

            const auto args = std::vector<std::string>{"--input",
                                                       input.getFullPathName().toStdString(),
                                                       "--output-json",
                                                       output.getFullPathName().toStdString(),
                                                       "--overwrite",
                                                       "--recursive",
                                                       "--timeout-seconds",
                                                       "15",
                                                       "--gui",
                                                       "--nokill",
                                                       "--force-scan"};
            const auto parsed = halionbridge::detail::parseVstPresetInspectionOptionsDetailed(args);
            expect(parsed.options.has_value());
            if (parsed.options)
            {
                expect(parsed.options->inputPath == halionbridge::detail::toStdPath(input));
                expect(parsed.options->outputJson == halionbridge::detail::toStdPath(output));
                expect(parsed.options->overwrite);
                expect(parsed.options->recursive);
                expectEquals(parsed.options->timeoutSeconds, 15);
                expect(parsed.options->showGui);
                expect(parsed.options->noKill);
                expect(parsed.options->forceScan);
            }

            root.deleteRecursively();
        }

        beginTest("Inspection CLI rejects missing and invalid paths");
        {
            auto root = makeTempRoot("cli_invalid");
            const auto input = root.getChildFile("not-a-preset.txt");
            const auto output = root.getChildFile("report.txt");
            expect(input.replaceWithText("text"));

            expect(!halionbridge::detail::parseVstPresetInspectionOptionsDetailed({}).options.has_value());
            const auto invalidExtensionArgs = std::vector<std::string>{"--input", input.getFullPathName().toStdString(), "--output-json",
                                                                       root.getChildFile("report.json").getFullPathName().toStdString()};
            expect(!halionbridge::detail::parseVstPresetInspectionOptionsDetailed(invalidExtensionArgs).options.has_value());

            const auto missingInputArgs =
                std::vector<std::string>{"--input", root.getChildFile("missing.vstpreset").getFullPathName().toStdString(), "--output-json",
                                         root.getChildFile("report.json").getFullPathName().toStdString()};
            expect(!halionbridge::detail::parseVstPresetInspectionOptionsDetailed(missingInputArgs).options.has_value());

            const auto invalidOutputArgs = std::vector<std::string>{"--input", input.getFullPathName().toStdString(), "--output-json",
                                                                    output.getFullPathName().toStdString()};
            expect(!halionbridge::detail::parseVstPresetInspectionOptionsDetailed(invalidOutputArgs).options.has_value());

            root.deleteRecursively();
        }

        beginTest("Inspection collection is deterministic and recursion is explicit");
        {
            auto root = makeTempRoot("collection");
            const auto nested = root.getChildFile("nested");
            expect(nested.createDirectory());
            expect(root.getChildFile("Zulu.vstpreset").replaceWithText("z"));
            expect(root.getChildFile("alpha.VSTPRESET").replaceWithText("a"));
            expect(root.getChildFile("ignored.txt").replaceWithText("x"));
            expect(nested.getChildFile("Beta.vstpreset").replaceWithText("b"));

            const auto flat = halionbridge::detail::collectVstPresetsForInspection(halionbridge::detail::toStdPath(root), false);
            expect(flat.errors.empty());
            expectEquals(static_cast<int>(flat.files.size()), 2);
            if (flat.files.size() == 2)
            {
                expectEquals(flat.files[0].relativePath.generic_string(), std::string("alpha.VSTPRESET"));
                expectEquals(flat.files[1].relativePath.generic_string(), std::string("Zulu.vstpreset"));
            }

            const auto recursive = halionbridge::detail::collectVstPresetsForInspection(halionbridge::detail::toStdPath(root), true);
            expect(recursive.errors.empty());
            expectEquals(static_cast<int>(recursive.files.size()), 3);
            if (recursive.files.size() == 3)
                expectEquals(recursive.files[1].relativePath.generic_string(), std::string("nested/Beta.vstpreset"));

            const auto single = halionbridge::detail::collectVstPresetsForInspection(
                halionbridge::detail::toStdPath(root.getChildFile("Zulu.vstpreset")), true);
            expect(single.errors.empty());
            expectEquals(static_cast<int>(single.files.size()), 1);
            if (!single.files.empty())
                expectEquals(single.files.front().relativePath.generic_string(), std::string("Zulu.vstpreset"));

            root.deleteRecursively();
        }

        beginTest("Inspection runtime configuration preserves absolute and relative paths");
        {
            const auto config = halionbridge::detail::VstPresetInspectionRuntimeConfig{
                std::filesystem::path("C:/Users/Test/Documents/Steinberg/halionbridge-inspect-unit"),
                std::filesystem::path("C:/Users/Test/Documents/Steinberg/halionbridge-inspect-unit/report.json"),
                {{std::filesystem::path("C:/Presets/One.vstpreset"), std::filesystem::path("One.vstpreset")},
                 {std::filesystem::path("C:/Presets/Nested/Two.vstpreset"), std::filesystem::path("Nested/Two.vstpreset")}}};

            const auto text = halionbridge::detail::createVstPresetInspectionRuntimeModuleText(config);
            expect(text.find("HALIONBRIDGE_PRESET_INSPECTION_ROOT") != std::string::npos);
            expect(text.find("HALIONBRIDGE_PRESET_INSPECTION_REPORT") != std::string::npos);
            expect(text.find("C:/Presets/One.vstpreset") != std::string::npos);
            expect(text.find("Nested/Two.vstpreset") != std::string::npos);
            expect(text.find("halionbridge_preset_inspect") != std::string::npos);
        }

        beginTest("Inspection report validation enforces its versioned contract");
        {
            const auto valid = std::string(
                R"({"format":"halionbridge-vstpreset-inspection","format_version":1,"presets":[{"path":"One.vstpreset","ok":true,"errors":[],"root":{}}],"summary":{"total":1,"inspected":1,"failed":0}})");
            auto summary = halionbridge::detail::VstPresetInspectionReportSummary{};
            auto error = std::string();
            expect(halionbridge::detail::validateVstPresetInspectionReport(valid, 1, summary, error));
            expect(error.empty());
            expectEquals(summary.total, 1);
            expectEquals(summary.inspected, 1);
            expectEquals(summary.failed, 0);

            const auto wrongVersion = std::string(
                R"({"format":"halionbridge-vstpreset-inspection","format_version":2,"presets":[],"summary":{"total":0,"inspected":0,"failed":0}})");
            expect(!halionbridge::detail::validateVstPresetInspectionReport(wrongVersion, 0, summary, error));
            expect(!error.empty());

            const auto inconsistentRecords = std::string(
                R"({"format":"halionbridge-vstpreset-inspection","format_version":1,"presets":[{"path":"One.vstpreset","ok":true,"errors":[]}],"summary":{"total":1,"inspected":0,"failed":1}})");
            expect(!halionbridge::detail::validateVstPresetInspectionReport(inconsistentRecords, 1, summary, error));
            expect(!error.empty());
        }

        beginTest("Inspection report publication is explicit and guarded");
        {
            auto root = makeTempRoot("publication");
            const auto source = root.getChildFile("runtime").getChildFile("report.json");
            const auto output = root.getChildFile("output").getChildFile("report.json");
            const auto unrelatedSidecar = root.getChildFile("output").getChildFile("report.json.halionbridge.tmp");
            expect(source.getParentDirectory().createDirectory());
            expect(output.getParentDirectory().createDirectory());
            expect(source.replaceWithText("first"));
            expect(unrelatedSidecar.replaceWithText("unrelated"));

            auto error = std::string();
            auto readText = std::string();
            expect(halionbridge::detail::readVstPresetInspectionReport(halionbridge::detail::toStdPath(source), readText, error));
            expectEquals(readText, std::string("first"));
            expect(halionbridge::detail::publishVstPresetInspectionReport(halionbridge::detail::toStdPath(source),
                                                                          halionbridge::detail::toStdPath(output), false, error));
            expectEquals(output.loadFileAsString().toStdString(), std::string("first"));

            expect(source.replaceWithText("second"));
            expect(!halionbridge::detail::publishVstPresetInspectionReport(halionbridge::detail::toStdPath(source),
                                                                           halionbridge::detail::toStdPath(output), false, error));
            expectEquals(output.loadFileAsString().toStdString(), std::string("first"));
            expect(halionbridge::detail::publishVstPresetInspectionReport(halionbridge::detail::toStdPath(source),
                                                                          halionbridge::detail::toStdPath(output), true, error));
            expectEquals(output.loadFileAsString().toStdString(), std::string("second"));
            expectEquals(unrelatedSidecar.loadFileAsString().toStdString(), std::string("unrelated"));

            root.deleteRecursively();
        }

        beginTest("Inspection cleanup only removes guarded temporary roots");
        {
            auto root = makeTempRoot("cleanup");
            const auto safe = root.getChildFile("halionbridge-inspect-unit");
            const auto unsafe = root.getChildFile("unrelated");
            expect(safe.createDirectory());
            expect(safe.getChildFile("report.json").replaceWithText("{}"));
            expect(unsafe.createDirectory());

            auto error = std::string();
            expect(halionbridge::detail::cleanupVstPresetInspectionDirectory(halionbridge::detail::toStdPath(safe),
                                                                             halionbridge::detail::toStdPath(root), error));
            expect(!safe.exists());
            expect(!halionbridge::detail::cleanupVstPresetInspectionDirectory(halionbridge::detail::toStdPath(unsafe),
                                                                              halionbridge::detail::toStdPath(root), error));
            expect(unsafe.isDirectory());

            root.deleteRecursively();
        }

        beginTest("Bridge rejects an existing inspection report before loading HALion");
        {
            auto root = makeTempRoot("bridge_preflight");
            const auto input = root.getChildFile("Voice.vstpreset");
            const auto output = root.getChildFile("report.json");
            expect(input.replaceWithText("preset"));
            expect(output.replaceWithText("existing"));

            auto options = halionbridge::VstPresetInspectionOptions{};
            options.inputPath = halionbridge::detail::toStdPath(input);
            options.outputJson = halionbridge::detail::toStdPath(output);

            auto bridge = halionbridge::Bridge{};
            expect(bridge.inspectVstPresetsDetailed(options) == halionbridge::RunResult::invalidOptions);

            root.deleteRecursively();
        }
    }

  private:
    static juce::File makeTempRoot(const char* suffix)
    {
        auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getNonexistentChildFile(juce::String("halionbridge_inspection_") + suffix, {}, false);
        root.deleteRecursively();
        root.createDirectory();
        return root;
    }
};

VstPresetInspectionTests vstPresetInspectionTests;

} // namespace
