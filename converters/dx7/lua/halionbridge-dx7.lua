-- halionbridge Yamaha DX7 conversion helper.
--
-- Generated voice entrypoints contain the complete, normalized DX7 values.
-- This module is the only place where those values are translated to HALion
-- FM Zone parameters, which keeps empirical mapping refinements reviewable and
-- avoids regenerating or reparsing the source SysEx corpus.

local dx7 = {}

-- HALion's native DX7 importer rounds the low LFO range to hundredths of a
-- hertz before storing it. Higher values use two simple linear branches.
local lfo_low_frequency_hz = {
    0.06, 0.12, 0.31, 0.44, 0.62, 0.74, 0.93, 1.12,
    1.24, 1.43, 1.56, 1.74, 1.86, 2.05, 2.24, 2.36,
    2.55, 2.68, 2.86, 2.99, 3.17, 3.35, 3.49, 3.65,
    3.80, 3.98, 4.17, 4.30, 4.48, 4.60, 4.79, 4.92,
}

-- These depth and source-offset tables are persisted by HALion's native FMLab
-- importer. The nonzero pitch offsets are part of that importer topology; they
-- must not be normalized away even when the destination depths already match.
local amplitude_sensitivity_depth = { 0, -13, -26, -53 }
local pitch_sensitivity_depth = { 0, -2.55646824837, -5.33722400665, -8.96085739136,
                                  -14.7621421814, -23.6194458008, -35.2307128906, -49.4348754883 }
local pitch_sensitivity_offset = { 0, 5.5, 2.75, 0, 0, 0.6, 0.35, 0.2 }
local lfo_waveform = { 1, 2, 2, 3, 0, 6 }
local lfo_shape = { 0, 0, 100, 50, 0, 0 }
local lfo_initial_phase = { 90, 0, 0, 180, 180, 180 }

-- DX7 curve order is -LIN, -EXP, +EXP, +LIN. HALion's native DX7 importer
-- represents those source curves with FM-X curve indices 2, 4, 3, and 1.
local key_level_curve = { 2, 4, 3, 1 }

local fm_level_destination = {
    ModulationDestination.fmOp1Level,
    ModulationDestination.fmOp2Level,
    ModulationDestination.fmOp3Level,
    ModulationDestination.fmOp4Level,
    ModulationDestination.fmOp5Level,
    ModulationDestination.fmOp6Level,
}

local fm_pitch_destination = {
    ModulationDestination.fmOp1Pitch,
    ModulationDestination.fmOp2Pitch,
    ModulationDestination.fmOp3Pitch,
    ModulationDestination.fmOp4Pitch,
    ModulationDestination.fmOp5Pitch,
    ModulationDestination.fmOp6Pitch,
}

local function has_method(value, name)
    if value == nil then return false end
    local ok, member = pcall(function() return value[name] end)
    return ok and type(member) == "function"
end

local function path_join(ctx, base, child)
    if ctx and type(ctx.path_join) == "function" then
        return ctx.path_join(base, child)
    end
    if base == nil or base == "" then return child end
    if child == nil or child == "" then return base end
    local last = base:sub(-1)
    if last == "/" or last == "\\" then return base .. child end
    return base .. "/" .. child
end

local function set_required(target, name, value)
    if target == nil then return false, "Cannot set " .. tostring(name) .. ": target is nil" end
    if not has_method(target, "hasParameter") then
        return false, "Cannot validate required HALion parameter " .. tostring(name)
    end
    local okHas, hasParameter = pcall(function() return target:hasParameter(name) end)
    if not okHas or not hasParameter then
        return false, "Required HALion FM Zone parameter is unavailable: " .. tostring(name)
    end
    local okSet, setError = pcall(function() target:setParameter(name, value) end)
    if not okSet then
        return false, "Could not set required HALion parameter " .. tostring(name) .. ": " .. tostring(setError)
    end
    return true, nil
end

