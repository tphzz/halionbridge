local hb = require("halionbridge-sfz")

local samplePath = "C:/develop/halionbridge/probes/sfz/020-helper-rewrite-regression/source/samples/probe_A3.wav"

local function build(ctx, outputFile, sampleEnd, loopEnd)
    local layer, layerErr = hb.create_layer(ctx, outputFile)
    if not layer then
        return hb.fail(layerErr)
    end

    local zone, zoneErr = hb.create_sample_zone(ctx, layer, "probe_A3")
    if not zone then
        return hb.fail(zoneErr)
    end

    local ok, err = hb.set_parameter_required(zone, "SampleOsc.Filename", samplePath)
    if not ok then return hb.fail(err) end

    ok, err = hb.set_parameter_required(zone, "SampleOsc.Rootkey", 57)
    if not ok then return hb.fail(err) end

    if sampleEnd then
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.SampleStart", 0)
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.SampleEnd", sampleEnd)
    end

    hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopModeA", 1)
    hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopStartA", 0)
    hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopEndA", loopEnd)

    ok, err = hb.set_amp_envelope_required(zone, {
        points = {
            { level = 0, duration = 0, curve = 0 },
            { level = 1, duration = 0.005, curve = 0 },
            { level = 0.8, duration = 0.01, curve = 0 },
            { level = 0, duration = 0.1, curve = 0 },
        },
        sustain_index = 3,
    })
    if not ok then return hb.fail(err) end

    ok, err = hb.assign_field_required(zone, "keyLow", 48)
    if not ok then return hb.fail(err) end
    ok, err = hb.assign_field_required(zone, "keyHigh", 72)
    if not ok then return hb.fail(err) end
    ok, err = hb.assign_field_required(zone, "velLow", 1)
    if not ok then return hb.fail(err) end
    ok, err = hb.assign_field_required(zone, "velHigh", 127)
    if not ok then return hb.fail(err) end

    local saved, saveErr = hb.save_layer_preset(ctx, layer, outputFile)
    if not saved then
        return hb.fail(saveErr)
    end

    return hb.ok("Built " .. outputFile, 1)
end

return function(ctx)
    return build(ctx, "probe_loop_end_199.vstpreset", nil, 199)
end
