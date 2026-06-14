# 050 Amplitude Decay Shape Diagnostic

This probe follows `049-amplitude-envelope-post-release-regression`.

Probe `049` confirmed that the release fix works, but manual validation showed
that decay is still too slow in the generated HALion presets. The SFZv2 spec
and sfizz implementation both indicate that decay uses a faster-than-linear
exponential shape by default. This probe isolates decay from attack/hold/release
where possible and checks candidate HALion decay durations and curves.

## Files

- `source/*.sfz` are the sforzando references. Several files intentionally
  duplicate source settings so every HALion candidate has a same-number source.
- `source/samples/amp_env_loop.wav` is copied from probe `043`.
- `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx` is copied from
  probe `043`.
- `halionbridge-build/*.lua` are hand-authored HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

## Cases

Start with `000` through `003`, because they target the `049/003` issue
directly.

| Case | Source behavior | HALion candidate |
| --- | --- | --- |
| `000` | `decay=1`, `sustain=40`, default `decay_zero=1` | current baseline: `0.600s`, linear |
| `001` | same | sfizz exponential estimate: `0.102s`, curve `-1.0` |
| `002` | same | user-near candidate: `0.200s`, curve `-1.0` |
| `003` | same | midpoint candidate: `0.300s`, curve `-1.0` |
| `004` | `decay=1`, `sustain=40`, explicit `decay_zero=0` | current baseline: `1.000s`, linear |
| `005` | same | release-scale candidate: `0.604s`, curve `-1.0` |
| `006` | same | half-time candidate: `0.500s`, curve `-1.0` |
| `007` | same | short candidate: `0.200s`, curve `-1.0` |
| `008` | `decay=0.8`, `sustain=50`, default `decay_zero=1` | current baseline: `0.400s`, linear |
| `009` | same | user-tuned candidate: `0.200s`, curve `-1.0` |
| `010` | same | sfizz exponential estimate: `0.0616s`, curve `-1.0` |
| `011` | same | curve-only candidate: `0.400s`, curve `-1.0` |
| `012` | `decay=1`, `sustain=0`, `release=0.7` | current bump baseline: linear decay plus fixed release early level |
| `013` | same | `1.000s`, curve `-1.0`, no release bump |
| `014` | same | `0.604s`, curve `-1.0`, no release bump |
| `015` | same | two-segment decay to zero, no release bump |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\050-amplitude-decay-shape-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx`.
3. Compare same-number pairs from `000` through `015`.

Use the phase-inverted/null-test setup where practical. Focus on the held-note
decay before note-off for `000` through `011`. For `012` through `015`, also
check whether HALion produces the release bump that was heard in `049/008`.

## Result

HALion build completed successfully on 2026-06-14: sixteen hand-authored
scripts processed, sixteen `.vstpreset` files saved.

Manual validation on 2026-06-14:

- Same-number comparisons are the intended measurement method for this probe.
- `000` still differs in decay.
- `001`, `002`, and `003` all match reasonably well for default
  `decay_zero=1`; `003` is the best of that group.
- `004` does not match well.
- `005` matches reasonably well for explicit `decay_zero=0`; `006` and `007`
  are worse.
- `008` does not match well, while `009` matches well for the
  `decay=0.8`/`sustain=50` default-`decay_zero` case.
- `010` and `011` are worse than `009`.
- `012` does not match well and confirms the release-bump baseline is wrong.
- `013` is not great, `014` is good, and `015` is the best sustain-zero
  candidate.

Decision: update the converter to use Lua curve `-1.0` for static decay. For
default `ampeg_decay_zero=1` with nonzero sustain, use
`ampeg_decay * (1 - sustain) * 0.5`. For explicit `ampeg_decay_zero=0`, use
`ampeg_decay * 0.604`. For sustain level `0`, use the curved two-segment decay
shape from `015` and avoid fixed positive release points that bump up after the
decay has already reached zero.
