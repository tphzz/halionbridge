# 052 Amplitude Start Diagnostic

This probe follows `051-amplitude-decay-implementation-regression`.

Probe `051` confirmed the calibrated decay mapping, but `006_start_50` still
did not match. That case mixed `ampeg_start`, attack, decay, sustain, and
release, so this probe isolates `ampeg_start` with attack-only envelopes.

## Files

- `source/*.sfz` are same-number sforzando references. Several files
  intentionally duplicate the same SFZ settings so different HALion candidates
  can be compared without changing probe directories.
- `source/samples/amp_env_loop.wav` is copied from probe `043`.
- `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx` is copied from the
  previous amplitude-envelope probes.
- `halionbridge-build/*.lua` are hand-authored HALion candidates.
- `halionbridge-build/*.vstpreset` are built through HALion and ready to load.

## Cases

All cases use a continuous loop, note 57, full velocity, `ampeg_attack=1.0`,
`ampeg_decay=0`, `ampeg_sustain=100`, and `ampeg_release=0`. The only intended
audible difference is the initial envelope level before the attack reaches
full level.

| Case | Source behavior | HALion candidate |
| --- | --- | --- |
| `000` | `ampeg_start=0` | control: start level `0` |
| `001` | `ampeg_start=50` | start level `0` |
| `002` | same | start level `0.25` |
| `003` | same | start level `0.5` |
| `004` | same | start level `0.70710678` |
| `005` | `ampeg_start=100` | start level `0` |
| `006` | same | start level `1` |
| `007` | `ampeg_start=25` | start level `0` |
| `008` | same | start level `0.25` |
| `009` | `ampeg_start=75` | start level `0.75` |

## Build

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe probes\sfz\052-amplitude-start-diagnostic\halionbridge-build --timeout-seconds 120
```

## Measurement Loop

1. Put Blue Cat Plug'n Script immediately before sforzando or HALion.
2. Load `plugnscript/halionbridge_sfz_amp_envelope_generator.cxx`.
3. Compare same-number pairs from `000` through `009`.

Use the phase-inverted/null-test setup where practical. Focus on the first
second after note-on, before the note reaches steady full level.

High-signal interpretation:

- If `001`, `005`, and `007` match best, sforzando is effectively behaving as
  if these `ampeg_start` values start at zero in this probe setup.
- If `003`, `006`, and `008` match best, the current percent-to-level mapping
  is basically correct and the earlier mismatch was caused by interaction with
  decay/release.
- If `002` or `004` wins for `ampeg_start=50`, the opcode likely needs a
  non-linear level conversion.

## Result

HALion build completed successfully on 2026-06-14: ten hand-authored scripts
processed, ten `.vstpreset` files saved.

Manual validation pending.
