#include "halionbridge_converters/dx7/Dx7Converter.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr auto nativePitchEnvelopeLevelOffsets = std::array<double, 100>{
    -55.254833221435575,
    -48.054628655322425,
    -41.99999942862066,
    -36.90868301783964,
    -32.62741851806642,
    -29.027315019157427,
    -26.000001358023848,
    -23.454344806853452,
    -21.313709259033203,
    -19.513658390222872,
    -18.000000529675617,
    -16.727171651972412,
    -15.6568546295166,
    -14.756828984410161,
    -14.00000054209181,
    -13.363585638756716,
    -12.828427314758299,
    -12.378413530010503,
    -11.9999992946353,
    -11.619999973282573,
    -11.250000000000002,
    -10.870000049471857,
    -10.500000044703485,
    -10.12000009417534,
    -9.75,
    -9.370000138878822,
    -9.000000044703484,
    -8.620000183582306,
    -8.25,
    -7.869999870657921,
    -7.500000223517418,
    -7.120000094175339,
    -6.750000000000002,
    -6.370000049471857,
    -6.000000044703485,
    -5.6200000941753405,
    -5.249999999999998,
    -4.869999960064886,
    -4.500000044703482,
    -4.12000000476837,
    -3.749999999999999,
    -3.3700000494718543,
    -3.0000000447034827,
    -2.6200000941753383,
    -2.2500000000000004,
    -1.8700000494718556,
    -1.5000000447034838,
    -1.1200000271201136,
    -0.7500000000000011,
    -0.37000000476837214,
    0.0,
    0.37000000476837214,
    0.7500000078729128,
    1.1199999968954584,
    1.5000000157458255,
    1.8700000047683711,
    2.249999855047519,
    2.6199998669206925,
    2.999999873685907,
    3.3699998855590807,
    3.749999963734184,
    4.119999950867169,
    4.499999898426098,
    4.869999885559083,
    5.249999972437761,
    5.620000052217219,
    5.999999805779621,
    6.369999885559079,
    6.749999847605241,
    7.120000006733736,
    7.499999726430591,
    7.869999885559086,
    8.249999682277345,
    8.620000168304554,
    8.999999958027384,
    9.369999885559078,
    9.750000020732218,
    10.119999634771036,
    10.499999623617784,
    10.869999885559084,
    11.2500001409935,
    11.619999803942157,
    11.999999756893878,
    12.378414154052727,
    12.828427610819741,
    13.36358509922638,
    13.999999320265484,
    14.756828308105465,
    15.656854198884494,
    16.727170939516316,
    17.999999306297457,
    19.513656616210927,
    21.31370953229544,
    23.45434308513223,
    26.000000830597866,
    29.02731513977052,
    32.6274193188931,
    36.90868603353353,
    42.00000240384908,
    48.05463027954103,
};

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

std::vector<std::uint8_t> rawBank(const std::string_view namePrefix)
{
    constexpr auto packedVoiceSize = std::size_t{128};
    constexpr auto bankVoiceCount = std::size_t{32};
    auto bytes = std::vector<std::uint8_t>(packedVoiceSize * bankVoiceCount, 0);
    for (auto voiceIndex = std::size_t{0}; voiceIndex < bankVoiceCount; ++voiceIndex)
    {
        const auto name = std::string{namePrefix} + "_" + (voiceIndex < 9 ? "0" : "") + std::to_string(voiceIndex + 1);
        const auto nameOffset = (voiceIndex * packedVoiceSize) + 118;
        std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(nameOffset), 10, static_cast<std::uint8_t>(' '));
        std::copy_n(name.begin(), std::min(name.size(), std::size_t{10}), bytes.begin() + static_cast<std::ptrdiff_t>(nameOffset));
    }
    return bytes;
}

