# 036 Sample Playback Range End-Initialized Follow-Up

This probe follows `035-sample-playback-range-diagnostic`.

Manual validation of `035` showed that setting `SampleOsc.SampleStart` without
also setting `SampleOsc.SampleEnd` can produce an invalid HALion range: the
sample end marker remains at `0`, and a later start marker is placed behind it.
This probe keeps the same SFZ references and segmented sample, but every HALion
candidate that sets a non-zero start also initializes `SampleOsc.SampleEnd`.

The Lua variant intentionally writes `SampleOsc.SampleEnd` before
`SampleOsc.SampleStart` so HALion does not clamp the start marker against the
default end marker.

## Sample

`source/samples/segmented_four_tones.wav` is copied from probe `035`. It is a
mono 44.1 kHz test sample with four 0.5-second segments:

| Segment | Sample range | Tone |
| --- | --- | --- |
| 0 | 0-22049 | 220 Hz |
| 1 | 22050-44099 | 330 Hz |
| 2 | 44100-66149 | 440 Hz |
| 3 | 66150-88199 | 660 Hz |

## Comparison Matrix

Do not compare SFZ and HALion files by the same numeric prefix in this probe.
The HALion files are a candidate matrix. For example, SFZ
`002_offset_44100.sfz` is meant to be compared with HALion
`003_start_44100_end_88199.vstpreset`.

| SFZ reference | HALion candidate(s) |
| --- | --- |
| `000_full_sample.sfz` | `000_full_default.vstpreset`, then `001_full_explicit_end_88199.vstpreset` as an explicit-end sanity check. |
| `001_offset_22050.sfz` | `002_start_22050_end_88199.vstpreset`. |
| `002_offset_44100.sfz` | `003_start_44100_end_88199.vstpreset`. |
| `003_end_22049.sfz` | `004_end_22049.vstpreset` vs `005_end_22050.vstpreset`. |
| `004_end_44099.sfz` | `006_end_44099.vstpreset` vs `007_end_44100.vstpreset`. |
| `005_offset_22050_end_44099.sfz` | `008_start_22050_end_44099.vstpreset` vs `009_start_22050_end_44100.vstpreset`. |

## Regenerate Presets

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\036-sample-playback-range-end-initialized\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_sample_playback_range_generator.cxx`.
3. Load one `source/*.sfz` reference in sforzando and observe the tone segment
   sequence by ear, oscilloscope, or meter.
4. Replace sforzando with HALion and load the candidate(s) listed above.
5. For end-boundary rows, report whether the literal end candidate or the
   `end + 1` candidate matches sforzando.
6. For start rows, confirm that HALion starts on the same tone segment as
   sforzando after the end marker has been initialized.

## Prior Result From 035

Manual validation on 2026-06-14:

- `000_full_sample` worked.
- Start-only candidates `001` and `002` did not work because `SampleEnd` stayed
  at `0` while `SampleStart` moved behind it.
- Literal `003_sample_end_22049` worked.
- The follow-up must determine whether larger and combined end cases need the
  literal SFZ inclusive `end` or `end + 1` once start/end write order is fixed.

## Partial Result

Manual validation on 2026-06-14:

- `000_full_sample.sfz` matched `000_full_default.vstpreset`.
- Comparing by equal numeric prefixes is misleading in this probe:
  `001_full_explicit_end_88199.vstpreset` intentionally has
  `SampleOsc.SampleStart=0`, so it is not the HALion candidate for
  `001_offset_22050.sfz`.
- `002_offset_44100.sfz` matched
  `003_start_44100_end_88199.vstpreset`, which is positive evidence that
  `SampleOsc.SampleStart=44100` works when `SampleOsc.SampleEnd=88199` is
  initialized first.
- `001_offset_22050.sfz` matched
  `002_start_22050_end_88199.vstpreset`.
- `003_end_22049.sfz` matched both `004_end_22049.vstpreset` and
  `005_end_22050.vstpreset` well enough that the one-sample boundary difference
  was not audible.
- `004_end_44099.sfz` matched both `006_end_44099.vstpreset` and
  `007_end_44100.vstpreset` well enough that the one-sample boundary difference
  was not audible.
- `005_offset_22050_end_44099.sfz` matched both
  `008_start_22050_end_44099.vstpreset` and
  `009_start_22050_end_44100.vstpreset` well enough that the one-sample
  boundary difference was not audible.

Decision: `offset` maps to `SampleOsc.SampleStart` when `SampleOsc.SampleEnd`
is initialized first. Probe `036` cannot decide whether SFZ inclusive `end`
maps to literal HALion `SampleOsc.SampleEnd` or `end + 1`; use probe
`037-sample-end-boundary-impulse` for that boundary-only decision.
