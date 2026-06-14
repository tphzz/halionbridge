# 063 Pitch Envelope Decay Curve Diagnostic

This probe follows `062`. Manual validation found:

- `062/001` matched the known-good one-second pitch attack control.
- `062/002` and `062/003` both matched zero-attack full-depth hold behavior.
- `062/004` used a one-second linear HALion decay and was much too slow; manual
  tuning suggested approximately `300 ms` with HALion GUI curve `-5`.

This probe therefore keeps `Pitch.EnvAmount=12` and the verified start-at-full
topology constant, then sweeps HALion pitch-envelope decay duration and Lua
curve. HALion Lua envelope curves use `-1..1`; GUI curve `-5` is treated as a
candidate near Lua `-0.5`.

## Files

- `source/*.sfz` are same-number sforzando references.
- `source/samples/saw_A3_single_cycle.wav` is copied from probe `062`.
- `plugnscript/halionbridge_sfz_pitch_env_generator.cxx` emits one long note 57
  at velocity 127.
- `halionbridge-build/*.lua` hand-authors HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

The SFZ references intentionally omit `end=` and use only `loop_mode`,
`loop_start`, and `loop_end`. HALion candidates still set
`SampleOsc.SampleEnd=200`.

## Generate And Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\063-pitch-envelope-decay-curve-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_pitch_env_generator.cxx`.
3. Compare same-number pairs from `000` through `010`.
4. Report the best matching decay candidate and whether neighboring cases are
   too fast, too slow, too bent, or too straight.

## Cases

- `000`: no pitch envelope baseline.
- `001`: known-good one-second attack control.
- `002`: decay duration `0.25`, Lua curve `-0.5`.
- `003`: decay duration `0.30`, Lua curve `-0.5`.
- `004`: decay duration `0.35`, Lua curve `-0.5`.
- `005`: decay duration `0.30`, Lua curve `-0.25`.
- `006`: decay duration `0.30`, Lua curve `-0.75`.
- `007`: decay duration `0.30`, Lua curve `-1.0`.
- `008`: decay duration `0.20`, Lua curve `-0.5`.
- `009`: decay duration `0.40`, Lua curve `-0.5`.
- `010`: decay duration `0.604`, Lua curve `-1.0`, included as an
  amp-envelope-style duration/curve cross-check.

## Result

Generated on 2026-06-14. HALion build completed successfully: 1 script
processed, 11 `.vstpreset` files saved.
