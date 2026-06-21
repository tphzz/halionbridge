#include "halionbridge_converters/sfz/SfzConverter.h"

#include "halionbridge_converters/BuildDirectoryEmitter.h"
#include "SfzHelperLua.h"

#include <sfizz/Defaults.h>
#include <sfizz/Opcode.h>
#include <sfizz/Region.h>
#include <sfizz/Synth.h>
#include <sfizz/parser/Parser.h>
#include <sfizz/parser/ParserListener.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace halionbridge::converters::sfz
{
namespace
{

constexpr const char* kSfzHelperLuaFileName = "halionbridge-sfz.lua";
constexpr float kHalionSfzLevelCompensationDb = 7.8f;
constexpr float kHalionAmpReleaseTotalScale = 0.604f;
constexpr float kHalionAmpReleaseEarlyScale = 0.097f;
constexpr float kHalionAmpReleaseEarlyLevel = 0.35f;
constexpr float kHalionAmpReleaseEarlyCurve = -0.24f;
constexpr float kHalionAmpReleaseTailCurve = -1.0f;
constexpr float kHalionAmpDecayZeroOneScale = 0.5f;
constexpr float kHalionPitchEnvelopeDecayScale = 0.30f;
constexpr float kHalionPitchEnvelopeDecayCurve = -0.60f;
constexpr float kHalionPitchEnvelopeReleaseDuration = 0.01f;
constexpr float kHalionPitchEnvelopeMinDepthCents = -6000.0f;
constexpr float kHalionPitchEnvelopeMaxDepthCents = 6000.0f;
constexpr float kHalionPitchLfoDepthScale = 1.0f / 20.0f;
constexpr float kHalionSquarePitchLfoDepthScale = 3.0f / 100.0f;
constexpr float kHalionSquarePitchLfoPitchOffsetScale = 0.5f;
constexpr float kHalionPitchLfoMinDepthCents = -1200.0f;
constexpr float kHalionPitchLfoMaxDepthCents = 1200.0f;
constexpr float kHalionPitchLfoMinFrequencyHz = 0.0f;
constexpr float kHalionPitchLfoMaxFrequencyHz = 20.0f;
constexpr float kHalionPitchLfoMinDurationSeconds = 0.0f;
constexpr float kHalionPitchLfoMaxDurationSeconds = 100.0f;
constexpr float kHalionFilterEnvelopeAttackAmountScale = 1.0f / 96.0f;
constexpr float kHalionFilterEnvelopeDecayAmountScale = 1.0f / 48.0f;
constexpr float kHalionFilterEnvelopePeakLevel = 0.72f;
constexpr float kHalionFilterEnvelopeDecayScale = 0.30f;
constexpr float kHalionFilterEnvelopeReleaseDuration = 0.01f;
constexpr float kHalionFilterEnvelopeMinAmount = -100.0f;
constexpr float kHalionFilterEnvelopeMaxAmount = 100.0f;
constexpr int kHalionClassicFilterType = 1;
constexpr int kHalionClassicSingleFilterMode = 0;
constexpr int kHalionClassicDualSerialMode = 1;
constexpr int kHalionClassicLp24Shape = 0;
constexpr int kHalionClassicLp12Shape = 2;
constexpr int kHalionClassicLp6Shape = 3;
constexpr int kHalionClassicBp12Shape = 4;
constexpr int kHalionClassicHp18Shape = 9;
constexpr int kHalionClassicHp12Shape = 10;
constexpr int kHalionClassicHp6Shape = 11;
constexpr int kHalionClassicBr12Shape = 12;
constexpr int kHalionClassicBr24Shape = 13;
constexpr int kHalionClassicApShape = 16;
constexpr int kHalionClassicBp12Br12Shape = 19;
constexpr float kHalionLpf2pMaxResonance = 86.0f;

struct ConvertedEnvelopePoint
{
    float level = 0.0f;
    float duration = 0.0f;
    float curve = 0.0f;
};

struct ConvertedEnvelope
{
    std::vector<ConvertedEnvelopePoint> points;
    int sustainIndex = 0;
};

struct ConvertedPitchEnvelope
{
    float amount = 0.0f;
    ConvertedEnvelope envelope;
};

struct ConvertedFilterEnvelope
{
    float amount = 0.0f;
    ConvertedEnvelope envelope;
};

struct ConvertedPitchLfo
{
    float depth = 0.0f;
    float rateHz = 0.0f;
    float phaseDegrees = 0.0f;
    float delayMs = 0.0f;
    float fadeMs = 0.0f;
    int waveForm = 0;
    float shape = 0.0f;
    float pitchOffsetCents = 0.0f;
};

struct ConvertedRegion
{
    std::string name;
    std::filesystem::path samplePath;
    int keyLow = 0;
    int keyHigh = 127;
    int velocityLow = 0;
    int velocityHigh = 127;
    int rootKey = 60;
    std::optional<int64_t> sampleOffset;
    std::optional<int64_t> sampleEnd;
    std::optional<int64_t> sampleNaturalEnd;
    std::optional<float> tuneCents;
    std::optional<float> pitchKeytrack;
    bool hasLoop = false;
    int64_t loopStart = 0;
    int64_t loopEnd = 0;
    const char* loopMode = "continuous";
    std::optional<float> sampleOscLevelDb;
    std::optional<float> ampVelocityToLevel;
    std::optional<float> ampPan;
    std::optional<float> filterCutoff;
    std::optional<float> filterResonance;
    std::optional<int> filterType;
    std::optional<int> filterMode;
    std::optional<int> filterShapeA;
    std::optional<int> filterShapeB;
    std::optional<ConvertedPitchEnvelope> pitchEnvelope;
    std::optional<ConvertedPitchLfo> pitchLfo;
    std::optional<ConvertedFilterEnvelope> filterEnvelope;
    struct
    {
        float start = 0.0f;
        float delay = 0.0f;
        float attack = 0.0f;
        float hold = 0.0f;
        float decay = 0.0f;
        int decayZero = 1;
        float sustain = 1.0f;
        float release = 0.001f;
        float attackCurve = 0.0f;
    } ampEnvelope;
};

struct ConvertedSfz
{
    std::filesystem::path sourceFile;
    std::string layerName;
    std::string presetFileStem;
    std::string presetFileName;
    std::vector<ConvertedRegion> regions;
};

struct SfzFileSearchResult
{
    std::vector<std::filesystem::path> files;
    std::vector<Diagnostic> diagnostics;
};

struct ExplicitPitchLfo
{
    int index = 1;
    bool legacy = false;
    std::optional<float> depthCents;
    std::optional<float> frequencyHz;
    std::optional<float> phase;
    std::optional<float> delaySeconds;
    std::optional<float> fadeSeconds;
    std::optional<int> wave;
};

struct ExplicitRegionOpcodes
{
    std::optional<int64_t> sampleEnd;
    std::optional<int64_t> loopStart;
    std::optional<int64_t> loopEnd;
    std::optional<int> decayZero;
    std::optional<float> pitchEnvelopeDepthCents;
    std::optional<float> filterEnvelopeDepthCents;
    std::optional<std::string> filterTypeText;
    std::vector<ExplicitPitchLfo> pitchLfos;
    std::set<std::string> unsupportedAmpEnvelopeOpcodes;
    std::set<std::string> unsupportedPitchEnvelopeOpcodes;
    std::set<std::string> unsupportedPitchLfoOpcodes;
    std::set<std::string> unsupportedFilterEnvelopeOpcodes;
};

class ExplicitRegionOpcodeCollector final : public ::sfz::ParserListener
{
  public:
    void onParseFullBlock(const std::string& header, const std::vector<::sfz::Opcode>& opcodes) override
    {
        if (header == "global")
        {
            globalOpcodes = opcodes;
            masterOpcodes.clear();
            groupOpcodes.clear();
            return;
        }

        if (header == "master")
        {
            masterOpcodes = opcodes;
            groupOpcodes.clear();
            return;
        }

        if (header == "group")
        {
            groupOpcodes = opcodes;
            return;
        }

        if (header == "region")
            regions.push_back(collectEffectiveRegionOpcodes(opcodes));
    }

    std::vector<ExplicitRegionOpcodes> regions;

  private:
    ExplicitRegionOpcodes collectEffectiveRegionOpcodes(const std::vector<::sfz::Opcode>& regionOpcodes) const
    {
        auto explicitOpcodes = ExplicitRegionOpcodes{};
        applyOpcodes(globalOpcodes, explicitOpcodes);
        applyOpcodes(masterOpcodes, explicitOpcodes);
        applyOpcodes(groupOpcodes, explicitOpcodes);
        applyOpcodes(regionOpcodes, explicitOpcodes);
        return explicitOpcodes;
    }

    static void applyOpcodes(const std::vector<::sfz::Opcode>& opcodes, ExplicitRegionOpcodes& explicitOpcodes)
    {
        for (const auto& rawOpcode : opcodes)
        {
            if (rawOpcode.name == "fil_type" || rawOpcode.name == "filtype")
                explicitOpcodes.filterTypeText = lowercaseAscii(rawOpcode.value);

            const auto opcode = rawOpcode.cleanUp(::sfz::kOpcodeScopeRegion);
            if (opcode.name == "end")
                explicitOpcodes.sampleEnd = opcode.read(::sfz::Default::sampleEnd);
            else if (opcode.name == "loop_start")
                explicitOpcodes.loopStart = opcode.read(::sfz::Default::loopStart);
            else if (opcode.name == "loop_end")
                explicitOpcodes.loopEnd = opcode.read(::sfz::Default::loopEnd);
            else if (opcode.name == "ampeg_decay_zero")
                explicitOpcodes.decayZero = readZeroFlag(opcode.value);
            else if (opcode.name == "pitcheg_depth")
                explicitOpcodes.pitchEnvelopeDepthCents = opcode.read(::sfz::Default::egDepth);
            else if (opcode.name == "fileg_depth")
                explicitOpcodes.filterEnvelopeDepthCents = opcode.read(::sfz::Default::egDepth);
            else if (opcode.name == "fil_type" || opcode.name == "fil&_type" || opcode.name == "filtype")
                explicitOpcodes.filterTypeText = lowercaseAscii(opcode.value);

            inspectPitchLfoOpcode(rawOpcode, explicitOpcodes);

            if (isUnsupportedAmpEnvelopeOpcode(opcode.name))
                explicitOpcodes.unsupportedAmpEnvelopeOpcodes.insert(opcode.name);
            if (isUnsupportedPitchEnvelopeOpcode(opcode.name))
                explicitOpcodes.unsupportedPitchEnvelopeOpcodes.insert(opcode.name);
            if (isUnsupportedLegacyPitchLfoOpcode(opcode.name))
                explicitOpcodes.unsupportedPitchLfoOpcodes.insert(opcode.name);
            if (isUnsupportedFilterEnvelopeOpcode(opcode.name))
                explicitOpcodes.unsupportedFilterEnvelopeOpcodes.insert(opcode.name);
        }
    }

    static std::optional<int> readZeroFlag(const std::string& value)
    {
        auto parsed = 0;
        const auto* begin = value.data();
        const auto* end = value.data() + value.size();
        const auto result = std::from_chars(begin, end, parsed);
        if (result.ec == std::errc{} && result.ptr == end && (parsed == 0 || parsed == 1))
            return parsed;

        return std::nullopt;
    }

    static std::string lowercaseAscii(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    static bool startsWith(const std::string& text, const std::string_view prefix)
    {
        return text.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin());
    }

    static std::optional<float> readFloat(const std::string& value)
    {
        auto parsed = 0.0f;
        const auto* begin = value.data();
        const auto* end = value.data() + value.size();
        const auto result = std::from_chars(begin, end, parsed);
        if (result.ec == std::errc{} && result.ptr == end)
            return parsed;

        return std::nullopt;
    }

    static std::optional<int> readInt(const std::string& value)
    {
        auto parsed = 0;
        const auto* begin = value.data();
        const auto* end = value.data() + value.size();
        const auto result = std::from_chars(begin, end, parsed);
        if (result.ec == std::errc{} && result.ptr == end)
            return parsed;

        return std::nullopt;
    }

    static ExplicitPitchLfo& pitchLfoFor(ExplicitRegionOpcodes& explicitOpcodes, const int index, const bool legacy)
    {
        const auto existing =
            std::find_if(explicitOpcodes.pitchLfos.begin(), explicitOpcodes.pitchLfos.end(),
                         [index, legacy](const ExplicitPitchLfo& lfo) { return lfo.index == index && lfo.legacy == legacy; });
        if (existing != explicitOpcodes.pitchLfos.end())
            return *existing;

        auto& lfo = explicitOpcodes.pitchLfos.emplace_back();
        lfo.index = index;
        lfo.legacy = legacy;
        return lfo;
    }

    static void inspectPitchLfoOpcode(const ::sfz::Opcode& opcode, ExplicitRegionOpcodes& explicitOpcodes)
    {
        const auto name = lowercaseAscii(opcode.name);
        if (startsWith(name, "pitchlfo_"))
        {
            inspectLegacyPitchLfoOpcode(name, opcode.value, explicitOpcodes);
            return;
        }

        if (!startsWith(name, "lfo") || name.size() <= 4)
            return;

        auto cursor = size_t{3};
        auto index = 0;
        while (cursor < name.size() && std::isdigit(static_cast<unsigned char>(name[cursor])))
        {
            index = (index * 10) + (name[cursor] - '0');
            ++cursor;
        }

        if (index <= 0 || cursor >= name.size() || name[cursor] != '_')
            return;

        const auto suffix = std::string_view{name}.substr(cursor + 1);
        inspectNumberedPitchLfoOpcode(index, suffix, opcode.value, explicitOpcodes);
    }

    static void inspectLegacyPitchLfoOpcode(const std::string& name, const std::string& value, ExplicitRegionOpcodes& explicitOpcodes)
    {
        const auto parsed = readFloat(value);
        if (name == "pitchlfo_depth")
        {
            auto& lfo = pitchLfoFor(explicitOpcodes, 1, true);
            lfo.depthCents = parsed.value_or(0.0f);
        }
        else if (name == "pitchlfo_freq")
        {
            auto& lfo = pitchLfoFor(explicitOpcodes, 1, true);
            lfo.frequencyHz = parsed.value_or(0.0f);
        }
        else if (name == "pitchlfo_delay")
        {
            auto& lfo = pitchLfoFor(explicitOpcodes, 1, true);
            lfo.delaySeconds = parsed.value_or(0.0f);
        }
        else if (name == "pitchlfo_fade")
        {
            auto& lfo = pitchLfoFor(explicitOpcodes, 1, true);
            lfo.fadeSeconds = parsed.value_or(0.0f);
        }
        else if (name == "pitchlfo_phase")
        {
            auto& lfo = pitchLfoFor(explicitOpcodes, 1, true);
            lfo.phase = parsed.value_or(0.0f);
        }
        else
        {
            explicitOpcodes.unsupportedPitchLfoOpcodes.insert(name);
        }
    }

    static void inspectNumberedPitchLfoOpcode(const int index, const std::string_view suffix, const std::string& value,
                                              ExplicitRegionOpcodes& explicitOpcodes)
    {
        if (suffix == "pitch" || suffix == "freq" || suffix == "phase" || suffix == "delay" || suffix == "fade" || suffix == "wave")
        {
            auto& lfo = pitchLfoFor(explicitOpcodes, index, false);
            if (suffix == "pitch")
            {
                const auto parsed = readFloat(value);
                lfo.depthCents = parsed.value_or(0.0f);
            }
            else if (suffix == "freq")
            {
                const auto parsed = readFloat(value);
                lfo.frequencyHz = parsed.value_or(0.0f);
            }
            else if (suffix == "phase")
            {
                const auto parsed = readFloat(value);
                lfo.phase = parsed.value_or(0.0f);
            }
            else if (suffix == "delay")
            {
                const auto parsed = readFloat(value);
                lfo.delaySeconds = parsed.value_or(0.0f);
            }
            else if (suffix == "fade")
            {
                const auto parsed = readFloat(value);
                lfo.fadeSeconds = parsed.value_or(0.0f);
            }
            else if (suffix == "wave")
            {
                const auto parsed = readInt(value);
                lfo.wave = parsed.value_or(1);
            }
            return;
        }

        explicitOpcodes.unsupportedPitchLfoOpcodes.insert("lfo" + std::to_string(index) + "_" + std::string{suffix});
    }

    static bool isSupportedStaticAmpEnvelopeOpcode(const std::string& name)
    {
        static const auto supported = std::set<std::string>{"ampeg_attack", "ampeg_decay",   "ampeg_decay_zero", "ampeg_delay",
                                                            "ampeg_hold",   "ampeg_release", "ampeg_start",      "ampeg_sustain"};
        return supported.contains(name);
    }

    static bool isSupportedStaticPitchEnvelopeOpcode(const std::string& name)
    {
        static const auto supported =
            std::set<std::string>{"pitcheg_attack", "pitcheg_decay", "pitcheg_delay", "pitcheg_depth", "pitcheg_hold", "pitcheg_sustain"};
        return supported.contains(name);
    }

    static bool isSupportedStaticFilterEnvelopeOpcode(const std::string& name)
    {
        static const auto supported =
            std::set<std::string>{"fileg_attack", "fileg_decay", "fileg_delay", "fileg_depth", "fileg_hold", "fileg_sustain"};
        return supported.contains(name);
    }

    static bool isUnsupportedAmpEnvelopeOpcode(const std::string& name)
    {
        if (startsWith(name, "ampeg_"))
            return !isSupportedStaticAmpEnvelopeOpcode(name);

        if (!startsWith(name, "eg"))
            return false;

        return name.find("_ampeg") != std::string::npos || name.find("_amplitude") != std::string::npos ||
               name.find("_volume") != std::string::npos;
    }

    static bool isUnsupportedPitchEnvelopeOpcode(const std::string& name)
    {
        if (startsWith(name, "pitcheg_"))
            return !isSupportedStaticPitchEnvelopeOpcode(name);

        if (!startsWith(name, "eg"))
            return false;

        return name.find("_pitcheg") != std::string::npos || name.find("_pitch") != std::string::npos;
    }

    static bool isUnsupportedLegacyPitchLfoOpcode(const std::string& name)
    {
        if (!startsWith(name, "pitchlfo_"))
            return false;

        static const auto supported =
            std::set<std::string>{"pitchlfo_depth", "pitchlfo_freq", "pitchlfo_phase", "pitchlfo_delay", "pitchlfo_fade"};
        return !supported.contains(name);
    }

    static bool isUnsupportedFilterEnvelopeOpcode(const std::string& name)
    {
        if (startsWith(name, "fileg_"))
            return !isSupportedStaticFilterEnvelopeOpcode(name);

        if (!startsWith(name, "eg"))
            return false;

        return name.find("_fileg") != std::string::npos || name.find("_filter") != std::string::npos;
    }

    std::vector<::sfz::Opcode> globalOpcodes;
    std::vector<::sfz::Opcode> masterOpcodes;
    std::vector<::sfz::Opcode> groupOpcodes;
};

Diagnostic makeDiagnostic(DiagnosticLevel level, const std::filesystem::path& source, std::string code, std::string message)
{
    return Diagnostic{level, source, 0, std::move(code), std::move(message)};
}

Diagnostic makeError(const std::filesystem::path& source, std::string code, std::string message)
{
    return makeDiagnostic(DiagnosticLevel::error, source, std::move(code), std::move(message));
}

Diagnostic makeWarning(const std::filesystem::path& source, std::string code, std::string message)
{
    return makeDiagnostic(DiagnosticLevel::warning, source, std::move(code), std::move(message));
}

Diagnostic makeInfo(const std::filesystem::path& source, std::string code, std::string message)
{
    return makeDiagnostic(DiagnosticLevel::info, source, std::move(code), std::move(message));
}

std::string toGenericString(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

std::uint16_t readLittleEndian16(const char* bytes)
{
    const auto low = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0]));
    const auto high = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[1]));
    return static_cast<std::uint16_t>(low | (high << 8));
}

