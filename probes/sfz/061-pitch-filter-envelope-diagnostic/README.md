# 061 Pitch/Filter Envelope Diagnostic

This probe starts calibration for SFZ `pitcheg_*` and `fileg_*` opcodes after
the static filter family mapping in probes `059` and `060`.

The SFZv2 spec defines both `pitcheg_depth` and `fileg_depth` in cents. HALion
exposes `Pitch.EnvAmount` in semitones and `Filter.EnvAmount` as a `-100..100`
parameter, so this probe is a candidate sweep rather than an implementation
regression.

## Files

- `source/*.sfz` are same-number sforzando references.
- `source/samples/saw_A3_single_cycle.wav` is copied from probe `060`.
- `plugnscript/halionbridge_sfz_pitch_filter_env_generator.cxx` emits one long
  note 57 at velocity 127.
- `halionbridge-build/*.lua` hand-authors HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

The SFZ references intentionally omit `end=` and use only `loop_mode`,
`loop_start`, and `loop_end`, matching the earlier working single-cycle filter
probes. The HALion candidates still write `SampleOsc.SampleEnd=200` through the
helper's inclusive-end conversion so HALion does not depend on an unset initial
sample end marker.

## Generate And Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\061-pitch-filter-envelope-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_pitch_filter_env_generator.cxx`.
3. Load same-number SFZ and HALion pairs and compare by ear/null test.
4. Report which numbered candidates match best and any direction errors.

## Cases

- `000_no_env`: looped saw baseline.
- `001`..`003`: `pitcheg_depth=1200`, `pitcheg_attack=1.0`,
  `pitcheg_sustain=100`; HALion `Pitch.EnvAmount` candidates `12`, `24`, `60`.
- `004`..`006`: `pitcheg_depth=1200`, `pitcheg_attack=0`,
  `pitcheg_decay=1.0`, `pitcheg_sustain=0`; HALion `Pitch.EnvAmount`
  candidates `12`, `24`, `60`.
- `007`..`009`: `lpf_2p`, `cutoff=500`, `resonance=6`,
  `fileg_depth=2400`, `fileg_attack=1.0`, `fileg_sustain=100`; HALion
  `Filter.EnvAmount` candidates `25`, `50`, `100`.
- `010`..`012`: same filter baseline with `fileg_attack=0`,
  `fileg_decay=1.0`, `fileg_sustain=0`; HALion `Filter.EnvAmount` candidates
  `25`, `50`, `100`.

## Result

Generated on 2026-06-14. HALion build completed successfully: 1 script
processed, 13 `.vstpreset` files saved.

Manual pitch feedback:

- `001` matches, confirming `pitcheg_depth=1200` can map to
  `Pitch.EnvAmount=12` for the one-second attack ramp.
- `002` and `003` are too high because their amount candidates intentionally
  exceed the source depth.
- `004` starts too high despite amount `12`, so pitch decay/start point
  topology needs a focused follow-up before implementing pitch envelopes.
- `006` is much too high because its amount candidate intentionally exceeds
  the source depth.

Follow-up generated as `062-pitch-envelope-decay-start-diagnostic`.
