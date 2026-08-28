-- HALion-side VSTPreset structure and parameter inspector.
--
-- halionbridge writes this module temporarily into HALion's user script
-- library and supplies a generated runtime module containing the requested
-- preset paths. Every preset is loaded through HALion's loadPreset() API; this
-- intentionally does not attempt to decode HALion's opaque preset state in the
-- native host. The resulting versioned JSON report is written below
-- Documents/Steinberg and published by the native host after validation.

local RUNTIME_ROOT = tostring(HALIONBRIDGE_PRESET_INSPECTION_ROOT or ""):gsub("\\", "/")
local REPORT_PATH = tostring(HALIONBRIDGE_PRESET_INSPECTION_REPORT or ""):gsub("\\", "/")
local PRESETS = HALIONBRIDGE_PRESET_INSPECTION_PRESETS or {}

if RUNTIME_ROOT ~= "" and RUNTIME_ROOT:sub(-1) ~= "/" then RUNTIME_ROOT = RUNTIME_ROOT .. "/" end

local STATUS_OK_PATH = RUNTIME_ROOT .. "halionbridge_status_ok.vstpreset"
local STATUS_FAILED_PATH = RUNTIME_ROOT .. "halionbridge_status_failed.vstpreset"
local SCRIPT_TIMEOUT_MS = 600000
local MAX_VALUE_DEPTH = 12
local MAX_VALUE_ENTRIES = 10000
local PARAMETER_DEFINITION_FIELDS = {
    "automatable", "default", "id", "longName", "max", "min", "name",
    "persistent", "readOnly", "type", "unit", "writeAlways",
}

local function jsonString(value)
    value = tostring(value or "")
    return '"' .. value:gsub('[%z\1-\31\\"]', function(char)
        local escapes = {
            ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
            ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
        }
        return escapes[char] or string.format("\\u%04X", string.byte(char))
    end) .. '"'
end

local function encodeNumber(value)
    if value ~= value then return '{"non_finite":"nan"}' end
    if value == math.huge then return '{"non_finite":"positive_infinity"}' end
    if value == -math.huge then return '{"non_finite":"negative_infinity"}' end
    return string.format("%.17g", value)
end

