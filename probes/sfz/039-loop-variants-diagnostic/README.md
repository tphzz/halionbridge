# 039 Loop Variants Diagnostic

This probe follows `038-sample-playback-range-implementation-regression`.

It is a hand-authored diagnostic for SFZ loop variants before changing the
converter. The current converter only emits basic continuous/sustain loop points;
it does not yet map `loop_crossfade`, `loop_tune`, or `loop_type`, and HALion's
`SampleOsc.SustainLoopModeA` enum still needs empirical calibration.

## Sample Fixture

`source/samples/loop_segments.wav` is mono 44.1 kHz and 132300 samples long:

| Range | Content |
| --- | --- |
| `0..11024` | 220 Hz attack/control tone. |
| `11025..22049` | 330 Hz loop segment. |
| `22050..66149` | 660 Hz post-loop tail tone. |
| `66150..132299` | silence. |

The SFZ loop end is intentionally `22049`. Probe `037` verified that this maps
to HALion marker value `22050`.

## SFZ References

| SFZ | Purpose |
| --- | --- |
| `000_no_loop.sfz` | No-loop baseline. |
| `001_one_shot.sfz` | One-shot behavior with loop points present. |
| `002_loop_continuous.sfz` | Continuous loop, including release. |
| `003_loop_sustain.sfz` | Sustain loop, expected to stop looping after note-off. |
| `004_loop_crossfade_20ms.sfz` | Continuous loop plus `loop_crossfade=0.02`. |
| `005_loop_tune_plus_50.sfz` | Continuous loop plus `loop_tune=50`. |
| `006_loop_type_forward.sfz` | Explicit forward loop type. |
| `007_loop_type_alternate.sfz` | Alternate loop type diagnostic. |
| `008_loop_type_backward.sfz` | Backward loop type diagnostic. |

## HALion Candidates

| HALion preset | Purpose |
| --- | --- |
| `000_no_loop.vstpreset` | No-loop HALion baseline. |
| `001_mode_0_natural_end.vstpreset` .. `006_mode_5_natural_end.vstpreset` | Sweep documented HALion `SampleOsc.SustainLoopModeA` values `0..5` while leaving sample end natural. |
| `007_mode_1_loop_end_sample_end.vstpreset` | Current helper-style behavior: mode `1` and `SampleOsc.SampleEnd=22050`. |
| `008_xfade_882_curve_0.vstpreset` | 20 ms crossfade candidate, curve 0. |
| `009_xfade_882_curve_50.vstpreset` | 20 ms crossfade candidate, curve 50. |
| `010_xfade_882_curve_100.vstpreset` | 20 ms crossfade candidate, curve 100. |
| `011_tune_plus_50.vstpreset` | `SampleOsc.SustainLoopTuningA=50` candidate. |

## Regenerate Presets

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\039-loop-variants-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_loop_variant_generator.cxx`.
3. Load one SFZ reference in sforzando and candidate HALion presets in HALion.
4. Compare `002_loop_continuous.sfz` and `003_loop_sustain.sfz` against HALion
   mode candidates `001` through `006`.
5. Compare `002_loop_continuous.sfz` against `007` to decide whether the current
   helper-style `SampleEnd=loop_end+1` truncation is still valid for looped
   samples with a post-loop tail.
6. Compare `004_loop_crossfade_20ms.sfz` against `008` through `010`.
7. Compare `005_loop_tune_plus_50.sfz` against `011`.
8. Treat `006`, `007`, and `008` SFZ loop-type files as diagnostic-only unless a
   HALion mode candidate clearly matches alternate or backward looping.

Record which candidates match by number. If a behavior only gets close by ear,
record that as approximate rather than verified.

## Result

HALion build completed successfully on 2026-06-14: twelve hand-authored scripts
processed, twelve `.vstpreset` files saved. Manual sforzando/HALion validation
is still required before changing the converter.

Manual validation found this probe unsuitable as loop-mode evidence: the
sforzando loop clicks, and the generated Plug'n Script notes are too short to
separate loop-mode behavior from the still-unmatched release envelope. Use
`040-loop-variants-clean-long-diagnostic` for the follow-up loop-mode decision.
