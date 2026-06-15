-- HALion preset sample-path remapper.
--
-- halionbridge writes this module temporarily into HALion's user script library
-- and loads it through the embedded bootstrap preset. It opens copied
-- .vstpreset files from a temporary user-preset folder, rewrites matching
-- SampleOsc.Filename prefixes in sample zones, asks HALion to save the copied
-- preset again, and reports completion through the same marker-preset transport
-- used by the generic build runner.

local RUNTIME_ROOT = tostring(HALIONBRIDGE_PRESET_REMAP_ROOT or ""):gsub("\\", "/")
local LIST_FILE = tostring(HALIONBRIDGE_PRESET_REMAP_LIST or ""):gsub("\\", "/")
local OLD_ROOT = tostring(HALIONBRIDGE_PRESET_REMAP_OLD_ROOT or ""):gsub("\\", "/")
local NEW_ROOT = tostring(HALIONBRIDGE_PRESET_REMAP_NEW_ROOT or ""):gsub("\\", "/")
local PLUGIN_CODE = tostring(HALIONBRIDGE_PRESET_REMAP_PLUGIN_CODE or "H7")

if RUNTIME_ROOT ~= "" and RUNTIME_ROOT:sub(-1) ~= "/" then RUNTIME_ROOT = RUNTIME_ROOT .. "/" end
if OLD_ROOT ~= "" and OLD_ROOT:sub(-1) ~= "/" then OLD_ROOT = OLD_ROOT .. "/" end
if NEW_ROOT ~= "" and NEW_ROOT:sub(-1) ~= "/" then NEW_ROOT = NEW_ROOT .. "/" end

local STATUS_OK_PATH = RUNTIME_ROOT .. "halionbridge_status_ok.vstpreset"
local STATUS_FAILED_PATH = RUNTIME_ROOT .. "halionbridge_status_failed.vstpreset"
local PROGRESS_SEQUENCE = 0
local PROGRESS_WRITE_FAILURES = 0
local MAX_PROGRESS_MESSAGE_BYTES = 88
local MAX_PROGRESS_MARKER_PATH_BYTES = 240
local TRUNCATION_SUFFIX = "..."
local SCRIPT_TIMEOUT_MS = 600000

local function pathJoin(root, rel)
    root = tostring(root or ""):gsub("\\", "/")
    rel = tostring(rel or ""):gsub("\\", "/")
    if root ~= "" and root:sub(-1) ~= "/" then root = root .. "/" end
    return root .. rel
end

local function trimLine(line)
    line = tostring(line or ""):gsub("^\239\187\191", "")
    line = line:gsub("^%s+", ""):gsub("%s+$", "")
    return line
end

local function progressLevel(value)
    value = tostring(value or ""):lower():gsub("[^%w]+", "_"):gsub("_+", "_"):gsub("^_+", ""):gsub("_+$", "")
    if value == "warning" or value == "warn" then return "w" end
    if value == "error" or value == "failed" or value == "failure" then return "e" end
    return "i"
end

local function progressMessageByteBudget()
    local markerOverheadBytes = #"hbp_000000_i_" + #".vstpreset"
    local availableBytes = MAX_PROGRESS_MARKER_PATH_BYTES - #RUNTIME_ROOT - markerOverheadBytes
    local encodedBudget = math.floor(availableBytes / 2)
    if encodedBudget < 0 then return 0 end
    if encodedBudget > MAX_PROGRESS_MESSAGE_BYTES then return MAX_PROGRESS_MESSAGE_BYTES end
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
    if RUNTIME_ROOT == "" then return false end

    PROGRESS_SEQUENCE = PROGRESS_SEQUENCE + 1

    local markerLayer = Layer()
    local markerLevel = progressLevel(kind)
    local markerMessage = encodeProgressMessage(message, progressMessageByteBudget())
    local markerName = string.format("hbp_%06d_%s_%s", PROGRESS_SEQUENCE, markerLevel, markerMessage)
    local markerPath = RUNTIME_ROOT .. markerName .. ".vstpreset"

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
    if type(getScriptExecTimeOut) ~= "function" or type(setScriptExecTimeOut) ~= "function" then
        return nil
    end

    local okGet, current = pcall(getScriptExecTimeOut)
    current = tonumber(current)
    if not okGet or current == nil or current >= SCRIPT_TIMEOUT_MS then
        return nil
    end

    local okSet = pcall(setScriptExecTimeOut, SCRIPT_TIMEOUT_MS)
    if not okSet then
        return nil
    end

    emitSafely("info", "Raised HALion script execution timeout from " .. current .. " ms to " .. SCRIPT_TIMEOUT_MS .. " ms.")
    return current
end

local function restoreScriptExecutionTimeout(previousTimeout)
    if previousTimeout == nil or type(setScriptExecTimeOut) ~= "function" then
        return
    end

    pcall(setScriptExecTimeOut, previousTimeout)
end