std::uint32_t readLittleEndian32(const char* bytes)
{
    const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0]));
    const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1]));
    const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2]));
    const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3]));
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

std::optional<int64_t> readWaveFrameCount(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    char riffHeader[12]{};
    if (!input.read(riffHeader, sizeof(riffHeader)))
        return std::nullopt;

    if (std::string_view{riffHeader, 4} != "RIFF" || std::string_view{riffHeader + 8, 4} != "WAVE")
        return std::nullopt;

    std::optional<std::uint16_t> blockAlign;
    std::optional<std::uint32_t> pendingDataSize;

    while (input)
    {
        char chunkHeader[8]{};
        if (!input.read(chunkHeader, sizeof(chunkHeader)))
            break;

        const auto chunkId = std::string_view{chunkHeader, 4};
        const auto chunkSize = readLittleEndian32(chunkHeader + 4);

        if (chunkId == "fmt ")
        {
            std::string fmtData(chunkSize, '\0');
            if (!input.read(fmtData.data(), static_cast<std::streamsize>(fmtData.size())))
                return std::nullopt;

            if (fmtData.size() >= 14)
            {
                blockAlign = readLittleEndian16(fmtData.data() + 12);
                if (pendingDataSize && *blockAlign > 0)
                    return static_cast<int64_t>(*pendingDataSize / *blockAlign);
            }
        }
        else if (chunkId == "data")
        {
            if (blockAlign && *blockAlign > 0)
                return static_cast<int64_t>(chunkSize / *blockAlign);

            pendingDataSize = chunkSize;
            input.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        }
        else
        {
            input.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        }

        if ((chunkSize % 2U) != 0U)
            input.seekg(1, std::ios::cur);
    }

    return std::nullopt;
}

std::string displayNameFromStem(std::string text)
{
    for (auto& c : text)
    {
        if (c == '_' || c == '-')
            c = ' ';
    }

    auto capitalizeNext = true;
    for (auto& c : text)
    {
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            capitalizeNext = true;
            continue;
        }

        if (capitalizeNext)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            capitalizeNext = false;
        }
    }

    return text.empty() ? "SFZ Instrument" : text;
}

bool startsWithAt(const std::string_view text, const size_t offset, const std::string_view token)
{
    return offset <= text.size() && token.size() <= text.size() - offset && text.substr(offset, token.size()) == token;
}

