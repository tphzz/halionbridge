# 057 Filter LPF2 Calibrated Validation

This probe follows `056-filter-cutoff-resonance-calibration`.

Probe `056` rejected cutoff scaling for `fil_type=lpf_2p` and found that
HALion Classic LP12 needs a resonance calibration instead. This probe validates
the current direct mapping candidate across multiple cutoff and resonance
values.

## Mapping Under Test

- SFZ `fil_type=lpf_2p` -> HALion Filter Type Classic, LP12.
- SFZ `cutoff` -> HALion `Filter.Cutoff` unchanged.
- SFZ `resonance` -> calibrated HALion `Filter.Resonance`.

Current resonance targets from probe `056`:

```text
sfz resonance 0  -> HALion resonance 32
sfz resonance 3  -> HALion resonance 39
sfz resonance 6  -> HALion resonance 48
sfz resonance 12 -> HALion resonance 63
sfz resonance 24 -> HALion resonance 78
sfz resonance 40 -> HALion resonance 90
```

The `40 -> 90` case is an extrapolation near the SFZ resonance maximum. It is
included to find out whether the curve saturates too early or too late.

## Files

- `source/*.sfz` are same-number sforzando references.
- `source/samples/saw_A3_single_cycle.wav` is copied from probe `056`.
- `plugnscript/halionbridge_sfz_filter_long_generator.cxx` emits note 57 at
  velocity 127 and holds it for eight seconds.
- `halionbridge-build/*.lua` are hand-authored HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

## Cases

All HALion filtered cases use Filter Type Classic, LP12.

| Case | Source behavior | HALion candidate |
| --- | --- | --- |
| `000` | no filter | no filter |
| `001` | `lpf_2p cutoff=500` | cutoff `500`, resonance `32` |
| `002` | `lpf_2p cutoff=1000` | cutoff `1000`, resonance `32` |
| `003` | `lpf_2p cutoff=2000` | cutoff `2000`, resonance `32` |
| `004` | `lpf_2p cutoff=3000` | cutoff `3000`, resonance `32` |
| `005` | `lpf_2p cutoff=1000 resonance=3` | cutoff `1000`, resonance `39` |
| `006` | `lpf_2p cutoff=1000 resonance=6` | cutoff `1000`, resonance `48` |
| `007` | `lpf_2p cutoff=1000 resonance=12` | cutoff `1000`, resonance `63` |
| `008` | `lpf_2p cutoff=1000 resonance=24` | cutoff `1000`, resonance `78` |
| `009` | `lpf_2p cutoff=1000 resonance=40` | cutoff `1000`, resonance `90` |
| `010` | `lpf_2p cutoff=500 resonance=6` | cutoff `500`, resonance `48` |
| `011` | `lpf_2p cutoff=2000 resonance=6` | cutoff `2000`, resonance `48` |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\057-filter-lpf2-calibrated-validation\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_filter_long_generator.cxx`.
3. Compare same-number pairs from `000` through `011`.

Focus on whether the same cutoff value now lands correctly and whether the
resonance targets remain stable at lower and higher cutoff.

## Result

HALion build completed successfully on 2026-06-14 and saved all twelve
`.vstpreset` files.

Manual validation on 2026-06-14:

- The same-number comparisons are usable but only approximate; none of the
  filter pairs match particularly well.
- HALion resonance `90` in case `009` sounds extremely rough. Resonance values
  above roughly `87` should be avoided for this mapping.
- Manual static filter probing is no longer a productive path for tighter
  parity. Proper filter matching would require rendered grids across cutoff,
  resonance, note/key, waveform, velocity/modulation state, and an optimizer
  over residuals.

Decision: implement `lpf_2p` as a conservative best-effort mapping, not a
parity-grade mapping. Use HALion Classic LP12, pass cutoff through unchanged,
use the measured resonance table from `056`, and cap HALion resonance at `86`.
