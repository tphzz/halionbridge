# Development Documentation

## Requirements
- **C++ Standard:** C++23 is strictly required.
- **Toolchain (Windows):** Visual Studio 2026 is required for Windows builds to ensure the highest code quality.
- **Toolchain (macOS):** Xcode and Clang supporting C++23.
- **CMake:** CMake 3.22+ is required.
- **Ninja (Windows):** Windows builds use CMake's Ninja generator. Visual Studio's CMake tools include Ninja, or it can be installed separately.

## Building the Project

1. Clone the repository and initialize submodules:
   ```bash
   git clone <repo_url>
   cd halionbridge
   git submodule update --init --recursive
   ```

On Windows, run all CMake configure/build/test commands from a Visual Studio 18 Developer PowerShell or Developer Command Prompt. A plain PowerShell session may find `cl.exe` but still fail because MSVC and Windows SDK include paths are not set, producing errors such as missing `windows.h`, `algorithm`, or `stdint.h`.

2. Configure and build Debug. Debug builds automatically enable JUCE assertion logging, JUCE VST3 debug logging, and crash dumps:

```powershell
cmake --preset debug
cmake --build --preset debug
```

3. Configure and build Release with a separate build directory:

```powershell
cmake --preset release
cmake --build --preset release
```

Diagnostic builds are available for plugin-host startup crashes and VST3 scan/instantiate failures:

```powershell
cmake --preset diagnostics-debug
cmake --build --preset diagnostics-debug

cmake --preset diagnostics-relwithdebinfo
cmake --build --preset diagnostics-relwithdebinfo
```

Use `diagnostics-debug` when stepping through host code in Visual Studio. Use `diagnostics-relwithdebinfo` when the failure only reproduces with optimized code but still needs symbols and JUCE/VST3 diagnostics.

To build and run tests:

```powershell
cmake --build --preset debug --target halionbridge_tests
build\halionbridge_tests_artefacts\Debug\halionbridge_tests.exe
```

Release build outputs are written to:

```text
build-release\halionbridge_artefacts\Release\halionbridge.exe
build-release\halionbridge_tests_artefacts\Release\halionbridge_tests.exe
build-release\halionbridge_library_artefacts\Release\halionbridge.dll
build-release\halionbridge_library_artefacts\Release\halionbridge.lib
build-release\halionbridge_library_artefacts\Release\halionbridge_static.lib
```

Diagnostic build outputs are written to:

```text
build-diagnostics\halionbridge_artefacts\Debug\halionbridge.exe
build-diagnostics\halionbridge_tests_artefacts\Debug\halionbridge_tests.exe
build-diagnostics\halionbridge_library_artefacts\Debug\halionbridge.dll
build-diagnostics\halionbridge_library_artefacts\Debug\halionbridge.lib
build-diagnostics\halionbridge_library_artefacts\Debug\halionbridge_static.lib
build-relwithdebinfo-diagnostics\halionbridge_artefacts\RelWithDebInfo\halionbridge.exe
build-relwithdebinfo-diagnostics\halionbridge_tests_artefacts\RelWithDebInfo\halionbridge_tests.exe
build-relwithdebinfo-diagnostics\halionbridge_library_artefacts\RelWithDebInfo\halionbridge.dll
build-relwithdebinfo-diagnostics\halionbridge_library_artefacts\RelWithDebInfo\halionbridge.lib
build-relwithdebinfo-diagnostics\halionbridge_library_artefacts\RelWithDebInfo\halionbridge_static.lib
```

On macOS, the same library artifact directory contains `libhalionbridge.dylib` and `libhalionbridge_static.a` instead of the Windows `.dll`/`.lib` files.

## CMake Targets

- `halionbridge_shared`: The C++ shared library target. It builds `halionbridge.dll` on Windows and `libhalionbridge.dylib` on macOS. This is the intended integration artifact for library users.
- `halionbridge_static`: The C++ static library target. It uses the same implementation sources as `halionbridge_shared` and is used by the CLI and tests. Treat it as an internal build product, not as a stable cross-toolchain ABI boundary.
- `halionbridge`: The command-line frontend. It links against `halionbridge_static` so the CLI remains a self-contained executable and does not require the shared library beside it.
- `halionbridge_tests`: The unit-test executable. It also links against `halionbridge_static`.
- `halionbridge_public_header_smoke`: A compile-only object target that includes the public headers without any JUCE include paths. If a public header starts depending on JUCE again, this target should fail to compile.
- `halionbridge_converter_common_static`: Shared first-party converter infrastructure for converter registration and deterministic halionbridge build-directory emission.
- `halionbridge_converter_sfz_static`: Built-in SFZ converter. It links sfizz privately and generates normal Lua build directories consumed by the existing builder workflow.

The public C++ headers live under `include/halionbridge` and must remain JUCE-free. Public API types use C++23/STL types such as `std::filesystem::path`, `std::string`, `std::vector`, `std::span`, and `std::optional`; JUCE is an implementation dependency only. The generated export header is written by CMake under the build directory and is included through `halionbridge/halionbridge_export.h`. Public headers use `HALIONBRIDGE_EXPORT` so the same declarations work for the shared library and for static consumers that define `HALIONBRIDGE_STATIC_DEFINE`.

