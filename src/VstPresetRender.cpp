#include "VstPresetRender.h"

#include "PathUtils.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <tuple>

namespace halionbridge::detail
{
namespace
{
constexpr std::int64_t kMaximumMidiFileSize = 128LL * 1024LL * 1024LL;
constexpr std::size_t kMaximumOutputFilenameUnits = 240;
constexpr double kSilenceThreshold = 1.0e-9;

bool portableFilenameFits(const juce::String& filename)
{
    return static_cast<std::size_t>(filename.length()) <= kMaximumOutputFilenameUnits &&
           static_cast<std::size_t>(filename.getNumBytesAsUTF8()) <= kMaximumOutputFilenameUnits;
}

std::string lowercaseAscii(std::string value)
{
    std::ranges::transform(value, value.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string comparablePathString(const std::filesystem::path& path)
{
    return toJuceString(path.lexically_normal()).toLowerCase().toStdString();
}

bool isRegularNonSymlinkFile(const std::filesystem::path& path, std::string& error)
{
    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(toFilesystemAccessPath(path), filesystemError);
    if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
    {
        error = "Expected an existing regular non-symlink file: " + path.string();
        return false;
    }
    return true;
}

bool isMidiExtension(const std::filesystem::path& path)
{
    const auto extension = lowercaseAscii(path.extension().string());
    return extension == ".mid" || extension == ".midi";
}

bool isVstPresetExtension(const std::filesystem::path& path)
{
    return lowercaseAscii(path.extension().string()) == ".vstpreset";
}

template <typename Value>
void sortAndValidatePortableDuplicates(std::vector<Value>& values, std::vector<std::string>& errors, const std::string_view kind)
{
    std::ranges::sort(values,
                      [](const Value& left, const Value& right)
                      {
                          const auto leftComparable = comparablePathString(left);
                          const auto rightComparable = comparablePathString(right);
                          if (leftComparable != rightComparable)
                              return leftComparable < rightComparable;
                          return toJuceString(left.lexically_normal()).compare(toJuceString(right.lexically_normal())) < 0;
                      });

    auto unique = std::vector<Value>{};
    unique.reserve(values.size());
    for (const auto& value : values)
    {
        if (!unique.empty() && value.lexically_normal() == unique.back().lexically_normal())
            continue;
        if (!unique.empty() && comparablePathString(value) == comparablePathString(unique.back()))
        {
            errors.push_back("Portable " + std::string(kind) + " path collision between " + unique.back().string() + " and " +
                             value.string() + ".");
            continue;
        }
        unique.push_back(value);
    }
    values = std::move(unique);
}

std::vector<std::filesystem::path> collectFilesFromDirectory(const std::filesystem::path& directory, const bool recursive, const bool midi,
                                                             std::vector<std::string>& errors)
{
    auto files = std::vector<std::filesystem::path>{};
    auto filesystemError = std::error_code{};
    constexpr auto options = std::filesystem::directory_options::none;

    const auto inspect = [&](const auto& entry)
    {
        const auto status = entry.symlink_status(filesystemError);
        if (filesystemError)
        {
            errors.push_back("Could not inspect input entry " + entry.path().string() + ": " + filesystemError.message());
            filesystemError.clear();
            return;
        }
        if (std::filesystem::is_symlink(status))
            return;
        if (std::filesystem::is_regular_file(status) && (midi ? isMidiExtension(entry.path()) : isVstPresetExtension(entry.path())))
            files.push_back(entry.path());
    };

    if (recursive)
    {
        for (auto iterator = std::filesystem::recursive_directory_iterator(directory, options, filesystemError);
             !filesystemError && iterator != std::filesystem::recursive_directory_iterator{}; iterator.increment(filesystemError))
            inspect(*iterator);
    }
    else
    {
        for (auto iterator = std::filesystem::directory_iterator(directory, options, filesystemError);
             !filesystemError && iterator != std::filesystem::directory_iterator{}; iterator.increment(filesystemError))
            inspect(*iterator);
    }

    if (filesystemError)
        errors.push_back("Could not enumerate input directory " + directory.string() + ": " + filesystemError.message());

    sortAndValidatePortableDuplicates(files, errors, midi ? "MIDI" : "VSTPreset");
    return files;
}

double ppqAtTime(const std::vector<MidiRenderTempoPoint>& tempoPoints, const double timeSeconds)
{
    const auto upper = std::ranges::upper_bound(tempoPoints, timeSeconds, {}, &MidiRenderTempoPoint::timeSeconds);
    const auto& point = upper == tempoPoints.begin() ? tempoPoints.front() : *std::prev(upper);
    return point.ppqPosition + (timeSeconds - point.timeSeconds) * point.bpm / 60.0;
}

class MidiRenderPlayHead final : public juce::AudioPlayHead
{
  public:
    MidiRenderPlayHead(const MidiRenderSequence& sequenceIn, const double sampleRateIn) : sequence(sequenceIn), sampleRate(sampleRateIn) {}

    void setSamplePosition(const std::int64_t value) noexcept
    {
        samplePosition = value;
    }

    juce::Optional<PositionInfo> getPosition() const override
    {
        const auto seconds = static_cast<double>(samplePosition) / sampleRate;
        const auto tempoUpper = std::ranges::upper_bound(sequence.tempoPoints, seconds, {}, &MidiRenderTempoPoint::timeSeconds);
        const auto& tempo = tempoUpper == sequence.tempoPoints.begin() ? sequence.tempoPoints.front() : *std::prev(tempoUpper);
        const auto signatureUpper =
            std::ranges::upper_bound(sequence.timeSignaturePoints, seconds, {}, &MidiRenderTimeSignaturePoint::timeSeconds);
        const auto& signature =
            signatureUpper == sequence.timeSignaturePoints.begin() ? sequence.timeSignaturePoints.front() : *std::prev(signatureUpper);

        const auto ppq = ppqAtTime(sequence.tempoPoints, seconds);
        const auto quarterNotesPerBar = static_cast<double>(signature.numerator) * 4.0 / signature.denominator;
        const auto barsSinceSignature = std::floor((ppq - signature.ppqPosition) / quarterNotesPerBar);

        auto result = PositionInfo{};
        result.setTimeInSamples(samplePosition);
        result.setTimeInSeconds(seconds);
        result.setBpm(tempo.bpm);
        result.setTimeSignature(TimeSignature{signature.numerator, signature.denominator});
        result.setPpqPosition(ppq);
        result.setPpqPositionOfLastBarStart(signature.ppqPosition + barsSinceSignature * quarterNotesPerBar);
        result.setIsPlaying(true);
        result.setIsRecording(false);
        result.setIsLooping(false);
        return result;
    }

  private:
    const MidiRenderSequence& sequence;
    double sampleRate = 48000.0;
    std::int64_t samplePosition = 0;
};

std::int64_t eventSamplePosition(const double timeSeconds, const int sampleRate)
{
    return static_cast<std::int64_t>(std::llround(timeSeconds * sampleRate));
}

std::int64_t nextTransportBoundary(const MidiRenderSequence& midi, const int sampleRate, const std::int64_t after,
                                   const std::int64_t fallback)
{
    auto result = fallback;
    const auto update = [&](const double seconds)
    {
        const auto sample = eventSamplePosition(seconds, sampleRate);
        if (sample > after)
            result = std::min(result, sample);
    };
    for (const auto& point : midi.tempoPoints)
        update(point.timeSeconds);
    for (const auto& point : midi.timeSignaturePoints)
        update(point.timeSeconds);
    return result;
}

bool writeBlock(juce::AudioFormatWriter& writer, const juce::AudioBuffer<float>& buffer, const int start, const int count,
                VstPresetAudioRenderMetrics& metrics, long double& sumSquares, std::string& error)
{
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto* samples = buffer.getReadPointer(channel, start);
        for (int sample = 0; sample < count; ++sample)
        {
            const auto value = static_cast<double>(samples[sample]);
            if (!std::isfinite(value))
            {
                error = "HALion produced a non-finite audio sample.";
                return false;
            }
            metrics.peak = std::max(metrics.peak, std::abs(value));
            sumSquares += static_cast<long double>(value) * static_cast<long double>(value);
        }
    }

    if (!writer.writeFromAudioSampleBuffer(buffer, start, count))
    {
        error = "Could not write rendered audio to the temporary WAV file.";
        return false;
    }
    return true;
}

} // namespace

std::optional<std::string> sha256RenderInputFile(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    if (!isRegularNonSymlinkFile(path, error))
        return std::nullopt;
    auto stream = toJuceFile(toFilesystemAccessPath(path)).createInputStream();
    if (stream == nullptr || !stream->openedOk())
    {
        error = "Could not open input for SHA-256 hashing: " + path.string();
        return std::nullopt;
    }
    return juce::SHA256(*stream).toHexString().toStdString();
}

VstPresetRenderSourceCollection collectVstPresetRenderSources(const std::filesystem::path& input, const bool recursive)
{
    auto result = VstPresetRenderSourceCollection{};
    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(toFilesystemAccessPath(input), filesystemError);
    if (filesystemError || std::filesystem::is_symlink(status))
    {
        result.errors.push_back("VSTPreset input is missing, inaccessible, or symlinked: " + input.string());
        return result;
    }

    auto files = std::vector<std::filesystem::path>{};
    auto relativeRoot = input.parent_path();
    if (std::filesystem::is_regular_file(status))
    {
        if (!isVstPresetExtension(input))
            result.errors.push_back("VSTPreset input file must use the .vstpreset extension: " + input.string());
        else
            files.push_back(input);
    }
    else if (std::filesystem::is_directory(status))
    {
        relativeRoot = input;
        files = collectFilesFromDirectory(input, recursive, false, result.errors);
    }
    else
    {
        result.errors.push_back("VSTPreset input must be a regular file or directory: " + input.string());
        return result;
    }

    for (const auto& file : files)
    {
        auto error = std::string{};
        const auto hash = sha256RenderInputFile(file, error);
        if (!hash)
        {
            result.errors.push_back(error);
            continue;
        }
        auto relative = std::filesystem::relative(file, relativeRoot, filesystemError);
        if (filesystemError || relative.empty() || relative.is_absolute())
        {
            result.errors.push_back("Could not determine a safe relative VSTPreset path for " + file.string());
            filesystemError.clear();
            continue;
        }
        result.files.push_back({file, relative.lexically_normal(), *hash});
    }

    if (result.files.empty() && result.errors.empty())
        result.errors.push_back("No .vstpreset files were found under: " + input.string());
    return result;
}

std::optional<MidiRenderSequence> parseMidiRenderSequence(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    if (!isMidiExtension(path))
    {
        error = "MIDI input file must use the .mid or .midi extension: " + path.string();
        return std::nullopt;
    }
    if (!isRegularNonSymlinkFile(path, error))
        return std::nullopt;

    const auto file = toJuceFile(toFilesystemAccessPath(path));
    if (file.getSize() <= 0 || file.getSize() > kMaximumMidiFileSize)
    {
        error = "MIDI input is empty or exceeds the 128 MiB safety limit: " + path.string();
        return std::nullopt;
    }
    auto stream = file.createInputStream();
    if (stream == nullptr || !stream->openedOk())
    {
        error = "Could not open MIDI input: " + path.string();
        return std::nullopt;
    }

    auto midi = juce::MidiFile{};
    auto fileType = -1;
    if (!midi.readFrom(*stream, false, &fileType))
    {
        error = "Invalid Standard MIDI File: " + path.string();
        return std::nullopt;
    }
    if (fileType != 0 && fileType != 1)
    {
        error = "MIDI format " + std::to_string(fileType) + " is not supported; use format 0 or 1: " + path.string();
        return std::nullopt;
    }
    if (midi.getTimeFormat() == 0)
    {
        error = "MIDI file has an invalid zero time format: " + path.string();
        return std::nullopt;
    }

    struct OriginalTimestamp
    {
        int track = 0;
        int event = 0;
        double timestamp = 0.0;
    };
    auto originalTimestamps = std::vector<OriginalTimestamp>{};
    for (int trackIndex = 0; trackIndex < midi.getNumTracks(); ++trackIndex)
    {
        const auto* track = midi.getTrack(trackIndex);
        if (track == nullptr)
            continue;
        for (int eventIndex = 0; eventIndex < track->getNumEvents(); ++eventIndex)
        {
            const auto* holder = track->getEventPointer(eventIndex);
            if (holder == nullptr || !std::isfinite(holder->message.getTimeStamp()) || holder->message.getTimeStamp() < 0.0)
            {
                error = "MIDI contains an invalid negative or non-finite timestamp: " + path.string();
                return std::nullopt;
            }
            const auto& message = holder->message;
            if (message.isSysEx())
            {
                error = "MIDI SysEx events are not supported: " + path.string();
                return std::nullopt;
            }
            if (message.isProgramChange())
            {
                error = "MIDI Program Change events are not supported: " + path.string();
                return std::nullopt;
            }
            if (message.isController() && (message.getControllerNumber() == 0 || message.getControllerNumber() == 32))
            {
                error = "MIDI bank-select CC0/CC32 events are not supported: " + path.string();
                return std::nullopt;
            }
            originalTimestamps.push_back({trackIndex, eventIndex, message.getTimeStamp()});
        }
    }

    const auto timeFormat = midi.getTimeFormat();
    midi.convertTimestampTicksToSeconds();

    auto result = MidiRenderSequence{};
    result.sourcePath = path;
    result.label = toJuceString(path.stem()).toStdString();
    result.fileType = fileType;
    result.timeFormat = timeFormat;
    result.tempoPoints.push_back({0.0, 0.0, 120.0});
    result.timeSignaturePoints.push_back({0.0, 0.0, 4, 4});

    for (const auto& original : originalTimestamps)
    {
        const auto* track = midi.getTrack(original.track);
        const auto* holder = track != nullptr ? track->getEventPointer(original.event) : nullptr;
        if (holder == nullptr || !std::isfinite(holder->message.getTimeStamp()) || holder->message.getTimeStamp() < 0.0)
        {
            error = "MIDI timestamp conversion failed: " + path.string();
            return std::nullopt;
        }
        const auto& message = holder->message;
        const auto seconds = message.getTimeStamp();
        result.durationSeconds = std::max(result.durationSeconds, seconds);

        const auto ppq = timeFormat > 0 ? original.timestamp / static_cast<double>(timeFormat) : 0.0;
        if (message.isTempoMetaEvent())
        {
            const auto secondsPerQuarter = message.getTempoSecondsPerQuarterNote();
            if (!std::isfinite(secondsPerQuarter) || secondsPerQuarter <= 0.0)
            {
                error = "MIDI contains an invalid tempo event: " + path.string();
                return std::nullopt;
            }
            result.tempoPoints.push_back({seconds, ppq, 60.0 / secondsPerQuarter});
        }
        else if (message.isTimeSignatureMetaEvent())
        {
            auto numerator = 0;
            auto denominator = 0;
            message.getTimeSignatureInfo(numerator, denominator);
            if (numerator <= 0 || denominator <= 0)
            {
                error = "MIDI contains an invalid time-signature event: " + path.string();
                return std::nullopt;
            }
            result.timeSignaturePoints.push_back({seconds, ppq, numerator, denominator});
        }
        else if (!message.isMetaEvent())
        {
            result.events.push_back({message, seconds, original.track, original.event});
        }
    }

    std::ranges::stable_sort(result.tempoPoints, {}, &MidiRenderTempoPoint::timeSeconds);
    std::ranges::stable_sort(result.timeSignaturePoints, {}, &MidiRenderTimeSignaturePoint::timeSeconds);
    std::ranges::stable_sort(result.events,
                             [](const MidiRenderEvent& left, const MidiRenderEvent& right)
                             {
                                 return std::tie(left.timeSeconds, left.trackIndex, left.eventIndex) <
                                        std::tie(right.timeSeconds, right.trackIndex, right.eventIndex);
                             });

    // A change at time zero supersedes the default. Later same-time events use stable track/event order and the last one wins.
    const auto compactSameTime = [](auto& points)
    {
        auto compacted = std::remove_reference_t<decltype(points)>{};
        for (const auto& point : points)
        {
            if (!compacted.empty() && compacted.back().timeSeconds == point.timeSeconds)
                compacted.back() = point;
            else
                compacted.push_back(point);
        }
        points = std::move(compacted);
    };
    compactSameTime(result.tempoPoints);
    compactSameTime(result.timeSignaturePoints);
    if (timeFormat < 0)
    {
        for (std::size_t index = 1; index < result.tempoPoints.size(); ++index)
        {
            const auto& previous = result.tempoPoints[index - 1];
            auto& current = result.tempoPoints[index];
            current.ppqPosition = previous.ppqPosition + (current.timeSeconds - previous.timeSeconds) * previous.bpm / 60.0;
        }
        for (auto& signature : result.timeSignaturePoints)
            signature.ppqPosition = ppqAtTime(result.tempoPoints, signature.timeSeconds);
    }

    const auto hash = sha256RenderInputFile(path, error);
    if (!hash)
        return std::nullopt;
    result.sha256 = *hash;
    return result;
}

MidiRenderCollection collectMidiRenderSequences(const std::vector<std::filesystem::path>& inputs)
{
    auto result = MidiRenderCollection{};
    auto files = std::vector<std::filesystem::path>{};
    for (const auto& input : inputs)
    {
        auto filesystemError = std::error_code{};
        const auto status = std::filesystem::symlink_status(toFilesystemAccessPath(input), filesystemError);
        if (filesystemError || std::filesystem::is_symlink(status))
        {
            result.errors.push_back("MIDI input is missing, inaccessible, or symlinked: " + input.string());
        }
        else if (std::filesystem::is_regular_file(status))
        {
            files.push_back(input);
        }
        else if (std::filesystem::is_directory(status))
        {
            auto directoryFiles = collectFilesFromDirectory(input, false, true, result.errors);
            files.insert(files.end(), directoryFiles.begin(), directoryFiles.end());
        }
        else
        {
            result.errors.push_back("MIDI input must be a regular file or directory: " + input.string());
        }
    }
    sortAndValidatePortableDuplicates(files, result.errors, "MIDI");

    auto labels = std::map<std::string, std::filesystem::path>{};
    for (const auto& file : files)
    {
        auto error = std::string{};
        auto parsed = parseMidiRenderSequence(file, error);
        if (!parsed)
        {
            result.errors.push_back(error);
            continue;
        }
        const auto key = juce::String::fromUTF8(parsed->label.c_str()).toLowerCase().toStdString();
        if (const auto existing = labels.find(key); existing != labels.end())
        {
            result.errors.push_back("MIDI output label collision between " + existing->second.string() + " and " + file.string() + ".");
            continue;
        }
        labels.emplace(key, file);
        result.files.push_back(std::move(*parsed));
    }
    if (result.files.empty() && result.errors.empty())
        result.errors.push_back("No .mid or .midi files were found in the selected MIDI inputs.");
    return result;
}

std::filesystem::path makeVstPresetRenderOutputPath(const VstPresetRenderSource& preset, const MidiRenderSequence& midi, std::string& error)
{
    error.clear();
    auto presetStem = toJuceString(preset.sourcePath.stem());
    auto midiStem = juce::String::fromUTF8(midi.label.c_str());
    auto filename = presetStem + "__" + midiStem + ".wav";
    if (!portableFilenameFits(filename))
    {
        const auto identity = toJuceString(preset.sourcePath.lexically_normal()) + "\n" + juce::String(preset.sha256) + "\n" +
                              toJuceString(midi.sourcePath.lexically_normal()) + "\n" + juce::String(midi.sha256);
        const auto identityHash =
            juce::SHA256(identity.toRawUTF8(), static_cast<std::size_t>(identity.getNumBytesAsUTF8())).toHexString().substring(0, 12);
        const auto suffix = juce::String("~") + identityHash + ".wav";
        filename = presetStem + "__" + midiStem + suffix;
        while (!portableFilenameFits(filename) && (presetStem.isNotEmpty() || midiStem.isNotEmpty()))
        {
            if (presetStem.getNumBytesAsUTF8() >= midiStem.getNumBytesAsUTF8() && presetStem.isNotEmpty())
                presetStem = presetStem.substring(0, presetStem.length() - 1);
            else if (midiStem.isNotEmpty())
                midiStem = midiStem.substring(0, midiStem.length() - 1);
            filename = presetStem + "__" + midiStem + suffix;
        }
        if (!portableFilenameFits(filename))
        {
            error = "Could not form a portable WAV output filename for " + preset.sourcePath.string() + " and " + midi.sourcePath.string() +
                    ".";
            return {};
        }
    }
    return toStdPath(toJuceFile(preset.sourcePath.parent_path()).getChildFile(filename));
}

std::optional<std::int64_t> calculateVstPresetRenderSampleCount(const double midiDurationSeconds, const double tailSeconds,
                                                                const int sampleRate, std::string& error)
{
    error.clear();
    if (!std::isfinite(midiDurationSeconds) || midiDurationSeconds < 0.0 || !std::isfinite(tailSeconds) || tailSeconds < 0.0 ||
        sampleRate <= 0)
    {
        error = "Cannot calculate render length from invalid duration, tail, or sample rate.";
        return std::nullopt;
    }
    const auto samples = std::ceil((midiDurationSeconds + tailSeconds) * sampleRate);
    if (!std::isfinite(samples) || samples > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
    {
        error = "Requested render length exceeds the supported sample count.";
        return std::nullopt;
    }
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(samples));
}

bool validateRenderedWav(const std::filesystem::path& path, const VstPresetAudioRenderConfig& config, const std::int64_t expectedSamples,
                         std::string& error)
{
    error.clear();
    auto format = juce::WavAudioFormat{};
    auto input = toJuceFile(toFilesystemAccessPath(path)).createInputStream();
    auto reader = std::unique_ptr<juce::AudioFormatReader>(format.createReaderFor(input.release(), true));
    if (reader == nullptr)
    {
        error = "Rendered temporary file is not a readable WAV: " + path.string();
        return false;
    }
    if (reader->numChannels != 2 || std::llround(reader->sampleRate) != config.sampleRate ||
        static_cast<int>(reader->bitsPerSample) != config.bitDepth || reader->lengthInSamples != expectedSamples)
    {
        error = "Rendered WAV header does not match the requested stereo format and sample count: " + path.string();
        return false;
    }
    if (config.bitDepth == 32 && !reader->usesFloatingPointData)
    {
        error = "Rendered 32-bit WAV is not IEEE floating point: " + path.string();
        return false;
    }
    return true;
}

VstPresetAudioRenderResult renderVstPresetMidiToWav(juce::AudioProcessor& processor, const MidiRenderSequence& midi,
                                                    const std::filesystem::path& outputPath, const VstPresetAudioRenderConfig& config)
{
    auto result = VstPresetAudioRenderResult{};
    if (config.sampleRate <= 0 || (config.bitDepth != 16 && config.bitDepth != 24 && config.bitDepth != 32) || config.maximumBlockSize <= 0)
    {
        result.error = "Invalid audio render configuration.";
        return result;
    }
    const auto expectedSamples =
        calculateVstPresetRenderSampleCount(midi.durationSeconds, config.tailSeconds, config.sampleRate, result.error);
    if (!expectedSamples)
        return result;

    const auto target = toJuceFile(toFilesystemAccessPath(outputPath));
    if (!config.overwrite && target.exists())
    {
        result.error = "Output WAV already exists: " + outputPath.string();
        return result;
    }
    const auto parent = target.getParentDirectory();
    if (!parent.isDirectory())
    {
        result.error = "Output WAV parent directory does not exist: " + outputPath.parent_path().string();
        return result;
    }

    auto temporary = juce::TemporaryFile(target);
    auto stream = std::unique_ptr<juce::OutputStream>(temporary.getFile().createOutputStream());
    if (stream == nullptr)
    {
        result.error = "Could not create temporary WAV beside: " + outputPath.string();
        return result;
    }
    auto writerOptions = juce::AudioFormatWriterOptions{}
                             .withSampleRate(config.sampleRate)
                             .withChannelLayout(juce::AudioChannelSet::stereo())
                             .withBitsPerSample(config.bitDepth)
                             .withSampleFormat(config.bitDepth == 32 ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                                     : juce::AudioFormatWriterOptions::SampleFormat::integral);
    auto wav = juce::WavAudioFormat{};
    auto writer = wav.createWriterFor(stream, writerOptions);
    if (writer == nullptr)
    {
        result.error = "Could not create the requested WAV writer.";
        return result;
    }

    result.metrics.sampleCount = *expectedSamples;
    result.metrics.latencySamples = std::max(0, processor.getLatencySamples());
    const auto processingSamples = *expectedSamples + result.metrics.latencySamples;
    const auto channels = std::max({2, processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels()});
    auto buffer = juce::AudioBuffer<float>(channels, config.maximumBlockSize);
    auto playHead = MidiRenderPlayHead(midi, config.sampleRate);
    processor.setPlayHead(&playHead);

    auto eventIndex = std::size_t{};
    auto processed = std::int64_t{};
    auto sumSquares = static_cast<long double>(0.0);
    auto processingOk = true;
    while (processed < processingSamples && processingOk)
    {
        auto blockEnd = std::min(processingSamples, processed + config.maximumBlockSize);
        blockEnd = nextTransportBoundary(midi, config.sampleRate, processed, blockEnd);
        const auto blockSize = static_cast<int>(blockEnd - processed);
        if (blockSize <= 0)
        {
            result.error = "Internal transport boundary did not advance during rendering.";
            processingOk = false;
            break;
        }

        buffer.setSize(channels, blockSize, false, false, true);
        buffer.clear();
        auto midiBuffer = juce::MidiBuffer{};
        while (eventIndex < midi.events.size())
        {
            const auto sample = eventSamplePosition(midi.events[eventIndex].timeSeconds, config.sampleRate);
            if (sample >= blockEnd)
                break;
            if (sample >= processed)
                midiBuffer.addEvent(midi.events[eventIndex].message, static_cast<int>(sample - processed));
            ++eventIndex;
        }

        playHead.setSamplePosition(processed);
        processor.processBlock(buffer, midiBuffer);
        if (processor.getTotalNumOutputChannels() == 1)
            buffer.copyFrom(1, 0, buffer, 0, 0, blockSize);

        const auto outputStart = std::max(processed, static_cast<std::int64_t>(result.metrics.latencySamples));
        const auto outputEnd = std::min(blockEnd, static_cast<std::int64_t>(result.metrics.latencySamples) + *expectedSamples);
        if (outputEnd > outputStart)
        {
            const auto offset = static_cast<int>(outputStart - processed);
            const auto count = static_cast<int>(outputEnd - outputStart);
            processingOk = writeBlock(*writer, buffer, offset, count, result.metrics, sumSquares, result.error);
        }
        processed = blockEnd;
    }
    processor.setPlayHead(nullptr);

    if (!processingOk || !writer->flush())
    {
        if (result.error.empty())
            result.error = "Could not finalize the rendered WAV file.";
        return result;
    }
    writer.reset();
    stream.reset();

    if (!validateRenderedWav(toStdPath(temporary.getFile()), config, *expectedSamples, result.error))
        return result;
    if (!config.overwrite && target.exists())
    {
        result.error = "Output WAV appeared while rendering; refusing to replace it: " + outputPath.string();
        return result;
    }
    if (!temporary.overwriteTargetFileWithTemporary())
    {
        result.error = "Could not atomically publish rendered WAV: " + outputPath.string();
        return result;
    }

    const auto valueCount = static_cast<long double>(*expectedSamples) * 2.0L;
    result.metrics.rms = std::sqrt(static_cast<double>(sumSquares / valueCount));
    result.metrics.silent = result.metrics.peak <= kSilenceThreshold;
    result.metrics.clipped = result.metrics.peak >= 1.0;
    result.succeeded = true;
    return result;
}

} // namespace halionbridge::detail