void appendSeparator(std::string& text)
{
    if (!text.empty() && text.back() != '_')
        text.push_back('_');
}

void appendFilenameWord(std::string& text, const std::string_view word)
{
    appendSeparator(text);
    text.append(word);
    appendSeparator(text);
}

bool isWindowsReservedDeviceStem(const std::string_view stem)
{
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        return true;

    if (stem.size() == 4 && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt") && stem[3] >= '1' && stem[3] <= '9')
        return true;

    return false;
}

std::string safeFilenameStem(const std::string_view sourceText)
{
    auto text = std::string();
    text.reserve(sourceText.size());

    for (size_t i = 0; i < sourceText.size();)
    {
        const auto c = static_cast<unsigned char>(sourceText[i]);
        if (std::isalnum(c))
        {
            text.push_back(static_cast<char>(std::tolower(c)));
            ++i;
            continue;
        }

        if (sourceText[i] == '#' || sourceText[i] == '^' || startsWithAt(sourceText, i, "\xE2\x99\xAF"))
        {
            appendFilenameWord(text, "sharp");
            i += startsWithAt(sourceText, i, "\xE2\x99\xAF") ? 3U : 1U;
            continue;
        }

        if (startsWithAt(sourceText, i, "\xE2\x99\xAD"))
        {
            appendFilenameWord(text, "flat");
            i += 3;
            continue;
        }

        if (startsWithAt(sourceText, i, "\xE2\x99\xAE"))
        {
            appendFilenameWord(text, "natural");
            i += 3;
            continue;
        }

        appendSeparator(text);
        ++i;
    }

    while (text.find("__") != std::string::npos)
        text.replace(text.find("__"), 2, "_");

    while (!text.empty() && text.front() == '_')
        text.erase(text.begin());

    while (!text.empty() && text.back() == '_')
        text.pop_back();

    if (text.empty())
        text = "sfz_instrument";

    if (isWindowsReservedDeviceStem(text))
        text = "sfz_" + text;

    return text;
}

std::string caseInsensitiveKey(std::string text)
{
    std::replace(text.begin(), text.end(), '\\', '/');
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string zeroPaddedIndex(const size_t index)
{
    std::ostringstream stream;
    stream.width(3);
    stream.fill('0');
    stream << index;
    return stream.str();
}

void assignUniquePresetFileNames(std::vector<ConvertedSfz>& convertedFiles)
{
    auto usedPresetNames = std::set<std::string>();
    for (auto& converted : convertedFiles)
    {
        auto candidateName = converted.presetFileStem + ".vstpreset";
        if (usedPresetNames.insert(caseInsensitiveKey(candidateName)).second)
        {
            converted.presetFileName = std::move(candidateName);
            continue;
        }

        for (size_t suffix = 2;; ++suffix)
        {
            candidateName = converted.presetFileStem + "_" + zeroPaddedIndex(suffix) + ".vstpreset";
            if (usedPresetNames.insert(caseInsensitiveKey(candidateName)).second)
            {
                converted.presetFileName = std::move(candidateName);
                break;
            }
        }
    }
}

bool hasSfzExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".sfz";
}

bool shouldStop(const ConverterRunContext* context)
{
    return context != nullptr && context->shouldStop();
}

template <typename Iterator>
void collectSfzFiles(Iterator iterator, const Iterator end, const ConverterRunContext* context, std::vector<std::filesystem::path>& files,
                     std::vector<Diagnostic>& diagnostics)
{
    std::error_code error;
    while (iterator != end)
    {
        if (shouldStop(context))
        {
            diagnostics.push_back(makeError({}, "stopped", "Conversion stopped by user request."));
            return;
        }

        const auto path = iterator->path();
        const auto isFile = iterator->is_regular_file(error);
        if (error)
        {
            diagnostics.push_back(makeError(path, "traversal-error", "Could not inspect " + path.string() + ": " + error.message()));
            error.clear();
        }
        else if (isFile && hasSfzExtension(path))
        {
            files.push_back(path);
        }

        iterator.increment(error);
        if (error)
        {
            diagnostics.push_back(
                makeError(path, "traversal-error", "Could not continue scanning after " + path.string() + ": " + error.message()));
            return;
        }
    }
}

SfzFileSearchResult findSfzFiles(const std::filesystem::path& sourceDirectory, const bool recursive, const ConverterRunContext* context)
{
    auto result = SfzFileSearchResult{};
    std::error_code error;

    if (recursive)
    {
        auto iterator = std::filesystem::recursive_directory_iterator(sourceDirectory, error);
        if (error)
        {
            result.diagnostics.push_back(
                makeError(sourceDirectory, "traversal-error", "Could not scan " + sourceDirectory.string() + ": " + error.message()));
            return result;
        }

        collectSfzFiles(iterator, std::filesystem::recursive_directory_iterator{}, context, result.files, result.diagnostics);
    }
    else
    {
        auto iterator = std::filesystem::directory_iterator(sourceDirectory, error);
        if (error)
        {
            result.diagnostics.push_back(
                makeError(sourceDirectory, "traversal-error", "Could not scan " + sourceDirectory.string() + ": " + error.message()));
            return result;
        }

        collectSfzFiles(iterator, std::filesystem::directory_iterator{}, context, result.files, result.diagnostics);
    }

    std::sort(result.files.begin(), result.files.end(),
              [&sourceDirectory](const std::filesystem::path& lhs, const std::filesystem::path& rhs)
              {
                  const auto lhsRelative = std::filesystem::relative(lhs, sourceDirectory).generic_string();
                  const auto rhsRelative = std::filesystem::relative(rhs, sourceDirectory).generic_string();
                  return lhsRelative < rhsRelative;
              });
    return result;
}

std::filesystem::path absoluteNormalizedPath(const std::filesystem::path& path)
{
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (!error)
        return canonical;

    return std::filesystem::absolute(path, error).lexically_normal();
}

std::filesystem::path configuredSourcePath(const ConversionOptions& options)
{
    return options.sourcePath.empty() ? options.sourceDirectory : options.sourcePath;
}

std::filesystem::path defaultOutputDirectoryForSourcePath(const std::filesystem::path& sourcePath)
{
    std::error_code error;
    if (std::filesystem::is_directory(sourcePath, error))
        return sourcePath;

    auto outputDirectory = sourcePath.parent_path();
    return outputDirectory.empty() ? std::filesystem::path{"."} : outputDirectory;
}

int clampMidi(const int value)
{
    return std::clamp(value, 0, 127);
}

int normalizedVelocityStartToMidi(const float value)
{
    return clampMidi(static_cast<int>(std::floor((static_cast<double>(value) * 128.0) + 0.000001)));
}

int normalizedVelocityEndToMidi(const float value)
{
    return clampMidi(static_cast<int>(std::floor((static_cast<double>(value) * 128.0) + 0.000001)) - 1);
}

float clampEnvelopeDuration(const std::filesystem::path& sourceFile, const int regionIndex, const std::string_view name, const float value,
                            std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= 0.0f && value <= 30.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 30.0f) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "amp-envelope-duration-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " " + std::string(name) + " value " +
                                          std::to_string(value) + " is outside HALion's 0..30 second envelope-point range; using " +
                                          std::to_string(clamped) + "."));
    return clamped;
}

float clampEnvelopeLevel(const std::filesystem::path& sourceFile, const int regionIndex, const std::string_view name, const float value,
                         std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= 0.0f && value <= 1.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "amp-envelope-level-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " " + std::string(name) + " value " +
                                          std::to_string(value) + " is outside HALion's 0..1 amp-envelope level range; using " +
                                          std::to_string(clamped) + "."));
    return clamped;
}

bool differsFromDefault(const float value, const float defaultValue)
{
    return std::abs(value - defaultValue) > 0.0001f;
}

float clampPitchTuneCents(const std::filesystem::path& sourceFile, const int regionIndex, const float value,
                          std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= -1200.0f && value <= 1200.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, -1200.0f, 1200.0f) : 0.0f;
    diagnostics.push_back(
        makeWarning(sourceFile, "pitch-tune-clamped",
                    "Region " + std::to_string(regionIndex + 1) + " combined transpose/tune value " + std::to_string(value) +
                        " cents is outside HALion's SampleOsc.Tune -1200..1200 cent range; using " + std::to_string(clamped) + "."));
    return clamped;
}

float clampPitchKeytrack(const std::filesystem::path& sourceFile, const int regionIndex, const float value,
                         std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= -200.0f && value <= 200.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, -200.0f, 200.0f) : 100.0f;
    diagnostics.push_back(makeWarning(sourceFile, "pitch-keytrack-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " pitch_keytrack value " + std::to_string(value) +
                                          " is outside HALion's Pitch.KeyFollow -200..200 percent range; using " + std::to_string(clamped) +
                                          "."));
    return clamped;
}

float clampPitchEnvelopeDepthCents(const std::filesystem::path& sourceFile, const int regionIndex, const float value,
                                   std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= kHalionPitchEnvelopeMinDepthCents && value <= kHalionPitchEnvelopeMaxDepthCents)
        return value;

    const auto clamped =
        std::isfinite(value) ? std::clamp(value, kHalionPitchEnvelopeMinDepthCents, kHalionPitchEnvelopeMaxDepthCents) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "pitch-envelope-depth-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " pitcheg_depth value " + std::to_string(value) +
                                          " cents is outside HALion's Pitch.EnvAmount -60..60 semitone range; using " +
                                          std::to_string(clamped) + " cents."));
    return clamped;
}

float clampPitchEnvelopeDuration(const std::filesystem::path& sourceFile, const int regionIndex, const std::string_view name,
                                 const float value, std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= 0.0f && value <= 30.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 30.0f) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "pitch-envelope-duration-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " " + std::string(name) + " value " +
                                          std::to_string(value) + " is outside HALion's 0..30 second envelope-point range; using " +
                                          std::to_string(clamped) + "."));
    return clamped;
}

float clampPitchEnvelopeLevel(const std::filesystem::path& sourceFile, const int regionIndex, const std::string_view name,
                              const float value, std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= 0.0f && value <= 1.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "pitch-envelope-level-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " " + std::string(name) + " value " +
                                          std::to_string(value) + " is outside HALion's 0..1 pitch-envelope level range; using " +
                                          std::to_string(clamped) + "."));
    return clamped;
}

float clampPitchLfoDepthCents(const std::filesystem::path& sourceFile, const int regionIndex, const float value,
                              std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= kHalionPitchLfoMinDepthCents && value <= kHalionPitchLfoMaxDepthCents)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, kHalionPitchLfoMinDepthCents, kHalionPitchLfoMaxDepthCents) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "pitch-lfo-depth-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " pitch LFO depth value " + std::to_string(value) +
                                          " cents is outside the verified -1200..1200 cent range; using " + std::to_string(clamped) +
                                          " cents."));
    return clamped;
}

float clampPitchLfoFrequencyHz(const std::filesystem::path& sourceFile, const int regionIndex, const float value,
                               std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= kHalionPitchLfoMinFrequencyHz && value <= kHalionPitchLfoMaxFrequencyHz)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, kHalionPitchLfoMinFrequencyHz, kHalionPitchLfoMaxFrequencyHz) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "pitch-lfo-frequency-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " pitch LFO frequency value " + std::to_string(value) +
                                          " Hz is outside the verified 0..20 Hz range; using " + std::to_string(clamped) + " Hz."));
    return clamped;
}

