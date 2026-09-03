-- HALion-side macro-page injection worker used by `inject-macro-page`.
--
-- HALionScript does not expose the macro page attached to a Program as an
-- object that can be copied. The macro-bearing donor Program must therefore
-- remain the saved root. halionbridge replaces its child graph and every
-- persistent, inspectable Program-root setting with state from the source.
-- The donor supplies only the opaque macro-page attachment.

local DONOR_PATH = tostring(HALIONBRIDGE_MACRO_PAGE_DONOR or "")
local OUTPUT_ROOT = tostring(HALIONBRIDGE_MACRO_PAGE_OUTPUT_ROOT or ""):gsub("\\", "/")
local RECEIPT_PATH = tostring(HALIONBRIDGE_MACRO_PAGE_RECEIPT or "")
local TOKEN = tostring(HALIONBRIDGE_MACRO_PAGE_TOKEN or "")
local PREFLIGHT = HALIONBRIDGE_MACRO_PAGE_PREFLIGHT == true
local VALIDATE_ONLY = HALIONBRIDGE_MACRO_PAGE_VALIDATE_ONLY == true
local FAIL_FAST = HALIONBRIDGE_MACRO_PAGE_FAIL_FAST == true
local PRESETS = HALIONBRIDGE_MACRO_PAGE_PRESETS or {}

local QUICK_CONTROL_COUNT = 11
local FLOAT_TOLERANCE = 1e-9
local MAX_VALUE_DEPTH = 64
local MAX_VALUE_NODES = 100000
local MAX_TREE_NODES = 100000

local function pathJoin(root, relative)
    root = tostring(root or ""):gsub("\\", "/"):gsub("/+$", "")
    relative = tostring(relative or ""):gsub("\\", "/"):gsub("^/+", "")
    if root == "" then return relative end
    if relative == "" then return root end
    return root .. "/" .. relative
end

local function elementIdentity(element)
    if not element then return "<nil>" end
    local okType, elementType = pcall(function() return element.type end)
    local okName, elementName = pcall(function() return element.name end)
    return string.format(
        "%s '%s'",
        okType and tostring(elementType) or "Element",
        okName and tostring(elementName) or ""
    )
end

local function load(path, label)
    local ok, result = pcall(loadPreset, path)
    if not ok or not result then
        error(string.format("Could not load %s preset %s: %s", label, path, tostring(result)))
    end
    return result
end

local function requireType(element, expectedType, label)
    local ok, actualType = pcall(function() return element.type end)
    if not ok or actualType ~= expectedType then
        error(string.format("%s must load as a %s, but loaded as %s.", label, expectedType, elementIdentity(element)))
    end
end

local function invoke(element, methodName, label, ...)
    local okMethod, method = pcall(function() return element[methodName] end)
    if not okMethod or type(method) ~= "function" then
        error(string.format("%s does not expose %s().", label, methodName))
    end
    local ok, result = pcall(method, element, ...)
    if not ok then
        error(string.format("%s failed in %s(): %s", label, methodName, tostring(result)))
    end
    return result
end

local function directChildren(element, label)
    local children = invoke(element, "findChildren", "Could not enumerate direct children of " .. label, false)
    if type(children) ~= "table" then
        error(string.format("Direct children of %s were not returned as a table.", label))
    end
    return children
end

local function directChildSignature(element, label)
    local signature = {}
    for index, child in ipairs(directChildren(element, label)) do
        local okType, childType = pcall(function() return child.type end)
        local okName, childName = pcall(function() return child.name end)
        if not okType or not okName then
            error(string.format("Could not identify direct child %d of %s.", index, label))
        end
        signature[index] = tostring(childType) .. "\31" .. tostring(childName)
    end
    return signature
end

local function signaturesMatch(expected, actual)
    if #expected ~= #actual then return false end
    for index, value in ipairs(expected) do
        if actual[index] ~= value then return false end
    end
    return true
end

