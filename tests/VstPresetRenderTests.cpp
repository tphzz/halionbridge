#include "halionbridge/Bridge.h"
#include "CliCommand.h"
#include "PathUtils.h"
#include "VstPresetRender.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

namespace
{

class LatencyImpulseProcessor : public juce::AudioProcessor
{
  public:
    LatencyImpulseProcessor() : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
        setLatencySamples(17);
    }

    const juce::String getName() const override
    {
        return "LatencyImpulseProcessor";
    }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    double getTailLengthSeconds() const override
    {
        return 0.0;
    }
    bool acceptsMidi() const override
    {
        return true;
    }
    bool producesMidi() const override
    {
        return false;
    }
    juce::AudioProcessorEditor* createEditor() override
    {
        return nullptr;
    }
    bool hasEditor() const override
    {
        return false;
    }
    int getNumPrograms() override
    {
        return 1;
    }
    int getCurrentProgram() override
    {
        return 0;
    }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override
    {
        return {};
    }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        if (const auto* currentPlayHead = getPlayHead())
        {
            if (const auto position = currentPlayHead->getPosition(); position && position->getTimeInSamples())
                playHeadSamples.push_back(*position->getTimeInSamples());
        }

        for (const auto metadata : midi)
            if (metadata.getMessage().isNoteOn())
                pendingSamples.push_back(processedSamples + metadata.samplePosition + getLatencySamples());

        while (!pendingSamples.empty() && pendingSamples.front() < processedSamples + buffer.getNumSamples())
        {
            const auto offset = static_cast<int>(pendingSamples.front() - processedSamples);
            if (offset >= 0)
                for (int channel = 0; channel < std::min(2, buffer.getNumChannels()); ++channel)
                    buffer.setSample(channel, offset, 0.5f);
            pendingSamples.pop_front();
        }
        processedSamples += buffer.getNumSamples();
    }

    std::vector<std::int64_t> playHeadSamples;

  private:
    std::int64_t processedSamples = 0;
    std::deque<std::int64_t> pendingSamples;
};

// Preset restoration is deliberately asynchronous here: neither an engine reset,
// message pumping alone, nor MIDI-file pre-roll can activate the requested state.
class DelayedPresetProcessor final : public LatencyImpulseProcessor
{
  public:
    struct ChannelState
    {
        bool sounding = true;
        bool heldNote = true;
        bool sustain = true;
        int modulation = 127;
        int pitchBend = 16383;
        int pressure = 127;
        int volume = 91;
        int pan = 35;
    };

    void reset() override
    {
        ++resetCount;
        activePreset = 0;
        pendingPreset = 0;
    }

    bool restore(const int preset)
    {
        cleanBeforeRestore = isClean();
        ++restoreCount;
        pendingPreset = preset;
        pumpedSinceRestore = false;
        channels.fill(ChannelState{});
        return true;
    }

    void pump(const int milliseconds)
    {
        pumpedSinceRestore = true;
        pumpDurations.push_back(milliseconds);
    }

    bool isClean() const
    {
        for (const auto& channel : channels)
            if (channel.sounding || channel.heldNote || channel.sustain || channel.modulation != 0 || channel.pitchBend != 8192 ||
                channel.pressure != 0 || channel.volume != 91 || channel.pan != 35)
                return false;
        return true;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        ++processCount;
        maximumObservedBlockSize = std::max(maximumObservedBlockSize, buffer.getNumSamples());
        if (!recording)
        {
            if (const auto* currentPlayHead = getPlayHead())
                if (const auto position = currentPlayHead->getPosition())
                    preparationTransportWasPlaying |= position->getIsPlaying();
            silentPreparationInput &= buffer.getMagnitude(0, buffer.getNumSamples()) == 0.0f;
        }

        if (pendingPreset != 0 && pumpedSinceRestore && midi.isEmpty())
        {
            activePreset = pendingPreset;
            pendingPreset = 0;
            setLatencySamples(29);
        }

        for (const auto metadata : midi)
        {
            const auto message = metadata.getMessage();
            if (recording)
            {
                if (message.isNoteOn())
                {
                    noteStartedClean &= isClean();
                    notePreset = activePreset;
                }
                continue;
            }

            if (!message.isController() || message.getChannel() < 1 || message.getChannel() > 16)
            {
                unexpectedPreparationMidi = true;
                continue;
            }
            auto& channel = channels[static_cast<std::size_t>(message.getChannel() - 1)];
            const auto controller = message.getControllerNumber();
            if (controller == 120)
                channel.sounding = false;
            else if (controller == 121)
            {
                channel.sustain = false;
                channel.modulation = 0;
                channel.pitchBend = 8192;
                channel.pressure = 0;
            }
            else if (controller == 123)
                channel.heldNote = false;
            else
                unexpectedPreparationMidi = true;
            if (message.getControllerValue() != 0)
                unexpectedPreparationMidi = true;
        }
        LatencyImpulseProcessor::processBlock(buffer, midi);
    }

