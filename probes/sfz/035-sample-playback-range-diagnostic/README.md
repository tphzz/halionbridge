# 035 Sample Playback Range Diagnostic

This probe identifies the HALion mapping for SFZ `offset` and `end` before the
converter implements those opcodes.

SFZ `end` is inclusive. The local SFZ v2 specification states that `end=133000`
plays through sample 133000, and that a 44100-sample file is numbered from 0 to
44099. HALion exposes `SampleOsc.SampleStart` and `SampleOsc.SampleEnd`, but this
probe intentionally tests whether `SampleOsc.SampleEnd` expects the literal SFZ
end sample or an exclusive `end + 1` value.

## Sample

`source/samples/segmented_four_tones.wav` is a mono 44.1 kHz test sample with
four 0.5-second segments:

| Segment | Sample range | Tone |
| --- | --- | --- |
| 0 | 0-22049 | 220 Hz |
| 1 | 22050-44099 | 330 Hz |
| 2 | 44100-66149 | 440 Hz |
| 3 | 66150-88199 | 660 Hz |

## SFZ References

| SFZ | Expected playback |
| --- | --- |
| `000_full_sample.sfz` | All four segments. |
| `001_offset_22050.sfz` | Starts at segment 1. |
| `002_offset_44100.sfz` | Starts at segment 2. |
| `003_end_22049.sfz` | Segment 0 only. |
| `004_end_44099.sfz` | Segments 0 and 1 only. |
| `005_offset_22050_end_44099.sfz` | Segment 1 only. |

## HALion Candidates

| Preset script | Purpose |
| --- | --- |
| `000_full_sample.lua` | Full-sample baseline. |
| `001_sample_start_22050.lua` | `SampleOsc.SampleStart = 22050`. |
| `002_sample_start_44100.lua` | `SampleOsc.SampleStart = 44100`. |
| `003_sample_end_22049.lua` | Literal candidate for `end=22049`. |
| `004_sample_end_22050.lua` | Exclusive-boundary candidate for `end=22049`. |
| `005_sample_end_44099.lua` | Literal candidate for `end=44099`. |
| `006_sample_end_44100.lua` | Exclusive-boundary candidate for `end=44099`. |
| `007_start_22050_end_44099.lua` | Literal combined candidate. |
| `008_start_22050_end_44100.lua` | Exclusive-boundary combined candidate. |

## Regenerate Presets

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\035-sample-playback-range-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_sample_playback_range_generator.cxx`.
3. For each SFZ reference, load the matching `source/*.sfz` in sforzando and
   observe the segment sequence by ear, oscilloscope, or meter.
4. Replace sforzando with HALion and load the matching candidate `.vstpreset`.
5. Compare `003_end_22049.sfz` against HALion presets `003` and `004`.
6. Compare `004_end_44099.sfz` against HALion presets `005` and `006`.
7. Compare `005_offset_22050_end_44099.sfz` against HALion presets `007` and
   `008`.
8. Report which candidate matches each SFZ reference. Use phase cancellation
   where practical, but the tone-segment order and cutoff boundary are the main
   diagnostic signal.

## Result

Manual validation on 2026-06-14:

- `000_full_sample.vstpreset` worked.
- `001_sample_start_22050.vstpreset` and
  `002_sample_start_44100.vstpreset` did not work because HALion left the
  sample end marker at `0` while the start marker moved after it.
- `003_sample_end_22049.vstpreset` worked against the first-segment SFZ
  reference.

This probe exposed a setup issue rather than disproving `SampleOsc.SampleStart`.
Use probe `036-sample-playback-range-end-initialized` for the corrected
`offset`/`end` validation because it initializes `SampleOsc.SampleEnd` before
writing `SampleOsc.SampleStart`.
