local hb = require("halionbridge-sfz")

local samplePath = "C:/develop/halionbridge/probes/sfz/062-pitch-envelope-decay-start-diagnostic/source/samples/saw_A3_single_cycle.wav"

local ampEnvelope = {
    points = {
        { level = 0, duration = 0, curve = 0 },
        { level = 1, duration = 0, curve = 0 },
        { level = 1, duration = 0, curve = 0 },
        { level = 0, duration = 0.01, curve = -1 },
    },
    sustain_index = 3,
}

local envelopes = {
    attack_linear = {
        points = {
            { level = 0, duration = 0, curve = 0 },
            { level = 1, duration = 1.0, curve = 0 },
            { level = 1, duration = 0, curve = 0 },
            { level = 0, duration = 0.01, curve = -1 },
        },
        sustain_index = 3,
    },
    instant_zero_to_one_hold = {
        points = {
            { level = 0, duration = 0, curve = 0 },
            { level = 1, duration = 0, curve = 0 },
            { level = 1, duration = 0, curve = 0 },
            { level = 0, duration = 0.01, curve = -1 },
        },
        sustain_index = 3,
    },
    instant_start_one_hold = {
        points = {
            { level = 1, duration = 0, curve = 0 },
            { level = 1, duration = 0, curve = 0 },
            { level = 0, duration = 0.01, curve = -1 },
        },
        sustain_index = 2,
    },
    decay_duplicate_start1 = {
        points = {
            { level = 1, duration = 0, curve = 0 },
            { level = 1, duration = 0, curve = 0 },
            { level = 0, duration = 1.0, curve = 0 },
            { level = 0, duration = 0.01, curve = -1 },
        },
        sustain_index = 3,
    },
    decay_simple_start1 = {
        points = {
            { level = 1, duration = 0, curve = 0 },
            { level = 0, duration = 1.0, curve = 0 },
            { level = 0, duration = 0.01, curve = -1 },
        },
        sustain_index = 2,
    },
    decay_zero_to_one = {
        points = {
            { level = 0, duration = 0, curve = 0 },
            { level = 1, duration = 0, curve = 0 },
            { level = 0, duration = 1.0, curve = 0 },
            { level = 0, duration = 0.01, curve = -1 },
        },
        sustain_index = 3,
    },
    decay_simple_start0p75 = {
        points = {
            { level = 0.75, duration = 0, curve = 0 },
            { level = 0, duration = 1.0, curve = 0 },
            { level = 0, duration = 0.01, curve = -1 },
        },
        sustain_index = 2,
    },
    decay_simple_start0p5 = {
        points = {
            { level = 0.5, duration = 0, curve = 0 },
            { level = 0, duration = 1.0, curve = 0 },
            { level = 0, duration = 0.01, curve = -1 },
        },
        sustain_index = 2,
    },
}

local function set_envelope(ctx, zone, parameter_prefix, envelope)
    local ok, err = pcall(function()
        local points = zone:getParameter(parameter_prefix .. ".EnvelopePoints")
        if type(points) ~= "table" or #points == 0 then
            error(parameter_prefix .. ".EnvelopePoints did not return an editable point table")
        end

        while #points > #envelope.points do
            removeEnvelopePoint(points, #points)
        end

        while #points < #envelope.points do
            insertEnvelopePoint(points, #points, 0, 0, 0)
        end

        for i, source in ipairs(envelope.points) do
            points[i].level = source.level
            points[i].duration = source.duration
            points[i].curve = source.curve
        end

        zone:setParameter(parameter_prefix .. ".EnvelopePoints", points)
        zone:setParameter(parameter_prefix .. ".SustainIndex", envelope.sustain_index)
    end)

    if not ok then
        hb.warn(ctx, "Skipped " .. parameter_prefix .. " envelope: " .. tostring(err))
    end
end

local function base_region(name)
    return {
        name = name,
        sample_playback = {
            sample = samplePath,
            finish = 199,
        },
        mapping = {
            key_low = 57,
            key_high = 57,
            velocity_low = 0,
            velocity_high = 127,
            root_key = 57,
        },
        amp_envelope = ampEnvelope,
        loop = {
            mode = "continuous",
            start = 0,
            finish = 199,
        },
        gain = {
            sample_osc_level_db = 7.8,
            amp_velocity_to_level = 100,
        },
    }
end

local cases = {
    { name = "000_no_env", output = "000_no_env.vstpreset" },
    { name = "001_attack_amt12_control", output = "001_attack_amt12_control.vstpreset", pitch_amount = 12, envelope = envelopes.attack_linear },
    { name = "002_instant_hold_zero_to_one", output = "002_instant_hold_zero_to_one.vstpreset", pitch_amount = 12, envelope = envelopes.instant_zero_to_one_hold },
    { name = "003_instant_hold_start_one", output = "003_instant_hold_start_one.vstpreset", pitch_amount = 12, envelope = envelopes.instant_start_one_hold },
    { name = "004_decay_duplicate_start1_amt12", output = "004_decay_duplicate_start1_amt12.vstpreset", pitch_amount = 12, envelope = envelopes.decay_duplicate_start1 },
    { name = "005_decay_simple_start1_amt12", output = "005_decay_simple_start1_amt12.vstpreset", pitch_amount = 12, envelope = envelopes.decay_simple_start1 },
    { name = "006_decay_zero_to_one_amt12", output = "006_decay_zero_to_one_amt12.vstpreset", pitch_amount = 12, envelope = envelopes.decay_zero_to_one },
    { name = "007_decay_simple_start0p75_amt12", output = "007_decay_simple_start0p75_amt12.vstpreset", pitch_amount = 12, envelope = envelopes.decay_simple_start0p75 },
    { name = "008_decay_simple_start0p5_amt12", output = "008_decay_simple_start0p5_amt12.vstpreset", pitch_amount = 12, envelope = envelopes.decay_simple_start0p5 },
    { name = "009_decay_simple_start1_amt9", output = "009_decay_simple_start1_amt9.vstpreset", pitch_amount = 9, envelope = envelopes.decay_simple_start1 },
    { name = "010_decay_simple_start1_amt6", output = "010_decay_simple_start1_amt6.vstpreset", pitch_amount = 6, envelope = envelopes.decay_simple_start1 },
}

local function build_case(ctx, case)
    local layer, layerErr = hb.create_layer(ctx, case.name)
    if not layer then
        return false, layerErr
    end

    local zone, zoneErr = hb.append_sample_zone(ctx, layer, base_region(case.name))
    if not zone then
        return false, zoneErr
    end

    if case.envelope then
        hb.set_parameter_if_available(ctx, zone, "Pitch.EnvAmount", case.pitch_amount)
        set_envelope(ctx, zone, "Pitch Env", case.envelope)
    end

    local saved, saveErr = hb.save_layer_preset(ctx, layer, case.output)
    if not saved then
        return false, saveErr
    end

    return true
end

return function(ctx)
    ctx.log("Building 062 pitch envelope decay/start diagnostic")

    local saved = 0
    for i, case in ipairs(cases) do
        ctx.progress(i - 1, #cases, "Building " .. case.name)
        local ok, err = build_case(ctx, case)
        if not ok then
            return hb.fail(err)
        end
        saved = saved + 1
    end

    ctx.progress(#cases, #cases, "Saved 062 pitch envelope decay/start diagnostic")
    return hb.ok("Built 062 pitch envelope decay/start diagnostic", saved)
end

