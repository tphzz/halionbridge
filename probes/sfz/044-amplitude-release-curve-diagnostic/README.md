# 044 Amplitude Release Curve Diagnostic

This probe follows the first `043` manual result:

- `000_default_release` matched.
- `001_release_1s` did not match.
- Manual HALion editing suggested the sforzando release is closer to a faster
  curved release than to HALion's linear one-second release. A HALion envelope
  point with approximately `Time=604 ms` and `Curve=-10` was closer by phase
  cancellation.

The SFZv2 specification documents this direction: default ARIA-style
`ampeg_release_shape` is `-10.3616`, and release/decay stages are faster than
linear. HALion's UI displays envelope curvature as `-10..10`, but the Lua
envelope point table does not accept negative UI-scale values directly. A quick
script check showed that non-negative table values save successfully, so this
probe keeps the sforzando source fixed at `ampeg_release=1.0` and sweeps
HALion's Lua table curve encoding.

## Files

- `source/000_release_1s_reference.sfz` is the sforzando reference.
- `source/samples/amp_env_loop.wav` is copied from probe `043`.
- `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx` is copied from
  probe `043`.
- `halionbridge-build/*.lua` are hand-authored HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

## HALion Candidates

- `000_linear_1s`: current converter shape, `duration=1.0`, table `curve=0`.
- `001_table_0p25_1s`: `duration=1.0`, table `curve=0.25`.
- `002_table_0p50_1s`: `duration=1.0`, table `curve=0.50`.
- `003_table_0p75_1s`: `duration=1.0`, table `curve=0.75`.
- `004_table_1p00_1s`: `duration=1.0`, table `curve=1.00`.
- `005_table_0p25_0p604`: `duration=0.604`, table `curve=0.25`.
- `006_table_0p50_0p604`: `duration=0.604`, table `curve=0.50`.
- `007_table_0p75_0p604`: `duration=0.604`, table `curve=0.75`.
- `008_table_1p00_0p604`: `duration=0.604`, table `curve=1.00`.

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\044-amplitude-release-curve-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx`.
3. Load `source/000_release_1s_reference.sfz` in sforzando.
4. Compare against each numbered HALion `.vstpreset`.

Use the phase-inverted/null-test setup and focus only on the release tail after
note-off. Case `000` should reproduce the known mismatch from probe `043`.

## Result

HALion build completed successfully on 2026-06-14: nine hand-authored scripts
processed, nine `.vstpreset` files saved.

Manual visual inspection on 2026-06-14 found the generated curves bend the
wrong way for SFZ's default release shape. Case `004_table_1p00_1s` displays as
HALion UI `Curve=+10.0`, which is slow initial fade followed by a fast drop.
SFZ's default release shape needs the opposite polarity: fast initial fadeout
with a slower quiet tail.

Decision: do not use `044` for audible validation. It remains a useful
wrong-polarity encoding discovery. Use probe `045`, which uses the documented
Lua envelope point curve range `-1..1`, for the corrected negative-curve sweep.
