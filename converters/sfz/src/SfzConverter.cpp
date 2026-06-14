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
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

namespace halionbridge::converters::sfz
{
namespace
{

constexpr const char* kSfzHelperLuaFileName = "halionbridge-sfz.lua";
constexpr float kHalionSfzLevelCompensationDb = 7.8f;

struct ConvertedRegion
{
    std::string name;
    std::filesystem::path samplePath;
    int keyLow = 0;
    int keyHigh = 127;
    int velocityLow = 0;
    int velocityHigh = 127;
    int rootKey = 60;
    std::optional<float> tuneCents;
    std::optional<float> pitchKeytrack;
    bool hasLoop = false;
    int64_t loopStart = 0;
    int64_t loopEnd = 0;
    std::optional<float> sampleOscLevelDb;
    std::optional<float> ampVelocityToLevel;
    std::optional<float> ampPan;
    std::optional<float> filterCutoff;
    struct
    {
        float start = 0.0f;
        float delay = 0.0f;
        float attack = 0.0f;
        float hold = 0.0f;
        float decay = 0.0f;
        float sustain = 1.0f;
        float release = 0.001f;
        float attackCurve = 0.0f;
        float decayCurve = 0.0f;
        float releaseCurve = 0.0f;
    } ampEnvelope;
};

struct ConvertedSfz
{
    std::filesystem::path sourceFile;
    std::string layerName;
    std::string presetFileName;
    std::vector<ConvertedRegion> regions;
};

struct SfzFileSearchResult
{
    std::vector<std::filesystem::path> files;
    std::vector<Diagnostic> diagnostics;
};

struct ExplicitRegionOpcodes
{
    std::optional<int64_t> loopStart;
    std::optional<int64_t> loopEnd;
    std::optional<int> decayZero;
    std::set<std::string> unsupportedAmpEnvelopeOpcodes;
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
            const auto opcode = rawOpcode.cleanUp(::sfz::kOpcodeScopeRegion);
            if (opcode.name == "loop_start")
                explicitOpcodes.loopStart = opcode.read(::sfz::Default::loopStart);
            else if (opcode.name == "loop_end")
                explicitOpcodes.loopEnd = opcode.read(::sfz::Default::loopEnd);
            else if (opcode.name == "ampeg_decay_zero")
                explicitOpcodes.decayZero = readZeroFlag(opcode.value);

            if (isUnsupportedAmpEnvelopeOpcode(opcode.name))
                explicitOpcodes.unsupportedAmpEnvelopeOpcodes.insert(opcode.name);
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

    static bool startsWith(const std::string& text, const std::string_view prefix)
    {
        return text.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin());
    }