local function set_required_name(element, name, element_label)
    if element == nil then return false, "Cannot name DX7 " .. element_label .. ": target is nil" end
    if type(name) ~= "string" or name == "" then return false, "DX7 voice name is missing" end
    if not has_method(element, "setName") then
        return false, "Required HALion " .. element_label .. " name assignment is unavailable"
    end
    local okSet, setError = pcall(function() element:setName(name) end)
    if not okSet then
        return false, "Could not set required HALion " .. element_label .. " name: " .. tostring(setError)
    end
    return true, nil
end

local function clamp(value, minimum, maximum)
    value = tonumber(value) or minimum
    if value < minimum then return minimum end
    if value > maximum then return maximum end
    return value
end

local function dx_envelope_time(rate)
    -- HALion's FM-X time controls run from short to long, opposite the DX7
    -- rate controls. Keeping this transform here makes an A/B adjustment apply
    -- identically to all four operator envelope stages.
    return 99 - clamp(rate, 0, 99)
end

local function fixed_frequency_hz(coarse, fine)
    -- DX7 fixed mode uses four frequency decades selected by coarse modulo 4.
    return 10 ^ ((clamp(coarse, 0, 31) % 4) + clamp(fine, 0, 99) / 100)
end

local function apply_operator(zone, number, operator, oscillator_sync)
    local prefix = "FM-Operator " .. tostring(number) .. "."
    local rates = operator.envelope_rates or {}
    local levels = operator.envelope_levels or {}
    local assignments = {
        { "Mute", false },
        { "FreqMode", operator.oscillator_mode == "fixed" and 0 or 1 },
        { "KeyOnReset", oscillator_sync == true },
        { "RatioCoarse", clamp(operator.frequency_coarse, 0, 31) },
        { "RatioFine", clamp(operator.frequency_fine, 0, 99) },
        { "Frequency", fixed_frequency_hz(operator.frequency_coarse, operator.frequency_fine) },
        { "Detune", clamp(operator.detune, 0, 14) - 7 },
        { "AEGHoldTime", 0 },
        { "AEGAttackTime", dx_envelope_time(rates[1]) },
        { "AEGDecay1Time", dx_envelope_time(rates[2]) },
        { "AEGDecay2Time", dx_envelope_time(rates[3]) },
        { "AEGReleaseTime", dx_envelope_time(rates[4]) },
        { "AEGAttackLevel", clamp(levels[1], 0, 99) },
        { "AEGDecay1Level", clamp(levels[2], 0, 99) },
        { "AEGDecay2Level", clamp(levels[3], 0, 99) },
        { "AEGReleaseLevel", clamp(levels[4], 0, 99) },
        { "KeyLevelBreakPoint", clamp(operator.key_level_breakpoint, 0, 99) + 21 },
        { "KeyLevelCurveLo", key_level_curve[clamp(operator.left_curve, 0, 3) + 1] },
        { "KeyLevelCurveHi", key_level_curve[clamp(operator.right_curve, 0, 3) + 1] },
        { "KeyLevelDepthLo", clamp(operator.key_level_left_depth, 0, 99) },
        { "KeyLevelDepthHi", clamp(operator.key_level_right_depth, 0, 99) },
        { "AEGTimeKeyFollow", clamp(operator.rate_scaling, 0, 7) },
        { "LevelVelocity", clamp(operator.key_velocity_sensitivity, 0, 7) },
        { "OutputLevel", clamp(operator.output_level, 0, 99) },
    }

    for _, assignment in ipairs(assignments) do
        local ok, err = set_required(zone, prefix .. assignment[1], assignment[2])
        if not ok then return false, err end
    end
    return true, nil
end

