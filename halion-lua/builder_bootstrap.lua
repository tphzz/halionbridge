-- Bootstrap script embedded in builder_bootstrap.vstpreset.
--
-- This is the complete Lua snippet that the generic HALion preset should run
-- directly in its single Lua MIDI module. It must be embedded inline in the
-- preset, because the runtime build directory cannot be found until
-- halionbridge_runtime.lua has passed it to the embedded builder module.
--
-- halionbridge.exe writes halionbridge_runtime.lua into HALion's user script
-- library before applying this preset. HALion can resolve that generated module
-- through its normal script search paths. The generated module points the
-- embedded builder module at the build directory passed to halionbridge.exe.

local ok, result = pcall(require, "halionbridge_runtime")
if not ok then
    error(
        "halionbridge bootstrap failed while loading halionbridge_runtime.lua.\n" ..
        "Run this preset through a current halionbridge.exe build. The bridge must write " ..
        "halionbridge_runtime.lua into HALion's user script library before the preset is applied.\n" ..
        "package.path:\n" .. tostring(package.path) .. "\n" ..
        "Original error:\n" .. tostring(result)
    )
end
