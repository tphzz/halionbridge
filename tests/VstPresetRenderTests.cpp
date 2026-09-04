#include "halionbridge/Bridge.h"
#include "CliCommand.h"
#include "PathUtils.h"
#include "VstPresetRender.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

namespace
{

class LatencyImpulseProcessor final : public juce::AudioProcessor
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