std::vector<std::uint8_t> framedBank(const std::string_view namePrefix)
{
    const auto payload = rawBank(namePrefix);
    auto bytes = std::vector<std::uint8_t>{0xf0, 0x43, 0x00, 0x09, 0x20, 0x00};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    auto sum = std::uint32_t{0};
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

std::vector<std::uint8_t> framedUnsupportedMessage()
{
    constexpr auto payload = std::array<std::uint8_t, 3>{1, 2, 3};
    auto bytes = std::vector<std::uint8_t>{0xf0, 0x43, 0x00, 0x7e, 0x00, static_cast<std::uint8_t>(payload.size())};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    auto sum = 0U;
    for (const auto value : payload)
        sum += value;
    bytes.push_back(static_cast<std::uint8_t>((128U - (sum & 0x7fU)) & 0x7fU));
    bytes.push_back(0xf7);
    return bytes;
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
            expectEquals(static_cast<int>(result.generatedFiles.size()), 36);

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
            expect(lua.contains("preset_type = \"program\""));
            expect(lua.contains("preset_target = \"halion\""));
            expect(lua.contains("amplitude_modulation_sensitivity = 2"));

            const auto templateFile = output.directory.getChildFile(".halionbridge/dx7/templates/dx7_05.vstpreset");
            const auto sourceTemplate =
                juce::File::getCurrentWorkingDirectory().getChildFile("resources/dx7/halion-sonic/dx7_05.vstpreset");
            auto emittedBytes = juce::MemoryBlock{};
            auto sourceBytes = juce::MemoryBlock{};
            expect(templateFile.loadFileAsData(emittedBytes));
            expect(sourceTemplate.loadFileAsData(sourceBytes));
            expect(emittedBytes == sourceBytes);
            expect(output.directory.getChildFile("halionbridge_build_manifest.json").loadFileAsString().contains("voice"));
            const auto report = output.directory.getChildFile("halionbridge_dx7_conversion_report.json");
            expect(report.existsAsFile());
            expect(report.loadFileAsString().contains("\"schema_version\": 1"));
            expect(report.loadFileAsString().contains("\"preset_type\": \"program\""));
            expect(report.loadFileAsString().contains("\"preset_target\": \"halion\""));
            auto reportRoot = juce::var{};
            expect(juce::JSON::parse(report.loadFileAsString(), reportRoot).wasOk());
            expect(reportRoot.isObject());
            const auto helper = output.directory.getChildFile("halionbridge-dx7.lua").loadFileAsString();
            expect(helper.contains("Required HALion \" .. element_label .. \" name assignment is unavailable"));
            expect(helper.contains("FM-Oscillator.EmulationMode"));
            expect(helper.contains("set_required(preset, \"InheritVelocitySettings\", false, \"program or layer\")"),
                   "Generated DX7 presets must use local velocity settings instead of inherited defaults");
            expect(helper.contains("set_required(preset, \"VelocityToLevelCurve\", 2, \"program or layer\")"),
                   "Generated DX7 presets must select HALion's Squared Inverse Main velocity curve");
            expect(helper.contains("Could not set required HALion \" .. element_label .. \" parameter"),
                   "Required Program/Layer assignment failures must identify the target element");
            expect(helper.contains("local preset_plugin_code = { halion = \"H7\", [\"halion-sonic\"] = \"HS\" }"));
            expect(helper.contains("ctx.save_preset(outputPath, preset, pluginCode, attributes)"));
            const auto pitchTableMarker = juce::String{"local pitch_envelope_level_offset = {"};
            const auto pitchTableStart = helper.indexOf(pitchTableMarker);
            expect(pitchTableStart >= 0, "Generated DX7 Lua must contain the native 100-entry pitch-envelope level table");
            if (pitchTableStart >= 0)
            {
                const auto valuesStart = pitchTableStart + pitchTableMarker.length();
                const auto valuesEnd = helper.indexOf(valuesStart, "}");
                expect(valuesEnd > valuesStart);
                auto tokens = juce::StringArray{};
                tokens.addTokens(helper.substring(valuesStart, valuesEnd), ",", "");
                tokens.trim();
                tokens.removeEmptyStrings();
                expectEquals(tokens.size(), static_cast<int>(nativePitchEnvelopeLevelOffsets.size()));
                if (tokens.size() == static_cast<int>(nativePitchEnvelopeLevelOffsets.size()))
                {
                    for (auto index = std::size_t{0}; index < nativePitchEnvelopeLevelOffsets.size(); ++index)
                    {
                        const auto actual = tokens[static_cast<int>(index)].getDoubleValue();
                        expect(std::abs(actual - nativePitchEnvelopeLevelOffsets[index]) <= 1.0e-12,
                               "Generated DX7 Lua pitch-envelope level table differs at source level " +
                                   juce::String{static_cast<int>(index)});
                    }
                }
            }
            expect(helper.contains("local neutral_durations = { 0, 0.1, 0.25, 0.2 }"));
            expect(helper.contains("set_required(zone, \"Pitch Env.SustainIndex\", 3)"),
                   "Neutral DX7 pitch envelopes must preserve FMLab's four-point default form");
            expect(helper.contains(
                       "return math.min(30, math.abs(target_offset - start_offset) * 0.0075 * 2 ^ ((99 - clamp(rate, 0, 99)) / 18))"),
                   "DX7 pitch-envelope segments must stay within HALion's 30-second point-duration ceiling");
            expect(helper.contains("set_required(zone, \"Pitch.EnvAmount\", amount)"),
                   "Non-neutral DX7 pitch envelopes must use the native maximum absolute offset");
            expect(helper.contains("set_required(zone, \"Pitch Env.SustainIndex\", 4)"));
            expect(!helper.contains("0.001 + normalized * normalized * 21.3"));
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

        beginTest("Generates explicit layer preset build entries");
        {
            auto source = ScopedTestDirectory("dx7_layer_source");
            auto output = ScopedTestDirectory("dx7_layer_output");
            const auto sourceFile = source.directory.getChildFile("voice.syx");
            expect(writeBytes(sourceFile, framedSingle("LAYER", 0)));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = sourceFile.getFullPathName().toStdString();
            options.outputDirectory = output.path();
            options.presetOutputType = halionbridge::converters::PresetOutputType::layer;
            const auto result = halionbridge::converters::dx7::convertSource(options);

            expect(result.succeeded);
            const auto lua = output.directory.getChildFile("000001_LAYER.lua").loadFileAsString();
            expect(lua.contains("preset_type = \"layer\""));
            expect(lua.contains("preset_target = \"halion\""));
        }

        beginTest("Generates HALion Sonic program entries and templates");
        {
            auto source = ScopedTestDirectory("dx7_sonic_source");
            auto output = ScopedTestDirectory("dx7_sonic_output");
            const auto sourceFile = source.directory.getChildFile("voice.syx");
            expect(writeBytes(sourceFile, framedSingle("SONIC", 4)));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = sourceFile.getFullPathName().toStdString();
            options.outputDirectory = output.path();
            options.presetTarget = halionbridge::converters::PresetTarget::halionSonic;
            const auto result = halionbridge::converters::dx7::convertSource(options);

            expect(result.succeeded);
            const auto lua = output.directory.getChildFile("000001_SONIC.lua").loadFileAsString();
            expect(lua.contains("preset_type = \"program\""));
            expect(lua.contains("preset_target = \"halion-sonic\""));

            const auto templateFile = output.directory.getChildFile(".halionbridge/dx7/templates/dx7_05.vstpreset");
            const auto sourceTemplate =
                juce::File::getCurrentWorkingDirectory().getChildFile("resources/dx7/halion-sonic/dx7_05.vstpreset");
            auto emittedBytes = juce::MemoryBlock{};
            auto sourceBytes = juce::MemoryBlock{};
            expect(templateFile.loadFileAsData(emittedBytes));
            expect(sourceTemplate.loadFileAsData(sourceBytes));
            expect(emittedBytes == sourceBytes);

            const auto report = output.directory.getChildFile("halionbridge_dx7_conversion_report.json").loadFileAsString();
            expect(report.contains("\"preset_type\": \"program\""));
            expect(report.contains("\"preset_target\": \"halion-sonic\""));
        }

        beginTest("Rejects HALion Sonic layer output before writing");
        {
            auto source = ScopedTestDirectory("dx7_sonic_layer_source");
            auto output = ScopedTestDirectory("dx7_sonic_layer_output");
            const auto sourceFile = source.directory.getChildFile("voice.syx");
            expect(writeBytes(sourceFile, framedSingle("INVALID", 0)));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = sourceFile.getFullPathName().toStdString();
            options.outputDirectory = output.path();
            options.presetOutputType = halionbridge::converters::PresetOutputType::layer;
            options.presetTarget = halionbridge::converters::PresetTarget::halionSonic;
            const auto result = halionbridge::converters::dx7::convertSource(options);

            expect(!result.succeeded);
            expect(!output.directory.getChildFile("halionbridge_build.lua").existsAsFile());
            expect(std::ranges::any_of(result.diagnostics, [](const auto& diagnostic) { return diagnostic.code == "preset-target-type"; }));
        }

        beginTest("A single bank omits the redundant bank directory");
        {
            auto source = ScopedTestDirectory("dx7_one_bank_source");
            auto output = ScopedTestDirectory("dx7_one_bank_output");
            const auto sourceFile = source.directory.getChildFile("one.syx");
            const auto bytes = rawBank("ONE");
            expect(writeBytes(sourceFile, bytes));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = sourceFile.getFullPathName().toStdString();
            options.outputDirectory = output.path();
            const auto result = halionbridge::converters::dx7::convertSource(options);

            expect(result.succeeded);
            expectEquals(result.bankMessagesConverted, 1);
            expectEquals(result.voicesConverted, 32);
            const auto firstLua = output.directory.getChildFile("000001_ONE_01.lua").loadFileAsString();
            expect(firstLua.contains("output_file = \"one/01_ONE_01.vstpreset\""));
            expect(!firstLua.contains("/bank_001/"));
        }

        beginTest("Multiple banks retain message directories");
        {
            auto source = ScopedTestDirectory("dx7_multiple_banks_source");
            auto output = ScopedTestDirectory("dx7_multiple_banks_output");
            const auto sourceFile = source.directory.getChildFile("multiple.syx");
            auto bytes = framedBank("FIRST");
            const auto secondBank = framedBank("SECOND");
            bytes.insert(bytes.end(), secondBank.begin(), secondBank.end());
            expect(writeBytes(sourceFile, bytes));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = sourceFile.getFullPathName().toStdString();
            options.outputDirectory = output.path();
            const auto result = halionbridge::converters::dx7::convertSource(options);

            expect(result.succeeded);
            expectEquals(result.bankMessagesConverted, 2);
            expectEquals(result.voicesConverted, 64);
            const auto firstLua = output.directory.getChildFile("000001_FIRST_01.lua").loadFileAsString();
            const auto secondBankLua = output.directory.getChildFile("000033_SECOND_01.lua").loadFileAsString();
            expect(firstLua.contains("output_file = \"multiple/bank_001/01_FIRST_01.vstpreset\""));
            expect(secondBankLua.contains("output_file = \"multiple/bank_002/01_SECOND_01.vstpreset\""));
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

        beginTest("Recovery converts validated messages and records skipped input without absolute paths");
        {
            auto source = ScopedTestDirectory("dx7_recovery_source");
            auto output = ScopedTestDirectory("dx7_recovery_output");

            auto mixed = framedSingle("FIRST", 0);
            const auto unsupported = framedUnsupportedMessage();
            const auto last = framedSingle("LAST", 1);
            mixed.insert(mixed.end(), unsupported.begin(), unsupported.end());
            mixed.insert(mixed.end(), last.begin(), last.end());
            expect(writeBytes(source.directory.getChildFile("mixed.syx"), mixed));

            auto corrupt = framedSingle("BAD", 0);
            corrupt[corrupt.size() - 2] ^= 1;
            expect(writeBytes(source.directory.getChildFile("corrupt.syx"), corrupt));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = source.path();
            options.outputDirectory = output.path();
            options.continueOnError = true;
            const auto result = halionbridge::converters::dx7::convertSource(options);

            expect(result.succeeded);
            expectEquals(result.syxFilesScanned, 2);
            expectEquals(result.syxFilesConverted, 1);
            expectEquals(result.syxFilesPartial, 1);
            expectEquals(result.syxFilesSkipped, 1);
            expectEquals(result.messagesConverted, 2);
            expectEquals(result.bankMessagesConverted, 0);
            expectEquals(result.singleMessagesConverted, 2);
            expectEquals(result.messagesSkipped, 2);
            expectEquals(result.voicesConverted, 2);
            expect(hasDiagnostic(result, "recovery-summary"));

            const auto firstLua = output.directory.getChildFile("000001_FIRST.lua").loadFileAsString();
            const auto lastLua = output.directory.getChildFile("000002_LAST.lua").loadFileAsString();
            expect(firstLua.contains("output_file = \"mixed/single_001_FIRST.vstpreset\""));
            expect(lastLua.contains("output_file = \"mixed/single_003_LAST.vstpreset\""));

            const auto report = output.directory.getChildFile("halionbridge_dx7_conversion_report.json").loadFileAsString();
            expect(report.contains("\"format\": \"halionbridge-dx7-conversion-report\""));
            expect(report.contains("\"outcome\": \"partial\""));
            expect(report.contains("\"outcome\": \"skipped\""));
            expect(report.contains("\"source\": \"mixed.syx\""));
            expect(!report.contains(source.directory.getFullPathName()));
        }

        beginTest("Recovery fails without writing when no valid DX7 voice remains");
        {
            auto source = ScopedTestDirectory("dx7_recovery_empty_source");
            auto output = ScopedTestDirectory("dx7_recovery_empty_output");
            auto corrupt = framedSingle("BAD", 0);
            corrupt[corrupt.size() - 2] ^= 1;
            expect(writeBytes(source.directory.getChildFile("corrupt.syx"), corrupt));

            auto options = halionbridge::converters::dx7::ConversionOptions{};
            options.sourcePath = source.path();
            options.outputDirectory = output.path();
            options.continueOnError = true;
            const auto result = halionbridge::converters::dx7::convertSource(options);

            expect(!result.succeeded);
            expectEquals(result.syxFilesSkipped, 1);
            expectEquals(result.voicesConverted, 0);
            expect(hasDiagnostic(result, "no-voices"));
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
