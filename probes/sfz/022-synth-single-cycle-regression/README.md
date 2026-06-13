# Synth single-cycle SFZ regression probe

This ready-to-use probe validates the SFZ helper rewrite and confirmed HALion
loop marker behavior on the existing multi-region single-cycle fixture.

The source files are copied from `examples/synth-single-cycle-sfz` so this probe
is self-contained for reference-sampler and HALion audition.

## Generate HALion build scripts

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe convert sfz `
  probes\sfz\022-synth-single-cycle-regression\source `
  probes\sfz\022-synth-single-cycle-regression\halionbridge-build `
  --overwrite
```

## Build HALion presets

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe `
  probes\sfz\022-synth-single-cycle-regression\halionbridge-build `
  --timeout-seconds 120
```

## Manual audition loop

1. Load `source/000_synth-single-cycle-six-regions.sfz` in sforzando.
2. Load `halionbridge-build/synth_single_cycle_six_regions.vstpreset` in HALion 7.
3. Compare key mapping across C2 to A#5, with special attention to region
   boundaries and sustained notes.
4. Load `source/001_synth-single-cycle-three-regions-three-velocity_layers.sfz`
   in sforzando.
5. Load `halionbridge-build/synth_single_cycle_three_regions_three_velocity_layers.vstpreset`
   in HALion 7.
6. Compare low/high velocity layers in each mapped key range.

Expected result: HALion and sforzando should agree on key ranges, velocity
splits, root key, sustain looping, and the absence of loop buzz or random loop
dropout. Generated region data should keep SFZ `loop_end=199` as `finish = 199`;
`halionbridge-sfz.lua` should write HALion marker `200` to both
`SampleOsc.SampleEnd` and `SampleOsc.SustainLoopEndA`.