Converter public headers live under `converters/*/include` and must remain free of JUCE, spdlog, and parser implementation types such as sfizz classes. Converter modules describe the input format only; `sfz` means "SFZ to the standard halionbridge Lua build-directory shape" because the Lua destination is implied by halionbridge.

The main public library entry point is `halionbridge::Bridge`. The CLI owns process signal handling and calls the public `requestStop()` / `resetStopRequest()` stop API; library processing loops poll `isStopRequested()` and leave through normal C++ control flow so temporary HALion script files, working-directory changes, and environment changes are cleaned up by RAII destructors.

## Runtime Linkage

The release CLI itself is packaged as a single binary. It links against the internal `halionbridge_static` target, so the bridge implementation, embedded Lua/bootstrap assets, JUCE module code, and spdlog header-only logging are linked into `halionbridge.exe` on Windows and `halionbridge` on macOS. The release archives include tracked examples for users to run, but intentionally do not include `halionbridge.dll`, `libhalionbridge.dylib`, import libraries, static libraries, test executables, generated example output presets, or build directories.

Windows builds use CMake's `MSVC_RUNTIME_LIBRARY` setting with the static MSVC runtime (`/MT` for Release, `/MTd` for Debug). Release binaries should not require the Visual C++ Redistributable on target machines. The GitHub Actions release workflow runs `dumpbin /DEPENDENTS` and fails if the staged Windows CLI depends on dynamic MSVC runtime DLLs such as `VCRUNTIME`, `MSVCP`, `CONCRT`, `UCRTBASE`, or `api-ms-win-crt`.

macOS builds keep Apple system libraries and frameworks dynamically linked, which is the normal distribution model for command-line tools. Project code, JUCE, and spdlog must not appear as separate dylib dependencies of the packaged CLI. The GitHub Actions release workflow runs `lipo -info` and `otool -L`, verifies the universal binary, and fails if the macOS CLI depends on any non-system dynamic library.

## Build Versioning

CMake computes the build version from Git. Developers do not pass version variables manually. Local builds and GitHub Actions builds use the same `cmake/halionbridge_version.cmake` logic.

Versions are intentionally source-identification strings, not calendar build IDs. A named branch checkout uses the active branch name even when `HEAD` also happens to point at a tag. Detached checkouts use an exact tag when one exists, otherwise `detached`. The short Git hash is always included. Modified worktrees append `-mod`; this includes tracked modifications and untracked, non-ignored files.

Examples:

```text
halionbridge-main-a1b2c3d
halionbridge-feature-lua-api-a1b2c3d
halionbridge-v0.5.0-a1b2c3d
halionbridge-feature-lua-api-a1b2c3d-mod
```

CMake writes the package basename to:

```text
build\halionbridge_package_basename.txt
build-release\halionbridge_package_basename.txt
```

The CLI exposes the same metadata with:

```powershell
build-release\halionbridge_artefacts\Release\halionbridge.exe --version
```

`BuildInfo.cpp` is generated during configure so the build tree has a source file, and regenerated during every normal `cmake --build` via `halionbridge_generate_build_info`. The generator rewrites the file only when the Git metadata changes, so switching branches or changing files only requires the normal build command to refresh `--version`.

## GitHub Actions

The repository includes a manual-only GitHub Actions workflow at `.github/workflows/build.yml`. It is intentionally triggered with `workflow_dispatch` from the Actions tab and does not run automatically on pushes, pull requests, or tags.

The workflow can build Debug, Release, or both. When starting the workflow from the Actions tab, select the `Build Debug` and `Build Release` checkboxes for the configurations you want. Debug and Release run as separate jobs on GitHub-hosted Windows and macOS runners using the existing CMake presets. Windows uses GitHub's `windows-2025-vs2026` image so the hosted build runs in a Visual Studio 2026 environment; macOS uses `macos-latest`, GitHub's current stable macOS arm64 label. Each selected job initializes submodules with full Git history and tags, prepares the MSVC environment on Windows without a JavaScript setup action, verifies first-party C++ formatting with `clang-format --dry-run --Werror`, builds with Ninja, and runs the `halionbridge_tests` CTest entry.

Release jobs upload the platform CLI plus the tracked `examples/` directory. The Windows release artifact stages `halionbridge.exe` and `examples/`. The macOS release artifact stages `halionbridge` and `examples/`. Tests, static libraries, import libraries, shared libraries, `.exp` files, generated example output presets, and build-directory structure are intentionally not uploaded. On exact semver tags such as `v0.5.0`, artifact names are `halionbridge-0.5.0-windows-x64` and `halionbridge-0.5.0-macos-universal`; GitHub downloads those artifacts as `.zip` files. Non-tag manual builds use CMake's generated package basename plus the same platform suffix.

The workflow validates compilation and unit tests only. It does not install, launch, or smoke-test HALion; HALion runtime testing still requires a licensed and activated local or self-hosted machine.

## Converter Subsystem

halionbridge supports setup-time converters through:

```text
halionbridge convert <converter-id> <source-directory> [output-directory] [converter-options]
```

Converters generate normal halionbridge build directories and then exit. They do not launch HALion. Users can inspect or edit the generated Lua and then run:

```text
halionbridge <build-directory>
```

