/** \file
*   Deterministic MIDI probe for halionbridge SFZ amp velocity calibration.
*
*   Load this Blue Cat Plug'n Script script before either sforzando or HALion.
*   It repeatedly plays the probe root note with four fixed velocities. Use
*   level meters, stereo meters, phase inversion, or ear comparison to check
*   whether sforzando and HALion respond the same way for each SFZ file.
*/

string name = "halionbridge SFZ amp velocity calibration generator";
string description = "Emits fixed velocities for amp_veltrack calibration";

array<int> testVelocities = {32, 64, 100, 127};

const int channel = 1;
const int note = 57;
const double leadInSeconds = 1.0;
const double noteDurationSeconds = 1.0;
const double stepSeconds = 1.5;
const double cycleGapSeconds = 2.0;

double freeRunningPositionSamples = 0.0;
bool wasPlaying = false;
MidiEvent event;

void pushMidiNote(BlockData& data, double timeStamp, int velocity, bool noteOn)
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
    pushMidiNote(data, 0.0, 0, false);
}

void emitEventIfInsideBlock(BlockData& data, double eventPosition, double blockStart, double blockEnd, int velocity, bool noteOn)
{
    if (eventPosition >= blockStart && eventPosition < blockEnd)
    {
        pushMidiNote(data, eventPosition - blockStart, velocity, noteOn);
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
    double stepSamples = stepSeconds * sampleRate;
    double cycleSamples = leadInSamples + double(testVelocities.length) * stepSamples + cycleGapSeconds * sampleRate;

    int firstCycle = int(floor((blockStart - leadInSamples - noteDurationSamples) / cycleSamples));
    if (firstCycle < 0)
    {
        firstCycle = 0;
    }

    for (int cycle = firstCycle; cycle < firstCycle + 3; cycle++)
    {
        double cycleStart = double(cycle) * cycleSamples;

        for (uint velocityIndex = 0; velocityIndex < testVelocities.length; velocityIndex++)
        {
            double noteStart = cycleStart + leadInSamples + double(velocityIndex) * stepSamples;
            double noteEnd = noteStart + noteDurationSamples;
            int velocity = testVelocities[velocityIndex];

            emitEventIfInsideBlock(data, noteStart, blockStart, blockEnd, velocity, true);
            emitEventIfInsideBlock(data, noteEnd, blockStart, blockEnd, velocity, false);
        }
    }

    if (@data.transport == null)
    {
        freeRunningPositionSamples += data.samplesToProcess;
    }
}