float clampPitchLfoDurationSeconds(const std::filesystem::path& sourceFile, const int regionIndex, const std::string_view name,
                                   const float value, std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= kHalionPitchLfoMinDurationSeconds && value <= kHalionPitchLfoMaxDurationSeconds)
        return value;

    const auto clamped =
        std::isfinite(value) ? std::clamp(value, kHalionPitchLfoMinDurationSeconds, kHalionPitchLfoMaxDurationSeconds) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "pitch-lfo-duration-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " " + std::string(name) + " value " +
                                          std::to_string(value) + " is outside the verified 0..100 second range; using " +
                                          std::to_string(clamped) + "."));
    return clamped;
}

float clampPitchLfoPhase(const std::filesystem::path& sourceFile, const int regionIndex, const float value,
                         std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= 0.0f && value <= 1.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "pitch-lfo-phase-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " pitch LFO phase value " + std::to_string(value) +
                                          " is outside the verified 0..1 cycle range; using " + std::to_string(clamped) + "."));
    return clamped;
}

float clampFilterEnvelopeAmount(const std::filesystem::path& sourceFile, const int regionIndex, const std::string_view name,
                                const float value, std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= kHalionFilterEnvelopeMinAmount && value <= kHalionFilterEnvelopeMaxAmount)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, kHalionFilterEnvelopeMinAmount, kHalionFilterEnvelopeMaxAmount) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "filter-envelope-amount-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " " + std::string(name) +
                                          " maps outside HALion's Filter.EnvAmount -100..100 range; using " + std::to_string(clamped) +
                                          "."));
    return clamped;
}

float clampFilterEnvelopeDuration(const std::filesystem::path& sourceFile, const int regionIndex, const std::string_view name,
                                  const float value, std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= 0.0f && value <= 30.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 30.0f) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "filter-envelope-duration-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " " + std::string(name) + " value " +
                                          std::to_string(value) + " is outside HALion's 0..30 second envelope-point range; using " +
                                          std::to_string(clamped) + "."));
    return clamped;
}

float clampFilterEnvelopeLevel(const std::filesystem::path& sourceFile, const int regionIndex, const std::string_view name,
                               const float value, std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= 0.0f && value <= 1.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "filter-envelope-level-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " " + std::string(name) + " value " +
                                          std::to_string(value) + " is outside HALion's 0..1 filter-envelope level range; using " +
                                          std::to_string(clamped) + "."));
    return clamped;
}

float filterEnvelopeDecayStartLevel(const float absDepthCents)
{
    if (absDepthCents <= 1200.0f)
        return kHalionFilterEnvelopePeakLevel;

    if (absDepthCents < 2400.0f)
    {
        const auto t = (absDepthCents - 1200.0f) / 1200.0f;
        return kHalionFilterEnvelopePeakLevel + ((0.40f - kHalionFilterEnvelopePeakLevel) * t);
    }

    return 0.40f;
}

float filterEnvelopeDecayCurve(const float absDepthCents)
{
    if (absDepthCents <= 1200.0f)
        return -0.77f;

    if (absDepthCents < 2400.0f)
    {
        const auto t = (absDepthCents - 1200.0f) / 1200.0f;
        return -0.77f + ((-0.40f - -0.77f) * t);
    }

    if (absDepthCents < 4800.0f)
    {
        const auto t = (absDepthCents - 2400.0f) / 2400.0f;
        return -0.40f + ((-0.60f - -0.40f) * t);
    }

    return -0.60f;
}

float clampSampleOscLevelDb(const std::filesystem::path& sourceFile, const int regionIndex, const float value,
                            std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= -96.0f && value <= 96.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, -96.0f, 96.0f) : kHalionSfzLevelCompensationDb;
    diagnostics.push_back(makeWarning(sourceFile, "sample-level-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " SFZ volume plus HALion compensation value " +
                                          std::to_string(value) + " dB is outside HALion's SampleOsc.Level -96..96 dB range; using " +
                                          std::to_string(clamped) + " dB."));
    return clamped;
}

float clampAmpVelocityToLevel(const std::filesystem::path& sourceFile, const int regionIndex, const float value,
                              std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= -100.0f && value <= 100.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, -100.0f, 100.0f) : 100.0f;
    diagnostics.push_back(makeWarning(sourceFile, "amp-veltrack-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " amp_veltrack value " + std::to_string(value) +
                                          " is outside SFZ's -100..100 percent range; using " + std::to_string(clamped) + "."));
    return clamped;
}

float clampAmpPan(const std::filesystem::path& sourceFile, const int regionIndex, const float value, std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= -100.0f && value <= 100.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, -100.0f, 100.0f) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "pan-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " pan value " + std::to_string(value) +
                                          " is outside HALion's Amp.Pan -100..100 range; using " + std::to_string(clamped) + "."));
    return clamped;
}

float mapLpf2pResonanceToHalion(const float sfzResonance)
{
    struct Point
    {
        float sfz;
        float halion;
    };

    constexpr Point points[] = {
        {0.0f, 32.0f}, {3.0f, 39.0f}, {6.0f, 48.0f}, {12.0f, 63.0f}, {24.0f, 78.0f}, {40.0f, kHalionLpf2pMaxResonance},
    };

    if (!std::isfinite(sfzResonance))
        return points[0].halion;

    const auto clamped = std::clamp(sfzResonance, points[0].sfz, points[std::size(points) - 1].sfz);
    for (size_t i = 1; i < std::size(points); ++i)
    {
        if (clamped <= points[i].sfz)
        {
            const auto span = points[i].sfz - points[i - 1].sfz;
            const auto t = span > 0.0f ? (clamped - points[i - 1].sfz) / span : 0.0f;
            return points[i - 1].halion + ((points[i].halion - points[i - 1].halion) * t);
        }
    }

    return kHalionLpf2pMaxResonance;
}

float mapBaselineResonanceToHalion(const float sfzResonance, const float halionAtSfzSix)
{
    if (!std::isfinite(sfzResonance) || sfzResonance <= 0.0f || halionAtSfzSix <= 0.0f)
        return 0.0f;

    return std::clamp((sfzResonance / 6.0f) * halionAtSfzSix, 0.0f, kHalionLpf2pMaxResonance);
}

struct HalionFilterMapping
{
    int mode = kHalionClassicSingleFilterMode;
    int shapeA = kHalionClassicLp12Shape;
    std::optional<int> shapeB;
    float cutoffScale = 1.0f;
    float resonanceAtSfzSix = 0.0f;
};

std::optional<HalionFilterMapping> roughFilterMapping(const ::sfz::FilterType type)
{
    switch (type)
    {
    case ::sfz::kFilterLpf1p:
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicLp6Shape, std::nullopt, 0.7f, 0.0f};
    case ::sfz::kFilterLpf2p:
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicLp12Shape, std::nullopt, 1.0f, 48.0f};
    case ::sfz::kFilterLpf4p:
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicLp24Shape, std::nullopt, 1.0f, 66.0f};
    case ::sfz::kFilterLpf6p:
        return HalionFilterMapping{kHalionClassicDualSerialMode, kHalionClassicLp24Shape, kHalionClassicLp12Shape, 1.0f, 59.0f};
    case ::sfz::kFilterHpf1p:
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicHp6Shape, std::nullopt, 1.0f, 0.0f};
    case ::sfz::kFilterHpf2p:
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicHp12Shape, std::nullopt, 1.0f, 47.0f};
    case ::sfz::kFilterHpf4p:
        return HalionFilterMapping{kHalionClassicDualSerialMode, kHalionClassicHp18Shape, kHalionClassicHp6Shape, 1.0f, 50.0f};
    case ::sfz::kFilterHpf6p:
        return HalionFilterMapping{kHalionClassicDualSerialMode, kHalionClassicHp12Shape, kHalionClassicHp18Shape, 1.0f, 55.0f};
    case ::sfz::kFilterBpf1p:
    case ::sfz::kFilterBpf2p:
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicBp12Shape, std::nullopt, 1.0f, 50.0f};
    case ::sfz::kFilterBrf1p:
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicBr12Shape, std::nullopt, 1.0f, 35.0f};
    case ::sfz::kFilterBrf2p:
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicBr24Shape, std::nullopt, 1.0f, 50.0f};
    case ::sfz::kFilterLsh:
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicBp12Br12Shape, std::nullopt, 1.0f, 0.0f};
    case ::sfz::kFilterHsh:
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicHp6Shape, std::nullopt, 1.0f, 0.0f};
    case ::sfz::kFilterPeq:
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicApShape, std::nullopt, 1.0f, 0.0f};
    default:
        return std::nullopt;
    }
}

std::optional<HalionFilterMapping> roughFilterMapping(const ::sfz::FilterType type, const std::optional<std::string>& explicitTypeText)
{
    if (explicitTypeText && *explicitTypeText == "brf_1p")
        return HalionFilterMapping{kHalionClassicSingleFilterMode, kHalionClassicBr12Shape, std::nullopt, 1.0f, 35.0f};

    return roughFilterMapping(type);
}

std::vector<ExplicitRegionOpcodes> collectExplicitRegionOpcodes(const std::filesystem::path& sfzFile, std::vector<Diagnostic>& diagnostics)
{
    auto collector = ExplicitRegionOpcodeCollector{};
    auto parser = ::sfz::Parser{};
    parser.setListener(&collector);
    parser.parseFile(::fs::path(sfzFile.string()));

    if (parser.getErrorCount() > 0)
    {
        diagnostics.push_back(makeWarning(sfzFile, "source-opcode-overlay",
                                          "Could not fully inspect explicit SFZ opcodes; falling back to sfizz's resolved region model."));
        return {};
    }

    return collector.regions;
}

std::string luaNumber(const float value)
{
    auto stream = std::ostringstream{};
    stream << std::setprecision(9) << value;
    return stream.str();
}

ConvertedPitchEnvelope convertPitchEnvelope(const std::filesystem::path& sourceFile, const int regionIndex,
                                            const ::sfz::EGDescription& pitchEG, const float depthCents,
                                            std::vector<Diagnostic>& diagnostics)
{
    auto converted = ConvertedPitchEnvelope{};
    converted.amount = clampPitchEnvelopeDepthCents(sourceFile, regionIndex, depthCents, diagnostics) / 100.0f;

    const auto delay = clampPitchEnvelopeDuration(sourceFile, regionIndex, "pitcheg_delay", pitchEG.delay, diagnostics);
    const auto attack = clampPitchEnvelopeDuration(sourceFile, regionIndex, "pitcheg_attack", pitchEG.attack, diagnostics);
    const auto hold = clampPitchEnvelopeDuration(sourceFile, regionIndex, "pitcheg_hold", pitchEG.hold, diagnostics);
    const auto decay = clampPitchEnvelopeDuration(sourceFile, regionIndex, "pitcheg_decay", pitchEG.decay, diagnostics);
    const auto sustain = clampPitchEnvelopeLevel(sourceFile, regionIndex, "pitcheg_sustain", pitchEG.sustain, diagnostics);

    auto& envelope = converted.envelope;
    auto appendPoint = [&envelope](const float level, const float duration, const float curve)
    { envelope.points.push_back(ConvertedEnvelopePoint{level, duration, curve}); };

    if (delay > 0.0f)
    {
        appendPoint(0.0f, 0.0f, 0.0f);
        appendPoint(0.0f, delay, 0.0f);
    }

    if (attack > 0.0f)
    {
        if (envelope.points.empty())
            appendPoint(0.0f, 0.0f, 0.0f);
        appendPoint(1.0f, attack, 0.0f);
    }
    else if (envelope.points.empty())
    {
        appendPoint(1.0f, 0.0f, 0.0f);
    }
    else
    {
        appendPoint(1.0f, 0.0f, 0.0f);
    }

    if (hold > 0.0f)
        appendPoint(1.0f, hold, 0.0f);

    if (decay > 0.0f && sustain < 1.0f)
        appendPoint(sustain, decay * kHalionPitchEnvelopeDecayScale, kHalionPitchEnvelopeDecayCurve);
    else
        appendPoint(sustain, 0.0f, 0.0f);

    envelope.sustainIndex = static_cast<int>(envelope.points.size());
    appendPoint(0.0f, kHalionPitchEnvelopeReleaseDuration, -1.0f);

    return converted;
}