The built-in `sfz` converter scans a source directory for `.sfz` files. It processes only top-level `.sfz` files by default and includes nested files only when `--recursive` is passed. If the output directory is omitted, the source directory is also the build root; if an output directory is supplied, generated files are written there instead. Each `.sfz` file currently generates one flat Lua build script in the build root; recursive source discovery does not create nested generated Lua. One generated `halionbridge_build.lua` lists all generated build entrypoint scripts in deterministic order. Converter-generated helper modules may be emitted beside build scripts and are treated as generated outputs for overwrite checks, but they are not listed in `halionbridge_build.lua`. `--overwrite` is required to replace existing generated files, including `halionbridge_build.lua`, `halionbridge-sfz.lua`, and generated entrypoint scripts. `--name <name>` is accepted only when exactly one `.sfz` file is converted. Conversion fails before writing files if two SFZ inputs would generate the same output `.vstpreset` filename.

Converter CLI diagnostics can be streamed through `ConverterRunContext` while still returning the full diagnostic vector for tests and embedded callers. Use the streaming path for long-running converters so scan, per-file conversion, warnings, and final summaries are visible immediately.

`converters/common` owns reusable converter infrastructure: converter registration, build-directory writing, Lua string escaping, overwrite checks, and deterministic LF output. Format-specific converter modules should pass generated Lua files to this common emitter instead of rewriting build-file logic. Generated Lua files have a role: `buildEntrypoint` files are written and listed in `halionbridge_build.lua`, while `helperModule` files are written but not listed. The emitter accepts only flat relative `.lua` filenames, rejects absolute paths, root paths, `.`/`..`, nested paths, non-`.lua` names, duplicate filenames, duplicate build-entrypoint module names, helper-only output, and reserved helper filenames used as build entrypoints. Validation and overwrite checks run before writing; later filesystem write failures can still leave partial output.

`converters/sfz` owns all sfizz interaction. sfizz is an implementation dependency only; sfizz headers and types must not appear in public converter APIs. The v1 SFZ mapping creates one HALion layer preset per source SFZ and maps sample paths, sample playback range, key ranges, velocity ranges, root key, sustain-loop fields, static amp-envelope fields, static pitch/tuning fields, static gain/velocity/pan fields, and simple tone fields into the tested HALion sample-zone Lua pattern. Generated Lua treats zone type, sample filename, key range, velocity range, root key assignment, and amp-envelope assignment as required; failure returns an unsuccessful build result before saving the preset. Optional naming, sample playback range, sustain-loop parameter assignment, sample oscillator level, amp velocity-to-level, amp pan, filter fields, and static pitch/tuning parameter assignment remain non-fatal and are logged by the generated build script when HALion rejects them. SFZ `offset` maps to HALion `SampleOsc.SampleStart`; SFZ inclusive `end=N` maps to HALion `SampleOsc.SampleEnd=N+1`, and the helper writes sample end before sample start so HALion does not clamp start markers against an unset end marker. For WAV-backed regions without explicit `end=`, the converter emits `sample_playback.natural_end` from the WAV frame count so the helper can initialize `SampleOsc.SampleEnd` before optional start or loop marker writes without treating the file-derived length as an authored SFZ end. SFZ `loop_mode=loop_continuous` maps to HALion sustain loop mode `1`, `loop_mode=loop_sustain` maps to HALion sustain loop mode `4`, and generated loop data writes `loop_end + 1` only to `SampleOsc.SustainLoopEndA`; loop assignment no longer truncates `SampleOsc.SampleEnd` to the loop end. Static gain conversion maps SFZ `volume` in dB plus the verified `+7.8 dB` HALion compensation to per-zone `SampleOsc.Level`; generated layers disable velocity-setting inheritance and set `VelocityToLevelCurve=1`, which probe `030` verified as HALion's squared Main velocity curve for SFZ default `amp_veltrack=100`. `amp_veltrack` is clamped to SFZ's -100..100 percent range with a converter warning before being written to HALion `Amp Env.VelocityToLevel`; probe `032` keeps `amp_veltrack=200` as an out-of-range clamp regression. SFZ `pan` maps to zone `Amp.Pan`; probe `033` verified center, half-left/right, and full-left/right against sforzando, and changing HALion pan law made the match worse, so HALion's default pan law is intentionally left unchanged. Static pitch conversion maps `pitch_keycenter` to `SampleOsc.Rootkey`, combines `transpose * 100 + tune` into `SampleOsc.Tune` with a -1200..1200 cent clamp, and maps `pitch_keytrack` to `Pitch.CenterKey` plus `Pitch.KeyFollow` with a -200..200 percent clamp. Static filter conversion is intentionally rough: supported sforzando-recognized LPF, HPF, BPF, BRF, `lsh`, `hsh`, and `peq` families map to HALion Classic filter type/mode/shape/cutoff/resonance values selected in probes `059`/`060`; `lpf_2p` keeps its earlier piecewise calibrated resonance table from probes `056`/`057`. State-variable `_sv` filter names and `pink` remain unsupported warnings rather than silent approximations.

Static pitch envelope conversion currently covers only the probe-verified simple `pitcheg_*` subset. `pitcheg_depth` is clamped to `-6000..6000` cents, converted to HALion `Pitch.EnvAmount` in semitones, and emitted as a required pitch-envelope assignment when nonzero. Zero-attack pitch envelopes start at level `1`; simple attack, hold, and sustain points are written directly. Positive `pitcheg_decay` values use the current probe-selected approximation of `0.30 * pitcheg_decay` with Lua envelope curve `-0.6`; probe `064` is the converter-backed regression awaiting manual same-number audition. Advanced pitch-envelope behavior, including shape, CC, velocity, and flex-EG pitch targets, is reported as `unsupported-pitch-envelope` rather than silently approximated.