local function set_pitch_envelope(zone, rates, levels)
    rates = rates or {}
    levels = levels or {}
    local okPoints, points = pcall(function() return zone:getParameter("Pitch Env.EnvelopePoints") end)
    if not okPoints or type(points) ~= "table" or #points == 0 then
        return false, "Required HALion pitch envelope point table is unavailable"
    end

    while #points > 5 do removeEnvelopePoint(points, #points) end
    while #points < 5 do insertEnvelopePoint(points, #points, 0, 0, 0) end

    local function pitch_level(value)
        -- DX7 pitch-envelope level 50 is the neutral point. Keep the decoded
        -- contour inspectable while its HALion response is calibrated.
        return clamp((clamp(value, 0, 99) - 50) / 50, -1, 1)
    end
    local function duration(value)
        local normalized = (99 - clamp(value, 0, 99)) / 99
        return 0.001 + normalized * normalized * 21.3
    end

    local sourceLevels = { levels[4], levels[1], levels[2], levels[3], levels[4] }
    local sourceRates = { 99, rates[1], rates[2], rates[3], rates[4] }
    for index = 1, 5 do
        points[index].level = pitch_level(sourceLevels[index])
        points[index].duration = index == 1 and 0 or duration(sourceRates[index])
        points[index].curve = 0
    end

    -- The algorithm templates currently inherit a non-neutral amount. Disable
    -- it explicitly so an uncalibrated contour cannot shift the voice pitch.
    local ok, err = set_required(zone, "Pitch.EnvAmount", 0)
    if not ok then return false, err end
    ok, err = set_required(zone, "Pitch Env.EnvelopePoints", points)
    if not ok then return false, err end
    return set_required(zone, "Pitch Env.SustainIndex", 4)
end

local function configure_modulation_row(zone, row_number, source, polarity, destination, depth,
                                        source_offset, source_minimum, source_maximum)
    local okRow, row = pcall(function() return zone:getModulationMatrixRow(row_number) end)
    if not okRow or row == nil then
        return false, "Required HALion modulation matrix row " .. tostring(row_number) .. " is unavailable"
    end
    source_offset = clamp(source_offset or 0, -100, 100)
    source_minimum = clamp(source_minimum or 0, 0, 100)
    source_maximum = clamp(source_maximum or 100, 0, 100)
    local okSet, setError = pcall(function()
        row:setSource1(source)
        row:setParameter("Source1.Polarity", polarity)
        -- Source assignment can restore HALion's default forward range, so
        -- persist the native importer range only after selecting the source.
        row:setParameter("Source1.Minimum", source_minimum)
        row:setParameter("Source1.Maximum", source_maximum)
        row:setParameter("Source1.Offset", source_offset)
        row:setParameter("Destination.Destination", destination)
        row:setParameter("Destination.Depth", depth)
        row:setParameter("Destination.Bypass", false)
    end)
    if not okSet then
        return false, "Could not configure HALion modulation matrix row " .. tostring(row_number) .. ": " .. tostring(setError)
    end
    return true, nil
end

local function clear_modulation_rows(zone)
    for rowNumber = 1, 32 do
        local ok, err = configure_modulation_row(zone, rowNumber, ModulationSource.unassigned, 0,
                                                  ModulationDestination.unassigned, 0)
        if not ok then return false, err end
    end
    return true, nil
end

local function lfo_raw_rate_hz(speed)
    if speed < 32 then return lfo_low_frequency_hz[speed + 1] end
    local rawRate = speed * 10 / 63
    if speed >= 64 then rawRate = speed - 53 end
    return rawRate
end

local function lfo_rate_hz(speed, waveform)
    local rawRate = lfo_raw_rate_hz(speed)
    if waveform == 5 then return rawRate / 2 end
    return math.min(rawRate, 30)
end

local function lfo_delay_ms(delay)
    if delay == 0 then return 0 end
    return 44 * 2 ^ (3 * delay / 50)
end

local function lfo_fade_ms(delay)
    if delay == 0 then return 0 end
    if delay <= 43 then return 30 * 2 ^ (3 * delay / 50) end
    if delay <= 50 then return 320 end
    return 640
end

