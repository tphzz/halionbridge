# 029 Amp Velocity Curve Sweep Diagnostic

This probe isolates HALion's amp-envelope **Level Velocity Curve** index.

Probe `028` showed that changing velocity depth and level compensation can make
the top velocity closer, but the lower velocities still do not follow the SFZ
default curve. The HALion manual says the relevant control is the amp envelope
**Level Velocity Curve**, and that the squared curve is commonly used for
normalized samples. This probe sweeps the integer Lua parameter
`Amp Env.VelocityToLevelCurve` from `0` through `9`.

## Files

- `source/000_amp_velocity_curve_reference.sfz` is the sforzando reference.
- `source/samples/sine_A3_single_cycle.wav` is copied into the probe.
- `plugnscript/halionbridge_sfz_amp_velocity_curve_sweep_generator.cxx`
  emits note 57 at velocities 32, 64, 100, and 127.
- `halionbridge-build/*.vstpreset` are ready-to-load HALion presets after the
  build command succeeds.

## HALion Variants

Every preset sets:

```lua
Amp Env.VelocityToLevel = 100
Amp Env.VelocityToLevelCurve = N
```

where `N` is the number in the preset name.

| Preset | Curve index |
| --- | ---: |
| `000_velocity_curve_0.vstpreset` | 0 |
| `001_velocity_curve_1.vstpreset` | 1 |
| `002_velocity_curve_2.vstpreset` | 2 |
| `003_velocity_curve_3.vstpreset` | 3 |
| `004_velocity_curve_4.vstpreset` | 4 |
| `005_velocity_curve_5.vstpreset` | 5 |
| `006_velocity_curve_6.vstpreset` | 6 |
| `007_velocity_curve_7.vstpreset` | 7 |
| `008_velocity_curve_8.vstpreset` | 8 |
| `009_velocity_curve_9.vstpreset` | 9 |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\029-amp-velocity-curve-sweep-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Load `source/000_amp_velocity_curve_reference.sfz` in sforzando.
2. Put Blue Cat Plug'n Script immediately before sforzando and load
   `plugnscript/halionbridge_sfz_amp_velocity_curve_sweep_generator.cxx`.
3. Record or meter the four steady-state note levels for velocities
   32, 64, 100, and 127.
4. Replace sforzando with each HALion preset, keeping the same Plug'n Script
   instance and meter.
5. Find the curve index whose relative differences between the four velocities
   most closely match sforzando. Do not judge absolute level yet; this probe is
   only for the velocity curve shape.

Expected sforzando reference values from probe `027`:

| Velocity | Sforzando |
| --- | ---: |
| 32 | -32.0 dB |
| 64 | -20.0 dB |
| 100 | -12.2 dB |
| 127 | -8.1 dB |