std::optional<ExplicitPitchLfo> selectPitchLfo(const ExplicitRegionOpcodes& explicitOpcodes)
{
    const auto candidate = std::find_if(explicitOpcodes.pitchLfos.begin(), explicitOpcodes.pitchLfos.end(), [](const ExplicitPitchLfo& lfo)
                                        { return lfo.depthCents && differsFromDefault(*lfo.depthCents, 0.0f); });
    if (candidate == explicitOpcodes.pitchLfos.end())
        return std::nullopt;

    return *candidate;
}

ConvertedPitchLfo convertPitchLfo(const std::filesystem::path& sourceFile, const int regionIndex, const ExplicitPitchLfo& source,
                                  std::vector<Diagnostic>& diagnostics)
{
    auto converted = ConvertedPitchLfo{};
    const auto depthCents = clampPitchLfoDepthCents(sourceFile, regionIndex, source.depthCents.value_or(0.0f), diagnostics);
    converted.depth = depthCents * kHalionPitchLfoDepthScale;
    converted.rateHz = clampPitchLfoFrequencyHz(sourceFile, regionIndex, source.frequencyHz.value_or(0.0f), diagnostics);
    converted.phaseDegrees = clampPitchLfoPhase(sourceFile, regionIndex, source.phase.value_or(0.0f), diagnostics) * 360.0f;
    converted.delayMs = clampPitchLfoDurationSeconds(sourceFile, regionIndex, source.legacy ? "pitchlfo_delay" : "lfo_delay",
                                                     source.delaySeconds.value_or(0.0f), diagnostics) *
                        1000.0f;
    converted.fadeMs = clampPitchLfoDurationSeconds(sourceFile, regionIndex, source.legacy ? "pitchlfo_fade" : "lfo_fade",
                                                    source.fadeSeconds.value_or(0.0f), diagnostics) *
                       1000.0f;
    switch (source.wave.value_or(1))
    {
    case 0:
        converted.waveForm = 1;
        converted.shape = 25.0f;
        break;
    case 1:
        converted.waveForm = 0;
        break;
    case 3:
        converted.depth = depthCents * kHalionSquarePitchLfoDepthScale;
        converted.waveForm = 3;
        converted.shape = 50.0f;
        converted.pitchOffsetCents = depthCents * kHalionSquarePitchLfoPitchOffsetScale;
        break;
    case 6:
        converted.waveForm = 4;
        break;
    case 7:
        converted.waveForm = 2;
        break;
    case 12:
        converted.waveForm = 6;
        break;
    default:
        break;
    }
    return converted;
}

bool isSupportedPitchLfoWave(const ExplicitPitchLfo& lfo)
{
    if (!lfo.wave)
        return true;

    switch (*lfo.wave)
    {
    case 0:
    case 1:
    case 3:
    case 6:
    case 7:
    case 12:
        return true;
    default:
        return false;
    }
}

ConvertedFilterEnvelope convertFilterEnvelope(const std::filesystem::path& sourceFile, const int regionIndex,
                                              const ::sfz::EGDescription& filterEG, const float depthCents,
                                              std::vector<Diagnostic>& diagnostics)
{
    auto converted = ConvertedFilterEnvelope{};
    const auto absDepthCents = std::abs(depthCents);

    const auto delay = clampFilterEnvelopeDuration(sourceFile, regionIndex, "fileg_delay", filterEG.delay, diagnostics);
    const auto attack = clampFilterEnvelopeDuration(sourceFile, regionIndex, "fileg_attack", filterEG.attack, diagnostics);
    const auto hold = clampFilterEnvelopeDuration(sourceFile, regionIndex, "fileg_hold", filterEG.hold, diagnostics);
    const auto decay = clampFilterEnvelopeDuration(sourceFile, regionIndex, "fileg_decay", filterEG.decay, diagnostics);
    const auto sustain = clampFilterEnvelopeLevel(sourceFile, regionIndex, "fileg_sustain", filterEG.sustain, diagnostics);

    auto& envelope = converted.envelope;
    auto appendPoint = [&envelope](const float level, const float duration, const float curve)
    { envelope.points.push_back(ConvertedEnvelopePoint{level, duration, curve}); };

    if (attack > 0.0f || sustain >= 1.0f)
    {
        converted.amount = clampFilterEnvelopeAmount(sourceFile, regionIndex, "fileg_depth",
                                                     depthCents * kHalionFilterEnvelopeAttackAmountScale, diagnostics);

        if (delay > 0.0f)
        {
            appendPoint(0.0f, 0.0f, 0.0f);
            appendPoint(0.0f, delay, 0.0f);
        }

        if (envelope.points.empty())
            appendPoint(0.0f, 0.0f, 0.0f);

        appendPoint(kHalionFilterEnvelopePeakLevel, attack, 0.0f);
        if (hold > 0.0f)
            appendPoint(kHalionFilterEnvelopePeakLevel, hold, 0.0f);

        const auto sustainLevel = kHalionFilterEnvelopePeakLevel * sustain;
        if (decay > 0.0f && sustain < 1.0f)
            appendPoint(sustainLevel, decay * kHalionFilterEnvelopeDecayScale, filterEnvelopeDecayCurve(absDepthCents));
        else
            appendPoint(sustainLevel, 0.0f, 0.0f);
    }
    else
    {
        converted.amount = clampFilterEnvelopeAmount(sourceFile, regionIndex, "fileg_depth",
                                                     depthCents * kHalionFilterEnvelopeDecayAmountScale, diagnostics);
        appendPoint(filterEnvelopeDecayStartLevel(absDepthCents), 0.0f, 0.0f);
        appendPoint(0.0f, decay * kHalionFilterEnvelopeDecayScale, filterEnvelopeDecayCurve(absDepthCents));
    }

    envelope.sustainIndex = static_cast<int>(envelope.points.size());
    appendPoint(0.0f, kHalionFilterEnvelopeReleaseDuration, -1.0f);

    return converted;
}

ConvertedRegion convertRegion(const std::filesystem::path& sourceFile, const int regionIndex, const ::sfz::Region& region,
                              const ExplicitRegionOpcodes& explicitOpcodes, std::vector<Diagnostic>& diagnostics)
{
    const auto samplePath = absoluteNormalizedPath(sourceFile.parent_path() / std::filesystem::path(region.sampleId->filename()));

    auto converted = ConvertedRegion{};
    converted.name = samplePath.stem().string();
    converted.samplePath = samplePath;
    converted.keyLow = clampMidi(static_cast<int>(region.keyRange.getStart()));
    converted.keyHigh = clampMidi(static_cast<int>(region.keyRange.getEnd()));
    converted.velocityLow = normalizedVelocityStartToMidi(region.velocityRange.getStart());
    converted.velocityHigh = normalizedVelocityEndToMidi(region.velocityRange.getEnd());
    converted.rootKey = clampMidi(static_cast<int>(region.pitchKeycenter));
    if (region.offset != ::sfz::Default::offset)
        converted.sampleOffset = region.offset;
    if (explicitOpcodes.sampleEnd)
        converted.sampleEnd = *explicitOpcodes.sampleEnd;
    if (!converted.sampleEnd)
    {
        if (const auto frameCount = readWaveFrameCount(samplePath); frameCount && *frameCount > 0)
            converted.sampleNaturalEnd = *frameCount - 1;
    }
    converted.sampleOscLevelDb = clampSampleOscLevelDb(sourceFile, regionIndex, region.volume + kHalionSfzLevelCompensationDb, diagnostics);

    const auto combinedTuneCents = region.pitch + (region.transpose * 100.0f);
    if (differsFromDefault(combinedTuneCents, 0.0f))
        converted.tuneCents = clampPitchTuneCents(sourceFile, regionIndex, combinedTuneCents, diagnostics);

    if (differsFromDefault(region.pitchKeytrack, static_cast<float>(::sfz::Default::pitchKeytrack)))
        converted.pitchKeytrack = clampPitchKeytrack(sourceFile, regionIndex, region.pitchKeytrack, diagnostics);

    if (region.pitchEG && explicitOpcodes.pitchEnvelopeDepthCents && differsFromDefault(*explicitOpcodes.pitchEnvelopeDepthCents, 0.0f))
    {
        converted.pitchEnvelope =
            convertPitchEnvelope(sourceFile, regionIndex, *region.pitchEG, *explicitOpcodes.pitchEnvelopeDepthCents, diagnostics);
    }
    if (const auto pitchLfo = selectPitchLfo(explicitOpcodes))
    {
        if (isSupportedPitchLfoWave(*pitchLfo))
        {
            converted.pitchLfo = convertPitchLfo(sourceFile, regionIndex, *pitchLfo, diagnostics);
        }
        else
        {
            diagnostics.push_back(makeWarning(sourceFile, "unsupported-pitch-lfo",
                                              "Region " + std::to_string(regionIndex + 1) + " uses lfo" + std::to_string(pitchLfo->index) +
                                                  "_wave=" + std::to_string(*pitchLfo->wave) +
                                                  "; generated Lua maps only verified static pitch LFO waveforms 0, 1, 3, 6, 7, and 12."));
        }
    }

    (void)clampEnvelopeLevel(sourceFile, regionIndex, "ampeg_start", region.amplitudeEG.start, diagnostics);
    converted.ampEnvelope.start = 0.0f;
    converted.ampEnvelope.delay = clampEnvelopeDuration(sourceFile, regionIndex, "ampeg_delay", region.amplitudeEG.delay, diagnostics);
    converted.ampEnvelope.attack = clampEnvelopeDuration(sourceFile, regionIndex, "ampeg_attack", region.amplitudeEG.attack, diagnostics);
    converted.ampEnvelope.hold = clampEnvelopeDuration(sourceFile, regionIndex, "ampeg_hold", region.amplitudeEG.hold, diagnostics);
    converted.ampEnvelope.sustain = clampEnvelopeLevel(sourceFile, regionIndex, "ampeg_sustain", region.amplitudeEG.sustain, diagnostics);
    converted.ampEnvelope.decay = clampEnvelopeDuration(sourceFile, regionIndex, "ampeg_decay", region.amplitudeEG.decay, diagnostics);
    converted.ampEnvelope.decayZero = explicitOpcodes.decayZero.value_or(1);
    converted.ampEnvelope.release =
        clampEnvelopeDuration(sourceFile, regionIndex, "ampeg_release", region.amplitudeEG.release, diagnostics);

    if (region.loopMode && (*region.loopMode == ::sfz::LoopMode::loop_continuous || *region.loopMode == ::sfz::LoopMode::loop_sustain))
    {
        converted.hasLoop = true;
        converted.loopStart = explicitOpcodes.loopStart.value_or(region.loopRange.getStart());
        converted.loopEnd = explicitOpcodes.loopEnd.value_or(region.loopRange.getEnd());
        converted.loopMode = *region.loopMode == ::sfz::LoopMode::loop_sustain ? "sustain" : "continuous";
    }

    converted.ampVelocityToLevel =
        clampAmpVelocityToLevel(sourceFile, regionIndex, ::sfz::Default::ampVeltrack.denormalizeInput(region.ampVeltrack), diagnostics);
    const auto ampPan = ::sfz::Default::pan.denormalizeInput(region.pan);
    if (differsFromDefault(ampPan, 0.0f))
        converted.ampPan = clampAmpPan(sourceFile, regionIndex, ampPan, diagnostics);

    if (!region.filters.empty())
    {
        const auto& filter = region.filters.front();
        if (const auto mapping = roughFilterMapping(filter.type, explicitOpcodes.filterTypeText))
        {
            converted.filterType = kHalionClassicFilterType;
            converted.filterMode = mapping->mode;
            converted.filterShapeA = mapping->shapeA;
            converted.filterShapeB = mapping->shapeB;
            converted.filterCutoff = ::sfz::Default::filterCutoff.denormalizeInput(filter.cutoff) * mapping->cutoffScale;
            const auto sfzResonance = ::sfz::Default::filterResonance.denormalizeInput(filter.resonance);
            converted.filterResonance = filter.type == ::sfz::kFilterLpf2p
                                            ? mapLpf2pResonanceToHalion(sfzResonance)
                                            : mapBaselineResonanceToHalion(sfzResonance, mapping->resonanceAtSfzSix);
        }
        else
        {
            diagnostics.push_back(makeWarning(sourceFile, "unsupported-filter-type",
                                              "Region " + std::to_string(regionIndex + 1) +
                                                  " uses an SFZ filter type that has not been mapped to HALion output."));
        }
    }

    if (region.filterEG && explicitOpcodes.filterEnvelopeDepthCents && differsFromDefault(*explicitOpcodes.filterEnvelopeDepthCents, 0.0f))
    {
        if (converted.filterCutoff)
        {
            converted.filterEnvelope =
                convertFilterEnvelope(sourceFile, regionIndex, *region.filterEG, *explicitOpcodes.filterEnvelopeDepthCents, diagnostics);
        }
        else
        {
            diagnostics.push_back(makeWarning(sourceFile, "filter-envelope-unmapped",
                                              "Region " + std::to_string(regionIndex + 1) +
                                                  " uses fileg_depth, but its filter type is not mapped to HALion output."));
        }
    }

    return converted;
}

