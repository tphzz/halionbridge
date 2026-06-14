# 037 Sample End Boundary Impulse

This probe follows `036-sample-playback-range-end-initialized`.

Probe `036` validated `offset`/`SampleOsc.SampleStart` behavior, but the
one-sample `end` difference was not audible with sustained tones and the current
release-envelope mismatch. This probe uses silence plus a single impulse exactly
at the target boundary sample. The right boundary candidate should be obvious as
a click/no-click result.

## Samples

Both samples are mono 44.1 kHz, 88200 samples long, and silent except for one
full-scale impulse:

| Sample | Impulse index |
| --- | ---: |
| `impulse_at_22049.wav` | 22049 |
| `impulse_at_44099.wav` | 44099 |

## SFZ References

| SFZ | Expected SFZ result |
| --- | --- |
| `000_full_impulse_22049.sfz` | Click at sample 22049. |
| `001_end_22048_impulse_22049.sfz` | No click. |
| `002_end_22049_impulse_22049.sfz` | Click if SFZ `end` is inclusive. |
| `003_full_impulse_44099.sfz` | Click at sample 44099. |
| `004_end_44098_impulse_44099.sfz` | No click. |
| `005_end_44099_impulse_44099.sfz` | Click if SFZ `end` is inclusive. |
| `006_offset_22050_end_44099_impulse_44099.sfz` | Click after offset playback reaches original sample 44099. |

## HALion Candidates

| HALion preset | Purpose |
| --- | --- |
| `000_full_impulse_22049.vstpreset` | Full-sample click control for the 22049 sample. |
| `001_end_22048_impulse_22049.vstpreset` | Omit-boundary control. |
| `002_end_22049_impulse_22049.vstpreset` | Literal candidate for SFZ `end=22049`. |
| `003_end_22050_impulse_22049.vstpreset` | `end + 1` candidate for SFZ `end=22049`. |
| `004_full_impulse_44099.vstpreset` | Full-sample click control for the 44099 sample. |
| `005_end_44098_impulse_44099.vstpreset` | Omit-boundary control. |
| `006_end_44099_impulse_44099.vstpreset` | Literal candidate for SFZ `end=44099`. |
| `007_end_44100_impulse_44099.vstpreset` | `end + 1` candidate for SFZ `end=44099`. |
| `008_start_22050_end_44098_impulse_44099.vstpreset` | Combined omit-boundary control. |
| `009_start_22050_end_44099_impulse_44099.vstpreset` | Literal combined candidate. |
| `010_start_22050_end_44100_impulse_44099.vstpreset` | `end + 1` combined candidate. |

## Regenerate Presets

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\037-sample-end-boundary-impulse\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_sample_end_boundary_generator.cxx`.
3. Compare click/no-click behavior:
   - SFZ `002_end_22049_impulse_22049.sfz` against HALion `002` and `003`.
   - SFZ `005_end_44099_impulse_44099.sfz` against HALion `006` and `007`.
   - SFZ `006_offset_22050_end_44099_impulse_44099.sfz` against HALion `008`,
     `009`, and `010`.
4. The matching HALion candidate is the one whose click/no-click behavior
   matches sforzando. Metering is fine, but this probe should not depend on
   phase cancellation or release-envelope shape.

## Result

Manual validation on 2026-06-14 used a phase-reversed null-test setup where
`no click` means the HALion candidate matched sforzando:

- SFZ `002_end_22049_impulse_22049.sfz` against HALion `002` left a click;
  against HALion `003_end_22050_impulse_22049.vstpreset` it canceled.
- SFZ `005_end_44099_impulse_44099.sfz` against HALion `006` left a click;
  against HALion `007_end_44100_impulse_44099.vstpreset` it canceled.
- SFZ `006_offset_22050_end_44099_impulse_44099.sfz` against HALion `008` and
  `009` left a click; against HALion
  `010_start_22050_end_44100_impulse_44099.vstpreset` it canceled.

Decision: SFZ `end=N` is inclusive and maps to HALion
`SampleOsc.SampleEnd=N+1`. For combined `offset`/`end`, write
`SampleOsc.SampleEnd` before `SampleOsc.SampleStart`.
