-- Generic halionbridge build script runner.
--
-- This file is embedded into halionbridge.exe and written temporarily to
-- HALion's user script directory as halionbridge_builder.lua. It provides a
-- stable runner for user-authored build script modules in the build directory and
-- does not contain sample-library, preset-shape, layer, zone, articulation, or
-- converter-specific build logic. New HALion build behavior is added by editing
-- halionbridge_build.lua and adding build script modules in the build directory; the
-- preset that calls this runner can stay generic.
--
-- Build script modules listed in halionbridge_build.lua use one of these entrypoint
-- forms:
--   return function(ctx) ... end
-- or:
--   return { run = function(ctx) ... end }
--
-- The build script owns all actual HALion construction work. It may create and save
-- programs, layers, zones, scripts, marker presets, diagnostic artifacts, or any
-- other HALion object. The runner only loads build scripts, passes a ctx table,
-- reports progress, aggregates result tables, and writes the final OK/failed
-- status marker that halionbridge waits for.

-- Set to false only when manually loading the builder in HALion to inspect the
-- exported helper table without running the build script batch.
local RUN_BUILD = true

local RUNTIME_ROOT_GLOBAL = "HALIONBRIDGE_RUNTIME_ROOT"
local OUTPUT_ROOT_GLOBAL = "HALIONBRIDGE_OUTPUT_ROOT"
local RUNTIME_ROOT_ENV = "HALIONBRIDGE_PRESET_DIR"

local function normalizeDirectory(path)
    path = tostring(path or ""):gsub("\\", "/")

    if path == "" then
        return ""
    end

    if path:sub(-1) ~= "/" then
        path = path .. "/"
    end

    return path
end

local function getEnvironmentVariable(name)
    if type(os) ~= "table" or type(os.getenv) ~= "function" then
        return ""
    end

    local ok, value = pcall(os.getenv, name)
    if not ok or value == nil then
        return ""
    end

    return tostring(value)
end

local function getGlobalString(name)
    local value = _G[name]
    if value == nil then
        return ""
    end

    return tostring(value)
end

local function getSourceDirectory()
    if type(debug) ~= "table" or type(debug.getinfo) ~= "function" then
        return ""
    end

    local ok, info = pcall(debug.getinfo, 1, "S")
    if not ok or type(info) ~= "table" then
        return ""
    end

    local source = tostring(info.source or ""):gsub("^@", "")
    return source:match("(.*[/\\])") or ""
end

-- The generated halionbridge_runtime.lua module sets HALIONBRIDGE_RUNTIME_ROOT
-- before it loads this file. That value is the build directory passed on the
-- command line, so the build file, build scripts, samples, generated presets,
-- and status markers all resolve inside the selected build directory.
local scriptDir = normalizeDirectory(getGlobalString(RUNTIME_ROOT_GLOBAL))
if scriptDir == "" then
    -- Older or manually edited presets may still expose the runtime root as an
    -- environment variable. HALion's Lua runtime does not expose os.getenv in
    -- every context, so this is a compatibility fallback rather than the main
    -- loading path.
    scriptDir = normalizeDirectory(getEnvironmentVariable(RUNTIME_ROOT_ENV))
end

if scriptDir == "" then
    -- When this file itself was loaded from disk, HALion's source metadata can
    -- still locate sibling build files and build script modules. Inline preset scripts
    -- cannot use this fallback because they have no file path of their own.
    scriptDir = normalizeDirectory(getSourceDirectory())
end

local SAMPLE_ROOT = scriptDir
local OUTPUT_ROOT = normalizeDirectory(getGlobalString(OUTPUT_ROOT_GLOBAL))
if OUTPUT_ROOT == "" then
    OUTPUT_ROOT = scriptDir
end
local STATUS_OK_PATH = scriptDir .. "halionbridge_status_ok.vstpreset"
local STATUS_FAILED_PATH = scriptDir .. "halionbridge_status_failed.vstpreset"
local PROGRESS_SEQUENCE = 0
local PROGRESS_WRITE_FAILURES = 0
local MAX_PROGRESS_MESSAGE_BYTES = 88
local MAX_PROGRESS_MARKER_PATH_BYTES = 240
local TRUNCATION_SUFFIX = "..."
local BUILD_SCRIPT_TIMEOUT_MS = 600000

