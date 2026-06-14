# 033 Pan Basics Diagnostic

This probe isolates SFZ `pan` after the verified gain and velocity mapping from
probes `030` through `032`.

The converter does not map `pan` yet. These HALion presets are hand-authored to
compare likely HALion targets against sforzando before adding converter support.

## Files

- `source/*.sfz` are the sforzando references.
- `source/samples/sine_A3_single_cycle.wav` is copied into the probe.
- `plugnscript/halionbridge_sfz_pan_basics_generator.cxx` emits note 57 at
  velocity 127.
- `halionbridge-build/*.vstpreset` are ready-to-load HALion presets after the
  build command succeeds.

## Fixed HALion Settings

Every preset sets the verified gain/velocity baseline:

```lua
Layer.InheritVelocitySettings = false
Layer.VelocityToLevelCurve = 1
Zone["SampleOsc.Level"] = 7.8
Zone["Amp Env.VelocityToLevel"] = 100
Zone["Amp Env.VelocityToLevelCurve"] = 0
```

## Source References

| SFZ | Purpose |
| --- | --- |
| `000_pan_center.sfz` | Default center pan reference. |
| `001_pan_left_100.sfz` | Full left SFZ pan. |
| `002_pan_left_50.sfz` | Partial left SFZ pan. |
| `003_pan_right_50.sfz` | Partial right SFZ pan. |
| `004_pan_right_100.sfz` | Full right SFZ pan. |

## HALion Candidates

| Preset | Change | Compare against |
| --- | --- | --- |
| `000_amp_pan_center.vstpreset` | `Amp.Pan=0` | `000_pan_center.sfz` |
| `001_amp_pan_left_100.vstpreset` | `Amp.Pan=-100` | `001_pan_left_100.sfz` |
| `002_amp_pan_left_50.vstpreset` | `Amp.Pan=-50` | `002_pan_left_50.sfz` |
| `003_amp_pan_right_50.vstpreset` | `Amp.Pan=50` | `003_pan_right_50.sfz` |
| `004_amp_pan_right_100.vstpreset` | `Amp.Pan=100` | `004_pan_right_100.sfz` |
| `005_sampleosc_pan_left_100.vstpreset` | `SampleOsc.Pan=-100` | `001_pan_left_100.sfz` |
| `006_sampleosc_pan_left_50.vstpreset` | `SampleOsc.Pan=-50` | `002_pan_left_50.sfz` |
| `007_sampleosc_pan_right_50.vstpreset` | `SampleOsc.Pan=50` | `003_pan_right_50.sfz` |
| `008_sampleosc_pan_right_100.vstpreset` | `SampleOsc.Pan=100` | `004_pan_right_100.sfz` |
| `009_layer_pan_left_100.vstpreset` | layer `Pan=-100` | fallback check |
| `010_layer_pan_right_100.vstpreset` | layer `Pan=100` | fallback check |
| `011_zone_pan_left_100.vstpreset` | zone `Pan=-100` | fallback check |
| `012_zone_pan_right_100.vstpreset` | zone `Pan=100` | fallback check |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\033-pan-basics-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Load one `source/*.sfz` file in sforzando.
2. Put Blue Cat Plug'n Script immediately before sforzando and load
   `plugnscript/halionbridge_sfz_pan_basics_generator.cxx`.
3. Measure left/right channel levels, stereo balance, and phase-cancel behavior
   for the sustained part of the note.
4. Replace sforzando with the matching HALion `.vstpreset`, keeping the same
   Plug'n Script instance and meter.
5. First compare `Amp.Pan` candidates. If those do not match sforzando, compare
   `SampleOsc.Pan`, then the layer/zone fallback presets.

Record which HALion target best matches center, partial pan, and hard pan. Do
not implement `pan` until one target is clearly better across the tested values.
