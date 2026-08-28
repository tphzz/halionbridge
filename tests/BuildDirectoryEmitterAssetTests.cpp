#include "halionbridge_converters/BuildDirectoryEmitter.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

namespace
{

using halionbridge::converters::BuildDirectoryRequest;
using halionbridge::converters::GeneratedBuildFile;
using halionbridge::converters::GeneratedLuaScript;

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

std::vector<std::byte> bytes(const std::initializer_list<unsigned int> values)
{
    auto result = std::vector<std::byte>{};
    result.reserve(values.size());
    for (const auto value : values)
        result.push_back(static_cast<std::byte>(value));
    return result;
}

bool hasDiagnostic(const halionbridge::converters::BuildDirectoryResult& result, const std::string_view code)
{
    return std::ranges::any_of(result.diagnostics, [code](const auto& diagnostic) { return diagnostic.code == code; });
}

class BuildDirectoryEmitterAssetTests final : public juce::UnitTest
{
  public:
    BuildDirectoryEmitterAssetTests() : juce::UnitTest("Converter emitter assets", "halionbridge") {}

    void runTest() override
    {
        beginTest("Writes nested binary artifacts through the transactional emitter");
        {
            auto temp = ScopedTestDirectory("emitter_assets");
            const auto expected = bytes({0x00, 0x7f, 0x80, 0xff});
            auto request = BuildDirectoryRequest{temp.path(), false, {GeneratedLuaScript{"voice.lua", "voice.lua", "return {}\n"}}};
            request.files.push_back(GeneratedBuildFile{".halionbridge/dx7/templates/dx7_01.vstpreset", expected});

            const auto result = halionbridge::converters::writeBuildDirectory(request);
            expect(result.succeeded);
            expectEquals(static_cast<int>(result.generatedFiles.size()), 2);

            const auto target = temp.directory.getChildFile(".halionbridge/dx7/templates/dx7_01.vstpreset");
            expect(target.existsAsFile());
            auto loaded = juce::MemoryBlock{};
            expect(target.loadFileAsData(loaded));
            expectEquals(static_cast<int>(loaded.getSize()), static_cast<int>(expected.size()));
            if (loaded.getSize() == expected.size())
                expect(std::equal(expected.begin(), expected.end(), static_cast<const std::byte*>(loaded.getData())));
        }

        beginTest("Rejects unsafe nested artifact paths before writing");
        {
            auto temp = ScopedTestDirectory("emitter_unsafe_asset");
            auto request = BuildDirectoryRequest{temp.path(), false, {GeneratedLuaScript{"voice.lua", "voice.lua", "return {}\n"}}};
            request.files.push_back(GeneratedBuildFile{"../escape.vstpreset", bytes({1})});

            const auto result = halionbridge::converters::writeBuildDirectory(request);
            expect(!result.succeeded);
            expect(hasDiagnostic(result, "invalid-generated-path"));
            expect(!temp.directory.getChildFile("halionbridge_build.lua").existsAsFile());
        }

        beginTest("Rejects case-insensitive collisions across Lua and binary artifacts");
        {
            auto temp = ScopedTestDirectory("emitter_asset_collision");
            auto request = BuildDirectoryRequest{temp.path(), false, {GeneratedLuaScript{"voice.lua", "Voice.lua", "return {}\n"}}};
            request.files.push_back(GeneratedBuildFile{"voice.lua", bytes({1})});

            const auto result = halionbridge::converters::writeBuildDirectory(request);
            expect(!result.succeeded);
            expect(hasDiagnostic(result, "duplicate-generated-path"));
            expect(!temp.directory.getChildFile("halionbridge_build.lua").existsAsFile());
        }
    }
};

BuildDirectoryEmitterAssetTests buildDirectoryEmitterAssetTests;

} // namespace
