# 055 Filter Static Long Diagnostic

This probe follows the inconclusive `054-filter-basics-diagnostic`.

Probe `054` showed that `Filter.Cutoff` alone leaves HALion's filter type off,
and manually enabling Classic did not immediately match. This probe removes
velocity tracking and short-note ambiguity. It uses one long sustained note to
match the static low-pass filter topology, slope, and cutoff scaling first.

Local HALion documentation says `Filter.Type=0` is **Off** and **Classic**
offers low-pass shapes LP24, LP18, LP12, and LP6. The empirical HALion parameter
dump lists `Filter.Type`, `Filter.Mode`, `Filter.ShapeA`, and `Filter.Cutoff`.

## Files

- `source/*.sfz` are same-number sforzando references. They intentionally
  duplicate the same `fil_type=lpf_2p cutoff=1000` source behavior.
- `source/samples/saw_A3_single_cycle.wav` is copied from probe `054`.
- `plugnscript/halionbridge_sfz_filter_long_generator.cxx` emits note 57 at
  velocity 127 and holds it for eight seconds.
- `halionbridge-build/*.lua` are hand-authored HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

## Cases

All filtered cases use the same source behavior: `fil_type=lpf_2p` and
`cutoff=1000`. Resonance and velocity tracking are intentionally omitted.

| Case | Source behavior | HALion candidate |
| --- | --- | --- |
| `000` | no filter | no filter parameters |
| `001` | `lpf_2p cutoff=1000` | Classic, LP24, cutoff `1000` |
| `002` | same | Classic, LP18, cutoff `1000` |
| `003` | same | Classic, LP12, cutoff `1000` |
| `004` | same | Classic, LP6, cutoff `1000` |
| `005` | same | Eco, cutoff `1000` |
| `006` | same | Classic, LP12, cutoff `500` |
| `007` | same | Classic, LP12, cutoff `750` |
| `008` | same | Classic, LP12, cutoff `1500` |
| `009` | same | Classic, LP12, cutoff `2000` |
| `010` | same | Classic, LP24, cutoff `1500` |
| `011` | same | Classic, LP24, cutoff `2000` |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\055-filter-static-long-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_filter_long_generator.cxx`.
3. Compare same-number pairs from `000` through `011`.

Use the long steady portion of the note for comparison. A spectrum analyzer or
null test should be more useful here than short repeated notes. Focus first on
which LP shape is closest at cutoff `1000`; then use the cutoff sweep to decide
whether HALion cutoff needs a scale correction.

## Result

HALion build completed successfully on 2026-06-14 and saved all twelve
`.vstpreset` files.

Manual validation on 2026-06-14:

- The fixed candidates did not yet capture resonance behavior well enough.
- Manual HALion adjustment against the `lpf_2p cutoff=1000` sforzando
  reference matched best with Filter Type Classic, LP12, cutoff `1200`, and
  resonance `20`.

Decision: use Classic LP12 as the first `lpf_2p` topology candidate. Before
implementing, generate a focused follow-up around cutoff scaling and resonance
calibration.