std::string luaInteger(const int64_t value)
{
    return std::to_string(value);
}

void appendEnvelopePointLua(std::ostringstream& lua, const float level, const float duration, const float curve)
{
    lua << "                { level = " << luaNumber(level) << ", duration = " << luaNumber(duration) << ", curve = " << luaNumber(curve)
        << " },\n";
}

int appendDecayEnvelopePointsLua(std::ostringstream& lua, const float sustain, const float decay, const int decayZero)
{
    if (decay <= 0.0f || sustain >= 1.0f)
    {
        appendEnvelopePointLua(lua, sustain, 0.0f, 0.0f);
        return 1;
    }

    if (sustain <= 0.0f)
    {
        const auto earlyDuration = decay * kHalionAmpReleaseEarlyScale;
        const auto totalDuration = decay * kHalionAmpReleaseTotalScale;
        const auto tailDuration = std::max(0.0f, totalDuration - earlyDuration);

        appendEnvelopePointLua(lua, kHalionAmpReleaseEarlyLevel, earlyDuration, kHalionAmpReleaseEarlyCurve);
        appendEnvelopePointLua(lua, 0.0f, tailDuration, kHalionAmpReleaseTailCurve);
        return 2;
    }

    const auto duration =
        decayZero == 0 ? decay * kHalionAmpReleaseTotalScale : decay * std::clamp(1.0f - sustain, 0.0f, 1.0f) * kHalionAmpDecayZeroOneScale;
    appendEnvelopePointLua(lua, sustain, duration, kHalionAmpReleaseTailCurve);
    return 1;
}

void appendReleaseEnvelopePointsLua(std::ostringstream& lua, const float release, const float releaseStartLevel)
{
    if (release <= 0.0f || releaseStartLevel <= 0.0f)
    {
        appendEnvelopePointLua(lua, 0.0f, 0.0f, 0.0f);
        return;
    }

    const auto earlyDuration = release * kHalionAmpReleaseEarlyScale;
    const auto totalDuration = release * kHalionAmpReleaseTotalScale;
    const auto tailDuration = std::max(0.0f, totalDuration - earlyDuration);

    appendEnvelopePointLua(lua, releaseStartLevel * kHalionAmpReleaseEarlyLevel, earlyDuration, kHalionAmpReleaseEarlyCurve);
    appendEnvelopePointLua(lua, 0.0f, tailDuration, kHalionAmpReleaseTailCurve);
}

void appendAmpEnvelopeLua(std::ostringstream& lua, const ConvertedRegion& region)
{
    auto sustainIndex = 0;
    auto pointIndex = 0;

    auto appendPoint = [&](const float level, const float duration, const float curve)
    {
        appendEnvelopePointLua(lua, level, duration, curve);
        ++pointIndex;
    };

    if (region.ampEnvelope.delay > 0.0f)
        ++sustainIndex;
    if (region.ampEnvelope.hold > 0.0f)
        ++sustainIndex;

    lua << "        amp_envelope = {\n"
        << "            points = {\n";

    appendPoint(region.ampEnvelope.start, 0.0f, 0.0f);

    if (region.ampEnvelope.delay > 0.0f)
        appendPoint(region.ampEnvelope.start, region.ampEnvelope.delay, 0.0f);

    appendPoint(1.0f, region.ampEnvelope.attack, region.ampEnvelope.attackCurve);

    if (region.ampEnvelope.hold > 0.0f)
        appendPoint(1.0f, region.ampEnvelope.hold, 0.0f);

    const auto decayPointCount =
        appendDecayEnvelopePointsLua(lua, region.ampEnvelope.sustain, region.ampEnvelope.decay, region.ampEnvelope.decayZero);
    pointIndex += decayPointCount;
    sustainIndex = pointIndex;

    appendReleaseEnvelopePointsLua(lua, region.ampEnvelope.release, region.ampEnvelope.sustain);

    lua << "            },\n"
        << "            sustain_index = " << sustainIndex << ",\n"
        << "        },\n";
}

void appendRegionLua(std::ostringstream& lua, const ConvertedRegion& region)
{
    lua << "    {\n"
        << "        name = " << luaQuotedString(region.name) << ",\n"
        << "        sample_playback = {\n"
        << "            sample = " << luaQuotedString(toGenericString(region.samplePath)) << ",\n";
    if (region.sampleOffset)
        lua << "            offset = " << luaInteger(*region.sampleOffset) << ",\n";
    if (region.sampleEnd)
        lua << "            finish = " << luaInteger(*region.sampleEnd) << ",\n";
    if (region.sampleNaturalEnd)
        lua << "            natural_end = " << luaInteger(*region.sampleNaturalEnd) << ",\n";
    lua << "        },\n"
        << "        mapping = {\n"
        << "            key_low = " << region.keyLow << ",\n"
        << "            key_high = " << region.keyHigh << ",\n"
        << "            velocity_low = " << region.velocityLow << ",\n"
        << "            velocity_high = " << region.velocityHigh << ",\n"
        << "            root_key = " << region.rootKey << ",\n"
        << "        },\n";

    appendAmpEnvelopeLua(lua, region);

    if (region.pitchEnvelope)
    {
        lua << "        pitch_envelope = {\n"
            << "            amount = " << luaNumber(region.pitchEnvelope->amount) << ",\n"
            << "            points = {\n";
        for (const auto& point : region.pitchEnvelope->envelope.points)
            lua << "                { level = " << luaNumber(point.level) << ", duration = " << luaNumber(point.duration)
                << ", curve = " << luaNumber(point.curve) << " },\n";
        lua << "            },\n"
            << "            sustain_index = " << region.pitchEnvelope->envelope.sustainIndex << ",\n"
            << "        },\n";
    }

    if (region.pitchLfo)
    {
        lua << "        pitch_lfo = {\n"
            << "            depth = " << luaNumber(region.pitchLfo->depth) << ",\n"
            << "            rate_hz = " << luaNumber(region.pitchLfo->rateHz) << ",\n"
            << "            phase_degrees = " << luaNumber(region.pitchLfo->phaseDegrees) << ",\n"
            << "            delay_ms = " << luaNumber(region.pitchLfo->delayMs) << ",\n"
            << "            fade_ms = " << luaNumber(region.pitchLfo->fadeMs) << ",\n"
            << "            waveform = " << region.pitchLfo->waveForm << ",\n"
            << "            shape = " << luaNumber(region.pitchLfo->shape) << ",\n"
            << "            pitch_offset_cents = " << luaNumber(region.pitchLfo->pitchOffsetCents) << ",\n"
            << "        },\n";
    }

    if (region.tuneCents || region.pitchKeytrack)
    {
        lua << "        pitch = {\n";
        if (region.tuneCents)
            lua << "            tune_cents = " << luaNumber(*region.tuneCents) << ",\n";
        if (region.pitchKeytrack)
            lua << "            keytrack = " << luaNumber(*region.pitchKeytrack) << ",\n";
        lua << "        },\n";
    }

    if (region.hasLoop)
    {
        lua << "        loop = {\n"
            << "            mode = " << luaQuotedString(region.loopMode) << ",\n"
            << "            start = " << luaInteger(region.loopStart) << ",\n"
            << "            finish = " << luaInteger(region.loopEnd) << ",\n"
            << "        },\n";
    }

    if (region.ampVelocityToLevel)
    {
        lua << "        gain = {\n"
            << "            sample_osc_level_db = " << luaNumber(*region.sampleOscLevelDb) << ",\n"
            << "            amp_velocity_to_level = " << luaNumber(*region.ampVelocityToLevel) << ",\n";
        if (region.ampPan)
            lua << "            amp_pan = " << luaNumber(*region.ampPan) << ",\n";
        lua << "        },\n";
    }

    if (region.filterCutoff)
    {
        lua << "        filter = {\n";
        if (region.filterType)
            lua << "            type = " << *region.filterType << ",\n";
        if (region.filterMode)
            lua << "            mode = " << *region.filterMode << ",\n";
        if (region.filterShapeA)
            lua << "            shape_a = " << *region.filterShapeA << ",\n";
        if (region.filterShapeB)
            lua << "            shape_b = " << *region.filterShapeB << ",\n";
        lua << "            cutoff = " << luaNumber(*region.filterCutoff) << ",\n";
        if (region.filterResonance)
            lua << "            resonance = " << luaNumber(*region.filterResonance) << ",\n";
        lua << "        },\n";
    }

    if (region.filterEnvelope)
    {
        lua << "        filter_envelope = {\n"
            << "            amount = " << luaNumber(region.filterEnvelope->amount) << ",\n"
            << "            points = {\n";
        for (const auto& point : region.filterEnvelope->envelope.points)
            lua << "                { level = " << luaNumber(point.level) << ", duration = " << luaNumber(point.duration)
                << ", curve = " << luaNumber(point.curve) << " },\n";
        lua << "            },\n"
            << "            sustain_index = " << region.filterEnvelope->envelope.sustainIndex << ",\n"
            << "        },\n";
    }

    lua << "    },\n";
}

