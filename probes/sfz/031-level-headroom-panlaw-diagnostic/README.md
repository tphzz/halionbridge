# 031 Level, Headroom, and Pan Law Diagnostic

This probe isolates the remaining static level offset after the confirmed
HALion squared velocity curve mapping.

Probe `030` confirmed that HALion layer `VelocityToLevelCurve=1` is the
**Squared** Main-section velocity curve. With that curve and a static `+7.8 dB`
offset, HALion almost nulls against sforzando. This probe tests where that
offset belongs and whether HALion amplifier headroom or pan law explains part
of it.

## Files

- `source/000_level_headroom_panlaw_reference.sfz` is the sforzando reference.
- `source/samples/sine_A3_single_cycle.wav` is copied into the probe.
- `plugnscript/halionbridge_sfz_level_headroom_panlaw_generator.cxx` emits note
  57 at velocities 32, 64, 100, and 127.
- `halionbridge-build/*.vstpreset` are ready-to-load HALion presets after the
  build command succeeds.

## Fixed HALion Settings

Every preset sets the confirmed velocity shape:

```lua
Layer.InheritVelocitySettings = false
Layer.VelocityToLevelCurve = 1
Zone["Amp Env.VelocityToLevel"] = 100
Zone["Amp Env.VelocityToLevelCurve"] = 0
```

## Variants

| Preset | Change | Purpose |
| --- | --- | --- |
| `000_squared_curve_baseline.vstpreset` | no additional change | Baseline after confirmed squared curve. |
| `001_sampleosc_level_plus_7p8.vstpreset` | `SampleOsc.Level=+7.8 dB` | Tests oscillator/sample gain compensation. |
| `002_amp_level_plus_7p8.vstpreset` | `Amp.Level=+7.8 dB` | Tests zone amplifier gain compensation. |
| `003_layer_level_plus_7p8.vstpreset` | layer `Level=+7.8 dB` | Tests layer gain compensation. |
| `004_amp_headroom_0.vstpreset` | `Amp.Headroom=0` | Headroom enum probe. HALion dump default is `0`; manual default is 12 dB. |
| `005_amp_headroom_1.vstpreset` | `Amp.Headroom=1` | Headroom enum probe. |
| `006_amp_headroom_2.vstpreset` | `Amp.Headroom=2` | Headroom enum probe. |
| `007_amp_panlaw_0.vstpreset` | `Amp.PanLaw=0` | Expected 0 dB pan mode from menu order. |
| `008_amp_panlaw_1.vstpreset` | `Amp.PanLaw=1` | Expected -3 dB pan mode and dump default. |
| `009_amp_panlaw_2.vstpreset` | `Amp.PanLaw=2` | Expected -6 dB pan mode. |
| `010_amp_panlaw_3.vstpreset` | `Amp.PanLaw=3` | Expected Off. |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\031-level-headroom-panlaw-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Load `source/000_level_headroom_panlaw_reference.sfz` in sforzando.
2. Put Blue Cat Plug'n Script immediately before sforzando and load
   `plugnscript/halionbridge_sfz_level_headroom_panlaw_generator.cxx`.
3. Measure the four steady-state note levels for velocities 32, 64, 100, and 127.
4. Replace sforzando with each HALion preset, keeping the same Plug'n Script
   instance and meter.
5. First identify whether a headroom or pan-law setting changes the constant
   offset. Then compare which `+7.8 dB` placement nulls best.

Expected sforzando reference values from probe `027`:

| Velocity | Sforzando |
| --- | ---: |
| 32 | -32.0 dB |
| 64 | -20.0 dB |
| 100 | -12.2 dB |
| 127 | -8.1 dB |
