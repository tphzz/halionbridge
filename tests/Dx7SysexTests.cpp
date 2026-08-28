#include <dxsyx/Dx7Sysex.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace
{

using Bytes = std::vector<std::uint8_t>;

constexpr auto kPackedVoiceSize = std::size_t{128};
constexpr auto kBankVoiceCount = std::size_t{32};

void writeName(std::span<std::uint8_t> destination, const std::string_view name)
{
    std::ranges::fill(destination, static_cast<std::uint8_t>(' '));
    const auto count = std::min(destination.size(), name.size());
    std::ranges::copy_n(name.begin(), static_cast<std::ptrdiff_t>(count), destination.begin());
}

Bytes makeRawBank()
{
    auto bytes = Bytes(kPackedVoiceSize * kBankVoiceCount, 0);
    for (auto voice = std::size_t{0}; voice < kBankVoiceCount; ++voice)
    {
        auto voiceData = std::span(bytes).subspan(voice * kPackedVoiceSize, kPackedVoiceSize);
        writeName(voiceData.subspan(118, 10), "INIT VOICE");
    }
    return bytes;
}

Bytes makeSingleVoice()
{
    auto bytes = Bytes(155, 0);
    writeName(std::span(bytes).subspan(145, 10), "SINGLE");
    return bytes;
}

std::uint8_t yamahaChecksum(const std::span<const std::uint8_t> payload)
{
    auto sum = std::uint32_t{0};
    for (const auto value : payload)
        sum += value;
    return static_cast<std::uint8_t>((128U - (sum & 0x7fU)) & 0x7fU);
}

Bytes frameMessage(const std::uint8_t format, const std::span<const std::uint8_t> payload, const std::uint8_t channel = 0)
{
    auto bytes = Bytes{};
    bytes.reserve(payload.size() + 8);
    bytes.push_back(0xf0);
    bytes.push_back(0x43);
    bytes.push_back(channel);
    bytes.push_back(format);
    bytes.push_back(static_cast<std::uint8_t>((payload.size() >> 7U) & 0x7fU));
    bytes.push_back(static_cast<std::uint8_t>(payload.size() & 0x7fU));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    bytes.push_back(yamahaChecksum(payload));
    bytes.push_back(0xf7);
    return bytes;
}

const dxsyx::BankMessage* getBank(const dxsyx::ParseResult& result, const std::size_t index = 0)
{
    if (index >= result.messages.size())
        return nullptr;
    return std::get_if<dxsyx::BankMessage>(&result.messages[index].data);
}

const dxsyx::SingleVoiceMessage* getSingle(const dxsyx::ParseResult& result, const std::size_t index = 0)
{
    if (index >= result.messages.size())
        return nullptr;
    return std::get_if<dxsyx::SingleVoiceMessage>(&result.messages[index].data);
}

class Dx7SysexTests final : public juce::UnitTest
{
  public:
    Dx7SysexTests() : juce::UnitTest("DX7 SysEx parser", "halionbridge") {}

    void runTest() override
    {
        beginTest("Parses framed banks into logically ordered operators");
        {
            auto payload = makeRawBank();
            payload[14] = 66;            // First serialized operator is DX7 operator 6.
            payload[(5 * 17) + 14] = 77; // Last serialized operator is DX7 operator 1.
            payload[110] = 31;
            writeName(std::span(payload).subspan(118, 10), "BANK VOICE");

            const auto result = dxsyx::parse(frameMessage(0x09, payload, 7));
            expect(result.succeeded());
            expectEquals(static_cast<int>(result.messages.size()), 1);

            const auto* bank = getBank(result);
            expect(bank != nullptr);
            if (bank != nullptr)
            {
                expectEquals(static_cast<int>(bank->channel), 7);
                expect(!bank->raw);
                expectEquals(static_cast<int>(bank->voices[0].operators[0].outputLevel), 77);
                expectEquals(static_cast<int>(bank->voices[0].operators[5].outputLevel), 66);
                expectEquals(static_cast<int>(bank->voices[0].algorithm), 31);
                expect(bank->voices[0].name.starts_with("BANK VOICE"));
            }
        }

        beginTest("Parses framed single voices and concatenated message streams");
        {
            auto singlePayload = makeSingleVoice();
            singlePayload[16] = 45;
            singlePayload[(5 * 21) + 16] = 88;
            singlePayload[134] = 12;
            auto single = frameMessage(0x00, singlePayload, 15);
            const auto bank = frameMessage(0x09, makeRawBank(), 3);
            single.insert(single.end(), bank.begin(), bank.end());

            const auto result = dxsyx::parse(single);
            expect(result.succeeded());
            expectEquals(static_cast<int>(result.messages.size()), 2);

            const auto* voice = getSingle(result, 0);
            expect(voice != nullptr);
            if (voice != nullptr)
            {
                expectEquals(static_cast<int>(voice->channel), 15);
                expectEquals(static_cast<int>(voice->voice.operators[0].outputLevel), 88);
                expectEquals(static_cast<int>(voice->voice.operators[5].outputLevel), 45);
                expectEquals(static_cast<int>(voice->voice.algorithm), 12);
            }
            expect(getBank(result, 1) != nullptr);
        }

        beginTest("Recognizes an exact whole-file raw bank");
        {
            const auto result = dxsyx::parse(makeRawBank());
            expect(result.succeeded());
            const auto* bank = getBank(result);
            expect(bank != nullptr);
            if (bank != nullptr)
                expect(bank->raw);
        }

        beginTest("Rejects checksum failures, high-bit payload data, and stray bytes");
        {
            auto badChecksum = frameMessage(0x09, makeRawBank());
            badChecksum[badChecksum.size() - 2] ^= 1;
            auto result = dxsyx::parse(badChecksum);
            expect(!result.succeeded());
            expect(std::ranges::any_of(result.issues, [](const auto& issue) { return issue.code == "checksum"; }));

            auto highBit = frameMessage(0x09, makeRawBank());
            highBit[6] = 0x80;
            result = dxsyx::parse(highBit);
            expect(!result.succeeded());
            expect(std::ranges::any_of(result.issues, [](const auto& issue) { return issue.code == "non-seven-bit"; }));

            auto stray = frameMessage(0x00, makeSingleVoice());
            stray.push_back(0);
            result = dxsyx::parse(stray);
            expect(!result.succeeded());
            expect(std::ranges::any_of(result.issues, [](const auto& issue) { return issue.code == "unsupported-data"; }));
        }

        beginTest("Normalizes illegal scalar values unless strict mode is requested");
        {
            auto raw = makeRawBank();
            raw[0] = 127;
            raw[12] = 120; // Packed detune 15 and rate scaling 0.
            raw[117] = 99;

            auto result = dxsyx::parse(raw);
            expect(result.succeeded());
            expectEquals(static_cast<int>(result.issues.size()), 3);
            const auto* bank = getBank(result);
            expect(bank != nullptr);
            if (bank != nullptr)
            {
                const auto& voice = bank->voices[0];
                expectEquals(static_cast<int>(voice.operators[5].envelopeRates[0]), 99);
                expectEquals(static_cast<int>(voice.operators[5].detune), 14);
                expectEquals(static_cast<int>(voice.transpose), 48);
            }
            expect(std::ranges::all_of(result.issues, [](const auto& issue) { return issue.severity == dxsyx::IssueSeverity::warning; }));

            result = dxsyx::parse(raw, dxsyx::ParseOptions{.strictParameters = true});
            expect(!result.succeeded());
            expect(std::ranges::all_of(result.issues, [](const auto& issue) { return issue.severity == dxsyx::IssueSeverity::error; }));
        }

        beginTest("Masks documented unused packed bits without warnings");
        {
            auto raw = makeRawBank();
            raw[11] = 0x79;
            raw[13] = 0x7f;
            raw[15] = 0x7f;
            raw[110] = 0x63;
            raw[111] = 0x7f;

            const auto result = dxsyx::parse(raw);
            expect(result.succeeded());
            expect(result.issues.empty());
            const auto* bank = getBank(result);
            expect(bank != nullptr);
            if (bank != nullptr)
            {
                const auto& voice = bank->voices[0];
                const auto& op6 = voice.operators[5];
                expectEquals(static_cast<int>(op6.leftCurve), 1);
                expectEquals(static_cast<int>(op6.rightCurve), 2);
                expectEquals(static_cast<int>(op6.amplitudeModulationSensitivity), 3);
                expectEquals(static_cast<int>(op6.keyVelocitySensitivity), 7);
                expect(op6.oscillatorMode == dxsyx::OscillatorMode::fixed);
                expectEquals(static_cast<int>(op6.frequencyCoarse), 31);
                expectEquals(static_cast<int>(voice.algorithm), 3);
                expectEquals(static_cast<int>(voice.feedback), 7);
                expect(voice.oscillatorSync);
            }
        }

        beginTest("Decodes the DX7 display character substitutions");
        {
            auto raw = makeRawBank();
            const auto nameOffset = std::size_t{118};
            raw[nameOffset] = 92;
            raw[nameOffset + 1] = 126;
            raw[nameOffset + 2] = 127;
            raw[nameOffset + 3] = 0;

            const auto result = dxsyx::parse(raw);
            expect(result.succeeded());
            const auto* bank = getBank(result);
            expect(bank != nullptr);
            if (bank != nullptr)
                expect(bank->voices[0].name.starts_with("Y<>_"));
        }
    }
};

Dx7SysexTests dx7SysexTests;

} // namespace
