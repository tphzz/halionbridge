# Loop end/sample end diagnostic probe

This probe isolates HALion sample-zone marker behavior for the 200-sample
single-cycle sine used by `020-helper-rewrite-regression`.

The HALion manual describes **Loop Mode: Continuous** as looping until the end of
the amplitude envelope, and describes **Sample End** as the marker where sample
playback stops. Steinberg's own sample import script sets
`SampleOsc.SampleEnd` before setting loop markers. These variants test whether a
HALion loop end of `200.0` is stable only when the sample end marker is also
explicitly set to `200.0`.

## Build presets

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe `
  probes\sfz\021-loop-end-sample-end-diagnostic\halionbridge-build `
  --timeout-seconds 120
```

## Audition order

1. `probe_loop_end_199.vstpreset`
2. `probe_loop_end_200.vstpreset`
3. `probe_sample_end_200_loop_end_200.vstpreset`

Compare them against:

```text
probes/sfz/020-helper-rewrite-regression/source/probe.sfz
```

Expected interpretation:

- If only variant 2 stops, `SustainLoopEndA=200` needs an explicit sample end.
- Confirmed: variant 3 sustains and sounds smooth. Generated SFZ output should
  preserve inclusive SFZ `loop_end` in region data, then write the exclusive
  HALion end marker to both `SampleOsc.SampleEnd` and
  `SampleOsc.SustainLoopEndA`.
- If variants 2 and 3 both stop, the fault is probably not sample end and we
  should inspect loop mode or envelope mode/readback next.
