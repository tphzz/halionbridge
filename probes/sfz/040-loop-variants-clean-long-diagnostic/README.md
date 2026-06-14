# 040 Loop Variants Clean Long Diagnostic

This probe follows `039-loop-variants-diagnostic`.

Probe `039` was not a reliable evidence source because the sforzando reference
loop clicked and the Plug'n Script notes were too short to distinguish loop
mode behavior from the still-unmatched release envelope. This follow-up keeps
the next probe number and narrows the test to clean loop-mode matching.

## Sample Fixture

`source/samples/clean_loop_with_tail.wav` is mono 44.1 kHz and 352800 samples
long.

| Range | Content |
| --- | --- |
| `0..21999` | 441 Hz sine. |
| `22000..110249` | 660 Hz post-loop tail. |
| `110250..352799` | silence. |

The loop section is `11000..21999`, exactly 110 cycles of 441 Hz. The next
natural sample after the loop end is also a zero crossing, so the loop can wrap
from `21999` back to `11000` without an intentional discontinuity. SFZ
`loop_end=21999` maps to HALion marker `22000`.

## SFZ References

| SFZ | Purpose |
| --- | --- |
| `000_no_loop.sfz` | No-loop baseline. |
| `001_one_shot.sfz` | One-shot behavior with loop points present. |
| `002_loop_continuous.sfz` | Continuous loop; should keep looping through release. |
| `003_loop_sustain.sfz` | Sustain loop; should leave the loop after note-off and play the 660 Hz tail during release. |
| `004_loop_tune_plus_50.sfz` | Continuous loop plus `loop_tune=50`. |

## HALion Candidates

| HALion preset | Purpose |
| --- | --- |
| `000_no_loop.vstpreset` | No-loop HALion baseline. |
| `001_mode_0_natural_end.vstpreset` .. `006_mode_5_natural_end.vstpreset` | Sweep documented HALion `SampleOsc.SustainLoopModeA` values `0..5` while leaving the full sample available after the loop. |
| `007_mode_1_loop_end_sample_end.vstpreset` | Current helper-style truncation candidate: mode `1` and `SampleOsc.SampleEnd=22000`. |
| `008_tune_plus_50.vstpreset` | `SampleOsc.SustainLoopTuningA=50` candidate. |

## Regenerate Presets

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\040-loop-variants-clean-long-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_long_loop_generator.cxx`.
3. Load `002_loop_continuous.sfz` in sforzando and compare HALion candidates
   `001` through `006`.
4. Load `003_loop_sustain.sfz` in sforzando and compare HALion candidates
   `001` through `006`.
5. Compare `002_loop_continuous.sfz` against `007` to decide whether the current
   helper-style `SampleEnd=loop_end+1` truncation should remain.
6. Compare `004_loop_tune_plus_50.sfz` against `008`.

The important observation is what happens after the five-second note-off:
continuous should keep the 441 Hz loop during release, while sustain should
move into the 660 Hz tail during release. Record the matching HALion candidate
numbers separately for continuous and sustain.

## Result

HALion build completed successfully on 2026-06-14: nine hand-authored scripts
processed, nine `.vstpreset` files saved. Manual sforzando/HALion validation is
still required before changing the converter.

Manual validation found this probe unsuitable as final loop evidence: HALion
started at sample offset `0` while the sforzando reference was already sounding
from the loop tone, and manual HALion marker edits still produced a click. Use
`041-loop-playback-mode-marker-diagnostic`, which explicitly tests
`PlaybackMode`, `LoopSelect`, `SampleStart`, `SampleEnd`, `ReleaseStart`, and
loop-marker off-by-one candidates.
