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

The public C++ headers live under `include/halionbridge` and must remain JUCE-free. Public API types use C++23/STL types such as `std::filesystem::path`, `std::string`, `std::vector`, `std::span`, and `std::optional`; JUCE is an implementation dependency only. The generated export header is written by CMake under the build directory and is included through `halionbridge/halionbridge_export.h`. Public headers use `HALIONBRIDGE_EXPORT` so the same declarations work for the shared library and for static consumers that define `HALIONBRIDGE_STATIC_DEFINE`.

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

macOS Release builds are configured as universal binaries with `CMAKE_OSX_ARCHITECTURES=arm64;x86_64` on the arm64 runner. macOS signing uses the same secret names as the SessionPrep workflow: `apple-actions/import-codesign-certs@v7.0.0`, `P12_BASE64`, `P12_PASSWORD`, and `MACOS_DEV_ID_CERT_NAME`. If the `Notarize macOS release zip` checkbox is enabled, the workflow submits a temporary zip containing the signed CLI to Apple with `xcrun notarytool`. The temporary notarization zip is not uploaded, and DMG packaging is intentionally not used for this command-line tool. Notarization requires `APPLE_NOTARY_USER`, `APPLE_NOTARY_APP_PASSWORD`, and `APPLE_TEAM_ID`.

The workflow validates compilation and unit tests only. It does not install, launch, or smoke-test HALion; HALion runtime testing still requires a licensed and activated local or self-hosted machine.

## Orientation in the Code Base
- `CMakeLists.txt`: Main CMake configuration file linking the JUCE 8 submodule and defining the C++23 requirements.
- `external/JUCE`: The JUCE 8 framework git submodule.
- `external/spdlog`: The spdlog logging submodule, used privately by the implementation and CLI.
- `include/halionbridge/Bridge.h`: JUCE-free public library API for command-line option parsing, HALion plugin location, VST preset inspection, runtime module generation, status marker paths, and bridge execution. `Bridge::runDetailed()` returns a `RunResult` for embedding applications; `Bridge::run()` remains the bool convenience wrapper. A moved-from `Bridge` returns `RunResult::invalidBridge` so API misuse is distinguishable from invalid user options.
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
- `tests/Tests.cpp`: Unit tests executable to verify argument parsing, build-status paths, runtime module generation, build file parsing, progress marker decoding, subprocess output buffering, and VST3 preset-container inspection.
- `HALION-LUA.md`: Contract for the generic HALion Lua build script runner, build script modules, progress reporting, and status reporting.

## Code Quality Gates

First-party C++ source files are compiled with warnings as errors: `/W4 /WX` on MSVC and `-Wall -Wextra -Wpedantic -Werror` on Clang/GCC-style compilers. Do not suppress warning classes globally to make the build pass; fix the warning or isolate an intentionally noisy platform boundary narrowly.

The repository uses `.clang-format` for first-party C++ formatting. GitHub Actions runs `clang-format --dry-run --Werror` on tracked `.cpp` and `.h` files outside `external/`. Vendored JUCE and spdlog code is not reformatted and is not held to project warning settings.

## Console Logging

Runtime output uses spdlog through the private halionbridge logging wrapper. Do not write new runtime output directly with `std::cout` or `std::cerr`; the exceptions are plain command output such as `--help`, `--version`, and the low-level Windows crash handler that may run while the process is already unstable.

The default log level is `info`. Users can set `HALIONBRIDGE_LOGLEVEL` to `trace`, `debug`, `info`, `warn`, `error`, or `off`. Invalid values fall back to `info` and emit one warning. Console logs use this timestamped pattern:

```text
[YYYY-MM-DD HH:mm:ss.mmm] [level] message
```

`trace`, `debug`, and `info` are routed to stdout. `warn` and `error` are routed to stderr. Logs flush at the configured minimum level so debug output remains visible during hangs. Host internals such as temporary runtime files, VST3 preset details, plugin scan details, marker paths, and cleanup are `debug`. Build script progress and batch summaries remain `info` so normal users can see build progress without enabling diagnostics. HALion Lua `print()` output is not treated as a reliable stdout transport; the embedded builder writes temporary `hbp_*.vstpreset` marker presets in the build directory, and the host polls those markers at a throttled interval to forward runner, `ctx.log`, and `ctx.progress` messages to the console. Marker filenames contain hex-encoded message bytes, so punctuation, paths, and percent signs round-trip in console output. Messages longer than 88 bytes are shortened with `...` inside that 88-byte budget before encoding to keep marker filenames within platform filename-component limits. Each consumed progress marker is deleted immediately after it is logged so long builds do not accumulate marker files. If stale progress markers cannot be deleted before a run, their filenames are suppressed from current-run logging and cleanup is retried later. The host also cleans up and parses legacy `halionbridge_progress_*.vstpreset` markers from earlier builds.

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
- **HALion 7 VST3:** The target plugin (must be installed on the host system to run the bridge).

