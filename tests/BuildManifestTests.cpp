#include "BuildManifest.h"
#include "halionbridge/Bridge.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <filesystem>
#include <string_view>

namespace
{

class ScopedTestDirectory
{
  public:
    explicit ScopedTestDirectory(const std::string_view name)
        : directory(juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getNonexistentChildFile("halionbridge_" + juce::String(name.data(), name.size()), "", false))
    {
        directory.createDirectory();
    }

    ~ScopedTestDirectory()
    {
        directory.deleteRecursively();
    }

    [[nodiscard]] std::filesystem::path path() const
    {
        return directory.getFullPathName().toStdString();
    }

    juce::File directory;
};

class BuildManifestTests final : public juce::UnitTest
{
  public:
    BuildManifestTests() : juce::UnitTest("Build manifest", "halionbridge") {}

    void runTest() override
    {
        beginTest("Parses schema version 1 output directories");
        {
            const auto result =
                halionbridge::detail::parseBuildManifest(R"({"schema_version":1,"output_directories":["bank_001","nested/preset"]})");
            expect(result.succeeded());
            expectEquals(static_cast<int>(result.manifest->outputDirectories.size()), 2);
            expectEquals(result.manifest->outputDirectories[1].generic_string(), std::string("nested/preset"));
        }

        beginTest("Rejects malformed and unsafe manifests");
        {
            const auto expectRejected = [this](const std::string_view json)
            {
                const auto result = halionbridge::detail::parseBuildManifest(json);
                expect(!result.succeeded(), juce::String(json.data(), json.size()));
                expect(result.error.has_value());
            };

            expectRejected("not json");
            expectRejected(R"({"schema_version":2,"output_directories":[]})");
            expectRejected(R"({"schema_version":1,"output_directories":"preset"})");
            expectRejected(R"({"schema_version":1,"output_directories":[1]})");
            expectRejected(R"({"schema_version":1,"output_directories":["../escape"]})");
            expectRejected(R"({"schema_version":1,"output_directories":["C:/escape"]})");
            expectRejected(R"({"schema_version":1,"output_directories":["nested\\preset"]})");
            expectRejected(R"({"schema_version":1,"output_directories":["Preset","preset"]})");
        }

        beginTest("Pre-creates nested directories below the effective output root");
        {
            auto build = ScopedTestDirectory("manifest_build");
            auto output = ScopedTestDirectory("manifest_output");
            expect(build.directory.getChildFile("halionbridge_build_manifest.json")
                       .replaceWithText(R"({"schema_version":1,"output_directories":["bank_001/01_voice","single_002_voice"]})"));

            const auto error = halionbridge::detail::prepareBuildManifestOutputDirectories(build.path(), output.path());
            expect(!error.has_value());
            expect(output.directory.getChildFile("bank_001/01_voice").isDirectory());
            expect(output.directory.getChildFile("single_002_voice").isDirectory());
        }

        beginTest("Fails before plugin instantiation when an output parent is not a directory");
        {
            auto build = ScopedTestDirectory("manifest_invalid_parent_build");
            auto output = ScopedTestDirectory("manifest_invalid_parent_output");
            expect(build.directory.getChildFile("halionbridge_build_manifest.json")
                       .replaceWithText(R"({"schema_version":1,"output_directories":["blocked/preset"]})"));
            expect(output.directory.getChildFile("blocked").replaceWithText("file"));

            const auto error = halionbridge::detail::prepareBuildManifestOutputDirectories(build.path(), output.path());
            expect(error.has_value());
            expectEquals(error->code, std::string("manifest-output-directory"));
            expect(!output.directory.getChildFile("blocked/preset").exists());
        }

        beginTest("Bridge prepares manifest directories before the plugin tripwire");
        {
            auto build = ScopedTestDirectory("manifest_bridge_build");
            auto output = ScopedTestDirectory("manifest_bridge_output");
            expect(build.directory.getChildFile("halionbridge_build.lua").replaceWithText("return { \"voice.lua\" }\n"));
            expect(build.directory.getChildFile("voice.lua").replaceWithText("return {}\n"));
            expect(build.directory.getChildFile("halionbridge_build_manifest.json")
                       .replaceWithText(R"({"schema_version":1,"output_directories":["nested/voice"]})"));

            auto options = halionbridge::AppOptions{};
            options.buildDirectory = build.path();
            options.outputDirectory = output.path();
            options.pluginPathOverride = build.path() / "tripwire.vst3";
            auto bridge = halionbridge::Bridge{};
            expect(bridge.runDetailed(options) == halionbridge::RunResult::pluginLoadFailed);
            expect(output.directory.getChildFile("nested/voice").isDirectory());
        }
    }
};

BuildManifestTests buildManifestTests;

} // namespace