    std::array<ChannelState, 16> channels{};
    std::vector<int> pumpDurations;
    int activePreset = 7;
    int pendingPreset = 0;
    int resetCount = 0;
    int restoreCount = 0;
    int processCount = 0;
    int maximumObservedBlockSize = 0;
    int notePreset = 0;
    bool cleanBeforeRestore = false;
    bool recording = false;
    bool noteStartedClean = true;
    bool silentPreparationInput = true;
    bool preparationTransportWasPlaying = false;
    bool unexpectedPreparationMidi = false;

  private:
    bool pumpedSinceRestore = false;
};

class MappedResetProcessor final : public LatencyImpulseProcessor
{
  public:
    enum class Mapping
    {
        valid,
        missingParameter,
        droppedMessage,
        duplicateChannel,
        ambiguousChange
    };

    explicit MappedResetProcessor(const Mapping mappingMode = Mapping::valid, const int familySize = 16) : mapping(mappingMode)
    {
        constexpr std::array names{"Contr. 120", "Reset Ctrl", "AllNoteOff"};
        for (std::size_t family = 0; family < names.size(); ++family)
            for (int channel = 0; channel < familySize; ++channel)
            {
                if (mapping == Mapping::missingParameter && family == 1 && channel == 15)
                    continue;
                auto parameter = std::make_unique<juce::AudioParameterFloat>(
                    juce::ParameterID{juce::String(static_cast<int>(family)) + "_" + juce::String(channel), 1}, names[family],
                    juce::NormalisableRange<float>{0.0f, 1.0f}, 0.1f + static_cast<float>(channel) * 0.01f);
                resetParameters[family].push_back(parameter.get());
                addParameter(parameter.release());
            }
        for (const auto* name : {"Volume", "Reset Ctrl Amount", "Contr. 120 Extended"})
        {
            auto parameter = std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{name, 1}, name,
                                                                         juce::NormalisableRange<float>{0.0f, 1.0f}, 0.73f);
            unrelatedParameters.push_back(parameter.get());
            addParameter(parameter.release());
        }
        originalValues = values();
    }

    std::vector<float> values() const
    {
        auto result = std::vector<float>{};
        for (const auto* parameter : getParameters())
            result.push_back(parameter->getValue());
        return result;
    }

    void changeMapping(const Mapping mappingMode)
    {
        mapping = mappingMode;
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        ++processCount;
        maximumObservedBlockSize = std::max(maximumObservedBlockSize, buffer.getNumSamples());
        silentInput &= buffer.getMagnitude(0, buffer.getNumSamples()) == 0.0f;
        for (const auto* parameter : unrelatedParameters)
            unrelatedControlsUntouched &= parameter->getValue() == 0.73f;
        temporaryValuesObserved |= values() != originalValues;
        if (temporaryValuesObserved && midi.isEmpty() && values() == originalValues)
            restorationDrained = true;

        for (const auto metadata : midi)
        {
            ++messageCount;
            const auto message = metadata.getMessage();
            if (!message.isController() || message.getControllerValue() != 0)
            {
                unexpectedMidi = true;
                continue;
            }
            const auto controller = message.getControllerNumber();
            const auto family = controller == 120 ? 0 : controller == 121 ? 1 : controller == 123 ? 2 : -1;
            if (family < 0 || message.getChannel() < 1 || message.getChannel() > 16)
            {
                unexpectedMidi = true;
                continue;
            }
            if (mapping == Mapping::droppedMessage && controller == 123 && message.getChannel() == 16)
                continue;
            const auto channel = mapping == Mapping::duplicateChannel && message.getChannel() == 16 ? 14 : message.getChannel() - 1;
            auto& candidates = resetParameters[static_cast<std::size_t>(family)];
            if (static_cast<std::size_t>(channel) < candidates.size())
                candidates[static_cast<std::size_t>(channel)]->setValue(0.0f);
            if (mapping == Mapping::ambiguousChange && controller == 121 && message.getChannel() == 1)
                candidates[1]->setValue(0.0f);
        }
        buffer.clear();
    }

    std::vector<float> originalValues;
    int processCount = 0;
    int messageCount = 0;
    int maximumObservedBlockSize = 0;
    bool silentInput = true;
    bool unexpectedMidi = false;
    bool unrelatedControlsUntouched = true;
    bool restorationDrained = false;

  private:
    Mapping mapping;
    bool temporaryValuesObserved = false;
    std::array<std::vector<juce::AudioProcessorParameter*>, 3> resetParameters;
    std::vector<juce::AudioProcessorParameter*> unrelatedParameters;
};

class VstPresetRenderTests final : public juce::UnitTest
{
  public:
    VstPresetRenderTests() : juce::UnitTest("VstPresetRenderTests", "halionbridge") {}

