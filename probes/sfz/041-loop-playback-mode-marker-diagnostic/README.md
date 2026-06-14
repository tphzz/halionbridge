# 041 Loop Playback Mode Marker Diagnostic

This probe follows `040-loop-variants-clean-long-diagnostic`.

Probe `040` was still not reliable because the HALion candidates changed only
the sustain loop mode while leaving `PlaybackMode`, `LoopSelect`, and sample
start/end interactions mostly untested. Manual validation also showed HALion
starting at sample offset `0` while the sforzando reference was already starting
from the loop tone, and manual HALion marker edits still clicked. This probe
tests those missing dimensions explicitly.

## Sample Fixture

`source/samples/pre_loop_tail_clean.wav` is mono 44.1 kHz and 154100 samples
long.

| Range | Content |
| --- | --- |
| `0..10999` | 220.5 Hz pre-loop tone. |
| `11000..21999` | 441 Hz loop tone. |
| `22000..109999` | 882 Hz post-loop tail. |
| `110000..154099` | silence. |

Every boundary is zero-crossing aligned:

- `0..10999` is 55 cycles of 220.5 Hz.
- `11000..21999` is 110 cycles of 441 Hz.
- `22000..109999` is 1760 cycles of 882 Hz.

SFZ `loop_end=21999` should map to HALion marker `22000` if the loop-end marker
uses the same exclusive marker behavior as `SampleOsc.SampleEnd`.

## SFZ References

| SFZ | Purpose |
| --- | --- |
| `000_no_loop_full_start.sfz` | Full sample baseline from sample offset 0. |
| `001_loop_continuous_full_start.sfz` | Full sample start with continuous loop points. |
| `002_loop_continuous_offset_to_loop.sfz` | Starts at the loop tone via `offset=11000`, then loops continuously. |
| `003_loop_sustain_offset_to_loop.sfz` | Starts at the loop tone via `offset=11000`, then should leave the loop after note-off. |

## HALion Candidates

| HALion preset | Purpose |
| --- | --- |
| `000_no_loop.vstpreset` | No-loop baseline. |
| `001_full_start_continuous.vstpreset` | Normal playback, loop set A, continuous loop, sample start 0. |
| `002_offset_continuous_reference.vstpreset` | Expected offset continuous candidate: `SampleStart=11000`, `SampleEnd=110000`, loop `11000..22000`. |
| `003_offset_until_release.vstpreset` | Expected sustain candidate if HALion mode value `4` is Until Release. |
| `004_offset_continuous_loopselect_1.vstpreset` | Same as `002`, but `LoopSelect=1`. |
| `005`..`007` | Playback mode values `1..3` with the offset continuous setup. |
| `008`..`011` | Loop start/end off-by-one sweep around the expected `11000..22000` markers. |
| `012_until_release_release_start_tail.vstpreset` | Until Release plus `ReleaseStart=22000`. |
| `013_mode_5_offset.vstpreset` | Mode value `5`, expected Alternate Until Release from the HALion manual menu order. |
| `014_current_helper_truncated.vstpreset` | Current helper-style truncation: sample end set to loop end marker. |

## Regenerate Presets

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\041-loop-playback-mode-marker-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_loop_marker_generator.cxx`.
3. First verify `000_no_loop_full_start.sfz` against `000_no_loop.vstpreset`.
4. Compare `001_loop_continuous_full_start.sfz` against
   `001_full_start_continuous.vstpreset`.
5. Compare `002_loop_continuous_offset_to_loop.sfz` against candidates `002`
   through `011` and `014`.
6. Compare `003_loop_sustain_offset_to_loop.sfz` against candidates `003`,
   `012`, and `013`.

Report whether HALion starts on the pre-loop tone, loop tone, or tail tone, and
which candidate has the cleanest loop transition. If none match, this is
evidence that the HALion loop-marker write path needs a separate GUI/readback
probe before converter implementation.

## Result

HALion build completed successfully on 2026-06-14: fifteen hand-authored
scripts processed, fifteen `.vstpreset` files saved. Manual sforzando/HALion
validation is still required before changing the converter.

Manual validation on 2026-06-14:

- `000_no_loop_full_start.sfz` matched `000_no_loop.vstpreset`.
- `001_loop_continuous_full_start.sfz` matched
  `001_full_start_continuous.vstpreset` when disregarding the known release
  envelope difference.
- `002_loop_continuous_offset_to_loop.sfz` matched
  `002_offset_continuous_reference.vstpreset` closely enough that remaining
  difference is likely the known release-envelope mismatch.
- Candidates `003` through `011` and `014` were off for the `002` continuous
  offset reference.
- `003_loop_sustain_offset_to_loop.sfz` matched
  `003_offset_until_release.vstpreset`.
- `012_until_release_release_start_tail.vstpreset` sounded similar but uses
  `ReleaseStart=22000`, which is less faithful to SFZ `loop_sustain` semantics
  because it forces a release jump.
- `013_mode_5_offset.vstpreset` did not match.

Decision: map `loop_continuous` to HALion sustain loop mode `1`, map
`loop_sustain` to mode `4`, keep `PlaybackMode=0` and `LoopSelect=0`, write
`loop_end + 1` to `SustainLoopEndA`, and do not derive `SampleEnd` or
`ReleaseStart` from loop points.
