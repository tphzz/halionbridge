# 047 Amplitude Release Two Segment Diagnostic

This probe follows `046-amplitude-release-duration-scale-diagnostic`.

Probe `046` showed that a single HALion release segment using
`duration = sfz_release * 0.604` and Lua table `curve=-1.0` works reasonably
for multiple explicit SFZ release times. Manual tuning of the longest case
found a closer-looking shape with an early release point around `194 ms`,
`Level=35`, and HALion UI `Curve=-2.4` before the final zero point. For the
`ampeg_release=2.0` case, that is approximately:

- early point time: `sfz_release * 0.097`,
- early point level: `0.35`,
- early point Lua curve: `-0.24`, assuming HALion's UI `-10..10` display maps
  to Lua table curve `-1..1`,
- final zero time: `sfz_release * 0.604`.

This probe checks whether that two-segment approximation is stable across
shorter and longer release times, and whether the final segment should stay
curved or be linear.

## Files

- `source/*.sfz` are the sforzando references. Several files intentionally
  duplicate the same SFZ release time so every HALion candidate has a
  same-number source reference.
- `source/samples/amp_env_loop.wav` is copied from probe `043`.
- `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx` is copied from
  probe `043`.
- `halionbridge-build/*.lua` are hand-authored HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

## Cases

Start with `000` through `003`. These are the direct two-segment candidates
based on the manually tuned `046/003` shape.

| Case | SFZ release | HALion shape |
| --- | ---: | --- |
| `000` | `0.25` | early `0.097r`, level `0.35`, curve `-0.24`; final `0.604r`, curve `-1.0` |
| `001` | `0.50` | early `0.097r`, level `0.35`, curve `-0.24`; final `0.604r`, curve `-1.0` |
| `002` | `1.00` | early `0.097r`, level `0.35`, curve `-0.24`; final `0.604r`, curve `-1.0` |
| `003` | `2.00` | early `0.097r`, level `0.35`, curve `-0.24`; final `0.604r`, curve `-1.0` |
| `004` | `0.25` | copied single-segment baseline: final `0.604r`, curve `-1.0` |
| `005` | `0.50` | copied single-segment baseline: final `0.604r`, curve `-1.0` |
| `006` | `1.00` | copied single-segment baseline: final `0.604r`, curve `-1.0` |
| `007` | `2.00` | copied single-segment baseline: final `0.604r`, curve `-1.0` |
| `008` | `0.25` | same as `000`, but final segment curve `0.0` |
| `009` | `0.50` | same as `001`, but final segment curve `0.0` |
| `010` | `1.00` | same as `002`, but final segment curve `0.0` |
| `011` | `2.00` | same as `003`, but final segment curve `0.0` |

`r` means the explicit SFZ `ampeg_release` value for that row. HALion envelope
point durations are segment durations, so the final segment duration is
`(0.604 - 0.097) * r` for the two-segment candidates.

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\047-amplitude-release-two-segment-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx`.
3. Compare same-number pairs:
   - `source/000_release_0p25_two_l35_c024_tailneg1_reference.sfz` vs `halionbridge-build/000_release_0p25_two_l35_c024_tailneg1.vstpreset`,
   - `source/001_release_0p50_two_l35_c024_tailneg1_reference.sfz` vs `halionbridge-build/001_release_0p50_two_l35_c024_tailneg1.vstpreset`,
   - continue with the same same-number rule through `011`.

Use the phase-inverted/null-test setup and focus on the release tail after
note-off. The priority result is whether `000` through `003` beat their
same-duration baselines `004` through `007`; `008` through `011` isolate
whether the final release segment should use a linear tail instead.

## Result

HALion build completed successfully on 2026-06-14: twelve hand-authored scripts
processed, twelve `.vstpreset` files saved.

Manual validation on 2026-06-14:

- The curved two-segment candidates `000` through `003` were better than the
  single-segment baselines in all tested release durations.
- The linear-tail two-segment candidates `008` through `011` were always worse.

Decision: implement the curved two-segment release mapping for positive SFZ
`ampeg_release` values. Keep explicit `ampeg_release=0` as an immediate final
zero point.
