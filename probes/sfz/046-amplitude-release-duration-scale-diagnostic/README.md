# 046 Amplitude Release Duration Scale Diagnostic

This probe follows `045-amplitude-release-negative-curve-diagnostic`.

Probe `045` found that, for a fixed sforzando `ampeg_release=1.0` reference,
the best HALion candidate was table `curve=-1.0` with duration `0.604`. This
probe checks whether that `0.604` duration scale holds for other explicit SFZ
release times.

## Files

- `source/*.sfz` are the sforzando references.
- `source/samples/amp_env_loop.wav` is copied from probe `043`.
- `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx` is copied from
  probe `043`.
- `halionbridge-build/*.lua` are hand-authored HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

## Cases

Each HALion candidate uses Lua envelope table `curve=-1.0` and
`duration = sfz_release * 0.604`.

| Case | SFZ release | HALion duration |
| --- | ---: | ---: |
| `000` | `0.25` | `0.151` |
| `001` | `0.50` | `0.302` |
| `002` | `1.00` | `0.604` |
| `003` | `2.00` | `1.208` |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\046-amplitude-release-duration-scale-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx`.
3. Compare same-number pairs:
   - `000_release_0p25_reference.sfz` vs `000_release_0p25_scaled.vstpreset`,
   - `001_release_0p50_reference.sfz` vs `001_release_0p50_scaled.vstpreset`,
   - `002_release_1p00_reference.sfz` vs `002_release_1p00_scaled.vstpreset`,
   - `003_release_2p00_reference.sfz` vs `003_release_2p00_scaled.vstpreset`.

Use the phase-inverted/null-test setup and focus on the release tail after
note-off.

## Result

HALion build completed successfully on 2026-06-14: four hand-authored scripts
processed, four `.vstpreset` files saved.

Manual validation on 2026-06-14:

- All four same-number pairs work reasonably well.
- While tuning the longest `003` case, a better phase-cancellation shape was
  found by adding an early release point around `194 ms`, `Level=35`, with UI
  `Curve=-2.4`, before the final zero point around the scaled release tail.

Interpretation: the single-segment candidate `duration = sfz_release * 0.604`
with Lua table `curve=-1.0` is a useful first approximation, but it is not the
best shape. The user-tuned `003` shape is better explained as a two-segment
approximation of sfizz's exponential release: quick initial fadeout, then a
slower quiet tail. For `ampeg_release=2.0`, the tuned early point corresponds
to roughly `sfz_release * 0.097` at level `0.35`; if this is implemented, it
should get a converter-backed regression probe rather than being folded into
the current single-segment mapping silently.