local function apply_lfo(zone, voice)
    local lfo = voice.lfo or {}
    local speed = math.floor(clamp(lfo.speed, 0, 99))
    local waveform = math.floor(clamp(lfo.waveform, 0, 5))
    local delay = math.floor(clamp(lfo.delay, 0, 99))
    local assignments = {
        { "LFO 1.WaveForm", lfo_waveform[waveform + 1] },
        { "LFO 1.Shape", lfo_shape[waveform + 1] },
        { "LFO 1.Sync", 0 },
        { "LFO 1.Rate", lfo_rate_hz(speed, waveform) },
        { "LFO 1.Delay", lfo_delay_ms(delay) },
        { "LFO 1.FadeIn", lfo_fade_ms(delay) },
        { "LFO 1.RandomPhase", false },
        { "LFO 1.InitPhase", lfo_initial_phase[waveform + 1] },
        { "LFO 1.Trigger", lfo.sync == true and 1 or 0 },
        { "LFO 1.SharedPhase", true },
    }
    for _, assignment in ipairs(assignments) do
        local ok, err = set_required(zone, assignment[1], assignment[2])
        if not ok then return false, err end
    end

    local ok, err = clear_modulation_rows(zone)
    if not ok then return false, err end

    local rowNumber = 1
    local amplitudeDepth = clamp(lfo.amplitude_modulation_depth, 0, 99)
    local amplitudeTargets = {}
    for targetOperatorIndex = 1, 6 do
        local sourceOperatorIndex = 7 - targetOperatorIndex
        local operator = (voice.operators or {})[sourceOperatorIndex] or {}
        local sensitivity = math.floor(clamp(operator.amplitude_modulation_sensitivity, 0, 3))
        if sensitivity > 0 then
            amplitudeTargets[#amplitudeTargets + 1] = {
                destination = fm_level_destination[targetOperatorIndex],
                depth = amplitude_sensitivity_depth[sensitivity + 1],
            }
        end
    end
    -- FMLab retains the input-to-Bus-1 row for every nonzero AMD value, even
    -- when all operators use AMS 0 and no Bus-1 output row follows it.
    if amplitudeDepth > 0 then
        ok, err = configure_modulation_row(zone, rowNumber, ModulationSource.lfo1, 0,
                                           ModulationDestination.bus1, amplitudeDepth * 100 / 99,
                                           0, 100, 0)
        if not ok then return false, err end
        rowNumber = rowNumber + 1
        for _, target in ipairs(amplitudeTargets) do
            ok, err = configure_modulation_row(zone, rowNumber, ModulationSource.bus1, 1, target.destination, target.depth)
            if not ok then return false, err end
            rowNumber = rowNumber + 1
        end
    end

    local pitchDepth = clamp(lfo.pitch_modulation_depth, 0, 99)
    local pitchSensitivity = math.floor(clamp(lfo.pitch_modulation_sensitivity, 0, 7))
    if pitchDepth > 0 and pitchSensitivity > 0 then
        ok, err = configure_modulation_row(zone, rowNumber, ModulationSource.lfo1, 1,
                                           ModulationDestination.bus2, -pitchDepth * 100 / 99)
        if not ok then return false, err end
        rowNumber = rowNumber + 1
        for targetOperatorIndex = 1, 6 do
            ok, err = configure_modulation_row(zone, rowNumber, ModulationSource.bus2, 1,
                                               fm_pitch_destination[targetOperatorIndex],
                                               pitch_sensitivity_depth[pitchSensitivity + 1],
                                               pitch_sensitivity_offset[pitchSensitivity + 1])
            if not ok then return false, err end
            rowNumber = rowNumber + 1
        end
    end
    return true, nil
end