local function tableEntries(value)
    local entries = {}
    local ok, iterationError = pcall(function()
        for key, entryValue in pairs(value) do
            if type(key) == "string" or type(key) == "number" then
                entries[#entries + 1] = { key = key, value = entryValue }
                if #entries > MAX_VALUE_ENTRIES then
                    error("table exceeds " .. MAX_VALUE_ENTRIES .. " entries")
                end
            end
        end
    end)
    if not ok then return nil, tostring(iterationError) end
    return entries, nil
end

local function isArray(entries)
    local maximum = 0
    for _, entry in ipairs(entries) do
        if type(entry.key) ~= "number" or entry.key < 1 or entry.key % 1 ~= 0 then return false, 0 end
        if entry.key > maximum then maximum = entry.key end
    end
    if maximum ~= #entries then return false, 0 end
    return true, maximum
end

local function encodeValue(value, depth, ancestors)
    local valueType = type(value)
    if value == nil then return "null" end
    if valueType == "boolean" then return value and "true" or "false" end
    if valueType == "number" then return encodeNumber(value) end
    if valueType == "string" then return jsonString(value) end
    if valueType ~= "table" then
        return '{"unsupported_lua_type":' .. jsonString(valueType) .. '}'
    end
    if depth >= MAX_VALUE_DEPTH then return '{"truncated":"maximum_depth"}' end
    if ancestors[value] then return '{"truncated":"cycle"}' end

    ancestors[value] = true
    local entries, entryError = tableEntries(value)
    if not entries then
        ancestors[value] = nil
        return '{"table_read_error":' .. jsonString(entryError) .. '}'
    end

    local array, maximum = isArray(entries)
    local output = {}
    if array then
        local byIndex = {}
        for _, entry in ipairs(entries) do byIndex[entry.key] = entry.value end
        for index = 1, maximum do
            output[#output + 1] = encodeValue(byIndex[index], depth + 1, ancestors)
        end
        ancestors[value] = nil
        return "[" .. table.concat(output, ",") .. "]"
    end

    table.sort(entries, function(left, right)
        local leftKey = type(left.key) .. ":" .. tostring(left.key)
        local rightKey = type(right.key) .. ":" .. tostring(right.key)
        return leftKey < rightKey
    end)
    for _, entry in ipairs(entries) do
        output[#output + 1] = jsonString(tostring(entry.key)) .. ":" .. encodeValue(entry.value, depth + 1, ancestors)
    end
    ancestors[value] = nil
    return "{" .. table.concat(output, ",") .. "}"
end

local function jsonEncode(value)
    return encodeValue(value, 0, {})
end

local function safeField(object, name)
    local ok, value = pcall(function() return object[name] end)
    if ok then return value, nil end
    return nil, tostring(value)
end

local function safeElementIdentity(element)
    local elementType = safeField(element, "type")
    local name = safeField(element, "name")
    return tostring(elementType or "Element") .. ":" .. tostring(name or "")
end

local function definitionSortKey(definition)
    local name = safeField(definition, "name")
    local id = safeField(definition, "id")
    return tostring(name or "") .. "\0" .. tostring(id or "")
end

local function parameterDefinitions(element)
    local ok, definitions = pcall(function() return element.parameterDefinitions end)
    if not ok or type(definitions) ~= "table" then
        return {}, ok and "parameterDefinitions is not a table" or tostring(definitions)
    end

    local copy = {}
    for _, definition in ipairs(definitions) do copy[#copy + 1] = definition end
    table.sort(copy, function(left, right) return definitionSortKey(left) < definitionSortKey(right) end)
    return copy, nil
end

local function inspectParameter(owner, definition)
    local record = { definition = {} }
    for _, field in ipairs(PARAMETER_DEFINITION_FIELDS) do
        local value, fieldError = safeField(definition, field)
        if fieldError then
            record.definition[field .. "_read_error"] = fieldError
        elseif value ~= nil then
            record.definition[field] = value
        end
    end

    local parameterId = record.definition.id
    if parameterId == nil then parameterId = record.definition.name end
    local okValue, value = pcall(owner.getParameter, owner, parameterId)
    if okValue then
        record.value = value
        if type(value) == "number" or type(value) == "string" then
            local okDisplay, display = pcall(definition.getDisplayString, definition, value)
            if okDisplay and type(display) == "string" then record.display = display end
        end
    else
        record.value_read_error = tostring(value)
    end
    return record
end

local function inspectParameters(element)
    local definitions, definitionsError = parameterDefinitions(element)
    local parameters = {}
    for _, definition in ipairs(definitions) do
        parameters[#parameters + 1] = inspectParameter(element, definition)
    end
    return parameters, definitionsError
end

local function inspectModulationRows(zone)
    local rows = {}
    for rowIndex = 1, 32 do
        local okRow, row = pcall(zone.getModulationMatrixRow, zone, rowIndex)
        if okRow and row then
            local parameters, definitionsError = inspectParameters(row)
            local record = { index = rowIndex, parameters = parameters }
            if definitionsError then record.parameter_definitions_error = definitionsError end

            local okSource2, sourceType, sourceObject, sourceIndex = pcall(row.getSource2, row)
            if okSource2 then
                record.source2 = {
                    source_type = sourceType,
                    source_element = sourceObject and safeElementIdentity(sourceObject) or nil,
                    source_index = sourceIndex,
                }
            else
                record.source2_read_error = tostring(sourceType)
            end
            rows[#rows + 1] = record
        else
            rows[#rows + 1] = { index = rowIndex, row_read_error = tostring(row) }
        end
    end
    return rows
end

local function addUniqueChildren(destination, seen, candidates, excludedType)
    if type(candidates) ~= "table" then return end
    for _, child in ipairs(candidates) do
        local childType = child and safeField(child, "type") or nil
        if child and childType ~= excludedType and not seen[child] then
            seen[child] = true
            destination[#destination + 1] = child
        end
    end
end

local function callChildFinder(element, methodName, argument)
    local method = safeField(element, methodName)
    if type(method) ~= "function" then return nil end
    local ok, children
    if argument == nil then
        ok, children = pcall(method, element)
    else
        ok, children = pcall(method, element, argument)
    end
    if ok and type(children) == "table" then return children end
    return nil
end

local function directChildren(element, elementType)
    local children = {}
    local seen = {}
    if elementType == "Instance" then
        addUniqueChildren(children, seen, callChildFinder(element, "findSlots", nil))
        addUniqueChildren(children, seen, callChildFinder(element, "findBusses", nil))
    elseif elementType == "Slot" then
        local okProgram, program = pcall(element.getProgram, element)
        if okProgram and program then addUniqueChildren(children, seen, { program }) end
    elseif elementType == "Program" then
        addUniqueChildren(children, seen, callChildFinder(element, "findChildren", false))
    elseif elementType == "Layer" then
        addUniqueChildren(children, seen, callChildFinder(element, "findZones", false))
        -- HALion can return fresh proxy objects for the same Zone through
        -- findZones() and findChildren(), so Lua table identity is not a safe
        -- de-duplication key. Keep Zones from findZones() and use
        -- findChildren() only for the remaining direct element types.
        addUniqueChildren(children, seen, callChildFinder(element, "findChildren", false), "Zone")
    elseif elementType == "Bus" then
        addUniqueChildren(children, seen, callChildFinder(element, "findEffects", nil))
    end
    return children
end

local function inspectElement(element, ancestors, depth)
    if ancestors[element] then
        return { traversal_error = "element cycle", depth = depth }
    end
    ancestors[element] = true

    local elementType, typeError = safeField(element, "type")
    local name, nameError = safeField(element, "name")
    local record = {
        depth = depth,
        name = name,
        type = elementType,
        parameters = {},
        children = {},
    }
    if typeError then record.type_read_error = typeError end
    if nameError then record.name_read_error = nameError end

    local moduleType = safeField(element, "moduleType")
    if moduleType ~= nil then record.module_type = moduleType end
    local isAuxBus = safeField(element, "isAuxBus")
    if isAuxBus ~= nil then record.is_aux_bus = isAuxBus end

    record.parameters, record.parameter_definitions_error = inspectParameters(element)

    if elementType == "Layer" or elementType == "Zone" then
        local okBus, bus = pcall(element.getOutputBus, element)
        if okBus and bus then record.output_bus = safeElementIdentity(bus) end
    end

    if elementType == "Zone" then
        record.mapping = {}
        for _, field in ipairs({ "keyLow", "keyHigh", "rootKey", "velLow", "velHigh" }) do
            local value, fieldError = safeField(element, field)
            if fieldError then
                record.mapping[field .. "_read_error"] = fieldError
            elseif value ~= nil then
                record.mapping[field] = value
            end
        end
        record.modulation_rows = inspectModulationRows(element)
    end

    local children = directChildren(element, elementType)
    for _, child in ipairs(children) do
        record.children[#record.children + 1] = inspectElement(child, ancestors, depth + 1)
    end

    ancestors[element] = nil
    return record
end

local function writeProgress(message)
    pcall(print, tostring(message or ""))
end

local function extendScriptExecutionTimeout()
    if type(getScriptExecTimeOut) ~= "function" or type(setScriptExecTimeOut) ~= "function" then return nil end
    local okGet, current = pcall(getScriptExecTimeOut)
    current = tonumber(current)
    if not okGet or current == nil or current >= SCRIPT_TIMEOUT_MS then return nil end
    if not pcall(setScriptExecTimeOut, SCRIPT_TIMEOUT_MS) then return nil end
    return current
end

local function restoreScriptExecutionTimeout(previousTimeout)
    if previousTimeout ~= nil and type(setScriptExecTimeOut) == "function" then
        pcall(setScriptExecTimeOut, previousTimeout)
    end
end

local function writeStatus(ok)
    local markerLayer = Layer()
    local markerName = ok and "halionbridge_status_ok" or "halionbridge_status_failed"
    local markerPath = ok and STATUS_OK_PATH or STATUS_FAILED_PATH
    markerLayer:setName(markerName)
    return savePreset(markerPath, markerLayer, "H7") == true
end

local function inspectPreset(specification)
    local relativePath = tostring(specification.relative or "")
    local sourcePath = tostring(specification.source or "")
    local record = { path = relativePath, ok = false, errors = {} }

    local okLoad, root = pcall(loadPreset, sourcePath)
    if not okLoad or not root then
        record.errors[#record.errors + 1] = "Could not load preset through HALion: " .. tostring(root)
        return record
    end

    local okInspect, result = pcall(inspectElement, root, {}, 0)
    if not okInspect then
        record.errors[#record.errors + 1] = "Could not inspect loaded preset: " .. tostring(result)
        return record
    end

    record.root = result
    record.ok = true
    return record
end

local function writeReport(report)
    local okOpen, fileOrError, openError = pcall(io.open, REPORT_PATH, "wb")
    if not okOpen or not fileOrError then
        return false, "Could not open inspection report: " .. tostring(openError or fileOrError)
    end

    local file = fileOrError
    local okWrite, writeResult, writeError = pcall(file.write, file, jsonEncode(report))
    local okClose, closeResult = pcall(file.close, file)
    if not okWrite or not writeResult then return false, "Could not write inspection report: " .. tostring(writeError or writeResult) end
    if not okClose or closeResult == false then return false, "Could not close inspection report: " .. tostring(closeResult) end
    return true, nil
end

local function runInspection()
    local report = {
        format = "halionbridge-vstpreset-inspection",
        format_version = 1,
        presets = {},
        summary = { total = #PRESETS, inspected = 0, failed = 0 },
    }

    if RUNTIME_ROOT == "" or REPORT_PATH == "" or type(PRESETS) ~= "table" then
        return report, false, "Preset-inspection runtime configuration is incomplete."
    end

    writeProgress("Starting HALion VSTPreset inspection...")
    for index, specification in ipairs(PRESETS) do
        local relativePath = tostring(specification.relative or "")
        writeProgress(string.format("Inspecting %d/%d: %s", index, #PRESETS, relativePath))
        local record = inspectPreset(specification)
        report.presets[#report.presets + 1] = record
        if record.ok then
            report.summary.inspected = report.summary.inspected + 1
        else
            report.summary.failed = report.summary.failed + 1
            writeProgress("Error: " .. tostring(record.errors[1] or relativePath))
        end
    end

    local ok = report.summary.failed == 0
    return report, ok, nil
end

local previousTimeout = extendScriptExecutionTimeout()
local okRun, report, inspectionOk, fatalError = pcall(runInspection)
if not okRun then
    fatalError = "Unhandled inspection error: " .. tostring(report)
    report = {
        format = "halionbridge-vstpreset-inspection",
        format_version = 1,
        presets = {},
        summary = { total = #PRESETS, inspected = 0, failed = #PRESETS },
    }
    inspectionOk = false
end

local reportWritten, reportError = writeReport(report)
if not reportWritten then
    writeProgress("Error: " .. tostring(reportError))
    inspectionOk = false
end
if fatalError then writeProgress("Error: " .. tostring(fatalError)) end

local okStatus, statusWritten = pcall(writeStatus, inspectionOk == true and reportWritten == true)
restoreScriptExecutionTimeout(previousTimeout)
if not okStatus or not statusWritten then
    writeProgress("Error: Could not write preset-inspection status marker.")
end

return report
