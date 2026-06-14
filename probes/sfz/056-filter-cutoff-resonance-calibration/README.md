# 056 Filter Cutoff Resonance Calibration

This probe follows `055-filter-static-long-diagnostic`.

Probe `055` selected HALion Classic LP12 as the closest topology for SFZ
`fil_type=lpf_2p`. Manual tuning against the `cutoff=1000` sforzando reference
matched best around HALion cutoff `1200` and resonance `20`. This probe narrows
the comparison to Classic LP12 and checks whether that observation holds around
nearby cutoff and resonance values.

## Files

- `source/*.sfz` are same-number sforzando references.
- `source/samples/saw_A3_single_cycle.wav` is copied from probe `055`.
- `plugnscript/halionbridge_sfz_filter_long_generator.cxx` emits note 57 at
  velocity 127 and holds it for eight seconds.
- `halionbridge-build/*.lua` are hand-authored HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

## Cases

All HALion filtered cases use Filter Type Classic, LP12.

| Case | Source behavior | HALion candidate |
| --- | --- | --- |
| `000` | no filter | no filter |
| `001` | `lpf_2p cutoff=1000` | cutoff `1000`, resonance `0` |
| `002` | same | cutoff `1100`, resonance `0` |
| `003` | same | cutoff `1200`, resonance `0` |
| `004` | same | cutoff `1200`, resonance `10` |
| `005` | same | cutoff `1200`, resonance `15` |
| `006` | same | cutoff `1200`, resonance `20` |
| `007` | same | cutoff `1200`, resonance `25` |
| `008` | same | cutoff `1300`, resonance `20` |
| `009` | same | cutoff `1400`, resonance `20` |
| `010` | `lpf_2p cutoff=1000 resonance=3` | cutoff `1200`, resonance `20` |
| `011` | same | cutoff `1200`, resonance `25` |
| `012` | `lpf_2p cutoff=1000 resonance=6` | cutoff `1200`, resonance `25` |
| `013` | same | cutoff `1200`, resonance `30` |
| `014` | `lpf_2p cutoff=1000 resonance=12` | cutoff `1200`, resonance `35` |
| `015` | same | cutoff `1200`, resonance `45` |
| `016` | `lpf_2p cutoff=1000 resonance=24` | cutoff `1200`, resonance `60` |
| `017` | same | cutoff `1200`, resonance `75` |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\056-filter-cutoff-resonance-calibration\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_filter_long_generator.cxx`.
3. Compare same-number pairs from `000` through `017`.

Use the long steady portion of the note. A spectrum analyzer should be useful
for the cutoff and resonance peak; phase cancellation may be too strict until
both values are close.

First confirm which of `001` through `009` best matches the implicit-resonance
`lpf_2p cutoff=1000` reference. Then compare `010` through `017` to determine
how explicit SFZ `resonance=` should map.

## Result

HALion build completed successfully on 2026-06-14 and saved all eighteen
`.vstpreset` files.

Manual validation on 2026-06-14:

- For the implicit SFZ resonance case, `lpf_2p cutoff=1000` matched best with
  HALion cutoff `1000` and resonance around `31..32`.
- `001` matched well when manually adjusted to HALion resonance `31`.
- `002` matched well when manually adjusted to HALion cutoff `1000` and
  resonance `32`; this same cutoff/resonance target applies to all
  `lpf_2p cutoff=1000` implicit-resonance cases.
- For SFZ `resonance=3`, cases `010` and `011` matched at HALion cutoff
  `1000`, resonance `39`.
- For SFZ `resonance=6`, cases `012` and `013` matched at HALion cutoff
  `1000`, resonance `48`.
- For SFZ `resonance=12`, cases `014` and `015` matched at HALion cutoff
  `1000`, resonance `63`.
- For SFZ `resonance=24`, cases `016` and `017` matched at HALion cutoff
  `1000`, resonance `78`.

Decision: the previous `055` cutoff scale hypothesis is rejected for this
sample and filter family. Keep `lpf_2p` on HALion Classic LP12 with cutoff
unchanged. The main mapping work is resonance calibration: SFZ `resonance=0`
needs a HALion resonance floor around `32`, and explicit SFZ resonance needs a
nonlinear increase above that floor.
