# 059 Filter Family Rough Mapping

This probe follows `058-filter-lpf2-implementation-regression`.

The goal is not tight filter parity. It is a fast rough-mapping pass across the
SFZ filter families recognized by sfizz/sforzando-style parsing. Each source
case has one same-number HALion baseline preset. Load a same-number pair,
tweak HALion by ear if needed, and report the closest HALion settings.

## Files

- `source/*.sfz` are same-number sforzando references.
- `source/samples/saw_A3_single_cycle.wav` is copied from probe `058`.
- `plugnscript/halionbridge_sfz_filter_long_generator.cxx` is copied from
  probe `058` and emits one long note at velocity 127.
- `halionbridge-build/filter_family_rough_mapping.lua` hand-authors one
  baseline HALion preset per SFZ filter type.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\059-filter-family-rough-mapping\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_filter_long_generator.cxx`.
3. Load one same-number source/preset pair, for example
   `source/006_hpf_2p.sfz` and `halionbridge-build/006_hpf_2p.vstpreset`.
4. If the HALion baseline is not close, tweak the HALion filter settings by
   ear and report the closest settings as:

```text
006 hpf_2p -> Type ?, Mode ?, ShapeA ?, Cutoff ?, Resonance ?
```

For `lsh`, `hsh`, `peq`, and `pink`, also report if there is no useful HALion
sample-zone filter equivalent.

## Cases

All SFZ cases use the same sample, loop, note, and `cutoff=1000`. Most cases
also use `resonance=6`; shelf/peq cases additionally use `fil_gain=6`.

The HALion baseline assumes `Filter.Type=1` is Classic. Shape numbers are rough
guesses from the documented Classic shape order; this probe is allowed to
correct them.

| Case | SFZ `fil_type` | Baseline HALion candidate |
| --- | --- | --- |
| `000` | none | no filter |
| `001` | `lpf_1p` | Classic LP6, cutoff `1000`, resonance `32` |
| `002` | `lpf_2p` | Classic LP12, cutoff `1000`, resonance `48` |
| `003` | `lpf_4p` | Classic LP24, cutoff `1000`, resonance `48` |
| `004` | `lpf_6p` | Classic LP24, cutoff `1000`, resonance `48` |
| `005` | `hpf_1p` | Classic HP6, cutoff `1000`, resonance `48` |
| `006` | `hpf_2p` | Classic HP12, cutoff `1000`, resonance `48` |
| `007` | `hpf_4p` | Classic HP24, cutoff `1000`, resonance `48` |
| `008` | `hpf_6p` | Classic HP24, cutoff `1000`, resonance `48` |
| `009` | `bpf_1p` | Classic BP12, cutoff `1000`, resonance `48` |
| `010` | `bpf_2p` | Classic BP12, cutoff `1000`, resonance `48` |
| `011` | `brf_1p` | Classic BR12, cutoff `1000`, resonance `48` |
| `012` | `brf_2p` | Classic BR12, cutoff `1000`, resonance `48` |
| `013` | `lpf_2p_sv` | Classic LP12, cutoff `1000`, resonance `48` |
| `014` | `hpf_2p_sv` | Classic HP12, cutoff `1000`, resonance `48` |
| `015` | `bpf_2p_sv` | Classic BP12, cutoff `1000`, resonance `48` |
| `016` | `brf_2p_sv` | Classic BR12, cutoff `1000`, resonance `48` |
| `017` | `lsh` | weak baseline: Classic LP6 |
| `018` | `hsh` | weak baseline: Classic HP6 |
| `019` | `peq` | weak baseline: Classic AP |
| `020` | `pink` | weak baseline: Classic LP6 |

## Result

Generated and built on 2026-06-14. HALion build completed successfully: one
script processed, 21 `.vstpreset` files saved.

Manual feedback on 2026-06-14 selected these rough mappings:

| Case | Decision |
| --- | --- |
| `001_lpf_1p` | Classic, Single Filter, LP6, cutoff `700`, resonance `0` |
| `002_lpf_2p` | Classic, Single Filter, LP12, cutoff `1000`, resonance `48` |
| `003_lpf_4p` | Classic, Single Filter, LP24, cutoff `1000`, resonance `66` |
| `004_lpf_6p` | Classic, Dual Filter Serial, ShapeA LP24, ShapeB LP12, cutoff `1000`, resonance `59` |
| `005_hpf_1p` | Classic, Single Filter, HP6, cutoff `1000`, resonance `0` |
| `006_hpf_2p` | Classic, Single Filter, HP12, cutoff `1000`, resonance `47` |
| `007_hpf_4p` | Classic, Dual Filter Serial, ShapeA HP18, ShapeB HP6, cutoff `1000`, resonance `50` |
| `008_hpf_6p` | Classic, Dual Filter Serial, ShapeA HP12, ShapeB HP18, cutoff `1000`, resonance `55` |
| `009_bpf_1p` | Classic, Single Filter, BP12, cutoff `1000`, resonance `50` |
| `010_bpf_2p` | Classic, Single Filter, BP12, cutoff `1000`, resonance `50` |
| `011_brf_1p` | Classic, Single Filter, BR12, cutoff `1000`, resonance `35` |
| `012_brf_2p` | Classic, Single Filter, BR24, cutoff `1000`, resonance `50` |
| `013_lpf_2p_sv` | sforzando reports unknown opcode; keep unsupported |
| `014_hpf_2p_sv` | sforzando reports unknown opcode; keep unsupported |
| `015_bpf_2p_sv` | sforzando reports unknown opcode; keep unsupported |
| `016_brf_2p_sv` | sforzando reports unknown opcode; keep unsupported |
| `017_lsh` | Classic, Single Filter, BP12+BR12, cutoff `1000`, resonance `0` |
| `018_hsh` | Classic, Single Filter, HP6, cutoff `1000`, resonance `0` |
| `019_peq` | Classic, Single Filter, AP, cutoff `1000`, resonance `0` |
| `020_pink` | closest found was BP12+BR12, resonance `15`, but still off; keep unsupported |

Decision: implement these as rough static filter-family mappings. Keep `_sv`
types and `pink` as unsupported diagnostics for the sforzando-centered path.