The SFZ converter-owned helper module lives at `converters/sfz/lua/halionbridge-sfz.lua` and is embedded into the converter at build time. The generated C++ header splits the embedded helper into small adjacent raw string literal chunks so MSVC does not reject the helper as an oversized string literal as the converter-owned Lua module grows. Converted SFZ output directories include it as a helper module named `halionbridge-sfz.lua`; it is not listed in `halionbridge_build.lua`. Generated SFZ entrypoint scripts require this helper, keep readable normalized region data blocks for sample playback, mapping, amp envelope, pitch envelope, loop, gain, filter, and pitch fields, and call the helper for HALion layer, sample-zone, region-application, and save behavior instead of duplicating assignment functions in every script. Helper version `1` exposes conservative capabilities and defensive HALion assignment helpers for the fields already covered by the current converter output, including sample playback offset/end, the verified layer squared velocity curve, per-zone `SampleOsc.Level` gain offset, zone `Amp.Pan` pan mapping, and static pitch-envelope assignment through HALion `Pitch.EnvAmount` plus `Pitch Env.EnvelopePoints`. Unsupported or unverified ConvertWithMoss/SFZv2 behavior must stay marked unsupported and must warn or fail visibly instead of being silently ignored. The helper is SFZ converter support code, not a generic `halionbridge` Lua API.

HALion's Lua constructors may be callable without reporting `type(...) == "function"`. The SFZ helper therefore probes `Layer()` and `Zone()` construction with `pcall` and reports the actual construction error instead of rejecting the constructor by Lua type.

The SFZ converter uses `sfz::Synth::loadSfzFile()` as the primary region model, but it also runs sfizz's parser listener over the same source file to recover whether loop opcodes were explicitly written. This is necessary because sfizz's resolved model can replace `loop_start=0` with a WAV `smpl` chunk loop start: `0` is both a valid SFZ value and sfizz's default sentinel. Explicit source loop values must win over audio-file metadata so converted HALion presets match the SFZ author's text. SFZ `end` and `loop_end` are inclusive sample indices, while HALion's sample and loop end fields are marker positions; the SFZ helper therefore writes `sample_playback.finish + 1` to `SampleOsc.SampleEnd` for explicit SFZ playback ends, and writes `loop.finish + 1` to `SampleOsc.SustainLoopEndA` for sustain-loop playback. Loop markers do not truncate `SampleOsc.SampleEnd`; probe `041` showed that truncation breaks looped samples with post-loop tails. The helper writes explicit sample end markers before sample start markers because HALion can clamp a later start against the current end marker. The SFZ helper writes `SampleOsc.Filename`, root key, sample playback range, loop parameters, optional tone parameters, and optional pitch/tuning parameters before writing `zone.keyLow`, `zone.keyHigh`, `zone.velLow`, and `zone.velHigh`, because loading a sample can populate HALion fields from sampler metadata.

The converter always emits an explicit HALion amp envelope for every generated sample zone. SFZv2 defines `ampeg_release` with a default of `0.001` seconds and allows `0`; sfizz resolves that value in each region. HALion sample zones have their own default amp envelope, so relying on HALion defaults causes audible mismatches, especially for immediate SFZ releases. The generated envelope point order is SFZ's DAHDSR shape: start, optional delay, attack, optional hold, sustain, release. For positive `ampeg_release` values, probe `047` verified the current HALion approximation: an early release point at `0.097 * ampeg_release`, level `0.35`, Lua curve `-0.24`, followed by a final zero point whose segment duration completes `0.604 * ampeg_release` with Lua curve `-1.0`. The release early level is scaled by the current sustain level so a sustain-zero decay does not jump back up on note-off. Explicit `ampeg_release=0` stays an immediate final zero point. The generated Lua sets `Amp Env.EnvelopePoints` and `Amp Env.SustainIndex` through HALion's envelope-point API and treats failure as a corrupted build. Probe `050` calibrated static decay: for SFZ's default `ampeg_decay_zero=1`, nonzero sustain reaches the sustain level after `ampeg_decay * (1 - ampeg_sustain) * 0.5` with Lua curve `-1.0`; explicit `ampeg_decay_zero=0` uses `ampeg_decay * 0.604` with Lua curve `-1.0`; sustain level `0` uses a two-segment decay-to-zero shape with an early point at `0.097 * ampeg_decay`, level `0.35`, Lua curve `-0.24`, followed by a final zero point completing `0.604 * ampeg_decay` with Lua curve `-1.0`. Probe `052` found that sforzando matches HALion start level `0` for tested `ampeg_start=25`, `50`, and `100`; the converter therefore emits a zero initial envelope level for static `ampeg_start` cases. HALion envelope point durations are limited to 30 seconds, so longer SFZ envelope times are clamped with a converter warning. Advanced amp-envelope features that are not yet mapped exactly, including `ampeg_*_shape`, `ampeg_*_oncc`, `ampeg_*ccN`, `ampeg_release_zero`, `ampeg_dynamic`, velocity-to-envelope modulation, and flex EGs targeting amplitude, are surfaced as converter warnings rather than silently claiming exact parity.

