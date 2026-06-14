# HALion Lua Build Script API

`halion-lua/builder.lua` is the source for the generic halionbridge Lua runner embedded into `halionbridge.exe`. It must stay neutral: it loads build script modules, reports progress, aggregates results, and writes halionbridge status markers. Build script modules own all HALion-specific build behavior.

## Build File

`halionbridge_build.lua` in the build directory passed to `halionbridge` returns the ordered list of build script modules to run:

```lua
return {
    "kick_builder",
    "another_builder",
}
```

Module names may be listed with or without `.lua`. They are loaded with `require`, so they must be resolvable from the build directory passed to halionbridge. The host-side helper `Bridge::parseBuildFileModuleNames()` inspects the common top-level list shape `return { "module_a", "module_b" }`; it is not a full Lua parser, and it ignores quoted strings in line comments, Lua long comments, nested tables, local variables, and metadata fields.

For statically parseable build files, halionbridge runs the list in host-controlled chunks of 15 modules by default. Each chunk launches a fresh HALion instance and the generated runtime module sets `HALIONBRIDGE_BUILD_SLICE_START`, `HALIONBRIDGE_BUILD_SLICE_COUNT`, and `HALIONBRIDGE_BUILD_TOTAL` before loading this builder. The builder uses those globals only to select the requested list slice and to report file-level progress against the full list. Build script modules and the `ctx` API are unchanged. If the host cannot statically parse the build file, it falls back to one full-list invocation.

`halionbridge init <build-directory>` can generate a simple `halionbridge_build.lua` from top-level `.lua` files. It sorts filenames, keeps the `.lua` suffix in each entry, and excludes halionbridge infrastructure files such as `halionbridge_runtime.lua`, `halionbridge_builder.lua`, `halionbridge_build.lua`, and `builder_bootstrap.lua`; converter-owned infrastructure helpers may also be excluded. It does not recurse into subdirectories and does not launch HALion. Review the generated list before building: ordinary helper modules such as `helpers.lua` or `shared_mapping.lua` are still top-level Lua files, but they should be removed from `halionbridge_build.lua` unless they return a valid build script entrypoint.

## Runtime Root

When halionbridge runs, it applies the embedded bootstrap vstpreset and writes temporary `halionbridge_runtime.lua` and `halionbridge_builder.lua` modules into HALion's user script library. This is why this must be configured in HALion in the library search paths in Options / Scripting Section. The vstpreset's inline bootstrap loads `halionbridge_runtime` with `require("halionbridge_runtime")`. The generated runtime module sets the Lua global `HALIONBRIDGE_RUNTIME_ROOT`, prepends the build directory to `package.path`, clears any cached `halionbridge_builder` module, and then loads the embedded builder module as `halionbridge_builder`. It clears its own `package.loaded` entry after the builder returns so repeated host invocations can load a fresh runtime module.

The builder treats the required positional build directory as the runtime root for:

- `halionbridge_build.lua`
- build script modules
- `ctx.script_dir`
- `ctx.sample_root`
- generated vstpresets
- `halionbridge_status_ok.vstpreset` and `halionbridge_status_failed.vstpreset`
- temporary `hbp_*.vstpreset` files used to forward progress back to the halionbridge console; messages are hex-encoded in the marker filename so punctuation and paths round-trip, messages longer than the marker filename budget are shortened with `...` before encoding, the budget is at most 88 bytes and shrinks for long build-directory paths, consumed progress markers are deleted by the host immediately after logging, and final cleanup retries progress-marker deletion after HALion resources are released

The generic bootstrap vstpreset must contain inline Lua that calls `require("halionbridge_runtime")`. `halion-lua/builder_bootstrap.lua` is the reference source for the inline script embedded in `halion-lua/builder_bootstrap.vstpreset`; it is not a runtime module to load from the build directory.

## Build Script Entrypoint

Each listed module must return either a function:

```lua
return function(ctx)
    ctx.log("Building example")
    return {
        ok = true,
        saved = 0,
        failed = 0,
        message = "Example complete",
    }
end
```

Or a table with a `run(ctx)` function:

```lua
return {
    run = function(ctx)
        return {
            ok = true,
            saved = 0,
            failed = 0,
            message = "Example complete",
        }
    end,
}
```

Build scripts are trusted HALion Lua code. They may create programs, layers, zones, scripts, presets, or other HALion objects directly. The builder does not inspect build-specific data schemas. The Lua build scripts are executed directly within the HALion VST3 plugin.

## Context API

The builder passes a `ctx` table to each build script:

```lua
ctx.script_dir
ctx.sample_root
ctx.module_name
ctx.path_join(root, rel)
ctx.save_preset(path, object, preset_type)
ctx.log(message)
ctx.progress(done, total, message)
```