local function writeStatus(ok)
    local statusOk, success, errorMessage = pcall(function()
        local markerLayer = Layer()
        local markerName = ok and "halionbridge_status_ok" or "halionbridge_status_failed"
        local markerPath = ok and STATUS_OK_PATH or STATUS_FAILED_PATH
        markerLayer:setName(markerName)

        if savePreset(markerPath, markerLayer, "H7") then
            return true, nil
        end

        return false, "Could not write preset-remap status marker: " .. markerPath
    end)

    if not statusOk then
        return false, tostring(success)
    end

    if success then
        return true, nil
    end

    return false, tostring(errorMessage)
end

local function readList()
    local list = {}
    for line in io.lines(LIST_FILE) do
        line = trimLine(line)
        if line ~= "" then table.insert(list, line) end
    end
    return list
end

local function remapPreset(relativePath)
    local presetPath = pathJoin(RUNTIME_ROOT, relativePath)
    emit("info", "Processing " .. relativePath)

    local okLoad, layer = pcall(loadPreset, presetPath)
    if not okLoad or not layer then
        return false, 0, "Could not load " .. relativePath .. ": " .. tostring(layer)
    end

    local okZones, zones = pcall(function()
        return layer:findZones(true)
    end)
    if not okZones or type(zones) ~= "table" then
        return false, 0, "Could not enumerate zones in " .. relativePath .. ": " .. tostring(zones)
    end

    local changed = 0
    for _, zone in ipairs(zones) do
        local okGet, samplePath = pcall(function()
            return zone:getParameter("SampleOsc.Filename")
        end)

        if okGet and type(samplePath) == "string" then
            local normalized = samplePath:gsub("\\", "/")
            if OLD_ROOT ~= "" and normalized:sub(1, #OLD_ROOT) == OLD_ROOT then
                local replacement = NEW_ROOT .. normalized:sub(#OLD_ROOT + 1)
                local okSet, setError = pcall(function()
                    zone:setParameter("SampleOsc.Filename", replacement)
                end)
                if not okSet then
                    return false, changed, "Could not set sample path in " .. relativePath .. ": " .. tostring(setError)
                end
                changed = changed + 1
            end
        end
    end

    if changed == 0 then
        emit("info", "No matching sample paths: " .. relativePath)
        return true, 0, ""
    end

    local okSave, saved = pcall(savePreset, presetPath, layer, PLUGIN_CODE)
    if not okSave or not saved then
        return false, changed, "Could not save " .. relativePath .. ": " .. tostring(saved)
    end

    emit("info", "Remapped " .. relativePath .. " (" .. changed .. " zone(s))")
    return true, changed, ""
end

local function runRemap()
    if RUNTIME_ROOT == "" then
        return { ok = false, total = 0, changed = 0, failed = 1, message = "Missing remap runtime root." }
    end
    if LIST_FILE == "" then
        return { ok = false, total = 0, changed = 0, failed = 1, message = "Missing remap list file." }
    end
    if OLD_ROOT == "" or NEW_ROOT == "" then
        return { ok = false, total = 0, changed = 0, failed = 1, message = "Both old and new sample roots are required." }
    end

    emit("info", "Starting HALion preset remap...")

    local presets = readList()
    local total = #presets
    local changedPresets = 0
    local changedZones = 0
    local failed = 0

    for index, relativePath in ipairs(presets) do
        emit("info", string.format("Remapping %d/%d: %s", index, total, relativePath))
        local ok, changed, message = remapPreset(relativePath)
        if ok then
            if changed > 0 then changedPresets = changedPresets + 1 end
            changedZones = changedZones + changed
        else
            failed = failed + 1
            emit("error", "Error: " .. tostring(message))
        end
    end

    local ok = failed == 0
    local message = ok
        and string.format("Preset remap complete. Presets: %d, changed: %d, zones changed: %d.", total, changedPresets, changedZones)
        or string.format("Preset remap completed with failures. Presets: %d, changed: %d, failed: %d.", total, changedPresets, failed)

    emit(ok and "info" or "error", message)
    return { ok = ok, total = total, changed = changedPresets, failed = failed, message = message }
end

local previousScriptTimeout = extendScriptExecutionTimeout()
local ok, status = pcall(runRemap)
if not ok then
    status = {
        ok = false,
        total = 0,
        changed = 0,
        failed = 1,
        message = "Unhandled Lua error: " .. tostring(status),
    }
    emitSafely("error", "Error: " .. status.message)
end

if type(status) ~= "table" then
    status = {
        ok = false,
        total = 0,
        changed = 0,
        failed = 1,
        message = "Preset remap did not return a result table.",
    }
end

local statusWritten, statusError = writeStatus(status.ok == true and tonumber(status.failed or 0) == 0)
restoreScriptExecutionTimeout(previousScriptTimeout)

if not statusWritten then
    emitSafely("error", "Error: " .. tostring(statusError))
end