SFZ behavior changes that can affect HALion output should get a small manual probe under `halionbridge_private/probes/sfz/<probe-id>/`. Keep each probe self-contained with `source/probe.sfz`, the required sample files, a generated `halionbridge-build/` directory when useful for review, and a short README describing the command-line generation step plus the sforzando/HALion 7 audition comparison. The automated checks validate generated Lua shape and HALion build success; the probe loop validates audible behavior. Probe `041` verified the current loop mapping: full-start continuous loops, offset-to-loop continuous loops, and offset-to-loop sustain loops match best with HALion normal playback, loop set A, `SustainLoopModeA=1` for continuous, `SustainLoopModeA=4` for sustain, no `ReleaseStart` jump, and no loop-driven `SampleOsc.SampleEnd` truncation. Probe `043` is the current converter-backed amplitude-envelope diagnostic for `ampeg_*` DAHDSR behavior. Its first manual result showed that the default release case matches but explicit one-second release does not match when generated as a linear HALion segment. Probe `044` showed that positive Lua envelope table curve values bend the wrong way for SFZ release behavior; probe `045` is the corrected negative-curve sweep using HALion's documented Lua `EnvelopePoints[].curve` range of `-1..1`, and manual validation selected `curve=-1.0` with duration `0.604` as the best match for the tested `ampeg_release=1.0` case. Probe `046` found that the same duration scale works reasonably for explicit releases `0.25`, `0.50`, `1.00`, and `2.00`, but manual tuning of the longest case suggests a two-segment approximation of sfizz's exponential release may cancel better than a single HALion release segment. Probe `047` validated that the curved two-segment release with an early `0.097 * ampeg_release` point at level `0.35` and Lua curve `-0.24`, followed by a final curved tail to `0.604 * ampeg_release`, is better than both the single-segment baseline and the linear-tail two-segment variant in all tested durations. Probe `048` is the converter-generated implementation regression for that mapping, and manual validation confirmed it works as well as the hand-authored `047` candidates. Probe `049` found that decay is still too slow in the broader converter-generated amp-envelope regression and exposed a sustain-zero release bump. Probe `050` selected the calibrated static decay shapes now implemented in the converter. Probe `051` confirmed the converter-generated decay mapping except for `006_start_50`. Probe `052` selected zero start level for tested nonzero `ampeg_start` values. Probe `053` confirmed the converter-generated implementation of that mapping. Probes `054` through `057` established only an approximate static `lpf_2p` mapping: `054` was inconclusive, `055` selected HALion Classic LP12 as the closest topology, `056` rejected cutoff scaling and measured the resonance table, and `057` showed that even the calibrated mapping remains only usable, not close. Probe `058` confirmed the converter-generated best-effort LPF2 mapping and high-resonance cap in ready-to-load HALion presets. Probe `059` selected rough static filter-family mappings, and probe `060` confirmed the converter-generated implementation while keeping `_sv` and `pink` unsupported. Do not spend further manual probe cycles trying to perfect static filter parity without a rendered-grid optimizer. Probe `061` is the first pitch/filter envelope diagnostic: its SFZ references omit `end=` and rely on explicit loop start/end like earlier working single-cycle probes, while HALion candidates still set `SampleEnd=200`; manual feedback confirmed `pitcheg_depth=1200` maps to `Pitch.EnvAmount=12` for the one-second attack ramp, but decay/start topology remains unresolved. Probe `062` confirmed the zero-attack hold cases and showed that a one-second linear HALion pitch decay is much too slow. Probe `063` focused the pitch-decay duration/curve sweep and supported a converter candidate around `0.30 * pitcheg_decay` with Lua curve `-0.6`. Probe `064` is the converter-backed implementation regression for the static pitch-envelope subset. Initial manual validation found the pitch-envelope behavior generally fine, but exposed that loop-only regions without `end=` left HALion `SampleOsc.SampleEnd` at `0`; the converter now emits WAV-derived `natural_end` for all no-explicit-end WAV regions, and regenerated `064` writes `natural_end=199` in all eight scripts.

Converter CMake options:

```text
HALIONBRIDGE_ENABLE_CONVERTERS=ON|OFF
HALIONBRIDGE_ENABLE_CONVERTER_SFZ=ON|OFF
HALIONBRIDGE_EXTRA_CONVERTER_DIRS=<semicolon-separated converter dirs>
```

Private or release-only converters are build-time drop-ins. A private converter can live under gitignored `converters/private/<name>`, be listed in gitignored `converters.local.cmake`, or be passed through `HALIONBRIDGE_EXTRA_CONVERTER_DIRS`. Drop-ins are statically linked into the CLI and must call `halionbridge_register_converter_target(TARGET ... REGISTRATION_FUNCTION ... HEADER ...)` from their CMake. Registration data is collected through CMake global properties so converter targets registered from nested `add_subdirectory()` scopes are included in the generated registration source. Runtime converter DLL/plugin loading is intentionally not used because it would add ABI, signing, notarization, and deployment failure modes.

