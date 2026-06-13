# 028 Amp Velocity Calibration Diagnostic

This controlled probe calibrates HALion's amplitude velocity response against
SFZ default `amp_veltrack=100`.

The SFZ v2 spec defines the default as:

```text
Gain(v) = 20 * log10((v / 127)^2) dB
```

The `027` baseline measurements showed HALion with
`Amp Env.VelocityToLevel=100` producing nearly half of that relative dynamic
range, matching `20 * log10(v / 127)`. This probe tests whether
`VelocityToLevel=200` gives the correct SFZ velocity shape and whether the
remaining full-scale offset should be compensated at `SampleOsc.Level` or
`Amp.Level`.

## Files

- `source/000_amp_velocity_reference.sfz` is the sforzando reference.
- `source/samples/sine_A3_single_cycle.wav` is copied from probe `027`.
- `plugnscript/halionbridge_sfz_amp_velocity_calibration_generator.cxx`
  emits note 57 at velocities 32, 64, 100, and 127.
- `halionbridge-build/*.vstpreset` are ready-to-load HALion presets after the
  build command succeeds.

## HALion Variants

| Preset | Parameters | Purpose |
| --- | --- | --- |
| `000_current_velocity_to_level_100.vstpreset` | `VelocityToLevel=100`, curve 0 | Current behavior from probe `027`. |
| `001_velocity_to_level_200.vstpreset` | `VelocityToLevel=200`, curve 0 | Tests SFZ relative velocity shape without gain compensation. |
| `002_velocity_to_level_200_sample_plus_7p8.vstpreset` | `VelocityToLevel=200`, `SampleOsc.Level=+7.8 dB` | Main candidate derived from the `027` vel-127 offset. |
| `003_velocity_to_level_200_sample_plus_8p0.vstpreset` | `VelocityToLevel=200`, `SampleOsc.Level=+8.0 dB` | Rounding check for the main candidate. |
| `004_velocity_to_level_200_amp_plus_7p8.vstpreset` | `VelocityToLevel=200`, `Amp.Level=+7.8 dB` | Checks whether compensation belongs at amp level instead. |
| `005_no_velocity_write.vstpreset` | no explicit velocity parameter write | Confirms whether HALion defaults differ from explicit `100`. |
| `006_no_velocity_write_sample_plus_7p8.vstpreset` | no explicit velocity write, `SampleOsc.Level=+7.8 dB` | Separates default velocity behavior from level offset. |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\028-amp-velocity-calibration-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Load `source/000_amp_velocity_reference.sfz` in sforzando.
2. Put Blue Cat Plug'n Script immediately before sforzando and load
   `plugnscript/halionbridge_sfz_amp_velocity_calibration_generator.cxx`.
3. Record or meter the four steady-state note levels for velocities
   32, 64, 100, and 127.
4. Replace sforzando with each HALion preset, keeping the same Plug'n Script
   instance and meter.
5. Compare the four levels. If `002` matches sforzando, the likely converter
   mapping is `amp_veltrack * 2` to `Amp Env.VelocityToLevel` plus a sampled
   oscillator gain compensation. If `004` matches better, the velocity depth is
   still likely `* 2`, but the compensation should be applied at `Amp.Level`.

Expected sforzando reference values from probe `027`:

| Velocity | Sforzando |
| --- | ---: |
| 32 | -32.0 dB |
| 64 | -20.0 dB |
| 100 | -12.2 dB |
| 127 | -8.1 dB |