local function cloneSupportedValue(value, label, state, depth)
    state = state or { nodes = 0, active = {} }
    depth = depth or 0
    state.nodes = state.nodes + 1
    if state.nodes > MAX_VALUE_NODES then
        error(label .. " exceeds the supported structured-value size limit.")
    end

    local valueType = type(value)
    if valueType == "nil" or valueType == "boolean" or valueType == "string" then
        return value
    end
    if valueType == "number" then
        if value ~= value or value == math.huge or value == -math.huge then
            error(label .. " contains a non-finite number.")
        end
        return value
    end
    if valueType ~= "table" then
        error(string.format("%s has unsupported Lua value type '%s'.", label, valueType))
    end
    if depth >= MAX_VALUE_DEPTH then
        error(label .. " exceeds the supported structured-value depth limit.")
    end
    if state.active[value] then
        error(label .. " contains a cyclic table.")
    end

    state.active[value] = true
    local copy = {}
    for key, child in pairs(value) do
        local keyType = type(key)
        if keyType ~= "boolean" and keyType ~= "number" and keyType ~= "string" then
            error(string.format("%s contains unsupported table key type '%s'.", label, keyType))
        end
        local copiedKey = cloneSupportedValue(key, label .. " key", state, depth + 1)
        copy[copiedKey] = cloneSupportedValue(child, label .. "[" .. tostring(key) .. "]", state, depth + 1)
    end
    state.active[value] = nil
    return copy
end

local function valuesMatch(expected, actual, path, depth)
    path = path or "value"
    depth = depth or 0
    if type(expected) ~= type(actual) then
        return false, path .. " has a different Lua type"
    end
    if type(expected) == "number" then
        if math.abs(expected - actual) > FLOAT_TOLERANCE then
            return false, path .. " differs"
        end
        return true
    end
    if type(expected) ~= "table" then
        if expected ~= actual then return false, path .. " differs" end
        return true
    end
    if depth >= MAX_VALUE_DEPTH then return false, path .. " exceeds the comparison depth limit" end

    local expectedCount = 0
    for key, expectedChild in pairs(expected) do
        expectedCount = expectedCount + 1
        if actual[key] == nil and expectedChild ~= nil then
            return false, path .. " is missing key " .. tostring(key)
        end
        local equal, reason = valuesMatch(expectedChild, actual[key], path .. "[" .. tostring(key) .. "]", depth + 1)
        if not equal then return false, reason end
    end
    local actualCount = 0
    for _ in pairs(actual) do actualCount = actualCount + 1 end
    if expectedCount ~= actualCount then return false, path .. " has a different key count" end
    return true
end

local function readDefinitionField(definition, field, label)
    local ok, value = pcall(function() return definition[field] end)
    if not ok then error(string.format("Could not read %s.%s: %s", label, field, tostring(value))) end
    return value
end