    void runTest() override
    {
        beginTest("CLI classifies and parses the VST preset render contract");
        {
            const auto command = std::vector<std::string>{"render-vstpresets"};
            expect(halionbridge::detail::classifyCliCommand(command) == halionbridge::detail::CliCommandKind::renderVstPresets);

            const auto root = makeTempRoot("cli");
            const auto preset = root.getChildFile("Voice.vstpreset");
            const auto midi = root.getChildFile("Probe.mid");
            expect(preset.replaceWithText("preset"));
            expect(midi.replaceWithText("midi"));

            const auto args = std::vector<std::string>{"--input",
                                                       preset.getFullPathName().toStdString(),
                                                       "--midi",
                                                       midi.getFullPathName().toStdString(),
                                                       "--sample-rate",
                                                       "96000",
                                                       "--bit-depth",
                                                       "32",
                                                       "--tail-seconds",
                                                       "1.5",
                                                       "--preset-settle-ms",
                                                       "250",
                                                       "--chunk-size",
                                                       "7",
                                                       "--jobs",
                                                       "2",
                                                       "--report-jsonl",
                                                       root.getChildFile("report.jsonl").getFullPathName().toStdString(),
                                                       "--recursive",
                                                       "--resume",
                                                       "--fail-fast",
                                                       "--skip-disk-space-check",
                                                       "--no-timeout",
                                                       "--force-scan"};

            const auto parsed = halionbridge::detail::parseVstPresetRenderOptionsDetailed(args);
            expect(parsed.options.has_value());
            if (parsed.options)
            {
                const auto& options = *parsed.options;
                expect(options.inputPath == halionbridge::detail::toStdPath(preset));
                expectEquals(static_cast<int>(options.midiInputs.size()), 1);
                expect(options.midiInputs.front() == halionbridge::detail::toStdPath(midi));
                expectEquals(options.sampleRate, 96000);
                expectEquals(options.bitDepth, 32);
                expectWithinAbsoluteError(options.tailSeconds, 1.5, 0.000001);
                expectEquals(options.presetSettleMilliseconds, 250);
                expectEquals(options.chunkSize, 7);
                expectEquals(options.jobs, 2);
                expect(options.recursive);
                expect(options.resume);
                expect(options.failFast);
                expect(options.skipDiskSpaceCheck);
                expectEquals(options.timeoutSeconds, 0);
                expect(options.forceScan);
            }

            root.deleteRecursively();
        }

        beginTest("CLI rejects conflicting and invalid render options");
        {
            const auto root = makeTempRoot("invalid_cli");
            const auto preset = root.getChildFile("Voice.vstpreset");
            const auto midi = root.getChildFile("Probe.mid");
            expect(preset.replaceWithText("preset"));
            expect(midi.replaceWithText("midi"));
            const auto base =
                std::vector<std::string>{"--input", preset.getFullPathName().toStdString(), "--midi", midi.getFullPathName().toStdString()};

            const auto defaults = halionbridge::detail::parseVstPresetRenderOptionsDetailed(base);
            expect(defaults.options.has_value());
            if (defaults.options)
                expectEquals(defaults.options->presetSettleMilliseconds, 500);
            expectEquals(halionbridge::VstPresetRenderOptions{}.presetSettleMilliseconds, 500);

            auto zeroSettle = base;
            zeroSettle.insert(zeroSettle.end(), {"--preset-settle-ms", "0"});
            const auto parsedZeroSettle = halionbridge::detail::parseVstPresetRenderOptionsDetailed(zeroSettle);
            expect(parsedZeroSettle.options.has_value());
            if (parsedZeroSettle.options)
                expectEquals(parsedZeroSettle.options->presetSettleMilliseconds, 0);
            auto negativeSettle = base;
            negativeSettle.insert(negativeSettle.end(), {"--preset-settle-ms", "-1"});
            expect(!halionbridge::detail::parseVstPresetRenderOptionsDetailed(negativeSettle).options.has_value());

            auto conflicting = base;
            conflicting.insert(conflicting.end(), {"--resume", "--overwrite"});
            expect(!halionbridge::detail::parseVstPresetRenderOptionsDetailed(conflicting).options.has_value());

            auto badDepth = base;
            badDepth.insert(badDepth.end(), {"--bit-depth", "20"});
            expect(!halionbridge::detail::parseVstPresetRenderOptionsDetailed(badDepth).options.has_value());

            auto guiJobs = base;
            guiJobs.insert(guiJobs.end(), {"--gui", "--jobs", "2"});
            expect(!halionbridge::detail::parseVstPresetRenderOptionsDetailed(guiJobs).options.has_value());

            root.deleteRecursively();
        }

        beginTest("Internal render worker arguments stay isolated from the public command");
        {
            const auto root = makeTempRoot("worker_cli");
            const auto manifest = root.getChildFile("chunk.json");
            const auto receipt = root.getChildFile("receipt.jsonl");
            expect(manifest.replaceWithText("{}"));
            const auto args =
                std::vector<std::string>{"--halionbridge-render-worker", "--worker-manifest", manifest.getFullPathName().toStdString(),
                                         "--worker-receipt", receipt.getFullPathName().toStdString()};
            expect(halionbridge::detail::classifyCliCommand(args) == halionbridge::detail::CliCommandKind::renderVstPresetsWorker);
            const auto parsed = halionbridge::detail::parseVstPresetRenderWorkerOptions(args);
            expect(parsed.has_value());
            if (parsed)
            {
                expect(halionbridge::detail::VstPresetRenderOptionsAccess::isWorkerMode(*parsed));
                expect(*halionbridge::detail::VstPresetRenderOptionsAccess::workerManifest(*parsed) ==
                       halionbridge::detail::toStdPath(manifest));
                expect(*halionbridge::detail::VstPresetRenderOptionsAccess::workerReceipt(*parsed) ==
                       halionbridge::detail::toStdPath(receipt));
            }
            root.deleteRecursively();
        }

        beginTest("Preset discovery is deterministic and recursion is explicit");
        {
            const auto root = makeTempRoot("collection");
            const auto nested = root.getChildFile("nested");
            expect(nested.createDirectory());
            expect(root.getChildFile("B.vstpreset").replaceWithText("b"));
            expect(root.getChildFile("ignored.txt").replaceWithText("x"));
            expect(nested.getChildFile("A.vstpreset").replaceWithText("a"));

            const auto shallow = halionbridge::detail::collectVstPresetRenderSources(halionbridge::detail::toStdPath(root), false);
            expect(shallow.errors.empty());
            expectEquals(static_cast<int>(shallow.files.size()), 1);
            const auto recursive = halionbridge::detail::collectVstPresetRenderSources(halionbridge::detail::toStdPath(root), true);
            expect(recursive.errors.empty());
            expectEquals(static_cast<int>(recursive.files.size()), 2);
            if (recursive.files.size() == 2)
            {
                expect(recursive.files[0].relativePath.generic_string() == "B.vstpreset");
                expect(recursive.files[1].relativePath.generic_string() == "nested/A.vstpreset");
            }
            root.deleteRecursively();
        }

        beginTest("MIDI parsing preserves timing data and rejects state-changing messages");
        {
            const auto root = makeTempRoot("midi");
            const auto valid = root.getChildFile("Phrase.mid");
            expect(writeMidi(valid));

            auto error = std::string{};
            const auto sequence = halionbridge::detail::parseMidiRenderSequence(halionbridge::detail::toStdPath(valid), error);
            expect(sequence.has_value(), error);
            if (sequence)
            {
                expectEquals(sequence->fileType, 0);
                expectEquals(sequence->timeFormat, static_cast<short>(960));
                expectEquals(static_cast<int>(sequence->events.size()), 2);
                expectWithinAbsoluteError(sequence->durationSeconds, 0.5, 0.000001);
                expectWithinAbsoluteError(sequence->tempoPoints.front().bpm, 120.0, 0.000001);
                expectEquals(sequence->timeSignaturePoints.front().numerator, 4);
                expectEquals(sequence->timeSignaturePoints.front().denominator, 4);
                expectEquals(static_cast<int>(sequence->sha256.size()), 64);
            }

            const auto invalid = root.getChildFile("ProgramChange.mid");
            expect(writeMidi(invalid, MidiExtra::programChange));
            expect(!halionbridge::detail::parseMidiRenderSequence(halionbridge::detail::toStdPath(invalid), error).has_value());
            expect(error.find("Program Change") != std::string::npos);

            const auto bankSelect = root.getChildFile("BankSelect.mid");
            expect(writeMidi(bankSelect, MidiExtra::bankSelect));
            expect(!halionbridge::detail::parseMidiRenderSequence(halionbridge::detail::toStdPath(bankSelect), error).has_value());
            expect(error.find("bank-select") != std::string::npos);

            const auto sysex = root.getChildFile("SysEx.mid");
            expect(writeMidi(sysex, MidiExtra::sysEx));
            expect(!halionbridge::detail::parseMidiRenderSequence(halionbridge::detail::toStdPath(sysex), error).has_value());
            expect(error.find("SysEx") != std::string::npos);

            const auto formatTwo = root.getChildFile("FormatTwo.mid");
            expect(writeMidi(formatTwo));
            expect(patchMidiFormat(formatTwo, 2));
            expect(!halionbridge::detail::parseMidiRenderSequence(halionbridge::detail::toStdPath(formatTwo), error).has_value());
            expect(error.find("format 2") != std::string::npos);
            root.deleteRecursively();
        }

        beginTest("MIDI directory selection is non-recursive and rejects duplicate output labels");
        {
            const auto root = makeTempRoot("midi_collection");
            const auto first = root.getChildFile("first");
            const auto second = root.getChildFile("second");
            const auto nested = first.getChildFile("nested");
            expect(first.createDirectory());
            expect(second.createDirectory());
            expect(nested.createDirectory());
            expect(writeMidi(first.getChildFile("A.mid")));
            expect(writeMidi(nested.getChildFile("ignored.mid")));
            expect(writeMidi(second.getChildFile("a.midi")));

            const auto shallow = halionbridge::detail::collectMidiRenderSequences({halionbridge::detail::toStdPath(first)});
            expect(shallow.errors.empty());
            expectEquals(static_cast<int>(shallow.files.size()), 1);

            const auto collision = halionbridge::detail::collectMidiRenderSequences(
                {halionbridge::detail::toStdPath(first), halionbridge::detail::toStdPath(second)});
            expect(!collision.errors.empty());
            expectEquals(static_cast<int>(collision.files.size()), 1);
            root.deleteRecursively();
        }

        beginTest("Long Unicode output names are shortened deterministically within portable limits");
        {
            const auto root = makeTempRoot("long_names");
            const auto repeated = juce::String::repeatedString(juce::String::charToString(0x4e2d), 100);
            const auto presetPath = root.getChildFile(repeated + ".vstpreset");
            const auto midiPath = root.getChildFile(repeated + ".mid");
            const auto preset = halionbridge::detail::VstPresetRenderSource{halionbridge::detail::toStdPath(presetPath),
                                                                            halionbridge::detail::toStdPath(presetPath.getFileName()),
                                                                            std::string(64, 'a')};
            auto midi = halionbridge::detail::MidiRenderSequence{};
            midi.sourcePath = halionbridge::detail::toStdPath(midiPath);
            midi.label = repeated.toStdString();
            midi.sha256 = std::string(64, 'b');

            auto error = std::string{};
            const auto first = halionbridge::detail::makeVstPresetRenderOutputPath(preset, midi, error);
            const auto second = halionbridge::detail::makeVstPresetRenderOutputPath(preset, midi, error);
            expect(!first.empty(), error);
            expect(first == second);
            const auto filename = halionbridge::detail::toJuceString(first.filename());
            expect(filename.endsWith(".wav"));
            expect(filename.contains("~"));
            expect(filename.length() <= 240);
            expect(filename.getNumBytesAsUTF8() <= 240);
            root.deleteRecursively();
        }

        beginTest("Offline rendering compensates latency and publishes the requested WAV format");
        {
            const auto root = makeTempRoot("audio");
            const auto output = root.getChildFile("Voice__Phrase.wav");

            auto midi = halionbridge::detail::MidiRenderSequence{};
            midi.durationSeconds = 0.01;
            midi.tempoPoints.push_back({0.0, 0.0, 120.0});
            midi.timeSignaturePoints.push_back({0.0, 0.0, 4, 4});
            midi.events.push_back({juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0.001, 0, 0});

            auto processor = LatencyImpulseProcessor{};
            processor.prepareToPlay(48000.0, 64);
            const auto config = halionbridge::detail::VstPresetAudioRenderConfig{48000, 32, 0.0, 64, false};
            const auto rendered =
                halionbridge::detail::renderVstPresetMidiToWav(processor, midi, halionbridge::detail::toStdPath(output), config);
            expect(rendered.succeeded, rendered.error);
            expectEquals(rendered.metrics.sampleCount, static_cast<std::int64_t>(480));
            expectEquals(rendered.metrics.latencySamples, 17);
            expectWithinAbsoluteError(rendered.metrics.peak, 0.5, 0.000001);
            expect(!rendered.metrics.silent);
            expect(!rendered.metrics.clipped);
            expect(!processor.playHeadSamples.empty());

            const auto refused =
                halionbridge::detail::renderVstPresetMidiToWav(processor, midi, halionbridge::detail::toStdPath(output), config);
            expect(!refused.succeeded);
            expect(refused.error.find("already exists") != std::string::npos);

            auto wav = juce::WavAudioFormat{};
            auto input = output.createInputStream();
            auto reader = std::unique_ptr<juce::AudioFormatReader>(wav.createReaderFor(input.release(), true));
            expect(reader != nullptr);
            if (reader != nullptr)
            {
                expectEquals(reader->lengthInSamples, static_cast<juce::int64>(480));
                expectEquals(static_cast<int>(reader->numChannels), 2);
                expect(reader->usesFloatingPointData);
                auto samples = juce::AudioBuffer<float>(2, 480);
                expect(reader->read(&samples, 0, 480, 0, true, true));
                expectWithinAbsoluteError(static_cast<double>(samples.getSample(0, 48)), 0.5, 0.000001);
            }
            root.deleteRecursively();
        }

        beginTest("MIDI reset transport verifies distinct mappings on all channels and restores every parameter");
        {
            for (const auto familySize : {16, 17})
            {
                auto processor = MappedResetProcessor{MappedResetProcessor::Mapping::valid, familySize};
                auto error = std::string{};
                expect(halionbridge::detail::verifyVstPresetMidiResetTransport(processor, 64, [] { return false; }, error), error);
                expectEquals(processor.messageCount, 48);
                expect(processor.values() == processor.originalValues);
                expect(processor.restorationDrained);
                expect(processor.unrelatedControlsUntouched);
                expect(!processor.unexpectedMidi);
                expect(processor.silentInput);
                expect(processor.maximumObservedBlockSize <= 64);
            }
        }

        beginTest("MIDI reset transport rejects missing parameter families before sending MIDI");
        {
            auto processor = MappedResetProcessor{MappedResetProcessor::Mapping::missingParameter};
            auto error = std::string{};
            expect(!halionbridge::detail::verifyVstPresetMidiResetTransport(processor, 64, [] { return false; }, error));
            expect(!error.empty());
            expectEquals(processor.messageCount, 0);
            expect(processor.values() == processor.originalValues);
            expect(processor.unrelatedControlsUntouched);
        }

        beginTest("MIDI reset transport rejects dropped, duplicate-channel, and ambiguous mappings without retaining probe values");
        {
            for (const auto mapping : {MappedResetProcessor::Mapping::droppedMessage, MappedResetProcessor::Mapping::duplicateChannel,
                                       MappedResetProcessor::Mapping::ambiguousChange})
            {
                auto processor = MappedResetProcessor{mapping};
                auto error = std::string{};
                expect(!halionbridge::detail::verifyVstPresetMidiResetTransport(processor, 64, [] { return false; }, error));
                expect(!error.empty());
                expect(processor.messageCount > 0);
                expect(processor.values() == processor.originalValues, "Temporary reset-parameter values must not survive failure");
                expect(processor.restorationDrained);
                expect(processor.unrelatedControlsUntouched);
                expect(!processor.unexpectedMidi);
            }
        }

        beginTest("MIDI reset transport rejects invalid processing sizes without touching parameters");
        {
            auto processor = MappedResetProcessor{};
            auto error = std::string{};
            expect(!halionbridge::detail::verifyVstPresetMidiResetTransport(processor, 0, [] { return false; }, error));
            expect(!error.empty());
            expect(!halionbridge::detail::verifyVstPresetMidiResetTransport(processor, -1, [] { return false; }, error));
            expect(!error.empty());
            expectEquals(processor.processCount, 0);
            expect(processor.values() == processor.originalValues);
        }

        beginTest("MIDI reset transport requires cancellation support and honors an already-stopped operation before mutation");
        {
            auto processor = MappedResetProcessor{};
            auto error = std::string{};
            expect(!halionbridge::detail::verifyVstPresetMidiResetTransport(processor, 64, {}, error));
            expect(!error.empty());
            expectEquals(processor.processCount, 0);
            expect(processor.values() == processor.originalValues);

            expect(!halionbridge::detail::verifyVstPresetMidiResetTransport(processor, 64, [] { return true; }, error));
            expect(!error.empty());
            expectEquals(processor.processCount, 0);
            expect(processor.values() == processor.originalValues);
        }

        beginTest("MIDI reset transport cancels between discarded blocks while still restoring and draining temporary values");
        {
            auto processor = MappedResetProcessor{};
            auto error = std::string{};
            expect(!halionbridge::detail::verifyVstPresetMidiResetTransport(
                processor, 64, [&processor] { return processor.processCount >= 3; }, error));
            expect(!error.empty());
            expectEquals(processor.processCount, 4, "Three verification blocks followed by the mandatory restoration drain");
            expect(processor.messageCount > 0 && processor.messageCount < 48);
            expect(processor.values() == processor.originalValues);
            expect(processor.restorationDrained);
            expect(processor.unrelatedControlsUntouched);
            expect(!processor.unexpectedMidi);
        }

        beginTest("Preparation revalidates MIDI reset transport after both preset restore and asynchronous activation");
        {
            for (const auto invalidateWhenPumping : {false, true})
            {
                auto processor = MappedResetProcessor{};
                auto restored = false;
                auto validationCount = 0;
                auto hooks = halionbridge::detail::VstPresetRenderPreparationHooks{};
                hooks.shouldStop = [] { return false; };
                hooks.validateMidiReset = [&processor, &validationCount, &hooks](std::string& validationError)
                {
                    ++validationCount;
                    return halionbridge::detail::verifyVstPresetMidiResetTransport(processor, 64, hooks.shouldStop, validationError);
                };
                hooks.restorePreset = [&processor, &restored, invalidateWhenPumping](std::string&)
                {
                    restored = true;
                    if (!invalidateWhenPumping)
                        processor.changeMapping(MappedResetProcessor::Mapping::droppedMessage);
                    return true;
                };
                hooks.pumpMessages = [&processor, &restored, invalidateWhenPumping](int)
                {
                    if (restored && invalidateWhenPumping)
                        processor.changeMapping(MappedResetProcessor::Mapping::droppedMessage);
                };
                auto error = std::string{};
                expect(!halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, 21, hooks, error));
                expect(!error.empty());
                expect(restored);
                expectEquals(validationCount, 2, "A pre-restore mapping result cannot establish post-activation transport safety");
                expect(processor.getPlayHead() == nullptr);
                expect(processor.unrelatedControlsUntouched);
            }
        }