## Orientation in the Code Base
- `CMakeLists.txt`: Main CMake configuration file linking the JUCE 8 submodule and defining the C++23 requirements.
- `external/JUCE`: The JUCE 8 framework git submodule.
- `external/spdlog`: The spdlog logging submodule, used privately by the implementation and CLI.
- `external/sfizz`: The sfizz SFZ parser/synth library git submodule, used privately by the built-in `sfz` converter.
- `converters/common`: First-party converter registry and generated build-directory emitter.
- `converters/sfz`: First-party SFZ converter that uses sfizz privately and emits HALion Lua build scripts.
- `converters/sfz/lua/halionbridge-sfz.lua`: SFZ-owned generated helper module embedded into converted SFZ output directories.
- `halionbridge_private/probes/sfz`: Small SFZ fixtures and generated build directories used for manual sforzando/HALion audition checks.
- `converters/private`: Gitignored location for optional private converter drop-ins used by local or release-only builds.
- `include/halionbridge/Bridge.h`: JUCE-free public library API for command-line option parsing, HALion plugin location, VST preset inspection, runtime module generation, status marker paths, and bridge execution. `Bridge::getDefaultHalionPluginPath()` reports the platform default (`C:\Program Files\Common Files\VST3\Steinberg\HALion 7.vst3` on Windows, `/Library/Audio/Plug-Ins/VST3/Steinberg/HALion 7.vst3` on macOS), and `Bridge::findHalionPlugin()` applies the optional `--plugin` override before checking that default path exists. `Bridge::runDetailed()` returns a `RunResult` for embedding applications; `Bridge::run()` remains the bool convenience wrapper. A moved-from `Bridge` returns `RunResult::invalidBridge` so API misuse is distinguishable from invalid user options.
- `include/halionbridge/CrashDiagnostics.h`: JUCE-free public crash-diagnostics entry points used by the CLI before JUCE/HALion startup.
- `src/Bridge.cpp`: Core bridge orchestration for preparing the embedded bootstrap preset, hosting the HALion 7 VST3 format, applying preset data, and driving the processing loop.
- `src/CrashDiagnostics.cpp`: Windows crash-dump support for failures that occur inside JUCE or a hosted VST3 before normal error reporting can run.
- `src/ChildProcessOutput.cpp`: Byte-buffered subprocess output forwarding. It preserves split UTF-8 sequences across reads before logging scan-worker output.
- `src/Log.cpp`: Private spdlog setup and log-level parsing. spdlog must not appear in the public `include/halionbridge` API.
- `src/BuildFile.cpp`: Host-side build file inspection for the top-level string list returned by `halionbridge_build.lua`, plus deterministic generation of a simple build file for `halionbridge init`.
- `src/PathUtils.cpp`: Private JUCE/std filesystem conversion and CLI path normalization helpers shared by the library, CLI, and tests.
- `src/PluginScan.cpp`: HALion plugin-description construction, in-process plugin scanning, and isolated scan-worker implementation.
- `src/ProgressMarkers.cpp`: Host-side progress marker decoding, logging, cleanup, and filename codec behavior shared by the processing loop and tests.
- `cli/Main.cpp`: Thin command-line frontend, usage text, console logging, process entry point, and dispatch into the private scan-worker mode.
- `tests/Tests.cpp`: Unit tests executable to verify argument parsing, HALion default plugin paths, build-status paths, runtime module generation, build file parsing, progress marker decoding, subprocess output buffering, and VST3 preset-container inspection.
- `HALION-LUA.md`: Contract for the generic HALion Lua build script runner, build script modules, progress reporting, and status reporting.

## Code Quality Gates

First-party C++ source files are compiled with warnings as errors: `/W4 /WX` on MSVC and `-Wall -Wextra -Wpedantic -Werror` on Clang/GCC-style compilers. Do not suppress warning classes globally to make the build pass; fix the warning or isolate an intentionally noisy platform boundary narrowly.

The repository uses `.clang-format` for first-party C++ formatting. GitHub Actions runs `clang-format --dry-run --Werror` on tracked `.cpp` and `.h` files outside `external/`. Vendored JUCE, spdlog, and sfizz code is not reformatted and is not held to project warning settings. sfizz is configured through a narrow CMake wrapper in `converters/CMakeLists.txt` which disables sfizz's unused tests, demos, clients, shared library, sndfile integration, Qt probe, and LTO path. The wrapper also scopes CMake developer/deprecation-warning suppression, MSVC C++23 STL deprecation macros, and Apple Clang C/C++ diagnostics from sfizz's bundled dependency tree, and prepends `cmake/sfizz-overrides` for first-party CMake shims required to keep vendored configure output quiet without editing `external/sfizz`. halionbridge keeps `CMAKE_CXX_STANDARD` cache-backed at C++23 because sfizz's original config otherwise creates a C++14 cache entry that changes Abseil public aliases such as `absl::optional`. On macOS, halionbridge sets the project deployment target to at least macOS 10.15 before `project()` and `cmake/sfizz-overrides/SfizzConfig.cmake` force-restores that value after sfizz's own config resets it. Together these settings keep sfizz's public C++ ABI, including `fs::path` and `absl::optional`, matching first-party converter code in universal release builds.

## Console Logging

Runtime output uses spdlog through the private halionbridge logging wrapper. Do not write new runtime output directly with `std::cout` or `std::cerr`; the exceptions are plain command output such as `--help`, `--version`, and the low-level Windows crash handler that may run while the process is already unstable.