std::string buildLuaSource(const ConvertedSfz& converted)
{
    std::ostringstream lua;
    lua << "local hb = require(\"halionbridge-sfz\")\n\n"
        << "local layerName = " << luaQuotedString(converted.layerName) << "\n"
        << "local outputFile = " << luaQuotedString(converted.presetFileName) << "\n"
        << "local progressInterval = 5\n"
        << "\n"
        << "-- This file was generated by halionbridge convert sfz. Region data is\n"
        << "-- grouped by source concern so converter authors can inspect the SFZ\n"
        << "-- sample playback, mapping, envelope, and optional tone fields before\n"
        << "-- HALion saves the preset.\n"
        << "local regions = {\n";

    for (const auto& region : converted.regions)
        appendRegionLua(lua, region);

    lua << "}\n\n"
        << "return function(ctx)\n"
        << "    ctx.log(\"Building \" .. layerName)\n\n"
        << "    local layer, layerErr = hb.create_layer(ctx, layerName)\n"
        << "    if not layer then\n"
        << "        return hb.fail(layerErr)\n"
        << "    end\n\n"
        << "    for i, region in ipairs(regions) do\n"
        << "        local label = hb.region_label(region)\n"
        << "        if i == 1 then\n"
        << "            ctx.progress(i - 1, #regions, \"Mapping \" .. label)\n"
        << "        end\n"
        << "        local zone, zoneErr = hb.append_sample_zone(ctx, layer, region)\n"
        << "        if not zone then\n"
        << "            return hb.fail(zoneErr)\n"
        << "        end\n"
        << "        if i == #regions or (i % progressInterval) == 0 then\n"
        << "            ctx.progress(i, #regions, \"Mapped \" .. label)\n"
        << "        end\n"
        << "    end\n\n"
        << "    ctx.progress(#regions, #regions + 1, \"Saving \" .. outputFile)\n"
        << "    local saved, saveErr = hb.save_layer_preset(ctx, layer, outputFile)\n"
        << "    if not saved then\n"
        << "        return hb.fail(saveErr)\n"
        << "    end\n\n"
        << "    ctx.progress(#regions + 1, #regions + 1, \"Saved \" .. outputFile)\n"
        << "    return hb.ok(\"Built \" .. outputFile, 1)\n"
        << "end\n";

    return lua.str();
}

std::optional<ConvertedSfz> loadSfz(const std::filesystem::path& sfzFile, const std::optional<std::string>& nameOverride,
                                    std::vector<Diagnostic>& diagnostics, int& regionsSkipped)
{
    ::sfz::Synth synth;
    if (!synth.loadSfzFile(::fs::path(sfzFile.string())))
    {
        diagnostics.push_back(makeError(sfzFile, "load-failed", "sfizz could not load " + sfzFile.string()));
        return std::nullopt;
    }

    for (const auto& opcode : synth.getUnknownOpcodes())
        diagnostics.push_back(makeWarning(sfzFile, "unsupported-opcode", "sfizz reported unsupported opcode: " + opcode));

    const auto explicitRegionOpcodes = collectExplicitRegionOpcodes(sfzFile, diagnostics);

    auto converted = ConvertedSfz{};
    converted.sourceFile = sfzFile;
    converted.layerName = nameOverride ? *nameOverride : displayNameFromStem(sfzFile.stem().string());
    converted.presetFileStem = safeFilenameStem(nameOverride ? *nameOverride : sfzFile.stem().string());
    converted.presetFileName = converted.presetFileStem + ".vstpreset";

    const auto numRegions = synth.getNumRegions();
    converted.regions.reserve(static_cast<size_t>(numRegions));

    for (int i = 0; i < numRegions; ++i)
    {
        const auto* region = synth.getRegionView(i);
        if (region == nullptr)
        {
            ++regionsSkipped;
            diagnostics.push_back(makeWarning(sfzFile, "region-missing", "sfizz returned an empty region view."));
            continue;
        }

        if (region->sampleId->filename().empty() || region->isGenerator())
        {
            ++regionsSkipped;
            diagnostics.push_back(makeWarning(sfzFile, "region-skipped", "Skipping region without a sample file."));
            continue;
        }

        const auto explicitOpcodes =
            static_cast<size_t>(i) < explicitRegionOpcodes.size() ? explicitRegionOpcodes[static_cast<size_t>(i)] : ExplicitRegionOpcodes{};
        for (const auto& opcodeName : explicitOpcodes.unsupportedAmpEnvelopeOpcodes)
        {
            diagnostics.push_back(makeWarning(sfzFile, "unsupported-amp-envelope",
                                              "Region " + std::to_string(i + 1) + " uses " + opcodeName +
                                                  "; generated Lua maps the static SFZ1 ampeg envelope but does not yet reproduce this "
                                                  "advanced envelope feature exactly."));
        }
        for (const auto& opcodeName : explicitOpcodes.unsupportedPitchEnvelopeOpcodes)
        {
            diagnostics.push_back(makeWarning(sfzFile, "unsupported-pitch-envelope",
                                              "Region " + std::to_string(i + 1) + " uses " + opcodeName +
                                                  "; generated Lua maps the current static pitcheg depth/attack/decay/hold/sustain "
                                                  "candidate but does not yet reproduce this advanced pitch envelope feature exactly."));
        }
        for (const auto& opcodeName : explicitOpcodes.unsupportedPitchLfoOpcodes)
        {
            diagnostics.push_back(makeWarning(
                sfzFile, "unsupported-pitch-lfo",
                "Region " + std::to_string(i + 1) + " uses " + opcodeName +
                    "; generated Lua maps only the verified static pitch LFO subset: pitch depth, rate, phase, delay, and fade."));
        }
        const auto pitchLfoCandidateCount =
            std::count_if(explicitOpcodes.pitchLfos.begin(), explicitOpcodes.pitchLfos.end(),
                          [](const ExplicitPitchLfo& lfo) { return lfo.depthCents && differsFromDefault(*lfo.depthCents, 0.0f); });
        if (pitchLfoCandidateCount > 1)
        {
            diagnostics.push_back(
                makeWarning(sfzFile, "unsupported-pitch-lfo",
                            "Region " + std::to_string(i + 1) +
                                " uses multiple pitch LFOs; generated Lua maps the first verified static pitch LFO only."));
        }
        for (const auto& opcodeName : explicitOpcodes.unsupportedFilterEnvelopeOpcodes)
        {
            diagnostics.push_back(makeWarning(sfzFile, "unsupported-filter-envelope",
                                              "Region " + std::to_string(i + 1) + " uses " + opcodeName +
                                                  "; generated Lua maps the current static fileg depth/attack/decay/hold/sustain "
                                                  "candidate but does not yet reproduce this advanced filter envelope feature exactly."));
        }

        converted.regions.push_back(convertRegion(sfzFile, i, *region, explicitOpcodes, diagnostics));
    }

    if (converted.regions.empty())
    {
        diagnostics.push_back(makeError(sfzFile, "no-regions", "No sample regions could be converted from " + sfzFile.string()));
        return std::nullopt;
    }

    return converted;
}

std::string helpText()
{
    return "Usage:\n"
           "  halionbridge convert sfz <source-path> [options]\n"
           "  halionbridge convert sfz <source-path> <output-directory> [options]\n\n"
           "<source-path> may be a single .sfz file or a directory containing .sfz files.\n"
           "When output-directory is omitted, generated Lua/build files are written beside the source .sfz file or flat into the source "
           "directory.\n\n"
           "Options:\n"
           "  --recursive             Convert .sfz files below a source directory recursively.\n"
           "  --overwrite             Replace existing generated Lua/build files.\n"
           "  --name <name>           Override the layer and preset basename. Only valid when one .sfz file is converted.\n"
           "  --help, -h              Show this help and exit.\n";
}

ConverterArgumentParseResult validateArguments(std::span<const std::string> args)
{
    auto result = ConverterArgumentParseResult{};
    std::vector<std::string> positional;
    auto recursive = false;

    const auto fail = [&result](const ConverterArgumentErrorKind kind, Diagnostic diagnostic)
    {
        result.exitCode = 1;
        result.errorKind = kind;
        result.diagnostics.push_back(std::move(diagnostic));
    };

    for (size_t i = 0; i < args.size(); ++i)
    {
        const auto& arg = args[i];

        if (arg == "--help" || arg == "-h")
            return result;

        if (arg == "--recursive")
        {
            recursive = true;
            continue;
        }

        if (arg == "--overwrite")
            continue;

        if (arg == "--name")
        {
            if (i + 1 >= args.size())
            {
                fail(ConverterArgumentErrorKind::syntax, makeError({}, "argument", "--name requires a value."));
                return result;
            }

            ++i;
            continue;
        }

        if (!arg.empty() && arg[0] == '-')
        {
            fail(ConverterArgumentErrorKind::syntax, makeError({}, "argument", "Unknown sfz converter argument: " + arg));
            return result;
        }

        positional.push_back(arg);
    }

    if (positional.empty() || positional.size() > 2)
    {
        fail(ConverterArgumentErrorKind::syntax,
             makeError({}, "argument", "halionbridge convert sfz requires a source path and optional output directory."));
        return result;
    }

    std::error_code error;
    if (!std::filesystem::exists(positional[0], error))
    {
        fail(ConverterArgumentErrorKind::validation,
             makeError(positional[0], "source-missing", "SFZ source path does not exist: " + positional[0]));
        return result;
    }

    error.clear();
    const auto isDirectory = std::filesystem::is_directory(positional[0], error);
    if (error)
    {
        fail(ConverterArgumentErrorKind::validation,
             makeError(positional[0], "source-inspection-failed",
                       "Could not inspect SFZ source path " + positional[0] + ": " + error.message()));
        return result;
    }

    error.clear();
    const auto isFile = std::filesystem::is_regular_file(positional[0], error);
    if (error)
    {
        fail(ConverterArgumentErrorKind::validation,
             makeError(positional[0], "source-inspection-failed",
                       "Could not inspect SFZ source path " + positional[0] + ": " + error.message()));
        return result;
    }

    if (!isDirectory && !isFile)
    {
        fail(ConverterArgumentErrorKind::validation,
             makeError(positional[0], "source-not-file-or-directory", "SFZ source path is not a file or directory: " + positional[0]));
        return result;
    }

    if (isFile && !hasSfzExtension(positional[0]))
    {
        fail(ConverterArgumentErrorKind::validation,
             makeError(positional[0], "source-not-sfz", "SFZ source file must have a .sfz extension: " + positional[0]));
        return result;
    }

    if (isFile && recursive)
    {
        fail(ConverterArgumentErrorKind::validation,
             makeError(positional[0], "recursive-with-file", "--recursive can only be used when the SFZ source path is a directory."));
        return result;
    }

    return result;
}