    static bool isSupportedStaticAmpEnvelopeOpcode(const std::string& name)
    {
        static const auto supported = std::set<std::string>{"ampeg_attack", "ampeg_decay",   "ampeg_decay_zero", "ampeg_delay",
                                                            "ampeg_hold",   "ampeg_release", "ampeg_start",      "ampeg_sustain"};
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

std::string toGenericString(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
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

std::string sanitizeIdentifier(std::string text)
{
    for (auto& c : text)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
        else
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    while (text.find("__") != std::string::npos)
        text.replace(text.find("__"), 2, "_");

    while (!text.empty() && text.front() == '_')
        text.erase(text.begin());

    while (!text.empty() && text.back() == '_')
        text.pop_back();

    return text.empty() ? "sfz_instrument" : text;
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

bool hasSfzExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".sfz";
}

template <typename Iterator>
void collectSfzFiles(Iterator iterator, const Iterator end, std::vector<std::filesystem::path>& files, std::vector<Diagnostic>& diagnostics)
{
    std::error_code error;
    while (iterator != end)
    {
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

SfzFileSearchResult findSfzFiles(const std::filesystem::path& sourceDirectory, const bool recursive)
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

        collectSfzFiles(iterator, std::filesystem::recursive_directory_iterator{}, result.files, result.diagnostics);
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

        collectSfzFiles(iterator, std::filesystem::directory_iterator{}, result.files, result.diagnostics);
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

float decayDurationToSustain(const float decay, const float sustain, const int decayZero)
{
    if (decayZero == 0)
        return decay;

    return decay * std::clamp(1.0f - sustain, 0.0f, 1.0f);
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
    diagnostics.push_back(makeWarning(sourceFile, "pitch-tune-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " combined transpose/tune value " +
                                          std::to_string(value) + " cents is outside HALion's SampleOsc.Tune -1200..1200 cent range; using " +
                                          std::to_string(clamped) + "."));
    return clamped;
}

float clampPitchKeytrack(const std::filesystem::path& sourceFile, const int regionIndex, const float value,
                         std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= -200.0f && value <= 200.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, -200.0f, 200.0f) : 100.0f;
    diagnostics.push_back(makeWarning(sourceFile, "pitch-keytrack-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " pitch_keytrack value " +
                                          std::to_string(value) + " is outside HALion's Pitch.KeyFollow -200..200 percent range; using " +
                                          std::to_string(clamped) + "."));
    return clamped;
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
                                      "Region " + std::to_string(regionIndex + 1) + " amp_veltrack value " +
                                          std::to_string(value) + " is outside SFZ's -100..100 percent range; using " +
                                          std::to_string(clamped) + "."));
    return clamped;
}

float clampAmpPan(const std::filesystem::path& sourceFile, const int regionIndex, const float value,
                  std::vector<Diagnostic>& diagnostics)
{
    if (std::isfinite(value) && value >= -100.0f && value <= 100.0f)
        return value;

    const auto clamped = std::isfinite(value) ? std::clamp(value, -100.0f, 100.0f) : 0.0f;
    diagnostics.push_back(makeWarning(sourceFile, "pan-clamped",
                                      "Region " + std::to_string(regionIndex + 1) + " pan value " + std::to_string(value) +
                                          " is outside HALion's Amp.Pan -100..100 range; using " + std::to_string(clamped) + "."));
    return clamped;
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
    converted.sampleOscLevelDb =
        clampSampleOscLevelDb(sourceFile, regionIndex, region.volume + kHalionSfzLevelCompensationDb, diagnostics);

    const auto combinedTuneCents = region.pitch + (region.transpose * 100.0f);
    if (differsFromDefault(combinedTuneCents, 0.0f))
        converted.tuneCents = clampPitchTuneCents(sourceFile, regionIndex, combinedTuneCents, diagnostics);

    if (differsFromDefault(region.pitchKeytrack, static_cast<float>(::sfz::Default::pitchKeytrack)))
        converted.pitchKeytrack = clampPitchKeytrack(sourceFile, regionIndex, region.pitchKeytrack, diagnostics);

    converted.ampEnvelope.start = clampEnvelopeLevel(sourceFile, regionIndex, "ampeg_start", region.amplitudeEG.start, diagnostics);
    converted.ampEnvelope.delay = clampEnvelopeDuration(sourceFile, regionIndex, "ampeg_delay", region.amplitudeEG.delay, diagnostics);
    converted.ampEnvelope.attack = clampEnvelopeDuration(sourceFile, regionIndex, "ampeg_attack", region.amplitudeEG.attack, diagnostics);
    converted.ampEnvelope.hold = clampEnvelopeDuration(sourceFile, regionIndex, "ampeg_hold", region.amplitudeEG.hold, diagnostics);
    converted.ampEnvelope.sustain = clampEnvelopeLevel(sourceFile, regionIndex, "ampeg_sustain", region.amplitudeEG.sustain, diagnostics);
    const auto decay = clampEnvelopeDuration(sourceFile, regionIndex, "ampeg_decay", region.amplitudeEG.decay, diagnostics);
    converted.ampEnvelope.decay = decayDurationToSustain(decay, converted.ampEnvelope.sustain, explicitOpcodes.decayZero.value_or(1));
    converted.ampEnvelope.release =
        clampEnvelopeDuration(sourceFile, regionIndex, "ampeg_release", region.amplitudeEG.release, diagnostics);

    if (region.loopMode && (*region.loopMode == ::sfz::LoopMode::loop_continuous || *region.loopMode == ::sfz::LoopMode::loop_sustain))
    {
        converted.hasLoop = true;
        converted.loopStart = explicitOpcodes.loopStart.value_or(region.loopRange.getStart());
        converted.loopEnd = explicitOpcodes.loopEnd.value_or(region.loopRange.getEnd());
    }

    converted.ampVelocityToLevel =
        clampAmpVelocityToLevel(sourceFile, regionIndex, ::sfz::Default::ampVeltrack.denormalizeInput(region.ampVeltrack), diagnostics);
    const auto ampPan = ::sfz::Default::pan.denormalizeInput(region.pan);
    if (differsFromDefault(ampPan, 0.0f))
        converted.ampPan = clampAmpPan(sourceFile, regionIndex, ampPan, diagnostics);

    if (!region.filters.empty())
        converted.filterCutoff = ::sfz::Default::filterCutoff.denormalizeInput(region.filters.front().cutoff);

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

void appendAmpEnvelopeLua(std::ostringstream& lua, const ConvertedRegion& region)
{
    auto sustainIndex = 3;
    if (region.ampEnvelope.delay > 0.0f)
        ++sustainIndex;
    if (region.ampEnvelope.hold > 0.0f)
        ++sustainIndex;

    lua << "        amp_envelope = {\n"
        << "            points = {\n";

    appendEnvelopePointLua(lua, region.ampEnvelope.start, 0.0f, 0.0f);

    if (region.ampEnvelope.delay > 0.0f)
        appendEnvelopePointLua(lua, region.ampEnvelope.start, region.ampEnvelope.delay, 0.0f);

    appendEnvelopePointLua(lua, 1.0f, region.ampEnvelope.attack, region.ampEnvelope.attackCurve);

    if (region.ampEnvelope.hold > 0.0f)
        appendEnvelopePointLua(lua, 1.0f, region.ampEnvelope.hold, 0.0f);

    appendEnvelopePointLua(lua, region.ampEnvelope.sustain, region.ampEnvelope.decay, region.ampEnvelope.decayCurve);
    appendEnvelopePointLua(lua, 0.0f, region.ampEnvelope.release, region.ampEnvelope.releaseCurve);

    lua << "            },\n"
        << "            sustain_index = " << sustainIndex << ",\n"
        << "        },\n";
}

void appendRegionLua(std::ostringstream& lua, const ConvertedRegion& region)
{
    lua << "    {\n"
        << "        name = " << luaQuotedString(region.name) << ",\n"
        << "        sample_playback = {\n"
        << "            sample = " << luaQuotedString(toGenericString(region.samplePath)) << ",\n"
        << "        },\n"
        << "        mapping = {\n"
        << "            key_low = " << region.keyLow << ",\n"
        << "            key_high = " << region.keyHigh << ",\n"
        << "            velocity_low = " << region.velocityLow << ",\n"
        << "            velocity_high = " << region.velocityHigh << ",\n"
        << "            root_key = " << region.rootKey << ",\n"
        << "        },\n";

    appendAmpEnvelopeLua(lua, region);

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
        lua << "        filter = {\n"
            << "            cutoff = " << luaNumber(*region.filterCutoff) << ",\n"
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
        << "        ctx.progress(i - 1, #regions, \"Mapping \" .. label)\n"
        << "        local zone, zoneErr = hb.append_sample_zone(ctx, layer, region)\n"
        << "        if not zone then\n"
        << "            return hb.fail(zoneErr)\n"
        << "        end\n"
        << "        ctx.progress(i, #regions, \"Mapped \" .. label)\n"
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
    converted.presetFileName = sanitizeIdentifier(nameOverride ? *nameOverride : sfzFile.stem().string()) + ".vstpreset";

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
           "  halionbridge convert sfz <source-directory> <output-directory> [options]\n\n"
           "Options:\n"
           "  --recursive             Convert .sfz files below the source directory recursively.\n"
           "  --overwrite             Replace existing generated Lua/build files.\n"
           "  --name <name>           Override the layer and preset basename. Only valid when one .sfz file is converted.\n"
           "  --help, -h              Show this help and exit.\n";
}

ConverterResult runConverter(std::span<const std::string> args)
{
    auto result = ConverterResult{};
    auto options = ConversionOptions{};
    std::vector<std::string> positional;

    for (size_t i = 0; i < args.size(); ++i)
    {
        const auto& arg = args[i];
        if (arg == "--help" || arg == "-h")
        {
            result.exitCode = 0;
            result.diagnostics.push_back(Diagnostic{DiagnosticLevel::info, {}, 0, "help", helpText()});
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
                return result;
            }

            options.name = args[++i];
            continue;
        }

        if (!arg.empty() && arg[0] == '-')
        {
            result.diagnostics.push_back(makeError({}, "argument", "Unknown sfz converter argument: " + arg));
            return result;
        }

        positional.push_back(arg);
    }

    if (positional.size() != 2)
    {
        result.diagnostics.push_back(
            makeError({}, "argument", "halionbridge convert sfz requires a source directory and output directory."));
        return result;
    }

    options.sourceDirectory = positional[0];
    options.outputDirectory = positional[1];

    const auto conversion = convertDirectory(options);
    result.diagnostics = conversion.diagnostics;
    result.exitCode = conversion.succeeded ? 0 : 1;
    if (conversion.succeeded)
    {
        result.diagnostics.push_back(Diagnostic{DiagnosticLevel::info, conversion.buildFile, 0, "generated",
                                                "Generated " + std::to_string(conversion.generatedLuaFiles.size()) +
                                                    " Lua file(s), including " + std::to_string(conversion.sfzFilesConverted) +
                                                    " build script(s), from " + std::to_string(conversion.sfzFilesConverted) +
                                                    " SFZ file(s)."});
    }

    return result;
}

} // namespace

ConversionResult convertDirectory(const ConversionOptions& options)
{
    auto result = ConversionResult{};

    std::error_code error;
    if (!std::filesystem::exists(options.sourceDirectory, error))
    {
        result.diagnostics.push_back(makeError(options.sourceDirectory, "source-missing",
                                               "SFZ source directory does not exist: " + options.sourceDirectory.string()));
        return result;
    }

    if (!std::filesystem::is_directory(options.sourceDirectory, error))
    {
        result.diagnostics.push_back(makeError(options.sourceDirectory, "source-not-directory",
                                               "SFZ source path is not a directory: " + options.sourceDirectory.string()));
        return result;
    }

    auto searchResult = findSfzFiles(options.sourceDirectory, options.recursive);
    result.diagnostics.insert(result.diagnostics.end(), searchResult.diagnostics.begin(), searchResult.diagnostics.end());
    if (!searchResult.diagnostics.empty())
        return result;

    const auto& sfzFiles = searchResult.files;
    if (sfzFiles.empty())
    {
        result.diagnostics.push_back(makeError(options.sourceDirectory, "no-sfz",
                                               "No .sfz files were found in " + options.sourceDirectory.string() +
                                                   (options.recursive ? "." : ". Use --recursive to include nested directories.")));
        return result;
    }

    if (options.name && sfzFiles.size() != 1)
    {
        result.diagnostics.push_back(makeError(options.sourceDirectory, "name-with-multiple-files",
                                               "--name can only be used when exactly one .sfz file is converted."));
        return result;
    }

    std::vector<ConvertedSfz> convertedFiles;
    convertedFiles.reserve(sfzFiles.size());

    for (size_t i = 0; i < sfzFiles.size(); ++i)
    {
        auto converted = loadSfz(sfzFiles[i], options.name, result.diagnostics, result.regionsSkipped);
        if (!converted)
            continue;

        ++result.sfzFilesConverted;
        result.regionsConverted += static_cast<int>(converted->regions.size());
        convertedFiles.push_back(std::move(*converted));
    }

    if (convertedFiles.empty())
    {
        result.diagnostics.push_back(makeError(options.sourceDirectory, "no-generated-scripts", "No Lua build scripts were generated."));
        return result;
    }

    std::map<std::string, std::filesystem::path> presetNames;
    for (const auto& converted : convertedFiles)
    {
        const auto key = caseInsensitiveKey(converted.presetFileName);
        const auto [it, inserted] = presetNames.emplace(key, converted.sourceFile);
        if (!inserted)
        {
            result.diagnostics.push_back(makeError(converted.sourceFile, "duplicate-preset-name",
                                                   "SFZ files generate the same preset filename '" + converted.presetFileName +
                                                       "': " + it->second.string() + " and " + converted.sourceFile.string()));
            result.succeeded = false;
            return result;
        }
    }

    std::vector<GeneratedLuaScript> scripts;
    scripts.reserve(convertedFiles.size() + 1);
    scripts.push_back(GeneratedLuaScript{"", kSfzHelperLuaFileName, std::string{detail::kSfzHelperLuaSource},
                                         GeneratedLuaFileRole::helperModule});

    for (size_t i = 0; i < convertedFiles.size(); ++i)
    {
        const auto baseName = sanitizeIdentifier(convertedFiles[i].sourceFile.stem().string());
        const auto moduleName = zeroPaddedIndex(i) + "_" + baseName + ".lua";
        scripts.push_back(GeneratedLuaScript{moduleName, moduleName, buildLuaSource(convertedFiles[i])});
    }

    auto emitResult = writeBuildDirectory(BuildDirectoryRequest{options.outputDirectory, options.overwrite, scripts});
    result.buildFile = emitResult.buildFile;
    result.generatedLuaFiles = emitResult.generatedLuaFiles;
    result.diagnostics.insert(result.diagnostics.end(), emitResult.diagnostics.begin(), emitResult.diagnostics.end());
    result.succeeded = emitResult.succeeded;
    return result;
}

void registerConverter(ConverterRegistry& registry)
{
    registry.registerConverter(
        ConverterDefinition{"sfz", "SFZ", "Generate HALion Lua build scripts from SFZ directories.", runConverter, helpText});
}

} // namespace halionbridge::converters::sfz