local function globalNumber(name)
    local value = tonumber(_G[name])
    if value == nil then
        return nil
    end

    return math.floor(value)
end

if scriptDir ~= "" then
    -- HALion Lua can load sibling files through require() when the builder's
    -- directory is appended to package.path. This avoids using io APIs that may
    -- be unavailable or sandboxed inside HALion.
    local scriptPathPrefix = scriptDir .. "?.lua;" .. scriptDir .. "?/init.lua;"
    if not package.path:find(scriptPathPrefix, 1, true) then
        package.path = scriptPathPrefix .. package.path
    end
end

local function pathJoin(root, rel)
    -- Build scripts receive simple, predictable path strings. HALion accepts forward
    -- slashes on supported platforms, and normalization here avoids repeated
    -- path separator handling in build script modules.
    root = tostring(root or ""):gsub("\\", "/")
    rel = tostring(rel or ""):gsub("\\", "/")

    if root == "" then
        return rel
    end

    if root:sub(-1) ~= "/" then
        root = root .. "/"
    end

    return root .. rel
end

local function isAbsolutePath(path)
    path = tostring(path or ""):gsub("\\", "/")
    if path:sub(1, 1) == "/" then
        return true
    end

    return path:match("^%a:/") ~= nil
end

local function outputPresetPath(path)
    path = tostring(path or ""):gsub("\\", "/")

    if OUTPUT_ROOT == "" or OUTPUT_ROOT == scriptDir then
        return path
    end

    if path:sub(1, #scriptDir) == scriptDir then
        return pathJoin(OUTPUT_ROOT, path:sub(#scriptDir + 1))
    end

    if not isAbsolutePath(path) then
        return pathJoin(OUTPUT_ROOT, path)
    end

    return path
end

local function progressLevel(value)
    -- Progress markers communicate back to the host through filenames because
    -- HALion Lua print() output is not a reliable process stdout transport in every
    -- host configuration. The marker uses a compact one-letter level so more of the
    -- filename budget remains available for the hex-encoded message.
    value = tostring(value or ""):lower():gsub("[^%w]+", "_"):gsub("_+", "_"):gsub("^_+", ""):gsub("_+$", "")

    if value == "warning" or value == "warn" then
        return "w"
    end

    if value == "error" or value == "failed" or value == "failure" then
        return "e"
    end

    return "i"
end

local function progressMessageByteBudget()
    -- Progress text is carried in the marker filename. The budget shrinks when
    -- the build directory path is long so Windows hosts do not have to delete
    -- marker paths that exceed the traditional MAX_PATH boundary.
    local markerOverheadBytes = #"hbp_000000_i_" + #".vstpreset"
    local availableBytes = MAX_PROGRESS_MARKER_PATH_BYTES - #scriptDir - markerOverheadBytes
    local encodedBudget = math.floor(availableBytes / 2)

    if encodedBudget < 0 then
        return 0
    end

    if encodedBudget > MAX_PROGRESS_MESSAGE_BYTES then
        return MAX_PROGRESS_MESSAGE_BYTES
    end

    return encodedBudget
end

local function encodeProgressMessage(message, byteBudget)
    message = tostring(message or "")
    byteBudget = byteBudget or MAX_PROGRESS_MESSAGE_BYTES

    if byteBudget <= 0 then
        message = ""
    elseif #message > byteBudget then
        local contentBudget = byteBudget - #TRUNCATION_SUFFIX
        if contentBudget > 0 then
            message = message:sub(1, contentBudget)
        else
            message = ""
        end

        while #message > 0 and string.byte(message, #message) >= 128 do
            message = message:sub(1, #message - 1)
        end

        if byteBudget >= #TRUNCATION_SUFFIX then
            message = message .. TRUNCATION_SUFFIX
        else
            message = TRUNCATION_SUFFIX:sub(1, byteBudget)
        end
    end

    return (message:gsub(".", function(char)
        return string.format("%02X", string.byte(char))
    end))
end

local function writeProgress(kind, message)
    -- The host polls these marker presets while HALion is processing. Writing
    -- them with savePreset() keeps the communication path inside HALion APIs
    -- that are known to be available to the Lua runtime.
    if scriptDir == "" then
        return false
    end

    PROGRESS_SEQUENCE = PROGRESS_SEQUENCE + 1

    local markerLayer = Layer()
    local markerLevel = progressLevel(kind)
    local markerMessage = encodeProgressMessage(message, progressMessageByteBudget())
    local markerName = string.format("hbp_%06d_%s_%s", PROGRESS_SEQUENCE, markerLevel, markerMessage)
    local markerPath = scriptDir .. markerName .. ".vstpreset"

    markerLayer:setName(markerName)
    local success = savePreset(markerPath, markerLayer, "H7")
    if success then
        PROGRESS_WRITE_FAILURES = 0
    else
        PROGRESS_WRITE_FAILURES = PROGRESS_WRITE_FAILURES + 1
        if PROGRESS_WRITE_FAILURES == 1 or PROGRESS_WRITE_FAILURES % 10 == 0 then
            print("Warning: Could not write halionbridge progress marker: " .. markerPath)
        end
    end

    return success
end

local function emit(kind, message)
    -- Every runner-facing message is sent both to HALion's print stream and to
    -- the host-readable marker channel. Build script authors can rely on ctx.log()
    -- and ctx.progress() reaching the halionbridge console even when HALion's
    -- own print stream is unavailable to the host.
    message = tostring(message or "")
    print(message)
    writeProgress(kind, message)
end

local function emitSafely(kind, message)
    local ok = pcall(emit, kind, message)
    if not ok then
        pcall(print, tostring(message or ""))
    end
end

local function extendScriptExecutionTimeout()
    -- HALion's default controller script timeout is short for build-time scripts
    -- that construct hundreds or thousands of zones. This API is valid from
    -- global controller code, unlike wait(), which HALion only allows inside
    -- callbacks.
    if type(getScriptExecTimeOut) ~= "function" or type(setScriptExecTimeOut) ~= "function" then
        return nil
    end

    local okGet, current = pcall(getScriptExecTimeOut)
    current = tonumber(current)
    if not okGet or current == nil or current >= BUILD_SCRIPT_TIMEOUT_MS then
        return nil
    end

    local okSet = pcall(setScriptExecTimeOut, BUILD_SCRIPT_TIMEOUT_MS)
    if not okSet then
        return nil
    end

    emitSafely("info", "Raised HALion script execution timeout from " .. current .. " ms to " .. BUILD_SCRIPT_TIMEOUT_MS .. " ms.")
    return current
end

local function restoreScriptExecutionTimeout(previousTimeout)
    if previousTimeout == nil or type(setScriptExecTimeOut) ~= "function" then
        return
    end

    pcall(setScriptExecTimeOut, previousTimeout)
end

local function writeStatus(status)
    -- halionbridge watches for these marker presets in the build directory.
    -- Markers are written through savePreset() because HALion scripting may not
    -- have general filesystem write access in every context.
    local ok, success, errorMessage = pcall(function()
        local markerLayer = Layer()
        local markerName = status.ok and "halionbridge_status_ok" or "halionbridge_status_failed"
        local markerPath = status.ok and STATUS_OK_PATH or STATUS_FAILED_PATH
        markerLayer:setName(markerName)

        local markerSaved = savePreset(markerPath, markerLayer, "H7")
        if markerSaved then
            return true, nil
        end

        return false, "Could not write build status marker: " .. markerPath
    end)

    if not ok then
        return false, tostring(success)
    end

    if success then
        return true, nil
    end

    return false, tostring(errorMessage)
end

local function normalizeModuleName(moduleName)
    return tostring(moduleName or ""):gsub("%.lua$", "")
end

local function moduleFileName(moduleName)
    return normalizeModuleName(moduleName) .. ".lua"
end

local function decimalWidth(value)
    value = math.floor(math.abs(tonumber(value) or 0))
    return #tostring(value)
end

local function printFileProgress(done, total)
    -- Runner-level progress is intentionally about build file entries only. Any
    -- finer-grained work, such as zones or samples, belongs to ctx.progress()
    -- calls inside the build script.
    local percent = 100
    if total > 0 then
        percent = math.floor((done * 100 / total) + 0.5)
    end

    emit("info", string.format("Completed %d/%d files (%d%%)", done, total, percent))
end

local function makeContext(moduleName)
    -- Public build script API, documented for build script authors in HALION-LUA.md.
    --
    -- ctx.script_dir, ctx.sample_root, and ctx.output_dir are strings ending in
    -- a directory separator as reported by HALion. ctx.sample_root currently
    -- equals ctx.script_dir; it is exposed separately so build script code can refer
    -- to sample roots without depending on where the runner itself lives.
    -- ctx.output_dir is either the requested --output-directory or ctx.script_dir.
    local context = {
        script_dir = scriptDir,
        sample_root = SAMPLE_ROOT,
        output_dir = OUTPUT_ROOT,
        module_name = normalizeModuleName(moduleName),
    }

    function context.path_join(root, rel)
        return pathJoin(root, rel)
    end

    function context.save_preset(path, object, presetType)
        -- Thin wrapper over HALion savePreset(). Build scripts choose what object to
        -- save and which preset type to request. "H7" is the current default for
        -- HALion 7 layer/program preset output. When --output-directory is set,
        -- ordinary build-directory-relative preset saves are redirected there while
        -- status/progress markers written by the runner remain in ctx.script_dir.
        return savePreset(outputPresetPath(path), object, presetType or "H7")
    end

    function context.log(message)
        emit("info", tostring(message or ""))
    end

    function context.progress(done, total, message)
        -- Build-script-defined progress. The runner does not interpret the unit:
        -- done/total may mean samples, zones, files, presets, phases, or any
        -- other unit that makes sense to the build script author.
        done = tonumber(done) or 0
        total = tonumber(total) or 0

        if done < 0 then done = 0 end
        if total < 0 then total = 0 end
        if total > 0 and done > total then done = total end

        -- TODO: Re-enable numeric progress when the build runner executes from
        -- a callback/onIdle path where progress markers can be observed in real
        -- time. In synchronous global/module code the host sees markers in
        -- bursts, and the prefix below is more confusing than useful.
        -- local percent = total > 0 and math.floor((done * 100 / total) + 0.5) or 100
        -- local unitWidth = math.max(decimalWidth(done), decimalWidth(total), 1)
        -- local line = string.format("Progress %" .. unitWidth .. "d/%" .. unitWidth .. "d (%3d%%)", done, total, percent)

        if message ~= nil and tostring(message) ~= "" then
            emit("info", tostring(message))
        else
            emit("info", "Progress marker")
        end
    end

    return context
end

local function invokeBuildScript(buildScript, context)
    -- Build script modules can use either documented entrypoint form. Invalid module
    -- return values become ordinary build script failures so halionbridge still gets
    -- a final failed status marker instead of waiting for a timeout.
    if type(buildScript) == "function" then
        return buildScript(context)
    end

    if type(buildScript) == "table" and type(buildScript.run) == "function" then
        return buildScript.run(context)
    end

    return {
        ok = false,
        saved = 0,
        failed = 1,
        message = "Build script must return a function or a table with run(ctx).",
    }
end

local function normalizeResult(result)
    -- Build script output is converted into the result shape expected by the batch
    -- aggregator. The normalized shape gives halionbridge consistent saved,
    -- failed, and message fields even when build script data is malformed.
    if type(result) ~= "table" then
        return {
            ok = false,
            saved = 0,
            failed = 1,
            message = "Build script did not return a result table.",
        }
    end

    return {
        ok = result.ok == true,
        saved = tonumber(result.saved) or 0,
        failed = tonumber(result.failed) or 0,
        message = tostring(result.message or ""),
    }
end

local function runBatch()
    emit("info", "Starting HALion build...")

    -- halionbridge_build.lua is only an ordered list of build script module
    -- names. Module names are not output names or preset types; build scripts
    -- decide what to build, where to save it, and how to report their own result.
    package.loaded["halionbridge_build"] = nil
    local okList, buildScriptNames = pcall(require, "halionbridge_build")

    if not okList or type(buildScriptNames) ~= "table" then
        local message = "Could not load halionbridge_build.lua or it does not return a table."
        emit("error", "Error: " .. message)
        return { ok = false, total = 0, saved = 0, failed = 1, message = message }
    end

    local total = tonumber(globalNumber("HALIONBRIDGE_BUILD_TOTAL")) or #buildScriptNames
    if total < #buildScriptNames then
        total = #buildScriptNames
    end

    local sliceStart = globalNumber("HALIONBRIDGE_BUILD_SLICE_START") or 1
    local sliceCount = globalNumber("HALIONBRIDGE_BUILD_SLICE_COUNT") or #buildScriptNames
    if sliceStart < 1 then sliceStart = 1 end
    if sliceCount < 0 then sliceCount = 0 end

    local sliceEnd = math.min(#buildScriptNames, sliceStart + sliceCount - 1)
    local totalSaved = 0
    local totalFailed = 0
    local scriptsProcessed = 0
    printFileProgress(sliceStart - 1, total)

    for index = sliceStart, sliceEnd do
        local moduleName = buildScriptNames[index]
        -- Repeated HALion runs in the same scripting session reload updated
        -- build script source files because package.loaded is cleared per module.
        moduleName = normalizeModuleName(moduleName)
        emit("info", string.format("Processing %d/%d: %s", index, total, moduleFileName(moduleName)))
        scriptsProcessed = scriptsProcessed + 1

        package.loaded[moduleName] = nil
        local okModule, buildScript = pcall(require, moduleName)

        if not okModule then
            local message = "Could not load build script module " .. moduleFileName(moduleName) .. ": " .. tostring(buildScript)
            emit("error", "Error: " .. message)
            totalFailed = totalFailed + 1
        else
            local context = makeContext(moduleName)
            local okRun, result = pcall(invokeBuildScript, buildScript, context)

            if not okRun then
                local message = "Build script " .. moduleFileName(moduleName) .. " failed: " .. tostring(result)
                emit("error", "Error: " .. message)
                totalFailed = totalFailed + 1
            else
                result = normalizeResult(result)
                totalSaved = totalSaved + result.saved
                totalFailed = totalFailed + result.failed

                if result.ok and result.failed == 0 then
                    emit("info", "Success: " .. moduleFileName(moduleName) .. " - " .. result.message)
                else
                    if result.failed == 0 then
                        totalFailed = totalFailed + 1
                    end
                    emit("error", "Failed: " .. moduleFileName(moduleName) .. " - " .. result.message)
                end
            end
        end

        -- File-level progress is reported after each build script finishes, so
        -- the printed percentage always reflects completed build file entries.
        printFileProgress(index, total)
    end

    local ok = totalFailed == 0
    local message = ok
        and ("Build chunk complete. Scripts processed: " .. scriptsProcessed .. ", presets saved: " .. totalSaved)
        or ("Build chunk completed with failures. Scripts processed: " .. scriptsProcessed .. ", presets saved: " .. totalSaved .. ", failed: " .. totalFailed)

    emit(ok and "info" or "error", message)
    return { ok = ok, total = total, saved = totalSaved, failed = totalFailed, message = message }
end

if RUN_BUILD then
    -- A status marker is written even when the runner itself throws. This gives
    -- halionbridge a concrete completion signal instead of relying only on the
    -- timeout path.
    local previousScriptTimeout = extendScriptExecutionTimeout()
    local ok, status = pcall(runBatch)
    if not ok then
        status = {
            ok = false,
            total = 0,
            saved = 0,
            failed = 1,
            message = "Unhandled Lua error: " .. tostring(status),
        }
        emitSafely("error", "Error: " .. status.message)
    end

    status = normalizeResult(status)
    if status.message == "" then
        status.message = status.ok and "Batch complete." or "Batch failed."
    end

    local statusWritten, statusError = writeStatus(status)
    restoreScriptExecutionTimeout(previousScriptTimeout)

    if not statusWritten then
        emitSafely("error", "Error: " .. tostring(statusError))
    end
end
