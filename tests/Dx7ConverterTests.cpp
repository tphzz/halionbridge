#include "halionbridge_converters/dx7/Dx7Converter.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

std::array<std::uint8_t, 155> singleVoicePayload(const std::string_view name, const std::uint8_t algorithm,
                                                 const std::uint8_t lfoWaveform = 4)
{
    auto payload = std::array<std::uint8_t, 155>{};
    for (auto serializedOperator = std::size_t{0}; serializedOperator < 6; ++serializedOperator)
    {
        const auto offset = serializedOperator * 21;
        payload[offset + 0] = 90;
        payload[offset + 1] = 70;
        payload[offset + 2] = 50;
        payload[offset + 3] = 30;
        payload[offset + 4] = 99;
        payload[offset + 5] = 80;
        payload[offset + 6] = 60;
        payload[offset + 7] = 0;
        payload[offset + 8] = 39;
        payload[offset + 9] = 12;
        payload[offset + 10] = 34;
        payload[offset + 11] = static_cast<std::uint8_t>(serializedOperator % 4);
        payload[offset + 12] = static_cast<std::uint8_t>((serializedOperator + 1) % 4);
        payload[offset + 13] = 3;
        payload[offset + 14] = 2;
        payload[offset + 15] = 5;
        payload[offset + 16] = static_cast<std::uint8_t>(90 - serializedOperator);
        payload[offset + 17] = serializedOperator == 0 ? 1 : 0;
        payload[offset + 18] = static_cast<std::uint8_t>(serializedOperator + 1);
        payload[offset + 19] = 25;
        payload[offset + 20] = 9;
    }

    payload[126] = 80;
    payload[127] = 60;
    payload[128] = 40;
    payload[129] = 20;
    payload[130] = 50;
    payload[131] = 70;
    payload[132] = 40;
    payload[133] = 50;
    payload[134] = algorithm;
    payload[135] = 6;
    payload[136] = 1;
    payload[137] = 42;
    payload[138] = 55;
    payload[139] = 66;
    payload[140] = 77;
    payload[141] = 1;
    payload[142] = lfoWaveform;
    payload[143] = 5;
    payload[144] = 12;
    std::fill(payload.begin() + 145, payload.end(), static_cast<std::uint8_t>(' '));
    std::copy_n(name.begin(), std::min(name.size(), std::size_t{10}), payload.begin() + 145);
    return payload;
}

std::vector<std::uint8_t> framedSingle(const std::string_view name, const std::uint8_t algorithm, const std::uint8_t lfoWaveform = 4)
{
    const auto payload = singleVoicePayload(name, algorithm, lfoWaveform);
    auto bytes = std::vector<std::uint8_t>{0xf0, 0x43, 0x00, 0x00, 0x01, 0x1b};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    auto sum = 0U;
    for (const auto value : payload)
        sum += value;
    bytes.push_back(static_cast<std::uint8_t>((128U - (sum & 0x7fU)) & 0x7fU));
    bytes.push_back(0xf7);
    return bytes;
}

bool writeBytes(const juce::File& file, const std::span<const std::uint8_t> bytes)
{
    return file.replaceWithData(bytes.data(), bytes.size());
}

bool hasDiagnostic(const halionbridge::converters::dx7::ConversionResult& result, const std::string_view code)
{
    return std::ranges::any_of(result.diagnostics, [code](const auto& diagnostic) { return diagnostic.code == code; });
}

class Dx7ConverterTests final : public juce::UnitTest
{
  public:
    Dx7ConverterTests() : juce::UnitTest("DX7 converter", "halionbridge") {}