local function load_template(ctx, voice)
    if not ctx or type(ctx.save_preset) ~= "function" then
        return nil, nil, "DX7 build context cannot save presets"
    end
    if type(voice) ~= "table" or type(voice.template_file) ~= "string" or voice.template_file == "" then
        return nil, nil, "DX7 build entry is missing its algorithm template path"
    end

    local templatePath = path_join(ctx, ctx.script_dir, voice.template_file)
    local okLoad, preset = pcall(loadPreset, templatePath)
    if not okLoad or preset == nil then
        return nil, nil, "Could not load DX7 algorithm template " .. templatePath .. ": " .. tostring(preset)
    end
    if not has_method(preset, "findZones") then
        return nil, nil, "DX7 algorithm template does not contain a searchable program or layer: " .. templatePath
    end

    local okZones, zones = pcall(function() return preset:findZones(true) end)
    if not okZones or type(zones) ~= "table" or #zones ~= 1 then
        return nil, nil, "DX7 algorithm template must contain exactly one FM Zone: " .. templatePath
    end
    local okParameter, hasParameter = pcall(function() return zones[1]:hasParameter("FM-Oscillator.FMFeedback") end)
    if not okParameter or not hasParameter then
        return nil, nil, "DX7 algorithm template zone is not an FM Zone: " .. templatePath
    end
    return preset, zones[1], nil
end

function dx7.build_voice(ctx, voice)
    local preset, zone, loadError = load_template(ctx, voice)
    if not preset then return { ok = false, saved = 0, failed = 1, message = loadError } end

    local ok, err = set_required_name(preset, voice.name, "program")
    if not ok then return { ok = false, saved = 0, failed = 1, message = err } end
    ok, err = set_required_name(zone, voice.name, "FM Zone")
    if not ok then return { ok = false, saved = 0, failed = 1, message = err } end

    ok, err = set_required(zone, "FM-Oscillator.FMFeedback", clamp(voice.feedback, 0, 7))
    if not ok then return { ok = false, saved = 0, failed = 1, message = err } end
    ok, err = set_required(zone, "FM-Oscillator.EmulationMode", 2)
    if not ok then return { ok = false, saved = 0, failed = 1, message = err } end
    ok, err = set_required(zone, "FM-Oscillator.UseDCARelease", false)
    if not ok then return { ok = false, saved = 0, failed = 1, message = err } end
    ok, err = set_required(zone, "FM-Oscillator.NoteShift", clamp(voice.transpose, 0, 48) - 24)
    if not ok then return { ok = false, saved = 0, failed = 1, message = err } end
    ok, err = set_required(zone, "FM-Oscillator.VelocityMin", 0)
    if not ok then return { ok = false, saved = 0, failed = 1, message = err } end
    ok, err = set_required(zone, "FM-Oscillator.VelocityMax", 100)
    if not ok then return { ok = false, saved = 0, failed = 1, message = err } end

    for sourceOperatorIndex = 1, 6 do
        local targetOperatorIndex = 7 - sourceOperatorIndex
        local operator = (voice.operators or {})[sourceOperatorIndex]
        if type(operator) ~= "table" then
            return { ok = false, saved = 0, failed = 1, message = "DX7 build entry is missing operator " .. sourceOperatorIndex }
        end
        ok, err = apply_operator(zone, targetOperatorIndex, operator, voice.oscillator_sync)
        if not ok then return { ok = false, saved = 0, failed = 1, message = err } end
    end
    for operatorIndex = 7, 8 do
        ok, err = set_required(zone, "FM-Operator " .. operatorIndex .. ".Mute", true)
        if not ok then return { ok = false, saved = 0, failed = 1, message = err } end
    end

    ok, err = set_pitch_envelope(zone, voice.pitch_envelope_rates, voice.pitch_envelope_levels)
    if not ok then return { ok = false, saved = 0, failed = 1, message = err } end
    ok, err = apply_lfo(zone, voice)
    if not ok then return { ok = false, saved = 0, failed = 1, message = err } end

    local outputPath = path_join(ctx, ctx.output_dir or ctx.script_dir, voice.output_file)
    local okSave, saved = pcall(function() return ctx.save_preset(outputPath, preset, "H7") end)
    if not okSave or not saved then
        return { ok = false, saved = 0, failed = 1, message = "Could not save " .. outputPath .. ": " .. tostring(saved) }
    end
    if type(ctx.progress) == "function" then ctx.progress(1, 1, "Saved " .. outputPath) end
    return { ok = true, saved = 1, failed = 0, message = "Saved " .. outputPath }
end

return dx7