The default log level is `info`. Users can set `HALIONBRIDGE_LOGLEVEL` to `trace`, `debug`, `info`, `warn`, `error`, or `off`. Invalid values fall back to `info` and emit one warning. Console logs use this timestamped pattern:

```text
[YYYY-MM-DD HH:mm:ss.mmm] [level] message
```

`trace`, `debug`, and `info` are routed to stdout. `warn` and `error` are routed to stderr. Every emitted log record is flushed immediately so long conversions and HALion builds are observable from calling shells. Host internals such as temporary runtime files, VST3 preset details, plugin scan details, marker paths, and cleanup are `debug`. Build script progress and batch summaries remain `info` so normal users can see build progress without enabling diagnostics. HALion Lua `print()` output is not treated as a reliable stdout transport; the embedded builder writes temporary `hbp_*.vstpreset` marker presets in the build directory, and the host polls those markers at a throttled interval to forward runner, `ctx.log`, and `ctx.progress` messages to the console. Marker filenames contain hex-encoded message bytes, so punctuation, paths, and percent signs round-trip in console output. Marker messages use an 88-byte maximum budget on short paths and a smaller dynamic budget on long build-directory paths so progress marker paths stay below a conservative Windows path-length boundary; shortened messages include `...` inside the available budget. Each consumed progress marker is deleted immediately after it is logged so long builds do not accumulate marker files. When a terminal OK or failed status marker appears, the host drains progress markers for a short quiet period, then retries deletion again after HALion plugin resources are released. If stale progress markers cannot be deleted before a run, their filenames are suppressed from current-run logging and cleanup is retried later. The host also cleans up and parses legacy `halionbridge_progress_*.vstpreset` markers from earlier builds.

## Diagnostic Builds

`CMakePresets.json` defines the supported build variants. Do not add undocumented one-off build directories when investigating plugin startup failures; add a preset if a new build shape is needed.

The diagnostic compile definitions are:

```text
JUCE_VST3_DEBUGGING=1
JUCE_LOG_ASSERTIONS=1
JUCE_CATCH_UNHANDLED_EXCEPTIONS=1
HALIONBRIDGE_DIAGNOSTIC_BUILD=1
```

They are enabled automatically for `Debug` builds and explicitly for presets that set `HALIONBRIDGE_ENABLE_DIAGNOSTICS=ON`.

`HALIONBRIDGE_ENABLE_CRASH_DUMPS=ON` is enabled by default. On Windows, the bridge installs an unhandled-exception filter before JUCE initialization. If HALion, JUCE, or the VST3 loader crashes the process, the bridge prints the Windows exception code/address and writes a minidump named `halionbridge_crash_YYYYMMDD_HHMMSS_<pid>.dmp` in the current working directory.

For the current HALion startup issue, use:

```powershell
cd halion-lua
..\build-diagnostics\halionbridge_artefacts\Debug\halionbridge.exe . --timeout-seconds 60
```

If Debug changes timing and the crash disappears, run the optimized diagnostic build:

```powershell
cd halion-lua
..\build-relwithdebinfo-diagnostics\halionbridge_artefacts\RelWithDebInfo\halionbridge.exe . --timeout-seconds 60
```

## Dependencies
- **JUCE 8:** Core implementation framework, handling VST3 plugin hosting and offline audio buffers. JUCE must not appear in public `include/halionbridge` interfaces.
- **spdlog 1.17.0:** Private implementation dependency for timestamped console logging and log-level filtering. It is vendored as `external/spdlog` and linked through `spdlog::spdlog_header_only`.
- **sfizz:** Private implementation dependency for the built-in SFZ converter. halionbridge initializes sfizz's nested submodules with normal Git submodule commands and disables sfizz's own test/demo/client targets in the halionbridge build. sfizz and its nested vendored dependencies are read-only; CMake compatibility workarounds live in first-party configuration, including `cmake/sfizz-overrides`.
- **HALion 7 VST3:** The target plugin (must be installed on the host system to run the bridge).

