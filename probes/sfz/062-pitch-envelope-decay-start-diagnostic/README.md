# 062 Pitch Envelope Decay/Start Diagnostic

This probe follows the first pitch results from `061`: `001` matched, so
`pitcheg_depth=1200` appears to map to HALion `Pitch.EnvAmount=12` for a
one-second attack ramp. `061` decay cases did not match, so this probe isolates
zero-attack and decay-start behavior before any filter-envelope work continues.

## Files

- `source/*.sfz` are same-number sforzando references.
- `source/samples/saw_A3_single_cycle.wav` is copied from probe `061`.
- `plugnscript/halionbridge_sfz_pitch_env_generator.cxx` emits one long note 57
  at velocity 127.
- `halionbridge-build/*.lua` hand-authors HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

The SFZ references intentionally omit `end=` and use only `loop_mode`,
`loop_start`, and `loop_end`, matching the corrected `061` source shape.
HALion candidates still set `SampleOsc.SampleEnd=200`.

## Generate And Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\062-pitch-envelope-decay-start-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_pitch_env_generator.cxx`.
3. Compare same-number pairs from `000` through `010`.
4. Report which instant-hold and decay candidates match best.

## Cases

- `000`: no pitch envelope baseline.
- `001`: known-good `pitcheg_depth=1200`, `pitcheg_attack=1.0`,
  `pitcheg_sustain=100` control with HALion amount `12`.
- `002`: SFZ zero attack to sustain, HALion starts at `0`, jumps to `1` at
  duration `0`, then sustains.
- `003`: same SFZ as `002`, but HALion starts directly at level `1`.
- `004`: `pitcheg_attack=0`, `pitcheg_decay=1.0`, `pitcheg_sustain=0`, using
  the original duplicated-start HALion decay from `061`.
- `005`: same SFZ, simplified HALion start level `1` to zero over one second.
- `006`: same SFZ, HALion starts at `0`, jumps to `1`, then decays to zero.
- `007`: same SFZ, HALion start level `0.75`, amount `12`.
- `008`: same SFZ, HALion start level `0.5`, amount `12`.
- `009`: same SFZ, HALion start level `1`, amount `9`.
- `010`: same SFZ, HALion start level `1`, amount `6`.

## Result

Generated on 2026-06-14. HALion build completed successfully: 1 script
processed, 11 `.vstpreset` files saved.
