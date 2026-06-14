# 054 Filter Basics Diagnostic

This probe follows `053-amplitude-start-implementation-regression`.

It is the first controlled filter probe for ConvertWithMoss/SFZv2 parity. The
current converter only emits optional `Filter.Cutoff`; this probe checks whether
that is enough for a basic SFZ low-pass filter, and starts calibrating
resonance and velocity-to-cutoff behavior.

Local HALion empirical parameter evidence comes from
`sampler-dev-docs/scripting-examples/halion/halion-parameters.md`, which lists
`Filter.Type`, `Filter.Cutoff`, `Filter.Resonance`, `Filter.Mode`,
`Filter.ShapeA`, and `Filter.VelocityToCutoff` on sample zones.

## Files

- `source/*.sfz` are same-number sforzando references. Several files
  intentionally duplicate SFZ settings so different HALion candidates can be
  compared without changing probe directories.
- `source/samples/saw_A3_single_cycle.wav` is copied from probe `022`.
- `plugnscript/halionbridge_sfz_filter_generator.cxx` emits note 57 at
  velocities 32, 64, 100, and 127.
- `halionbridge-build/*.lua` are hand-authored HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

## Cases

All cases use the same looped saw sample on note 57.

| Case | Source behavior | HALion candidate |
| --- | --- | --- |
| `000` | no filter | no filter parameters |
| `001` | `fil_type=lpf_2p`, `cutoff=1000` | `Filter.Cutoff=1000` only |
| `002` | same | `Filter.Type=0`, `Filter.Cutoff=1000` |
| `003` | same | `Filter.Type=0`, `Filter.Mode=0`, `Filter.ShapeA=0`, `Filter.Cutoff=1000` |
| `004` | same | `Filter.Type=1`, `Filter.Mode=0`, `Filter.ShapeA=0`, `Filter.Cutoff=1000` |
| `005` | `fil_type=lpf_2p`, `cutoff=1000`, `resonance=12` | cutoff, resonance `0` |
| `006` | same | cutoff, resonance `12` |
| `007` | same | cutoff, resonance `24` |
| `008` | `fil_type=lpf_2p`, `cutoff=1000`, `fil_veltrack=2400` | cutoff, velocity-to-cutoff `0` |
| `009` | same | cutoff, velocity-to-cutoff `100` |
| `010` | same | cutoff, velocity-to-cutoff `200` |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\054-filter-basics-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_filter_generator.cxx`.
3. Compare same-number pairs from `000` through `010`.

Use phase inversion/null testing where practical, but filters may leave larger
residuals than gain/pan/pitch probes. For `001` through `004`, focus on overall
brightness. For `005` through `007`, focus on the resonant edge around the
cutoff. For `008` through `010`, focus on whether brightness increases across
the four generated velocities similarly in sforzando and HALion.

## Result

HALion build completed successfully on 2026-06-14: eleven hand-authored
scripts processed, eleven `.vstpreset` files saved.

The Plug'n Script generator was corrected after initial manual loading showed
no MIDI output. The fix avoids a fixed-size global array and uses the same
conservative function-based style as the earlier working MIDI generators.

The hand-authored HALion region data was corrected to set
`sample_playback.finish = 199`, matching the 200-frame single-cycle source
sample. This initializes HALion `SampleOsc.SampleEnd` to marker position `200`
before loop playback and avoids random early playback stops caused by an unset
sample end marker.

Manual validation pending.