- `ctx.script_dir`: runtime root directory passed on the command line; it contains `halionbridge_build.lua` and Lua build script files.
- `ctx.sample_root`: default sample root; currently the same as `ctx.script_dir`.
- `ctx.module_name`: normalized module name without `.lua`.
- `ctx.path_join(root, rel)`: joins paths using forward slashes.
- `ctx.save_preset(path, object, preset_type)`: wraps HALion `savePreset`; defaults `preset_type` to `"H7"`.
- `ctx.log(message)`: prints a build script log line and writes a host-readable progress marker. halionbridge treats build script log and progress lines as `info` output, so they remain visible at the default `HALIONBRIDGE_LOGLEVEL=info`. Very long messages are shortened in the marker filename; keep essential context near the start of the message.
- `ctx.progress(done, total, message)`: prints generic build script progress such as `Progress 12/48 ( 25%) - Building zones` and writes the same message through the host-readable progress marker channel. Numeric fields are padded so the message after ` - ` starts at a stable column within the same progress group. Very long messages are shortened in the marker filename; keep essential context near the start of the message.

`ctx.progress` is build-script-defined progress. `done` and `total` can represent files, zones, presets, phases, or any other work unit meaningful to that build script.

## Build Script Result

Build scripts must return a result table:

```lua
return {
    ok = true,
    saved = 1,
    failed = 0,
    message = "Built example.vstpreset",
}
```

- `ok`: `true` only when the build script completed successfully.
- `saved`: number of artifacts or presets saved by the build script.
- `failed`: number of failed artifacts or build-script-level failures.
- `message`: short human-readable summary printed by the builder.

If a build script throws an error, returns an invalid entrypoint, or returns an invalid result, the builder treats that build script as failed.

## Builder Responsibilities

`builder.lua` is responsible for:

- loading `halionbridge_build.lua`
- loading and invoking the host-selected build script module slice in order
- printing and forwarding file-level progress such as `Processing x.lua` and `Progress x/y files (70%)`
- passing `ctx` to each build script
- aggregating build script results
- writing `halionbridge_status_ok.vstpreset` or `halionbridge_status_failed.vstpreset`

`builder.lua` must not assume build script fields or build shapes such as `articulations`, `slots`, `wav`, velocity layers, round robin, key mapping, program presets, layer presets, sample zones, or output filenames.

## Example: Saving a Layer Preset

```lua
return function(ctx)
    local layer = Layer()
    layer:setName("example_layer")

    -- Saving a layer that is attached to the current Program Tree matches
    -- HALion's documented savePreset() usage and preserves the layer as the
    -- loaded preset root.
    this.parent:appendLayer(layer)

    ctx.progress(0, 1, "Saving layer")
    local path = ctx.path_join(ctx.script_dir, "example_layer.vstpreset")
    local success = ctx.save_preset(path, layer, "H7")
    ctx.progress(1, 1, "Done")

    if not success then
        return {
            ok = false,
            saved = 0,
            failed = 1,
            message = "Failed to save " .. path,
        }
    end

    return {
        ok = true,
        saved = 1,
        failed = 0,
        message = "Built example_layer.vstpreset",
    }
end
```

More examples can be found in `examples`.

## Converter-generated Lua

Converters such as `halionbridge convert sfz` generate ordinary build directories that follow this same contract. The generated `halionbridge_build.lua` is just an ordered list of generated Lua build script modules, and each listed module returns a normal build script function. A converter-generated directory may also contain helper modules that are required by those build scripts but are intentionally not listed in `halionbridge_build.lua`. Generated scripts should remain readable source: comments should explain the HALion object assumptions, path handling, and parameter assignments so converter authors can inspect and adjust the output before running `halionbridge <build-directory>`.

Generated build scripts must distinguish assignments that are essential to a correct preset from optional decoration. For the built-in SFZ converter, sample-zone type, sample filename, root key, key range, velocity range, and amp-envelope assignment are required assignments: if HALion rejects any of them, the generated build script returns `ok = false` before saving. Optional assignments such as display names, sustain-loop parameters, amp velocity-to-level, and filter cutoff may be attempted defensively and logged as skipped when HALion rejects them. Generated SFZ scripts write the sample filename, root key, loop parameters, amp envelope, and optional tone parameters before the final key/velocity fields so audio-file sampler metadata cannot overwrite the intended SFZ mapping.

Generated SFZ scripts always set `Amp Env.EnvelopePoints` and `Amp Env.SustainIndex` explicitly. This is part of the build-script contract because SFZ amp-envelope defaults affect playback: for example, `ampeg_release=0` means an immediate release, while HALion's default sample-zone envelope may fade. Converter authors extending the generated scripts should preserve this required assignment behavior unless they intentionally replace it with a more accurate envelope implementation.
