local hb = require("halionbridge-sfz")

local samplePath = "C:/develop/halionbridge/probes/sfz/063-pitch-envelope-decay-curve-diagnostic/source/samples/saw_A3_single_cycle.wav"

local ampEnvelope = {
    points = {
        { level = 0, duration = 0, curve = 0 },
        { level = 1, duration = 0, curve = 0 },
        { level = 1, duration = 0, curve = 0 },
        { level = 0, duration = 0.01, curve = -1 },
    },
    sustain_index = 3,
}

local function attack_envelope()
    return {
        points = {
            { level = 0, duration = 0, curve = 0 },
            { level = 1, duration = 1.0, curve = 0 },
            { level = 1, duration = 0, curve = 0 },
            { level = 0, duration = 0.01, curve = -1 },
        },
        sustain_index = 3,
    }
end

local function decay_envelope(duration, curve)
    return {
        points = {
            { level = 1, duration = 0, curve = 0 },
            { level = 0, duration = duration, curve = curve },
            { level = 0, duration = 0.01, curve = -1 },
        },
        sustain_index = 2,
    }
end

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
    { name = "001_attack_amt12_control", output = "001_attack_amt12_control.vstpreset", pitch_amount = 12, envelope = attack_envelope() },
    { name = "002_decay_0p25_cneg0p5", output = "002_decay_0p25_cneg0p5.vstpreset", pitch_amount = 12, envelope = decay_envelope(0.25, -0.5) },
    { name = "003_decay_0p30_cneg0p5", output = "003_decay_0p30_cneg0p5.vstpreset", pitch_amount = 12, envelope = decay_envelope(0.30, -0.5) },
    { name = "004_decay_0p35_cneg0p5", output = "004_decay_0p35_cneg0p5.vstpreset", pitch_amount = 12, envelope = decay_envelope(0.35, -0.5) },
    { name = "005_decay_0p30_cneg0p25", output = "005_decay_0p30_cneg0p25.vstpreset", pitch_amount = 12, envelope = decay_envelope(0.30, -0.25) },
    { name = "006_decay_0p30_cneg0p75", output = "006_decay_0p30_cneg0p75.vstpreset", pitch_amount = 12, envelope = decay_envelope(0.30, -0.75) },
    { name = "007_decay_0p30_cneg1p0", output = "007_decay_0p30_cneg1p0.vstpreset", pitch_amount = 12, envelope = decay_envelope(0.30, -1.0) },
    { name = "008_decay_0p20_cneg0p5", output = "008_decay_0p20_cneg0p5.vstpreset", pitch_amount = 12, envelope = decay_envelope(0.20, -0.5) },
    { name = "009_decay_0p40_cneg0p5", output = "009_decay_0p40_cneg0p5.vstpreset", pitch_amount = 12, envelope = decay_envelope(0.40, -0.5) },
    { name = "010_decay_0p604_cneg1p0", output = "010_decay_0p604_cneg1p0.vstpreset", pitch_amount = 12, envelope = decay_envelope(0.604, -1.0) },
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
    ctx.log("Building 063 pitch envelope decay curve diagnostic")

    local saved = 0
    for i, case in ipairs(cases) do
        ctx.progress(i - 1, #cases, "Building " .. case.name)
        local ok, err = build_case(ctx, case)
        if not ok then
            return hb.fail(err)
        end
        saved = saved + 1
    end

    ctx.progress(#cases, #cases, "Saved 063 pitch envelope decay curve diagnostic")
    return hb.ok("Built 063 pitch envelope decay curve diagnostic", saved)
end

