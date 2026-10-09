#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace juce
{
class AudioProcessor;
}

namespace halionbridge::detail
{

inline constexpr int kVstPresetRenderRevision = 2;
inline constexpr auto kVstPresetMidiResetPolicy = "all-channels-cc120-121-123-v1";

bool verifyVstPresetMidiResetTransport(juce::AudioProcessor& processor, int maximumBlockSize, const std::function<bool()>& shouldStop,
                                       std::string& error);

// Host operations stay on the caller's message thread. The pump must service
// messages for the requested wall-clock interval; audio preparation is discarded.
struct VstPresetRenderPreparationHooks
{
    std::function<bool(std::string&)> restorePreset;
    std::function<bool(std::string&)> validateMidiReset;
    std::function<void(int)> pumpMessages;
    std::function<bool()> shouldStop;
};

bool prepareVstPresetRender(juce::AudioProcessor& processor, int sampleRate, int maximumBlockSize, int settleMilliseconds,
                            const VstPresetRenderPreparationHooks& hooks, std::string& error);

struct VstPresetRenderSource
{
    std::filesystem::path sourcePath;
    std::filesystem::path relativePath;
    std::string sha256;
};

struct VstPresetRenderSourceCollection
{
    std::vector<VstPresetRenderSource> files;
    std::vector<std::string> errors;
};

struct MidiRenderEvent
{
    juce::MidiMessage message;
    double timeSeconds = 0.0;
    int trackIndex = 0;
    int eventIndex = 0;
};

struct MidiRenderTempoPoint
{
    double timeSeconds = 0.0;
    double ppqPosition = 0.0;
    double bpm = 120.0;
};

struct MidiRenderTimeSignaturePoint
{
    double timeSeconds = 0.0;
    double ppqPosition = 0.0;
    int numerator = 4;
    int denominator = 4;
};

struct MidiRenderSequence
{
    std::filesystem::path sourcePath;
    std::string label;
    std::string sha256;
    int fileType = 0;
    short timeFormat = 0;
    double durationSeconds = 0.0;
    std::vector<MidiRenderEvent> events;
    std::vector<MidiRenderTempoPoint> tempoPoints;
    std::vector<MidiRenderTimeSignaturePoint> timeSignaturePoints;
};

struct MidiRenderCollection
{
    std::vector<MidiRenderSequence> files;
    std::vector<std::string> errors;
};

struct VstPresetAudioRenderConfig
{
    int sampleRate = 48000;
    int bitDepth = 24;
    double tailSeconds = 0.0;
    int maximumBlockSize = 512;
    bool overwrite = false;
};

struct VstPresetAudioRenderMetrics
{
    std::int64_t sampleCount = 0;
    int latencySamples = 0;
    double peak = 0.0;
    double rms = 0.0;
    bool silent = true;
    bool clipped = false;
};

struct VstPresetAudioRenderResult
{
    bool succeeded = false;
    VstPresetAudioRenderMetrics metrics;
    std::string error;
};

VstPresetRenderSourceCollection collectVstPresetRenderSources(const std::filesystem::path& input, bool recursive);
MidiRenderCollection collectMidiRenderSequences(const std::vector<std::filesystem::path>& inputs);
std::optional<MidiRenderSequence> parseMidiRenderSequence(const std::filesystem::path& path, std::string& error);
std::optional<std::string> sha256RenderInputFile(const std::filesystem::path& path, std::string& error);

std::filesystem::path makeVstPresetRenderOutputPath(const VstPresetRenderSource& preset, const MidiRenderSequence& midi,
                                                    std::string& error);
std::optional<std::int64_t> calculateVstPresetRenderSampleCount(double midiDurationSeconds, double tailSeconds, int sampleRate,
                                                                std::string& error);

VstPresetAudioRenderResult renderVstPresetMidiToWav(juce::AudioProcessor& processor, const MidiRenderSequence& midi,
                                                    const std::filesystem::path& outputPath, const VstPresetAudioRenderConfig& config);
bool validateRenderedWav(const std::filesystem::path& path, const VstPresetAudioRenderConfig& config, std::int64_t expectedSamples,
                         std::string& error);

} // namespace halionbridge::detail