## Preset Loading Notes
- Generic VST3 component/controller-state presets are applied through JUCE's VST3 client preset API.
- HALion program/layer presets may contain `Prog` program-data chunks without `Comp`/`Cont` state chunks. These are applied through Steinberg VST3 program/unit data interfaces exposed by HALion.
- halionbridge is a single-purpose builder host. Users call `halionbridge <build-directory>`. The directory must contain `halionbridge_build.lua`; build script modules may create and save program presets, layer presets, or other HALion artifacts independently. Users can run `halionbridge init <build-directory>` to generate a simple sorted `halionbridge_build.lua` from top-level Lua files before running the build.
- `halionbridge init` is a setup command, not a build command. It does not initialize JUCE's plugin-host path or launch HALion. It refuses to replace an existing build file unless `--overwrite` is passed. Normal build mode emits a warning pointing to `halionbridge init <directory>` when the build file is missing but top-level Lua files are present.
- Raw `.vstpreset` bytes must not be passed to `setStateInformation()` as a fallback; that API expects JUCE's hosted-plugin state representation, not a Steinberg preset container.
- The bridge drives HALion with `prepareToPlay()` and manual `processBlock()` calls. It does not use `AudioDeviceManager` or open a system audio device.
- Normal runs register JUCE's headless plugin formats with `addHeadlessDefaultFormatsToManager()`. `--gui` switches to JUCE's GUI-capable plugin formats with `addDefaultFormatsToManager()` and creates the HALion editor.
- The bridge applies the compile-time embedded `builder_bootstrap.vstpreset` internally and reads its VST3 class ID to construct the HALion plugin description directly, so it can instantiate HALion without scanning the plugin bundle first. If no preset class ID is available, plugin description scanning falls back to a short-lived internal worker process so the scan-time module load is isolated from the main process.
- Plugin scanning reads VST3 factory class metadata only (`GetPluginFactory`, `PClassInfo`/`PClassInfo2`/`PClassInfoW`) and never instantiates a component. HALion 7's module teardown (`ExitDll`) crashes intermittently with an access violation or fast-fail abort when an `IComponent` was initialized and terminated during the same module load, which is exactly what JUCE's `findAllTypesForFile()` does to fill in channel counts. The vendored JUCE is read-only, so the metadata-only scan lives in `src/PluginScan.cpp`; the required upstream change would be for JUCE to keep scanned VST3 modules loaded for the process lifetime again (as JUCE 8's `DLLHandleCache` did) or for HALion to fix its `ExitDll`. Channel counts in scanned descriptions stay 0; they are informational and not needed to instantiate HALion.
- `--force-scan` is a diagnostics-only switch that disables the embedded class-ID shortcut and forces the factory-metadata scan path. In CLI runs the scan happens in the isolated worker process. The embedded bootstrap preset class ID is still used to select the matching HALion description from scan results, so the switch reproduces scan-time behavior without intentionally changing which HALion class should be instantiated.
- The compile-time assets are `halion-lua/builder_bootstrap.vstpreset` and `halion-lua/builder.lua`, embedded through the `halionbridge_assets` binary-data target. The checked-in `halion-lua/builder_bootstrap.lua` is the canonical source text for the inline Lua saved inside `builder_bootstrap.vstpreset`; it should call `require("halionbridge_runtime")`.
- For builder runs, the bridge writes temporary `halionbridge_runtime.lua` and `halionbridge_builder.lua` files into the HALion user scripts directory under `Documents/Steinberg/HALion/Library/scripts`. HALion Lua does not reliably expose the host process environment or current working directory to `require()`, so this generated runtime module is the authoritative handoff. It sets the Lua global `HALIONBRIDGE_RUNTIME_ROOT`, prepends the build directory to `package.path` if needed, and then loads the temporary embedded builder module. Previous files with the same names are restored after the run; otherwise the temporary files are deleted. Because these filenames and the working directory/environment handoff are shared process/user state, halionbridge acquires a fail-fast inter-process lock for the whole runtime-staging lifetime and rejects overlapping runs.
- Running `halionbridge` without arguments is equivalent to `halionbridge --help` and exits successfully after printing usage.
- For HALion batch builds, `halion-lua/builder.lua` is a neutral build script runner. It loads build script modules listed in `halionbridge_build.lua`, passes each module a documented `ctx` API, forwards file-level progress through temporary `hbp_*.vstpreset` marker presets, aggregates build script results, and writes `halionbridge_status_ok.vstpreset` or `halionbridge_status_failed.vstpreset` in the build directory using HALion's `savePreset()` API. The bridge deletes stale status/progress files before applying the preset, deletes each consumed progress marker immediately after logging it, and deletes successful completion status files after observing success; failed status markers remain after failed runs for diagnostics.
- Build script modules own all HALion-specific build behavior: program presets, layer presets, zones, samples, and output filenames. Keep the build script API documented in `HALION-LUA.md` when changing builder or build script behavior.
- Generic build script modules are arbitrary code, so `halionbridge_build.lua` entries are not treated as expected output preset names. Build completion is reported only through HALion-written `.vstpreset` marker files.
- The default build timeout is `0`, which waits forever and emits a startup warning. Set `--timeout-seconds <n>` only when a finite build timeout is desired.
- `--nokill` is a diagnostics switch for inspecting HALion after the runtime and builder pipeline has run. When the bridge observes an OK marker, failed marker, timeout, or another processing-loop exit, it keeps the plugin instance alive instead of immediately hiding the GUI and releasing plugin resources. On successful completion, output marker cleanup happens after the hold exits. In headless mode the process remains alive until Ctrl+C requests a graceful stop. With `--gui`, closing the GUI window leaves the inspection hold and then the bridge shuts down normally. In both cases normal C++ cleanup still runs, so temporary HALion runtime files are restored or deleted.