        beginTest("Every preparation activates its preset and clears MIDI residue on all channels");
        {
            auto processor = DelayedPresetProcessor{};
            processor.prepareToPlay(48000.0, 64);
            auto requestedPreset = 11;
            auto hooks = preparationHooks(processor, requestedPreset);
            auto error = std::string{};

            for (const auto preset : {11, 11, 23})
            {
                requestedPreset = preset;
                processor.channels.fill(DelayedPresetProcessor::ChannelState{});
                expect(halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, 45, hooks, error), error);
                expectEquals(processor.activePreset, preset);
                expect(processor.cleanBeforeRestore, "The previous performance must be cleared before restoring the preset");
                expect(processor.isClean(), "Restored MIDI residue must be cleared before recording starts");
                expect(!processor.unexpectedPreparationMidi, "Preparation must not change programs or unrelated controllers");
                expect(processor.silentPreparationInput);
                expect(!processor.preparationTransportWasPlaying);
                expect(processor.getPlayHead() == nullptr);
            }
            expectEquals(processor.restoreCount, 3);
            expectEquals(processor.resetCount, 3);
            expect(processor.maximumObservedBlockSize <= 64);
            expect(!processor.pumpDurations.empty());
            for (const auto milliseconds : processor.pumpDurations)
                expect(milliseconds > 0 && milliseconds <= 20, "Settling must yield in bounded, cancellation-aware slices");
        }

        beginTest("Zero settling still resets MIDI and restores the preset without claiming asynchronous readiness");
        {
            auto processor = DelayedPresetProcessor{};
            auto requestedPreset = 11;
            const auto hooks = preparationHooks(processor, requestedPreset);
            auto error = std::string{};
            expect(halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, 0, hooks, error), error);
            expect(processor.cleanBeforeRestore);
            expect(processor.isClean());
            expectEquals(processor.restoreCount, 1);
            expectEquals(processor.resetCount, 1);
            expect(processor.pumpDurations.empty());
        }

        beginTest("Preparation validates its inputs and required hooks before touching processor state");
        {
            auto processor = DelayedPresetProcessor{};
            auto requestedPreset = 11;
            const auto hooks = preparationHooks(processor, requestedPreset);
            auto error = std::string{};
            expect(!halionbridge::detail::prepareVstPresetRender(processor, 0, 64, 1, hooks, error));
            expect(!error.empty());
            expect(!halionbridge::detail::prepareVstPresetRender(processor, 48000, 0, 1, hooks, error));
            expect(!error.empty());
            expect(!halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, -1, hooks, error));
            expect(!error.empty());

            auto missingRestore = hooks;
            missingRestore.restorePreset = {};
            expect(!halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, 1, missingRestore, error));
            expect(!error.empty());
            auto missingMappingValidation = hooks;
            missingMappingValidation.validateMidiReset = {};
            expect(!halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, 1, missingMappingValidation, error));
            expect(!error.empty());
            auto missingPump = hooks;
            missingPump.pumpMessages = {};
            expect(!halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, 1, missingPump, error));
            expect(!error.empty());
            expectEquals(processor.processCount, 0);
            expectEquals(processor.resetCount, 0);
            expectEquals(processor.restoreCount, 0);
        }

        beginTest("Unmapped reset messages and failed preset restores fail preparation with actionable errors");
        {
            auto processor = DelayedPresetProcessor{};
            auto requestedPreset = 11;
            auto hooks = preparationHooks(processor, requestedPreset);
            hooks.validateMidiReset = [](std::string& error)
            {
                error = "Controller 121 on channel 16 is not mapped";
                return false;
            };
            auto error = std::string{};
            expect(!halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, 1, hooks, error));
            expect(error.find("Controller 121 on channel 16") != std::string::npos, error);
            expectEquals(processor.processCount, 0);
            expectEquals(processor.resetCount, 0);
            expectEquals(processor.restoreCount, 0);

            hooks = preparationHooks(processor, requestedPreset);
            hooks.restorePreset = [](std::string& restoreError)
            {
                restoreError = "Preset state could not be restored";
                return false;
            };
            expect(!halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, 1, hooks, error));
            expect(error.find("Preset state could not be restored") != std::string::npos, error);
            expect(processor.pumpDurations.empty());
            expect(processor.getPlayHead() == nullptr);
        }

        beginTest("Preparation cancels before mutation or during a bounded settling interval");
        {
            auto processor = DelayedPresetProcessor{};
            auto requestedPreset = 11;
            auto hooks = preparationHooks(processor, requestedPreset);
            hooks.shouldStop = [] { return true; };
            auto error = std::string{};
            expect(!halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, 45, hooks, error));
            expect(!error.empty());
            expectEquals(processor.processCount, 0);
            expectEquals(processor.resetCount, 0);
            expectEquals(processor.restoreCount, 0);

            hooks.shouldStop = [&processor] { return !processor.pumpDurations.empty(); };
            expect(!halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, 45, hooks, error));
            expect(!error.empty());
            expectEquals(static_cast<int>(processor.pumpDurations.size()), 1);
            expectEquals(processor.restoreCount, 1);
            expect(processor.getPlayHead() == nullptr);
        }

        beginTest("Discarded preparation preserves time-zero notes, event offsets, latency compensation, and WAV tail");
        {
            const auto root = makeTempRoot("prepared_audio");
            const auto output = root.getChildFile("Voice__Phrase.wav");
            auto processor = DelayedPresetProcessor{};
            processor.prepareToPlay(48000.0, 64);
            auto requestedPreset = 11;
            const auto hooks = preparationHooks(processor, requestedPreset);
            auto error = std::string{};
            expect(halionbridge::detail::prepareVstPresetRender(processor, 48000, 64, 21, hooks, error), error);
            expectEquals(processor.getLatencySamples(), 29);

            auto midi = halionbridge::detail::MidiRenderSequence{};
            midi.durationSeconds = 0.01;
            midi.tempoPoints.push_back({0.0, 0.0, 120.0});
            midi.timeSignaturePoints.push_back({0.0, 0.0, 4, 4});
            midi.events.push_back({juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0.0, 0, 0});
            midi.events.push_back({juce::MidiMessage::noteOn(16, 64, static_cast<juce::uint8>(100)), 0.001, 0, 1});
            processor.recording = true;
            processor.playHeadSamples.clear();
            const auto config = halionbridge::detail::VstPresetAudioRenderConfig{48000, 32, 0.002, 64, false};
            const auto rendered =
                halionbridge::detail::renderVstPresetMidiToWav(processor, midi, halionbridge::detail::toStdPath(output), config);
            expect(rendered.succeeded, rendered.error);
            expectEquals(rendered.metrics.sampleCount, static_cast<std::int64_t>(576));
            expectEquals(rendered.metrics.latencySamples, 29);
            expectEquals(processor.notePreset, requestedPreset);
            expect(processor.noteStartedClean);
            expect(!processor.playHeadSamples.empty());
            if (!processor.playHeadSamples.empty())
                expectEquals(processor.playHeadSamples.front(), static_cast<std::int64_t>(0));
            expect(processor.getPlayHead() == nullptr);

            auto wav = juce::WavAudioFormat{};
            auto input = output.createInputStream();
            auto reader = std::unique_ptr<juce::AudioFormatReader>(wav.createReaderFor(input.release(), true));
            expect(reader != nullptr);
            if (reader != nullptr)
            {
                expectEquals(reader->lengthInSamples, static_cast<juce::int64>(576));
                auto samples = juce::AudioBuffer<float>(2, 576);
                expect(reader->read(&samples, 0, 576, 0, true, true));
                for (int channel = 0; channel < 2; ++channel)
                    for (int sample = 0; sample < 576; ++sample)
                        expectWithinAbsoluteError(samples.getSample(channel, sample), sample == 0 || sample == 48 ? 0.5f : 0.0f, 0.000001f);
            }
            root.deleteRecursively();
        }

        beginTest("Render length validation rejects invalid values and rounds up fractional samples");
        {
            auto error = std::string{};
            const auto samples = halionbridge::detail::calculateVstPresetRenderSampleCount(0.00101, 0.0, 48000, error);
            expect(samples.has_value());
            expectEquals(*samples, static_cast<std::int64_t>(49));
            expect(!halionbridge::detail::calculateVstPresetRenderSampleCount(-1.0, 0.0, 48000, error).has_value());
            expect(!halionbridge::detail::calculateVstPresetRenderSampleCount(1.0, -0.1, 48000, error).has_value());
        }
    }

  private:
    static halionbridge::detail::VstPresetRenderPreparationHooks preparationHooks(DelayedPresetProcessor& processor,
                                                                                  const int& requestedPreset)
    {
        auto hooks = halionbridge::detail::VstPresetRenderPreparationHooks{};
        hooks.restorePreset = [&processor, &requestedPreset](std::string&) { return processor.restore(requestedPreset); };
        hooks.validateMidiReset = [](std::string&) { return true; };
        hooks.pumpMessages = [&processor](const int milliseconds) { processor.pump(milliseconds); };
        hooks.shouldStop = [] { return false; };
        return hooks;
    }

    enum class MidiExtra
    {
        none,
        programChange,
        bankSelect,
        sysEx
    };

    static juce::File makeTempRoot(const char* suffix)
    {
        auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getNonexistentChildFile(juce::String("halionbridge_render_") + suffix, {}, false);
        root.deleteRecursively();
        root.createDirectory();
        return root;
    }

    static bool writeMidi(const juce::File& file, const MidiExtra extra = MidiExtra::none)
    {
        auto track = juce::MidiMessageSequence{};
        auto tempo = juce::MidiMessage::tempoMetaEvent(500000);
        tempo.setTimeStamp(0.0);
        track.addEvent(tempo);
        auto signature = juce::MidiMessage::timeSignatureMetaEvent(4, 4);
        signature.setTimeStamp(0.0);
        track.addEvent(signature);
        auto noteOn = juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100));
        noteOn.setTimeStamp(0.0);
        track.addEvent(noteOn);
        auto noteOff = juce::MidiMessage::noteOff(1, 60);
        noteOff.setTimeStamp(960.0);
        track.addEvent(noteOff);
        if (extra == MidiExtra::programChange)
        {
            auto program = juce::MidiMessage::programChange(1, 2);
            program.setTimeStamp(1.0);
            track.addEvent(program);
        }
        else if (extra == MidiExtra::bankSelect)
        {
            auto bank = juce::MidiMessage::controllerEvent(1, 0, 1);
            bank.setTimeStamp(1.0);
            track.addEvent(bank);
        }
        else if (extra == MidiExtra::sysEx)
        {
            constexpr std::array<juce::uint8, 3> payload{0x43, 0x00, 0x01};
            auto message = juce::MidiMessage::createSysExMessage(payload.data(), static_cast<int>(payload.size()));
            message.setTimeStamp(1.0);
            track.addEvent(message);
        }
        track.sort();

        auto midi = juce::MidiFile{};
        midi.setTicksPerQuarterNote(960);
        midi.addTrack(track);
        auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
        if (stream == nullptr || !midi.writeTo(*stream, 0))
            return false;
        stream->flush();
        return true;
    }

    static bool patchMidiFormat(const juce::File& file, const int format)
    {
        auto stream = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
        if (stream == nullptr || !stream->setPosition(8))
            return false;
        if (!stream->writeByte(static_cast<char>((format >> 8) & 0xff)) || !stream->writeByte(static_cast<char>(format & 0xff)))
            return false;
        stream->flush();
        return stream->getStatus().wasOk();
    }
};

VstPresetRenderTests vstPresetRenderTests;

} // namespace