## Preset Loading Notes
- Generic VST3 component/controller-state presets are applied through JUCE's VST3 client preset API.
- HALion program/layer presets may contain `Prog` program-data chunks without `Comp`/`Cont` state chunks. These are applied through Steinberg VST3 program/unit data interfaces exposed by HALion.
- halionbridge is a single-purpose builder host. Users call `halionbridge <build-directory>`. The directory must contain `halionbridge_build.lua`; build script modules may create and save program presets, layer presets, or other HALion artifacts independently. Users can run `halionbridge init <build-directory>` to generate a simple sorted `halionbridge_build.lua` from top-level Lua files before running the build.
- `halionbridge init` is a setup command, not a build command. It does not initialize JUCE's plugin-host path or launch HALion. It refuses to replace an existing build file unless `--overwrite` is passed. Normal build mode emits a warning pointing to `halionbridge init <directory>` when the build file is missing but top-level Lua files are present. The generated list includes every top-level non-infrastructure `.lua` file, so users must review it and remove helper modules that are not build entrypoints. The converter-owned SFZ helper filename `halionbridge-sfz.lua` is treated as infrastructure and excluded automatically.
- Raw `.vstpreset` bytes must not be passed to `setStateInformation()` as a fallback; that API expects JUCE's hosted-plugin state representation, not a Steinberg preset container.
- The bridge drives HALion with `prepareToPlay()` and manual `processBlock()` calls. It does not use `AudioDeviceManager` or open a system audio device.
- Normal runs register JUCE's headless plugin formats with `addHeadlessDefaultFormatsToManager()`. `--gui` switches to JUCE's GUI-capable plugin formats with `addDefaultFormatsToManager()` and creates the HALion editor.
- The bridge applies the compile-time embedded `builder_bootstrap.vstpreset` internally and reads its VST3 class ID to construct the HALion plugin description directly, so it can instantiate HALion without scanning the plugin bundle first. If no preset class ID is available, plugin description scanning falls back to a short-lived internal worker process so the scan-time module load is isolated from the main process.
- Plugin scanning reads VST3 factory class metadata only (`GetPluginFactory`, `PClassInfo`/`PClassInfo2`/`PClassInfoW`) and never instantiates a component. HALion 7's module teardown (`ExitDll`) crashes intermittently with an access violation or fast-fail abort when an `IComponent` was initialized and terminated during the same module load, which is exactly what JUCE's `findAllTypesForFile()` does to fill in channel counts. The vendored JUCE is read-only, so the metadata-only scan lives in `src/PluginScan.cpp`; the required upstream change would be for JUCE to keep scanned VST3 modules loaded for the process lifetime again (as JUCE 8's `DLLHandleCache` did) or for HALion to fix its `ExitDll`. Channel counts in scanned descriptions stay 0; they are informational and not needed to instantiate HALion.
- `--force-scan` is a diagnostics-only switch that disables the embedded class-ID shortcut and forces the factory-metadata scan path. In CLI runs the scan happens in the isolated worker process. The embedded bootstrap preset class ID is still used to select the matching HALion description from scan results, so the switch reproduces scan-time behavior without intentionally changing which HALion class should be instantiated.
- The compile-time assets are `halion-lua/builder_bootstrap.vstpreset` and `halion-lua/builder.lua`, embedded through the `halionbridge_assets` binary-data target. The checked-in `halion-lua/builder_bootstrap.lua` is the canonical source text for the inline Lua saved inside `builder_bootstrap.vstpreset`; it should call `require("halionbridge_runtime")`.
- For builder runs, the bridge writes temporary `halionbridge_runtime.lua` and `halionbridge_builder.lua` files into the HALion user scripts directory under `Documents/Steinberg/HALion/Library/scripts`. HALion Lua does not reliably expose the host process environment or current working directory to `require()`, so this generated runtime module is the authoritative handoff. It sets the Lua global `HALIONBRIDGE_RUNTIME_ROOT`, optional build-slice globals, prepends the build directory to `package.path` if needed, clears cached runtime/builder modules, and then loads the temporary embedded builder module. Previous files with the same names are restored after the run; otherwise the temporary files are deleted. Because these filenames and the working directory/environment handoff are shared process/user state, halionbridge acquires a fail-fast inter-process lock for the whole runtime-staging lifetime and rejects overlapping runs.
- Running `halionbridge` without arguments is equivalent to `halionbridge --help` and exits successfully after printing usage.
- For HALion batch builds, `halion-lua/builder.lua` is a neutral build script runner. It loads build script modules listed in `halionbridge_build.lua`, passes each module a documented `ctx` API, forwards file-level progress through temporary `hbp_*.vstpreset` marker presets, aggregates build script results, and writes `halionbridge_status_ok.vstpreset` or `halionbridge_status_failed.vstpreset` in the build directory using HALion's `savePreset()` API. For statically parseable build files, the host runs the list one script per HALion process by default. `--build-chunk-size <n>` changes the chunk size, and `--fail-fast` stops after the first failed chunk. Lua build failures and per-chunk timeouts are recorded but later chunks continue by default, with a final nonzero exit if any chunk failed; setup, plugin, preset-apply, stop, and cleanup failures stop immediately. The bridge deletes stale status/progress files before applying the preset, deletes each consumed progress marker immediately after logging it, drains late progress markers when a terminal status marker appears, and performs one final progress-marker sweep after releasing HALion plugin resources. Successful OK status markers are deleted; failed status markers remain after failed runs for diagnostics.
- Build script modules own all HALion-specific build behavior: program presets, layer presets, zones, samples, and output filenames. Keep the build script API documented in `HALION-LUA.md` when changing builder or build script behavior.
- Generic build script modules are arbitrary code, so `halionbridge_build.lua` entries are not treated as expected output preset names. Build completion is reported only through HALion-written `.vstpreset` marker files.
- The default build timeout is `0`, which waits forever and emits a startup warning. Set `--timeout-seconds <n>` only when a finite build timeout is desired.
- `--nokill` is a diagnostics switch for inspecting HALion after the runtime and builder pipeline has run. When the bridge observes an OK marker, failed marker, timeout, or another processing-loop exit, it keeps the plugin instance alive instead of immediately hiding the GUI and releasing plugin resources. Terminal progress-marker draining still happens before the inspection hold, and final status/progress cleanup happens after the hold exits and HALion resources are released. In headless mode the process remains alive until Ctrl+C requests a graceful stop. With `--gui`, closing the GUI window leaves the inspection hold and then the bridge shuts down normally. In both cases normal C++ cleanup still runs, so temporary HALion runtime files are restored or deleted.
