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

-- HALion's native FMLab importer converts the nonlinear DX7 pitch-envelope
-- level scale to signed semitone offsets. The complete table was measured from
-- native imports so generated voices do not interpolate across the steep
-- ranges near source levels 0 and 99.
local pitch_envelope_level_offset = {
    -55.254833221435575, -48.054628655322425, -41.99999942862066, -36.90868301783964, -32.62741851806642,
    -29.027315019157427, -26.000001358023848, -23.454344806853452, -21.313709259033203, -19.513658390222872,
    -18.000000529675617, -16.727171651972412, -15.6568546295166, -14.756828984410161, -14.00000054209181,
    -13.363585638756716, -12.828427314758299, -12.378413530010503, -11.9999992946353, -11.619999973282573,
    -11.250000000000002, -10.870000049471857, -10.500000044703485, -10.12000009417534, -9.75,
    -9.370000138878822, -9.000000044703484, -8.620000183582306, -8.25, -7.869999870657921,
    -7.500000223517418, -7.120000094175339, -6.750000000000002, -6.370000049471857, -6.000000044703485,
    -5.6200000941753405, -5.249999999999998, -4.869999960064886, -4.500000044703482, -4.12000000476837,
    -3.749999999999999, -3.3700000494718543, -3.0000000447034827, -2.6200000941753383, -2.2500000000000004,
    -1.8700000494718556, -1.5000000447034838, -1.1200000271201136, -0.7500000000000011, -0.37000000476837214,
    0.0, 0.37000000476837214, 0.7500000078729128, 1.1199999968954584, 1.5000000157458255,
    1.8700000047683711, 2.249999855047519, 2.6199998669206925, 2.999999873685907, 3.3699998855590807,
    3.749999963734184, 4.119999950867169, 4.499999898426098, 4.869999885559083, 5.249999972437761,
    5.620000052217219, 5.999999805779621, 6.369999885559079, 6.749999847605241, 7.120000006733736,
    7.499999726430591, 7.869999885559086, 8.249999682277345, 8.620000168304554, 8.999999958027384,
    9.369999885559078, 9.750000020732218, 10.119999634771036, 10.499999623617784, 10.869999885559084,
    11.2500001409935, 11.619999803942157, 11.999999756893878, 12.378414154052727, 12.828427610819741,
    13.36358509922638, 13.999999320265484, 14.756828308105465, 15.656854198884494, 16.727170939516316,
    17.999999306297457, 19.513656616210927, 21.31370953229544, 23.45434308513223, 26.000000830597866,
    29.02731513977052, 32.6274193188931, 36.90868603353353, 42.00000240384908, 48.05463027954103,
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

local function set_required(target, name, value, element_label)
    element_label = element_label or "FM Zone"
    if target == nil then return false, "Cannot set " .. tostring(name) .. ": target is nil" end
    if not has_method(target, "hasParameter") then
        return false, "Cannot validate required HALion " .. element_label .. " parameter " .. tostring(name)
    end
    local okHas, hasParameter = pcall(function() return target:hasParameter(name) end)
    if not okHas or not hasParameter then
        return false, "Required HALion " .. element_label .. " parameter is unavailable: " .. tostring(name)
    end
    local okSet, setError = pcall(function() target:setParameter(name, value) end)
    if not okSet then
        return false, "Could not set required HALion " .. element_label .. " parameter " .. tostring(name) .. ": " .. tostring(setError)
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

local function pitch_envelope_duration(start_offset, target_offset, rate)
    -- HALion rejects the complete envelope-point table when any duration is
    -- above 30 seconds instead of clamping the offending point itself.
    return math.min(30, math.abs(target_offset - start_offset) * 0.0075 * 2 ^ ((99 - clamp(rate, 0, 99)) / 18))
end

local function set_pitch_envelope(zone, rates, levels)
    rates = rates or {}
    levels = levels or {}
    local okPoints, points = pcall(function() return zone:getParameter("Pitch Env.EnvelopePoints") end)
    if not okPoints or type(points) ~= "table" or #points == 0 then
        return false, "Required HALion pitch envelope point table is unavailable"
    end

    local source_levels = {
        clamp(levels[1], 0, 99),
        clamp(levels[2], 0, 99),
        clamp(levels[3], 0, 99),
        clamp(levels[4], 0, 99),
    }
    local neutral = source_levels[1] == 50 and source_levels[2] == 50
                    and source_levels[3] == 50 and source_levels[4] == 50
    if neutral then
        while #points > 4 do removeEnvelopePoint(points, #points) end
        while #points < 4 do insertEnvelopePoint(points, #points, 0, 0, 0) end
        local neutral_durations = { 0, 0.1, 0.25, 0.2 }
        for index = 1, 4 do
            points[index].level = 0
            points[index].duration = neutral_durations[index]
            points[index].curve = 0
        end
        local ok, err = set_required(zone, "Pitch.EnvAmount", 0)
        if not ok then return false, err end
        ok, err = set_required(zone, "Pitch Env.EnvelopePoints", points)
        if not ok then return false, err end
        return set_required(zone, "Pitch Env.SustainIndex", 3)
    end

    while #points > 5 do removeEnvelopePoint(points, #points) end
    while #points < 5 do insertEnvelopePoint(points, #points, 0, 0, 0) end

    local offsets = {
        pitch_envelope_level_offset[source_levels[1] + 1],
        pitch_envelope_level_offset[source_levels[2] + 1],
        pitch_envelope_level_offset[source_levels[3] + 1],
        pitch_envelope_level_offset[source_levels[4] + 1],
    }
    local amount = 0
    for index = 1, 4 do amount = math.max(amount, math.abs(offsets[index])) end
    local ordered_offsets = { offsets[4], offsets[1], offsets[2], offsets[3], offsets[4] }
    local source_rates = { rates[1], rates[2], rates[3], rates[4] }
    for index = 1, 5 do
        points[index].level = ordered_offsets[index] / amount
        points[index].duration = index == 1 and 0 or
            pitch_envelope_duration(ordered_offsets[index - 1], ordered_offsets[index], source_rates[index - 1])
        points[index].curve = 0
    end

    local ok, err = set_required(zone, "Pitch.EnvAmount", amount)
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

    -- HALion's native DX7 import leaves the Main-section velocity remap at its
    -- linear default. The DX7 response is concave, so use HALion's third menu
    -- entry (zero-based value 2, Squared Inverse) before operator sensitivity.
    -- HALion remap contract: https://www.steinberg.help/r/halion/7.1/en/halion/topics/editing_programs_and_layers/sound_editor_main_section_r.html
    -- DX7 velocity table: https://github.com/asb2m10/dexed/blob/master/Source/msfa/dx7note.cc#L80-L93
    ok, err = set_required(preset, "InheritVelocitySettings", false, "program or layer")
    if not ok then return { ok = false, saved = 0, failed = 1, message = err } end
    ok, err = set_required(preset, "VelocityToLevelCurve", 2, "program or layer")
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