    void runTest() override
    {
        beginTest("Generates one inspectable entrypoint and embeds all algorithm templates");
        {
            auto source = ScopedTestDirectory("dx7_single_source");
            auto output = ScopedTestDirectory("dx7_single_output");
            const auto sourceFile = source.directory.getChildFile("voice.syx");
            expect(writeBytes(sourceFile, framedSingle("BASS 1", 4)));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = sourceFile.getFullPathName().toStdString();
            options.outputDirectory = output.path();
            const auto result = halionbridge::converters::dx7::convertSource(options);

            expect(result.succeeded);
            expectEquals(result.syxFilesConverted, 1);
            expectEquals(result.messagesConverted, 1);
            expectEquals(result.voicesConverted, 1);
            expectEquals(static_cast<int>(result.generatedLuaFiles.size()), 2);
            expectEquals(static_cast<int>(result.generatedFiles.size()), 35);

            auto luaFiles = juce::Array<juce::File>{};
            output.directory.findChildFiles(luaFiles, juce::File::findFiles, false, "*.lua");
            expectEquals(luaFiles.size(), 3);
            auto entrypoint = juce::File{};
            for (const auto& file : luaFiles)
            {
                if (file.getFileName() != "halionbridge_build.lua" && file.getFileName() != "halionbridge-dx7.lua")
                    entrypoint = file;
            }
            expect(entrypoint.existsAsFile());
            const auto lua = entrypoint.loadFileAsString();
            expect(lua.startsWith("-- Generated by halionbridge from Yamaha DX7 SysEx.\n"));
            expect(!lua.contains("voice.syx"));
            expect(lua.contains("algorithm = 5"));
            expect(lua.contains("template_file = \".halionbridge/dx7/templates/dx7_05.vstpreset\""));
            expect(lua.contains("output_file = \"voice/single_001_BASS_1.vstpreset\""));
            expect(lua.contains("amplitude_modulation_sensitivity = 2"));

            const auto templateFile = output.directory.getChildFile(".halionbridge/dx7/templates/dx7_05.vstpreset");
            const auto sourceTemplate = juce::File::getCurrentWorkingDirectory().getChildFile("resources/dx7/dx7_05.vstpreset");
            auto emittedBytes = juce::MemoryBlock{};
            auto sourceBytes = juce::MemoryBlock{};
            expect(templateFile.loadFileAsData(emittedBytes));
            expect(sourceTemplate.loadFileAsData(sourceBytes));
            expect(emittedBytes == sourceBytes);
            expect(output.directory.getChildFile("halionbridge_build_manifest.json").loadFileAsString().contains("voice"));
            const auto helper = output.directory.getChildFile("halionbridge-dx7.lua").loadFileAsString();
            expect(helper.contains("Required HALion \" .. element_label .. \" name assignment is unavailable"));
            expect(helper.contains("FM-Oscillator.EmulationMode"));
            expect(helper.contains("set_required(zone, \"Pitch.EnvAmount\", 0)"),
                   "Generated DX7 Lua must neutralize the algorithm template pitch-envelope amount");
            expect(helper.contains("local prefix = \"FM-Operator \""));
            expect(helper.contains("local key_level_curve = { 2, 4, 3, 1 }"));
            expect(helper.contains("local targetOperatorIndex = 7 - sourceOperatorIndex"));
            expect(helper.contains("apply_operator(zone, targetOperatorIndex, operator, voice.oscillator_sync)"));
            expect(helper.contains("fm_level_destination[targetOperatorIndex]"));
            expect(helper.contains("local lfo_low_frequency_hz = {"));
            expect(helper.contains("4.17, 4.30, 4.48, 4.60, 4.79, 4.92"));
            expect(helper.contains("local lfo_waveform = { 1, 2, 2, 3, 0, 6 }"));
            expect(helper.contains("local lfo_shape = { 0, 0, 100, 50, 0, 0 }"));
            expect(helper.contains("local lfo_initial_phase = { 90, 0, 0, 180, 180, 180 }"));
            expect(helper.contains("local amplitude_sensitivity_depth = { 0, -13, -26, -53 }"));
            expect(helper.contains("local pitch_sensitivity_depth = { 0, -2.55646824837, -5.33722400665, -8.96085739136,"));
            expect(helper.contains("local pitch_sensitivity_offset = { 0, 5.5, 2.75, 0, 0, 0.6, 0.35, 0.2 }"));
            expect(helper.contains("if speed < 32 then return lfo_low_frequency_hz[speed + 1] end"));
            expect(helper.contains("if speed >= 64 then rawRate = speed - 53 end"));
            expect(helper.contains("if waveform == 5 then return rawRate / 2 end"));
            expect(helper.contains("return math.min(rawRate, 30)"));
            expect(helper.contains("if delay <= 43 then return 30 * 2 ^ (3 * delay / 50) end"));
            expect(helper.contains("if delay <= 50 then return 320 end"));
            expect(helper.contains("for rowNumber = 1, 32 do"));
            expect(helper.contains("configure_modulation_row(zone, rowNumber, ModulationSource.unassigned, 0,"));
            expect(helper.contains("row:setParameter(\"Source1.Minimum\", source_minimum)"),
                   "Generated DX7 Lua must persist modulation-source minimum values");
            expect(helper.contains("row:setParameter(\"Source1.Maximum\", source_maximum)"),
                   "Generated DX7 Lua must persist modulation-source maximum values");
            expect(helper.contains("ModulationDestination.bus1, amplitudeDepth * 100 / 99,\n"
                                   "                                           0, 100, 0)"),
                   "The native FMLab amplitude-bus input uses an inverted source range");
            expect(helper.contains("ModulationDestination.unassigned"));
            expect(helper.contains("ModulationDestination.bus1"));
            expect(helper.contains("ModulationDestination.bus2"));
            expect(helper.contains("ModulationSource.bus1"));
            expect(helper.contains("ModulationSource.bus2"));
            expect(helper.contains("if amplitudeDepth > 0 then"));
            expect(helper.contains("pitch_sensitivity_offset[pitchSensitivity + 1]"));
            expect(helper.contains("fm_pitch_destination[targetOperatorIndex]"));
            expect(!helper.contains("ModulationDestination.pitch"));
            expect(helper.contains("ModulationDestination.fmOp6Level"));
            expect(helper.contains("ModulationDestination.fmOp6Pitch"));
        }

        beginTest("Blank names and portable path collisions are deterministic");
        {
            auto source = ScopedTestDirectory("dx7_name_source");
            auto output = ScopedTestDirectory("dx7_name_output");
            expect(writeBytes(source.directory.getChildFile("A#.syx"), framedSingle("", 0)));
            expect(writeBytes(source.directory.getChildFile("A^.syx"), framedSingle("", 0)));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = source.path();
            options.outputDirectory = output.path();
            const auto result = halionbridge::converters::dx7::convertSource(options);

            expect(result.succeeded);
            expectEquals(result.voicesConverted, 2);
            const auto firstLua = output.directory.getChildFile("000001_Unnamed.lua").loadFileAsString();
            const auto secondLua = output.directory.getChildFile("000002_Unnamed.lua").loadFileAsString();
            expect(firstLua.contains("name = \"Unnamed\""));
            expect(firstLua.contains("output_file = \"A/single_001_unnamed.vstpreset\""));
            expect(secondLua.contains("output_file = \"A/single_001_unnamed_002.vstpreset\""));
        }

        beginTest("A structural error aborts the complete batch before writing");
        {
            auto source = ScopedTestDirectory("dx7_abort_source");
            auto output = ScopedTestDirectory("dx7_abort_output");
            expect(writeBytes(source.directory.getChildFile("good.syx"), framedSingle("GOOD", 0)));
            auto bad = framedSingle("BAD", 0);
            bad[bad.size() - 2] ^= 1;
            expect(writeBytes(source.directory.getChildFile("bad.syx"), bad));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = source.path();
            options.outputDirectory = output.path();
            const auto result = halionbridge::converters::dx7::convertSource(options);
            expect(!result.succeeded);
            expect(hasDiagnostic(result, "checksum"));
            expect(!output.directory.getChildFile("halionbridge_build.lua").exists());
        }

        beginTest("Strict parameter mode rejects values normalized by default");
        {
            auto source = ScopedTestDirectory("dx7_strict_source");
            auto lenientOutput = ScopedTestDirectory("dx7_lenient_output");
            auto strictOutput = ScopedTestDirectory("dx7_strict_output");
            const auto sourceFile = source.directory.getChildFile("wave.syx");
            expect(writeBytes(sourceFile, framedSingle("WAVE", 0, 7)));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = sourceFile.getFullPathName().toStdString();
            options.outputDirectory = lenientOutput.path();
            auto result = halionbridge::converters::dx7::convertSource(options);
            expect(result.succeeded);
            expect(hasDiagnostic(result, "parameter-range"));

            options.outputDirectory = strictOutput.path();
            options.strictParameters = true;
            result = halionbridge::converters::dx7::convertSource(options);
            expect(!result.succeeded);
            expect(hasDiagnostic(result, "parameter-range"));
            expect(!strictOutput.directory.getChildFile("halionbridge_build.lua").exists());
        }

        beginTest("Recursive discovery is explicit and case-insensitive");
        {
            auto source = ScopedTestDirectory("dx7_recursive_source");
            auto flatOutput = ScopedTestDirectory("dx7_flat_output");
            auto recursiveOutput = ScopedTestDirectory("dx7_recursive_output");
            const auto nested = source.directory.getChildFile("nested");
            expect(nested.createDirectory());
            expect(writeBytes(source.directory.getChildFile("top.SYX"), framedSingle("TOP", 0)));
            expect(writeBytes(nested.getChildFile("deep.syx"), framedSingle("DEEP", 1)));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = source.path();
            options.outputDirectory = flatOutput.path();
            auto result = halionbridge::converters::dx7::convertSource(options);
            expect(result.succeeded);
            expectEquals(result.voicesConverted, 1);

            options.outputDirectory = recursiveOutput.path();
            options.recursive = true;
            result = halionbridge::converters::dx7::convertSource(options);
            expect(result.succeeded);
            expectEquals(result.voicesConverted, 2);
            expect(recursiveOutput.directory.getChildFile("halionbridge_build_manifest.json").loadFileAsString().contains("nested/deep"));
        }
    }
};

Dx7ConverterTests dx7ConverterTests;

} // namespace