local function snapshotRootParameters(program, label)
    local ok, definitions = pcall(function() return program.parameterDefinitions end)
    if not ok or type(definitions) ~= "table" then
        error(string.format("Could not enumerate Program-root parameters of %s: %s", label, tostring(definitions)))
    end

    local snapshot = { entries = {}, byName = {} }
    for index, definition in ipairs(definitions) do
        local definitionLabel = string.format("%s parameter definition %d", label, index)
        local persistent = readDefinitionField(definition, "persistent", definitionLabel)
        if persistent == true then
            local name = readDefinitionField(definition, "name", definitionLabel)
            local id = readDefinitionField(definition, "id", definitionLabel)
            local parameterType = readDefinitionField(definition, "type", definitionLabel)
            local readOnly = readDefinitionField(definition, "readOnly", definitionLabel)
            if type(name) ~= "string" or name == "" or (type(id) ~= "number" and type(id) ~= "string") or
                type(parameterType) ~= "string" or type(readOnly) ~= "boolean" then
                error(definitionLabel .. " has an unsupported schema.")
            end
            if snapshot.byName[name] then
                error(string.format("%s exposes duplicate persistent Program-root parameter '%s'.", label, name))
            end
            local okValue, value = pcall(program.getParameter, program, name)
            if not okValue then
                error(string.format("Could not read %s Program-root parameter '%s': %s", label, name, tostring(value)))
            end
            local entry = {
                name = name,
                id = id,
                type = parameterType,
                readOnly = readOnly,
                value = cloneSupportedValue(value, label .. " Program-root parameter '" .. name .. "'"),
            }
            snapshot.entries[#snapshot.entries + 1] = entry
            snapshot.byName[name] = entry
        end
    end
    return snapshot
end

local function requireMatchingParameterSchemas(expected, actual, label)
    if #expected.entries ~= #actual.entries then
        error(string.format("%s persistent Program-root parameter count differs (%d versus %d).", label,
            #expected.entries, #actual.entries))
    end
    for _, expectedEntry in ipairs(expected.entries) do
        local actualEntry = actual.byName[expectedEntry.name]
        if not actualEntry then
            error(string.format("%s is missing persistent Program-root parameter '%s'.", label, expectedEntry.name))
        end
        if expectedEntry.id ~= actualEntry.id or expectedEntry.type ~= actualEntry.type or
            expectedEntry.readOnly ~= actualEntry.readOnly then
            error(string.format("%s Program-root parameter '%s' has a different ID, type, or read-only state.",
                label, expectedEntry.name))
        end
    end
end

local function copyRootParameters(sourceSnapshot, donor, donorSnapshot)
    requireMatchingParameterSchemas(sourceSnapshot, donorSnapshot, "Macro donor")
    for _, entry in ipairs(sourceSnapshot.entries) do
        if not entry.readOnly then
            local value = cloneSupportedValue(entry.value, "Source Program-root parameter '" .. entry.name .. "'")
            local ok, setError = pcall(donor.setParameter, donor, entry.name, value)
            if not ok then
                error(string.format("Could not copy Program-root parameter '%s': %s", entry.name, tostring(setError)))
            end
        end
    end
end

local function requireMatchingRootParameters(expected, program, label)
    local actual = snapshotRootParameters(program, label)
    requireMatchingParameterSchemas(expected, actual, label)
    for _, expectedEntry in ipairs(expected.entries) do
        local equal, reason = valuesMatch(expectedEntry.value, actual.byName[expectedEntry.name].value,
            "Program-root parameter '" .. expectedEntry.name .. "'")
        if not equal then error(label .. " " .. reason .. ".") end
    end
end

local function elementPath(root, element, label)
    local path = {}
    local current = element
    for _ = 1, 256 do
        if current == root then return path end
        local okParent, parent = pcall(function() return current.parent end)
        if not okParent or not parent then
            error(label .. " Quick Control scope is outside the source Program.")
        end
        local found = nil
        for index, child in ipairs(directChildren(parent, label .. " scope parent")) do
            if child == current then
                local okType, childType = pcall(function() return child.type end)
                local okName, childName = pcall(function() return child.name end)
                if not okType or not okName then error(label .. " Quick Control scope cannot be identified.") end
                found = { index = index, type = tostring(childType), name = tostring(childName) }
                break
            end
        end
        if not found then error(label .. " Quick Control scope is not present in its reported parent.") end
        table.insert(path, 1, found)
        current = parent
    end
    error(label .. " Quick Control scope exceeds the supported Program Tree depth.")
end

local function resolveElementPath(root, path, label)
    local current = root
    for depth, step in ipairs(path) do
        local children = directChildren(current, label .. " scope path")
        local child = children[step.index]
        if not child then error(string.format("%s scope path is missing element %d.", label, depth)) end
        local okType, childType = pcall(function() return child.type end)
        local okName, childName = pcall(function() return child.name end)
        if not okType or not okName or tostring(childType) ~= step.type or tostring(childName) ~= step.name then
            error(string.format("%s scope path differs at element %d.", label, depth))
        end
        current = child
    end
    return current
end

local function snapshotQuickControls(program, label)
    local controls = {}
    for qc = 1, QUICK_CONTROL_COUNT do
        local count = invoke(program, "getNumQCAssignments", label .. " Quick Control " .. qc, qc)
        if type(count) ~= "number" or count < 0 or count % 1 ~= 0 or count > MAX_TREE_NODES then
            error(string.format("%s Quick Control %d returned an invalid assignment count.", label, qc))
        end
        local assignments = {}
        for assignment = 1, count do
            local assignmentLabel = string.format("%s Quick Control %d assignment %d", label, qc, assignment)
            local scope = invoke(program, "getQCAssignmentScope", assignmentLabel, qc, assignment)
            if not scope then error(assignmentLabel .. " has no scope.") end
            assignments[assignment] = {
                parameterId = invoke(program, "getQCAssignmentParamId", assignmentLabel, qc, assignment),
                scopePath = elementPath(program, scope, assignmentLabel),
                min = invoke(program, "getQCAssignmentMin", assignmentLabel, qc, assignment),
                max = invoke(program, "getQCAssignmentMax", assignmentLabel, qc, assignment),
                curve = invoke(program, "getQCAssignmentCurve", assignmentLabel, qc, assignment),
                mode = invoke(program, "getQCAssignmentMode", assignmentLabel, qc, assignment),
                bypass = invoke(program, "getQCAssignmentBypass", assignmentLabel, qc, assignment),
            }
        end
        controls[qc] = assignments
    end
    return controls
end

local function clearQuickControls(program, label)
    for qc = 1, QUICK_CONTROL_COUNT do
        local count = invoke(program, "getNumQCAssignments", label .. " Quick Control " .. qc, qc)
        for assignment = count, 1, -1 do
            invoke(program, "removeQCAssignment", label .. " Quick Control " .. qc, qc, assignment)
        end
        if invoke(program, "getNumQCAssignments", label .. " Quick Control " .. qc, qc) ~= 0 then
            error(string.format("Could not clear %s Quick Control %d.", label, qc))
        end
    end
end

local function findMatchingElement(scope, parameterId, label)
    local stack = { scope }
    local visited = 0
    while #stack > 0 do
        local element = table.remove(stack)
        visited = visited + 1
        if visited > MAX_TREE_NODES then error(label .. " scope exceeds the supported Program Tree size.") end
        local hasParameter = invoke(element, "hasParameter", label .. " parameter lookup", parameterId)
        if hasParameter == true then return element end
        local children = directChildren(element, label .. " parameter lookup")
        for index = #children, 1, -1 do stack[#stack + 1] = children[index] end
    end
    error(string.format("%s cannot resolve parameter ID %s within its scope.", label, tostring(parameterId)))
end

local function restoreQuickControls(program, expected, label)
    clearQuickControls(program, label)
    for qc = 1, QUICK_CONTROL_COUNT do
        for _, assignment in ipairs(expected[qc]) do
            local before = invoke(program, "getNumQCAssignments", label .. " Quick Control " .. qc, qc)
            local assignmentLabel = string.format("%s Quick Control %d assignment %d", label, qc, before + 1)
            local scope = resolveElementPath(program, assignment.scopePath, assignmentLabel)
            local target = findMatchingElement(scope, assignment.parameterId, assignmentLabel)
            invoke(program, "addQCAssignment", assignmentLabel, qc, target, assignment.parameterId, scope)
            local after = invoke(program, "getNumQCAssignments", label .. " Quick Control " .. qc, qc)
            if after ~= before + 1 then error(assignmentLabel .. " was not appended.") end
            invoke(program, "setQCAssignmentParamId", assignmentLabel, qc, after, assignment.parameterId)
            invoke(program, "setQCAssignmentScope", assignmentLabel, qc, after, scope)
            invoke(program, "setQCAssignmentMode", assignmentLabel, qc, after, assignment.mode)
            invoke(program, "setQCAssignmentMin", assignmentLabel, qc, after, assignment.min)
            invoke(program, "setQCAssignmentMax", assignmentLabel, qc, after, assignment.max)
            invoke(program, "setQCAssignmentCurve", assignmentLabel, qc, after, assignment.curve)
            invoke(program, "setQCAssignmentBypass", assignmentLabel, qc, after, assignment.bypass)
        end
    end
end

local function requireMatchingQuickControls(expected, program, label)
    local actual = snapshotQuickControls(program, label)
    local equal, reason = valuesMatch(expected, actual, "Quick Control assignments")
    if not equal then error(label .. " " .. reason .. ".") end
end

local function validateDonor(donor)
    requireType(donor, "Program", "Macro donor preset")
    directChildren(donor, "macro donor Program")
    snapshotRootParameters(donor, "macro donor Program")
    snapshotQuickControls(donor, "macro donor Program")
end

local function requireSupportedSourceChildren(children)
    local supported = { Bus = true, Layer = true, MidiModule = true, Zone = true }
    for index, child in ipairs(children) do
        local ok, childType = pcall(function() return child.type end)
        if not ok or not supported[tostring(childType)] then
            error(string.format("Source Program direct child %d has unsupported type '%s'.", index, tostring(childType)))
        end
    end
end

local function clearDonorChildren(donor)
    for index, child in ipairs(directChildren(donor, "macro donor Program")) do
        invoke(child, "removeFromParent", "Could not remove macro donor direct child " .. index)
    end
    if #directChildren(donor, "cleared macro donor Program") ~= 0 then
        error("Macro donor Program still contains direct children after cleanup.")
    end
end

local function appendChild(destination, child, index)
    local appendByType = {
        Bus = "appendBus",
        Layer = "appendLayer",
        MidiModule = "appendMidiModule",
        Zone = "appendZone",
    }
    local childType = tostring(child.type)
    local method = appendByType[childType]
    if not method then error(string.format("Source Program direct child %d has unsupported type '%s'.", index, childType)) end
    invoke(child, "removeFromParent", "Could not detach source Program direct child " .. index)
    invoke(destination, method, "Could not append source Program direct child " .. index, child)
end

local function validateConfiguration(ctx)
    if DONOR_PATH == "" or TOKEN == "" then error("Macro-page runtime configuration is incomplete.") end
    if not PREFLIGHT and (OUTPUT_ROOT == "" or RECEIPT_PATH == "") then
        error("Macro-page chunk output or receipt path is missing.")
    end
    local contextOutput = tostring(ctx.output_dir or ""):gsub("\\", "/"):gsub("/+$", "")
    if not PREFLIGHT and contextOutput ~= OUTPUT_ROOT:gsub("/+$", "") then
        error("Macro-page runtime output root does not match the build context output directory.")
    end
end

local function openReceipt()
    local ok, file, openError = pcall(io.open, RECEIPT_PATH, "wb")
    if not ok or not file then
        error("Could not create macro-page receipt " .. RECEIPT_PATH .. ": " .. tostring(openError or file))
    end
    return file
end

local function writeReceipt(file, index)
    local line = string.format("HBMPI1\t%s\t%d\n", TOKEN, index)
    local okWrite, writeResult, writeError = pcall(file.write, file, line)
    if not okWrite or not writeResult then
        error("Could not write macro-page receipt: " .. tostring(writeError or writeResult))
    end
    local okFlush, flushResult = pcall(file.flush, file)
    if not okFlush or flushResult == false then
        error("Could not flush macro-page receipt: " .. tostring(flushResult))
    end
end

local function writeFailure(file, index, message)
    message = tostring(message or "unknown error"):gsub("[\r\n\t]", " ")
    local line = string.format("HBMPE1\t%s\t%d\t%s\n", TOKEN, index, message)
    local okWrite, writeResult, writeError = pcall(file.write, file, line)
    if not okWrite or not writeResult then
        error("Could not write macro-page failure receipt: " .. tostring(writeError or writeResult))
    end
    local okFlush, flushResult = pcall(file.flush, file)
    if not okFlush or flushResult == false then
        error("Could not flush macro-page failure receipt: " .. tostring(flushResult))
    end
end

local function readSpecification(specification)
    local sourcePath = tostring(specification.source or "")
    local relativePath = tostring(specification.relative or "")
    local index = tonumber(specification.index)
    if sourcePath == "" or relativePath == "" or not index or index < 0 or index % 1 ~= 0 then
        error("Macro-page preset specification is incomplete.")
    end
    return sourcePath, relativePath, index
end

local function savePresetWithMacro(ctx, specification)
    local sourcePath, relativePath = readSpecification(specification)
    local source = load(sourcePath, "source")
    requireType(source, "Program", "Source preset")
    local donor = load(DONOR_PATH, "macro donor")
    validateDonor(donor)

    -- Snapshot state before moving children because Quick Control scopes belong
    -- to the original source tree and become invalid on the discarded root.
    local sourceName = tostring(source.name)
    local sourceChildren = directChildren(source, "source Program")
    requireSupportedSourceChildren(sourceChildren)
    local sourceSignature = directChildSignature(source, "source Program")
    local sourceParameters = snapshotRootParameters(source, "source Program")
    local sourceQuickControls = snapshotQuickControls(source, "source Program")
    local donorParameters = snapshotRootParameters(donor, "macro donor Program")
    requireMatchingParameterSchemas(sourceParameters, donorParameters, "Macro donor")

    clearQuickControls(donor, "macro donor Program")
    clearDonorChildren(donor)
    for index, child in ipairs(sourceChildren) do appendChild(donor, child, index) end
    invoke(donor, "setName", "Could not copy the source Program name", sourceName)
    copyRootParameters(sourceParameters, donor, donorParameters)
    restoreQuickControls(donor, sourceQuickControls, "transplanted Program")

    if not signaturesMatch(sourceSignature, directChildSignature(donor, "transplanted Program")) then
        error("Transplanted Program direct-child structure differs from the source Program.")
    end
    requireMatchingRootParameters(sourceParameters, donor, "Transplanted Program")
    requireMatchingQuickControls(sourceQuickControls, donor, "Transplanted Program")

    local okSave, saved = pcall(ctx.save_preset, relativePath, donor, "HS", "program")
    if not okSave or not saved then
        error("Could not save HALion Sonic Program " .. relativePath .. ": " .. tostring(saved))
    end
end

local function validateSavedPreset(_, specification)
    local sourcePath, relativePath = readSpecification(specification)
    local source = load(sourcePath, "source")
    requireType(source, "Program", "Source preset")
    local outputPath = pathJoin(OUTPUT_ROOT, relativePath)
    local reloaded = load(outputPath, "saved output")
    requireType(reloaded, "Program", "Saved output preset")

    if tostring(source.name) ~= tostring(reloaded.name) then
        error("Saved Program name differs from the source Program: " .. relativePath)
    end
    if not signaturesMatch(directChildSignature(source, "source Program"),
        directChildSignature(reloaded, "saved Program")) then
        error("Saved Program direct-child structure differs from the source Program: " .. relativePath)
    end
    requireMatchingRootParameters(snapshotRootParameters(source, "source Program"), reloaded, "Saved Program")
    requireMatchingQuickControls(snapshotQuickControls(source, "source Program"), reloaded, "Saved Program")
end

return function(ctx)
    validateConfiguration(ctx)
    local donor = load(DONOR_PATH, "macro donor")
    validateDonor(donor)
    if PREFLIGHT then
        return { ok = true, saved = 0, failed = 0, message = "Program-root macro donor preflight passed." }
    end

    local receipt = openReceipt()
    local saved = 0
    local failed = 0
    for ordinal, specification in ipairs(PRESETS) do
        local index = tonumber(specification.index) or -1
        local operation = VALIDATE_ONLY and "Validating macro page" or "Injecting macro page"
        ctx.progress(ordinal, #PRESETS, string.format("%s %d/%d: %s", operation, ordinal, #PRESETS,
            tostring(specification.relative or "")))
        local processor = VALIDATE_ONLY and validateSavedPreset or savePresetWithMacro
        local ok, processError = pcall(processor, ctx, specification)
        if ok then
            if VALIDATE_ONLY then
                local receiptOk, receiptError = pcall(writeReceipt, receipt, index)
                if not receiptOk then
                    failed = failed + 1
                    ctx.log("Error: " .. tostring(receiptError))
                    break
                end
            end
            saved = saved + 1
        else
            failed = failed + 1
            ctx.log("Error: " .. tostring(processError))
            local failureReceiptOk, failureReceiptError = pcall(writeFailure, receipt, index, processError)
            if not failureReceiptOk then
                ctx.log("Error: " .. tostring(failureReceiptError))
                break
            end
            if FAIL_FAST then break end
        end
    end

    local okClose, closeResult = pcall(receipt.close, receipt)
    if not okClose or closeResult == false then
        failed = failed + 1
        ctx.log("Error: Could not close macro-page receipt: " .. tostring(closeResult))
    end

    return {
        ok = failed == 0,
        saved = saved,
        failed = failed,
        message = string.format("Macro-page %s chunk: %d succeeded, %d failed.",
            VALIDATE_ONLY and "validation" or "injection", saved, failed),
    }
end