ConverterResult runConverterWithContext(std::span<const std::string> args, const ConverterRunContext& context)
{
    auto result = ConverterResult{};
    auto options = ConversionOptions{};
    options.context = &context;
    std::vector<std::string> positional;

    for (size_t i = 0; i < args.size(); ++i)
    {
        const auto& arg = args[i];
        if (arg == "--help" || arg == "-h")
        {
            result.exitCode = 0;
            result.diagnostics.push_back(Diagnostic{DiagnosticLevel::info, {}, 0, "help", helpText()});
            context.report(result.diagnostics.back());
            return result;
        }

        if (arg == "--recursive")
        {
            options.recursive = true;
            continue;
        }

        if (arg == "--overwrite")
        {
            options.overwrite = true;
            continue;
        }

        if (arg == "--name")
        {
            if (i + 1 >= args.size())
            {
                result.diagnostics.push_back(makeError({}, "argument", "--name requires a value."));
                context.report(result.diagnostics.back());
                return result;
            }

            options.name = args[++i];
            continue;
        }

        if (!arg.empty() && arg[0] == '-')
        {
            result.diagnostics.push_back(makeError({}, "argument", "Unknown sfz converter argument: " + arg));
            context.report(result.diagnostics.back());
            return result;
        }

        positional.push_back(arg);
    }

    if (positional.empty() || positional.size() > 2)
    {
        result.diagnostics.push_back(
            makeError({}, "argument", "halionbridge convert sfz requires a source path and optional output directory."));
        context.report(result.diagnostics.back());
        return result;
    }

    options.sourcePath = positional[0];
    options.outputDirectory =
        positional.size() == 2 ? std::filesystem::path{positional[1]} : defaultOutputDirectoryForSourcePath(options.sourcePath);

    const auto conversion = convertSource(options);
    result.diagnostics = conversion.diagnostics;
    result.exitCode = conversion.succeeded ? 0 : 1;
    if (conversion.succeeded)
    {
        result.diagnostics.push_back(Diagnostic{DiagnosticLevel::info, conversion.buildFile, 0, "generated",
                                                "Generated " + std::to_string(conversion.generatedLuaFiles.size()) +
                                                    " Lua file(s), including " + std::to_string(conversion.sfzFilesConverted) +
                                                    " build script(s), from " + std::to_string(conversion.sfzFilesConverted) +
                                                    " SFZ file(s)."});
        context.report(result.diagnostics.back());
    }

    return result;
}

ConverterResult runConverter(std::span<const std::string> args)
{
    return runConverterWithContext(args, ConverterRunContext{});
}

} // namespace

ConversionResult convertSource(const ConversionOptions& options)
{
    auto result = ConversionResult{};
    size_t reportedDiagnostics = 0;

    auto reportPendingDiagnostics = [&]()
    {
        if (options.context == nullptr)
        {
            reportedDiagnostics = result.diagnostics.size();
            return;
        }

        for (; reportedDiagnostics < result.diagnostics.size(); ++reportedDiagnostics)
            options.context->report(result.diagnostics[reportedDiagnostics]);
    };

    auto addDiagnostic = [&](Diagnostic diagnostic)
    {
        result.diagnostics.push_back(std::move(diagnostic));
        reportPendingDiagnostics();
    };

    const auto sourcePath = configuredSourcePath(options);
    const auto outputDirectory =
        options.outputDirectory.empty() ? defaultOutputDirectoryForSourcePath(sourcePath) : options.outputDirectory;

    std::error_code error;
    if (!std::filesystem::exists(sourcePath, error))
    {
        result.diagnostics.push_back(makeError(sourcePath, "source-missing", "SFZ source path does not exist: " + sourcePath.string()));
        reportPendingDiagnostics();
        return result;
    }

    error.clear();
    const auto isSourceDirectory = std::filesystem::is_directory(sourcePath, error);
    if (error)
    {
        result.diagnostics.push_back(makeError(sourcePath, "source-inspection-failed",
                                               "Could not inspect SFZ source path " + sourcePath.string() + ": " + error.message()));
        reportPendingDiagnostics();
        return result;
    }

    error.clear();
    const auto isSourceFile = std::filesystem::is_regular_file(sourcePath, error);
    if (error)
    {
        result.diagnostics.push_back(makeError(sourcePath, "source-inspection-failed",
                                               "Could not inspect SFZ source path " + sourcePath.string() + ": " + error.message()));
        reportPendingDiagnostics();
        return result;
    }

    if (!isSourceDirectory && !isSourceFile)
    {
        result.diagnostics.push_back(
            makeError(sourcePath, "source-not-file-or-directory", "SFZ source path is not a file or directory: " + sourcePath.string()));
        reportPendingDiagnostics();
        return result;
    }

    auto sfzFiles = std::vector<std::filesystem::path>{};
    if (isSourceFile)
    {
        if (!hasSfzExtension(sourcePath))
        {
            result.diagnostics.push_back(
                makeError(sourcePath, "source-not-sfz", "SFZ source file must have a .sfz extension: " + sourcePath.string()));
            reportPendingDiagnostics();
            return result;
        }

        if (options.recursive)
        {
            result.diagnostics.push_back(
                makeError(sourcePath, "recursive-with-file", "--recursive can only be used when the SFZ source path is a directory."));
            reportPendingDiagnostics();
            return result;
        }

        addDiagnostic(makeInfo(sourcePath, "scan-complete", "Using SFZ file " + sourcePath.string() + "."));
        sfzFiles.push_back(sourcePath);
    }
    else
    {
        addDiagnostic(makeInfo(sourcePath, "scan-started", "Scanning " + sourcePath.string() + " for SFZ files."));
        auto searchResult = findSfzFiles(sourcePath, options.recursive, options.context);
        result.diagnostics.insert(result.diagnostics.end(), searchResult.diagnostics.begin(), searchResult.diagnostics.end());
        reportPendingDiagnostics();
        if (!searchResult.diagnostics.empty())
            return result;

        if (shouldStop(options.context))
        {
            addDiagnostic(makeError(sourcePath, "stopped", "Conversion stopped by user request."));
            return result;
        }

        sfzFiles = std::move(searchResult.files);
    }

    if (sfzFiles.empty())
    {
        result.diagnostics.push_back(makeError(sourcePath, "no-sfz",
                                               "No .sfz files were found in " + sourcePath.string() +
                                                   (options.recursive ? "." : ". Use --recursive to include nested directories.")));
        reportPendingDiagnostics();
        return result;
    }

    if (isSourceDirectory)
        addDiagnostic(makeInfo(sourcePath, "scan-complete",
                               "Found " + std::to_string(sfzFiles.size()) + " SFZ file(s) in " + sourcePath.string() + "."));

    if (options.name && sfzFiles.size() != 1)
    {
        result.diagnostics.push_back(
            makeError(sourcePath, "name-with-multiple-files", "--name can only be used when exactly one .sfz file is converted."));
        reportPendingDiagnostics();
        return result;
    }

    std::vector<ConvertedSfz> convertedFiles;
    convertedFiles.reserve(sfzFiles.size());

    for (size_t i = 0; i < sfzFiles.size(); ++i)
    {
        if (shouldStop(options.context))
        {
            addDiagnostic(makeError(sfzFiles[i], "stopped", "Conversion stopped by user request."));
            return result;
        }

        addDiagnostic(
            makeInfo(sfzFiles[i], "convert-started",
                     "Converting " + std::to_string(i + 1) + "/" + std::to_string(sfzFiles.size()) + ": " + sfzFiles[i].string()));
        auto converted = loadSfz(sfzFiles[i], options.name, result.diagnostics, result.regionsSkipped);
        reportPendingDiagnostics();
        if (!converted)
            continue;

        ++result.sfzFilesConverted;
        result.regionsConverted += static_cast<int>(converted->regions.size());
        addDiagnostic(makeInfo(sfzFiles[i], "convert-complete",
                               "Converted " + sfzFiles[i].string() + " with " + std::to_string(converted->regions.size()) + " region(s)."));
        convertedFiles.push_back(std::move(*converted));
    }

    if (convertedFiles.empty())
    {
        result.diagnostics.push_back(makeError(sourcePath, "no-generated-scripts", "No Lua build scripts were generated."));
        reportPendingDiagnostics();
        return result;
    }

    auto originalPresetFileNames = std::vector<std::string>();
    originalPresetFileNames.reserve(convertedFiles.size());
    for (const auto& converted : convertedFiles)
        originalPresetFileNames.push_back(converted.presetFileName);

    assignUniquePresetFileNames(convertedFiles);
    for (size_t i = 0; i < convertedFiles.size(); ++i)
    {
        if (convertedFiles[i].presetFileName != originalPresetFileNames[i])
        {
            addDiagnostic(makeInfo(convertedFiles[i].sourceFile, "preset-name-disambiguated",
                                   "Disambiguated duplicate output preset filename '" + originalPresetFileNames[i] + "' to '" +
                                       convertedFiles[i].presetFileName + "'."));
        }
    }

    if (shouldStop(options.context))
    {
        addDiagnostic(makeError(outputDirectory, "stopped", "Conversion stopped by user request before writing generated files."));
        return result;
    }

    std::vector<GeneratedLuaScript> scripts;
    scripts.reserve(convertedFiles.size() + 1);
    scripts.push_back(
        GeneratedLuaScript{"", kSfzHelperLuaFileName, std::string{detail::kSfzHelperLuaSource}, GeneratedLuaFileRole::helperModule});

    for (size_t i = 0; i < convertedFiles.size(); ++i)
    {
        const auto baseName = safeFilenameStem(convertedFiles[i].sourceFile.stem().string());
        const auto moduleName = zeroPaddedIndex(i) + "_" + baseName + ".lua";
        scripts.push_back(GeneratedLuaScript{moduleName, moduleName, buildLuaSource(convertedFiles[i])});
    }

    addDiagnostic(makeInfo(outputDirectory, "write-started",
                           "Writing " + std::to_string(scripts.size()) + " generated Lua file(s) to " + outputDirectory.string() + "."));
    auto emitResult = writeBuildDirectory(BuildDirectoryRequest{outputDirectory, options.overwrite, scripts});
    result.buildFile = emitResult.buildFile;
    result.generatedLuaFiles = emitResult.generatedLuaFiles;
    result.diagnostics.insert(result.diagnostics.end(), emitResult.diagnostics.begin(), emitResult.diagnostics.end());
    reportPendingDiagnostics();
    result.succeeded = emitResult.succeeded;
    return result;
}

ConversionResult convertDirectory(const ConversionOptions& options)
{
    return convertSource(options);
}

void registerConverter(ConverterRegistry& registry)
{
    auto definition = ConverterDefinition{"sfz",
                                          "SFZ",
                                          "Generate HALion Lua build scripts from SFZ files or directories.",
                                          runConverter,
                                          runConverterWithContext,
                                          helpText,
                                          validateArguments};
    definition.sourcePathKind = ConverterSourcePathKind::fileOrDirectory;
    registry.registerConverter(std::move(definition));
}

} // namespace halionbridge::converters::sfz
