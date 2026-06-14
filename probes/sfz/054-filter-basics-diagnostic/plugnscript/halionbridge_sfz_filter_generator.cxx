/** \file
*   Deterministic MIDI probe for HALion filter checks.
*
*   Load this Blue Cat Plug'n Script script before either sforzando or HALion.
*   It emits note 57 at four velocities so static cutoff/resonance and
*   velocity-to-cutoff behavior can be compared in the same preset.
*/

string name = "halionbridge SFZ filter generator";
string description = "Emits fixed notes for SFZ filter checks";

const int channel = 1;
const int note = 57;
const int velocitiesCount = 4;
const double leadInSeconds = 1.0;
const double noteDurationSeconds = 1.2;
const double noteGapSeconds = 0.5;
const double cycleGapSeconds = 2.0;

double freeRunningPositionSamples = 0.0;
bool wasPlaying = false;
MidiEvent event;

int velocityAt(int index)
{
    if (index == 0) return 32;
    if (index == 1) return 64;
    if (index == 2) return 100;
    return 127;
}

void pushMidiNote(BlockData& data, double timeStamp, bool noteOn, int velocity)
{
    event.timeStamp = timeStamp;
    event.byte0 = uint8((noteOn ? 0x90 : 0x80) | ((channel - 1) & 0x0F));
    event.byte1 = uint8(note & 0x7F);
    event.byte2 = uint8((noteOn ? velocity : 0) & 0x7F);
    event.byte3 = 0;
    data.outputMidiEvents.push(event);
}

void stopProbeNote(BlockData& data)
{
    pushMidiNote(data, 0.0, false, 0);
}

void emitEventIfInsideBlock(BlockData& data, double eventPosition, double blockStart, double blockEnd, bool noteOn, int velocity)
{
    if (eventPosition >= blockStart && eventPosition < blockEnd)
    {
        pushMidiNote(data, eventPosition - blockStart, noteOn, velocity);
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
            stopProbeNote(data);
        }
        wasPlaying = false;
        return;
    }

    wasPlaying = true;

    double blockEnd = blockStart + data.samplesToProcess;
    double leadInSamples = leadInSeconds * sampleRate;
    double noteDurationSamples = noteDurationSeconds * sampleRate;
    double noteGapSamples = noteGapSeconds * sampleRate;
    double sequenceDurationSamples = double(velocitiesCount) * (noteDurationSamples + noteGapSamples);
    double cycleSamples = leadInSamples + sequenceDurationSamples + cycleGapSeconds * sampleRate;

    int firstCycle = int(floor((blockStart - leadInSamples - sequenceDurationSamples) / cycleSamples));
    if (firstCycle < 0)
    {
        firstCycle = 0;
    }

    for (int cycle = firstCycle; cycle < firstCycle + 3; cycle++)
    {
        double cycleStart = double(cycle) * cycleSamples;
        for (int i = 0; i < velocitiesCount; i++)
        {
            int velocity = velocityAt(i);
            double noteStart = cycleStart + leadInSamples + double(i) * (noteDurationSamples + noteGapSamples);
            double noteEnd = noteStart + noteDurationSamples;
            emitEventIfInsideBlock(data, noteStart, blockStart, blockEnd, true, velocity);
            emitEventIfInsideBlock(data, noteEnd, blockStart, blockEnd, false, 0);
        }
    }

    if (@data.transport == null)
    {
        freeRunningPositionSamples += data.samplesToProcess;
    }
}
