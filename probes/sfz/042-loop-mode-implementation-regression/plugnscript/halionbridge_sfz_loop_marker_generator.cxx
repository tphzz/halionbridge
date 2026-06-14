/** \file
*   Deterministic MIDI probe for HALion loop marker and playback-mode checks.
*
*   Load this Blue Cat Plug'n Script script before either sforzando or HALion.
*   It holds note 57 for five seconds, then releases it and leaves a long gap.
*/

string name = "halionbridge SFZ loop marker generator";
string description = "Emits a long fixed note for loop marker/playback mode checks";

const int channel = 1;
const int note = 57;
const int velocity = 127;
const double leadInSeconds = 1.0;
const double noteDurationSeconds = 5.0;
const double cycleGapSeconds = 5.0;

double freeRunningPositionSamples = 0.0;
bool wasPlaying = false;
MidiEvent event;

void pushMidiNote(BlockData& data, double timeStamp, bool noteOn)
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
    pushMidiNote(data, 0.0, false);
}

void emitEventIfInsideBlock(BlockData& data, double eventPosition, double blockStart, double blockEnd, bool noteOn)
{
    if (eventPosition >= blockStart && eventPosition < blockEnd)
    {
        pushMidiNote(data, eventPosition - blockStart, noteOn);
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
    double cycleSamples = leadInSamples + noteDurationSamples + cycleGapSeconds * sampleRate;

    int firstCycle = int(floor((blockStart - leadInSamples - noteDurationSamples) / cycleSamples));
    if (firstCycle < 0)
    {
        firstCycle = 0;
    }

    for (int cycle = firstCycle; cycle < firstCycle + 3; cycle++)
    {
        double cycleStart = double(cycle) * cycleSamples;
        double noteStart = cycleStart + leadInSamples;
        double noteEnd = noteStart + noteDurationSamples;

        emitEventIfInsideBlock(data, noteStart, blockStart, blockEnd, true);
        emitEventIfInsideBlock(data, noteEnd, blockStart, blockEnd, false);
    }

    if (@data.transport == null)
    {
        freeRunningPositionSamples += data.samplesToProcess;
    }
}
