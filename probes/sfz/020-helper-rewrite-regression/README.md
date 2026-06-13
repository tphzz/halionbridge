# SFZ helper rewrite regression probe

This probe validates the Phase 2 SFZ converter rewrite, where generated build
scripts require `halionbridge-sfz.lua` and pass normalized region tables into
the helper.

## Generate HALion build scripts

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe convert sfz `
  probes\sfz\020-helper-rewrite-regression\source `
  probes\sfz\020-helper-rewrite-regression\halionbridge-build `
  --overwrite
```

Expected generated files:

- `halionbridge-build/000_probe.lua`
- `halionbridge-build/halionbridge-sfz.lua`
- `halionbridge-build/halionbridge_build.lua`

## Manual audition loop

1. Load `source/probe.sfz` in sforzando.
2. Run the generated HALion build directory with halionbridge.
3. Load the resulting HALion preset in HALion 7.
4. Compare by ear on A3, nearby mapped keys, and low/high velocities.

The expected result is one looping sine region mapped from C3 to C5, rooted at
A3, with a short attack/release envelope and a low-pass filter. HALion should
not need any inline helper functions in `000_probe.lua`; all HALion object
assignment should go through `halionbridge-sfz.lua`.

The source declares `loop_end=199` for a 200-sample single-cycle WAV. Generated
region data should preserve `finish = 199`, while `halionbridge-sfz.lua` should
write HALion end marker `200` to both `SampleOsc.SampleEnd` and
`SampleOsc.SustainLoopEndA`.
