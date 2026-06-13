# 030 Main Velocity Curve Sweep Diagnostic

This probe isolates HALion's Main-section **Level Velocity Curve** on the
generated layer.

The control shown in HALion's Main section is not the same parameter as
`Amp Env.VelocityToLevelCurve`. The HALion user manual says this Main-section
curve remaps incoming MIDI velocity before it is sent to the program or layer.
This probe sets the layer-local parameter `VelocityToLevelCurve` from `0`
through `9` while keeping the amp-envelope velocity curve linear.

## Files

- `source/000_main_velocity_curve_reference.sfz` is the sforzando reference.
- `source/samples/sine_A3_single_cycle.wav` is copied into the probe.
- `plugnscript/halionbridge_sfz_main_velocity_curve_sweep_generator.cxx`
  emits note 57 at velocities 32, 64, 100, and 127.
- `halionbridge-build/*.vstpreset` are ready-to-load HALion presets after the
  build command succeeds.

## HALion Variants

Every preset sets these layer parameters:

```lua
InheritVelocitySettings = false
VelocityToLevelCurve = N
```

and these zone amp-envelope parameters:

```lua
Amp Env.VelocityToLevel = 100
Amp Env.VelocityToLevelCurve = 0
```

Based on the visible HALion menu order, index `1` is expected to be **Squared**,
but all ten indices are provided for measurement.

| Preset | Main velocity curve index | Expected menu label |
| --- | ---: | --- |
| `000_main_velocity_curve_0.vstpreset` | 0 | Linear |
| `001_main_velocity_curve_1.vstpreset` | 1 | Squared |
| `002_main_velocity_curve_2.vstpreset` | 2 | Squared Inverse |
| `003_main_velocity_curve_3.vstpreset` | 3 | 2 Poles Squared |
| `004_main_velocity_curve_4.vstpreset` | 4 | 2 Poles Squared Inverse |
| `005_main_velocity_curve_5.vstpreset` | 5 | Cubic |
| `006_main_velocity_curve_6.vstpreset` | 6 | Quadric |
| `007_main_velocity_curve_7.vstpreset` | 7 | dB |
| `008_main_velocity_curve_8.vstpreset` | 8 | Logarithmic |
| `009_main_velocity_curve_9.vstpreset` | 9 | Constant (127) |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\030-main-velocity-curve-sweep-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Load `source/000_main_velocity_curve_reference.sfz` in sforzando.
2. Put Blue Cat Plug'n Script immediately before sforzando and load
   `plugnscript/halionbridge_sfz_main_velocity_curve_sweep_generator.cxx`.
3. Record or meter the four steady-state note levels for velocities
   32, 64, 100, and 127.
4. Replace sforzando with each HALion preset, keeping the same Plug'n Script
   instance and meter.
5. First compare relative velocity shape. Absolute level and pan law should be
   tested separately after the closest curve shape is known.

Expected sforzando reference values from probe `027`:

| Velocity | Sforzando |
| --- | ---: |
| 32 | -32.0 dB |
| 64 | -20.0 dB |
| 100 | -12.2 dB |
| 127 | -8.1 dB |
