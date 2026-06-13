/** \file
*   Deterministic MIDI velocity-boundary probe for halionbridge SFZ conversion.
*
*   This Blue Cat Plug'n Script script emits a repeating sequence of note events
*   around the SFZ velocity split used by this probe. It does not depend on the
*   factory Midi.hxx include, so it can be loaded directly from this probe folder.
*/

string name = "halionbridge SFZ velocity boundary generator";
string description = "Emits deterministic notes at velocities 62, 63, 64, and 65 for SFZ layer boundary checks";

// These notes cover the low, middle, and high SFZ key ranges, including their
// range boundaries:
//   36..51, 52..67, and 68..82.
array<int> testNotes = {36, 51, 52, 67, 68, 82};
array<int> testVelocities = {62, 63, 64, 65};

const int channel = 1;
const double leadInSeconds = 1.0;
const double noteDurationSeconds = 0.55;
const double stepSeconds = 0.85;
const double cycleGapSeconds = 2.0;

double freeRunningPositionSamples = 0.0;
bool wasPlaying = false;
MidiEvent event;

void pushMidiNote(BlockData& data, double timeStamp, int note, int velocity, bool noteOn)
{
    event.timeStamp = timeStamp;
    event.byte0 = uint8((noteOn ? 0x90 : 0x80) | ((channel - 1) & 0x0F));
    event.byte1 = uint8(note & 0x7F);
    event.byte2 = uint8(velocity & 0x7F);
    event.byte3 = 0;
    data.outputMidiEvents.push(event);
}

void stopAllProbeNotes(BlockData& data)
{
    for (uint noteIndex = 0; noteIndex < testNotes.length; noteIndex++)
    {
        pushMidiNote(data, 0.0, testNotes[noteIndex], 0, false);
    }
}

void emitEventIfInsideBlock(BlockData& data, double eventPosition, double blockStart, double blockEnd, int note, int velocity, bool noteOn)
{
    if (eventPosition >= blockStart && eventPosition < blockEnd)
    {
        pushMidiNote(data, eventPosition - blockStart, note, velocity, noteOn);
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
    int eventCount = int(testNotes.length * testVelocities.length);
    double cycleSamples = leadInSamples + double(eventCount) * stepSamples + cycleGapSeconds * sampleRate;

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
            for (uint velocityIndex = 0; velocityIndex < testVelocities.length; velocityIndex++)
            {
                int step = int(noteIndex * testVelocities.length + velocityIndex);
                double noteStart = cycleStart + leadInSamples + double(step) * stepSamples;
                double noteEnd = noteStart + noteDurationSamples;
                int note = testNotes[noteIndex];
                int velocity = testVelocities[velocityIndex];

                emitEventIfInsideBlock(data, noteStart, blockStart, blockEnd, note, velocity, true);
                emitEventIfInsideBlock(data, noteEnd, blockStart, blockEnd, note, velocity, false);
            }
        }
    }

    if (@data.transport == null)
    {
        freeRunningPositionSamples += data.samplesToProcess;
    }
}
