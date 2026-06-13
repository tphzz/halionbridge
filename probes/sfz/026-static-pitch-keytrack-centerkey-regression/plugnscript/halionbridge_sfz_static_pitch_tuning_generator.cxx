/** \file
*   Deterministic MIDI pitch probe for halionbridge SFZ conversion.
*
*   Load this Blue Cat Plug'n Script script before either sforzando or HALion.
*   It repeatedly plays three notes across the probe's mapped range at a fixed
*   velocity. Use a tuner, spectrum view, oscilloscope, or ear comparison to
*   check whether sforzando and HALion respond the same way for each SFZ file.
*/

string name = "halionbridge SFZ static pitch tuning generator";
string description = "Emits fixed notes for pitch_keycenter, tune, transpose, and pitch_keytrack checks";

array<int> testNotes = {57, 69, 81};

const int channel = 1;
const int velocity = 100;
const double leadInSeconds = 1.0;
const double noteDurationSeconds = 1.2;
const double stepSeconds = 1.7;
const double cycleGapSeconds = 2.0;

double freeRunningPositionSamples = 0.0;
bool wasPlaying = false;
MidiEvent event;

void pushMidiNote(BlockData& data, double timeStamp, int note, bool noteOn)
{
    event.timeStamp = timeStamp;
    event.byte0 = uint8((noteOn ? 0x90 : 0x80) | ((channel - 1) & 0x0F));
    event.byte1 = uint8(note & 0x7F);
    event.byte2 = uint8((noteOn ? velocity : 0) & 0x7F);
    event.byte3 = 0;
    data.outputMidiEvents.push(event);
}

void stopAllProbeNotes(BlockData& data)
{
    for (uint noteIndex = 0; noteIndex < testNotes.length; noteIndex++)
    {
        pushMidiNote(data, 0.0, testNotes[noteIndex], false);
    }
}

void emitEventIfInsideBlock(BlockData& data, double eventPosition, double blockStart, double blockEnd, int note, bool noteOn)
{
    if (eventPosition >= blockStart && eventPosition < blockEnd)
    {
        pushMidiNote(data, eventPosition - blockStart, note, noteOn);
    }
}

void processBlock(BlockData& data)
{
    bool isPlaying = true;
    double blockStart = freeRunningPositionSamples;

    if (@data.transport != null)
    {
        isPlaying = data.transport.isPlaying;
        blockStart = data.transport.positionInSeconds * sampleRate;
    }

    if (!isPlaying)
    {
        if (wasPlaying)
        {
            stopAllProbeNotes(data);
        }
        wasPlaying = false;
        return;
    }

    wasPlaying = true;

    double blockEnd = blockStart + data.samplesToProcess;
    double leadInSamples = leadInSeconds * sampleRate;
    double noteDurationSamples = noteDurationSeconds * sampleRate;
    double stepSamples = stepSeconds * sampleRate;
    double cycleSamples = leadInSamples + double(testNotes.length) * stepSamples + cycleGapSeconds * sampleRate;

    int firstCycle = int(floor((blockStart - leadInSamples - noteDurationSamples) / cycleSamples));
    if (firstCycle < 0)
    {
        firstCycle = 0;
    }

    for (int cycle = firstCycle; cycle < firstCycle + 3; cycle++)
    {
        double cycleStart = double(cycle) * cycleSamples;

        for (uint noteIndex = 0; noteIndex < testNotes.length; noteIndex++)
        {
            double noteStart = cycleStart + leadInSamples + double(noteIndex) * stepSamples;
            double noteEnd = noteStart + noteDurationSamples;
            int note = testNotes[noteIndex];

            emitEventIfInsideBlock(data, noteStart, blockStart, blockEnd, note, true);
            emitEventIfInsideBlock(data, noteEnd, blockStart, blockEnd, note, false);
        }
    }

    if (@data.transport == null)
    {
        freeRunningPositionSamples += data.samplesToProcess;
    }
}
