# 032 Gain and Velocity Implementation Regression

This probe validates the implemented SFZ gain and amplitude-velocity mapping
after probes `030` and `031`.

The generated converter output should now set:

```lua
Layer.InheritVelocitySettings = false
Layer.VelocityToLevelCurve = 1
Zone["SampleOsc.Level"] = sfz_volume + 7.8
Zone["Amp Env.VelocityToLevel"] = clamp(amp_veltrack, -100, 100)
```

## Files

- `source/*.sfz` are the sforzando references.
- `source/samples/sine_A3_single_cycle.wav` is copied into the probe.
- `plugnscript/halionbridge_sfz_gain_velocity_implementation_generator.cxx`
  emits note 57 at velocities 32, 64, 100, and 127.
- `halionbridge-build/*.vstpreset` are ready-to-load HALion presets after the
  build command succeeds.

## Cases

| SFZ | Purpose |
| --- | --- |
| `000_gain_velocity_baseline.sfz` | Default `volume=0`, default `amp_veltrack=100`. |
| `001_volume_minus_6.sfz` | Verifies -6 dB static volume relative to baseline. |
| `002_volume_plus_6.sfz` | Verifies +6 dB static volume relative to baseline. |
| `003_amp_veltrack_0.sfz` | Verifies velocity no longer changes level. |
| `004_amp_veltrack_50.sfz` | Verifies reduced dynamic velocity range. |
| `005_amp_veltrack_200.sfz` | Out-of-range SFZ value; conversion should warn and clamp to `100`; this is not an audio-parity target. |

## Regenerate

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe convert sfz probes\sfz\032-gain-velocity-implementation-regression\source probes\sfz\032-gain-velocity-implementation-regression\halionbridge-build --overwrite
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\032-gain-velocity-implementation-regression\halionbridge-build --timeout-seconds 120
```

The conversion step is expected to warn for `005_amp_veltrack_200.sfz` because
SFZ `amp_veltrack` is limited to `-100..100`; the generated Lua should write
`amp_velocity_to_level = 100`. Manual validation showed that sforzando plays the
out-of-range source with a non-monotonic velocity response, so this case is only
kept to verify the converter warning/clamp behavior. Do not use it as an audible
parity acceptance case.

## Measurement Loop

1. Load one `source/*.sfz` file in sforzando.
2. Put Blue Cat Plug'n Script immediately before sforzando and load
   `plugnscript/halionbridge_sfz_gain_velocity_implementation_generator.cxx`.
3. Measure or null-test the four steady-state note levels for velocities
   32, 64, 100, and 127.
4. Replace sforzando with the matching HALion `.vstpreset`, keeping the same
   Plug'n Script instance and meter.
5. Confirm the baseline nearly nulls, volume cases differ by +/-6 dB, and the
   valid `amp_veltrack` cases follow the expected velocity response. Treat
   `005_amp_veltrack_200` as a converter warning/clamp check only.
