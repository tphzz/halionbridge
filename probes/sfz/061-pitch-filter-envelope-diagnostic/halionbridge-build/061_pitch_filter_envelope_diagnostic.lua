local hb = require("halionbridge-sfz")

local samplePath = "C:/develop/halionbridge/probes/sfz/061-pitch-filter-envelope-diagnostic/source/samples/saw_A3_single_cycle.wav"

local ampEnvelope = {
    points = {
        { level = 0, duration = 0, curve = 0 },
        { level = 1, duration = 0, curve = 0 },
        { level = 1, duration = 0, curve = 0 },
        { level = 0, duration = 0.01, curve = -1 },
    },
    sustain_index = 3,
}

local pitchAttackEnvelope = {
    points = {
        { level = 0, duration = 0, curve = 0 },
        { level = 1, duration = 1.0, curve = 0 },
        { level = 1, duration = 0, curve = 0 },
        { level = 0, duration = 0.01, curve = -1 },
    },
    sustain_index = 3,
}

local pitchDecayEnvelope = {
    points = {
        { level = 1, duration = 0, curve = 0 },
        { level = 1, duration = 0, curve = 0 },
        { level = 0, duration = 1.0, curve = 0 },
        { level = 0, duration = 0.01, curve = -1 },
    },
    sustain_index = 3,
}

local filterAttackEnvelope = pitchAttackEnvelope
local filterDecayEnvelope = pitchDecayEnvelope

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

local function with_filter(region)
    region.filter = {
        type = 1,
        mode = 0,
        shape_a = 2,
        cutoff = 500,
        resonance = 48,
    }
    return region
end

local cases = {
    {
        name = "000_no_env",
        output = "000_no_env.vstpreset",
    },
    {
        name = "001_pitch_attack_amt12",
        output = "001_pitch_attack_amt12.vstpreset",
        pitch_envelope = pitchAttackEnvelope,
        pitch_amount = 12,
    },
    {
        name = "002_pitch_attack_amt24",
        output = "002_pitch_attack_amt24.vstpreset",
        pitch_envelope = pitchAttackEnvelope,
        pitch_amount = 24,
    },
    {
        name = "003_pitch_attack_amt60",
        output = "003_pitch_attack_amt60.vstpreset",
        pitch_envelope = pitchAttackEnvelope,
        pitch_amount = 60,
    },
    {
        name = "004_pitch_decay_amt12",
        output = "004_pitch_decay_amt12.vstpreset",
        pitch_envelope = pitchDecayEnvelope,
        pitch_amount = 12,
    },
    {
        name = "005_pitch_decay_amt24",
        output = "005_pitch_decay_amt24.vstpreset",
        pitch_envelope = pitchDecayEnvelope,
        pitch_amount = 24,
    },
    {
        name = "006_pitch_decay_amt60",
        output = "006_pitch_decay_amt60.vstpreset",
        pitch_envelope = pitchDecayEnvelope,
        pitch_amount = 60,
    },
    {
        name = "007_filter_attack_amt25",
        output = "007_filter_attack_amt25.vstpreset",
        filter_envelope = filterAttackEnvelope,
        filter_amount = 25,
    },
    {
        name = "008_filter_attack_amt50",
        output = "008_filter_attack_amt50.vstpreset",
        filter_envelope = filterAttackEnvelope,
        filter_amount = 50,
    },
    {
        name = "009_filter_attack_amt100",
        output = "009_filter_attack_amt100.vstpreset",
        filter_envelope = filterAttackEnvelope,
        filter_amount = 100,
    },
    {
        name = "010_filter_decay_amt25",
        output = "010_filter_decay_amt25.vstpreset",
        filter_envelope = filterDecayEnvelope,
        filter_amount = 25,
    },
    {
        name = "011_filter_decay_amt50",
        output = "011_filter_decay_amt50.vstpreset",
        filter_envelope = filterDecayEnvelope,
        filter_amount = 50,
    },
    {
        name = "012_filter_decay_amt100",
        output = "012_filter_decay_amt100.vstpreset",
        filter_envelope = filterDecayEnvelope,
        filter_amount = 100,
    },
}

local function build_case(ctx, case)
    local layer, layerErr = hb.create_layer(ctx, case.name)
    if not layer then
        return false, layerErr
    end

    local region = base_region(case.name)
    if case.filter_envelope or case.filter_amount then
        region = with_filter(region)
    end

    local zone, zoneErr = hb.append_sample_zone(ctx, layer, region)
    if not zone then
        return false, zoneErr
    end

    if case.pitch_envelope then
        hb.set_parameter_if_available(ctx, zone, "Pitch.EnvAmount", case.pitch_amount)
        set_envelope(ctx, zone, "Pitch Env", case.pitch_envelope)
    end

    if case.filter_envelope then
        hb.set_parameter_if_available(ctx, zone, "Filter.EnvAmount", case.filter_amount)
        set_envelope(ctx, zone, "Filter Env", case.filter_envelope)
    end

    local saved, saveErr = hb.save_layer_preset(ctx, layer, case.output)
    if not saved then
        return false, saveErr
    end

    return true
end

return function(ctx)
    ctx.log("Building 061 pitch/filter envelope diagnostic")

    local saved = 0
    for i, case in ipairs(cases) do
        ctx.progress(i - 1, #cases, "Building " .. case.name)
        local ok, err = build_case(ctx, case)
        if not ok then
            return hb.fail(err)
        end
        saved = saved + 1
    end

    ctx.progress(#cases, #cases, "Saved 061 pitch/filter envelope diagnostic")
    return hb.ok("Built 061 pitch/filter envelope diagnostic", saved)
end

