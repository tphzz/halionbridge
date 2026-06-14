# 045 Amplitude Release Negative Curve Diagnostic

This probe supersedes `044` for audible release-shape validation.

Probe `044` established that positive Lua envelope table curve values bend the
wrong way for SFZ's default amplitude release. HALion's script reference says
`EnvelopePoints[].curve` uses the range `-1..1`, while the HALion UI displays
envelope curve as `-10..10`. Therefore UI `Curve=-10` should be represented as
Lua table `curve=-1`, not `curve=-10`.

The SFZv2 specification documents the expected direction:

- default ARIA-style `ampeg_release_shape` is `-10.3616`,
- negative release shape values are faster, with quick initial fadeout and a
  slower quiet tail,
- decay and release stages are faster than linear.

This probe keeps the sforzando source fixed at `ampeg_release=1.0` and varies
only HALion's Lua table release curve and duration.

## Files

- `source/000_release_1s_reference.sfz` is the sforzando reference.
- `source/samples/amp_env_loop.wav` is copied from probe `043`.
- `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx` is copied from
  probe `043`.
- `halionbridge-build/*.lua` are hand-authored HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

## HALion Candidates

- `000_linear_1s`: current converter control, `duration=1.0`, table `curve=0`.
- `001_table_neg0p25_1s`: `duration=1.0`, table `curve=-0.25`.
- `002_table_neg0p50_1s`: `duration=1.0`, table `curve=-0.50`.
- `003_table_neg0p75_1s`: `duration=1.0`, table `curve=-0.75`.
- `004_table_neg1p00_1s`: `duration=1.0`, table `curve=-1.00`.
- `005_table_neg0p25_0p604`: `duration=0.604`, table `curve=-0.25`.
- `006_table_neg0p50_0p604`: `duration=0.604`, table `curve=-0.50`.
- `007_table_neg0p75_0p604`: `duration=0.604`, table `curve=-0.75`.
- `008_table_neg1p00_0p604`: `duration=0.604`, table `curve=-1.00`.

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\045-amplitude-release-negative-curve-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx`.
3. Load `source/000_release_1s_reference.sfz` in sforzando.
4. Compare against each numbered HALion `.vstpreset`.

Use the phase-inverted/null-test setup and focus only on the release tail after
note-off. Case `000` should reproduce the known linear mismatch from probe
`043`.

## Result

HALion build completed successfully on 2026-06-14: nine hand-authored scripts
processed, nine `.vstpreset` files saved.

Manual validation on 2026-06-14:

- `008_table_neg1p00_0p604` is the best match for the fixed sforzando
  `ampeg_release=1.0` reference.

Decision: for the tested one-second release case, HALion Lua table
`curve=-1.0` with duration `0.604` is the current best candidate. This is
evidence for the explicit `ampeg_release=1.0` case only; broader release-time
scaling should either be implemented conservatively from this ratio and
regression-tested, or verified with a follow-up duration sweep.
