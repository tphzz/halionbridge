local hb = require("halionbridge-sfz")

local sampleFile = "C:/develop/halionbridge/probes/sfz/031-level-headroom-panlaw-diagnostic/source/samples/sine_A3_single_cycle.wav"

local function make_region()
    return {
        name = "sine_A3_single_cycle",
        sample_playback = { sample = sampleFile },
        mapping = { key_low = 57, key_high = 57, velocity_low = 0, velocity_high = 127, root_key = 57 },
        amp_envelope = {
            points = {
                { level = 0, duration = 0, curve = 0 },
                { level = 1, duration = 0.001, curve = 0 },
                { level = 1, duration = 0, curve = 0 },
                { level = 0, duration = 0.05, curve = 0 },
            },
            sustain_index = 3,
        },
        loop = { start = 0, finish = 199 },
    }
end

local M = {}

function M.build(ctx, outputStem, label, options)
    options = options or {}

    ctx.log("Building " .. label)
    local layer, layerErr = hb.create_layer(ctx, label)
    if not layer then return hb.fail(layerErr) end

    hb.set_parameter_if_available(ctx, layer, "InheritVelocitySettings", false)
    hb.set_parameter_if_available(ctx, layer, "VelocityToLevelCurve", 1)

    if options.layerLevel ~= nil then
        hb.set_parameter_if_available(ctx, layer, "Level", options.layerLevel)
    end

    local zone, zoneErr = hb.append_sample_zone(ctx, layer, make_region())
    if not zone then return hb.fail(zoneErr) end

    hb.set_parameter_if_available(ctx, zone, "Amp Env.VelocityToLevel", 100)
    hb.set_parameter_if_available(ctx, zone, "Amp Env.VelocityToLevelCurve", 0)

    if options.sampleOscLevel ~= nil then
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.Level", options.sampleOscLevel)
    end
    if options.ampLevel ~= nil then
        hb.set_parameter_if_available(ctx, zone, "Amp.Level", options.ampLevel)
    end
    if options.ampHeadroom ~= nil then
        hb.set_parameter_if_available(ctx, zone, "Amp.Headroom", options.ampHeadroom)
    end
    if options.ampPanLaw ~= nil then
        hb.set_parameter_if_available(ctx, zone, "Amp.PanLaw", options.ampPanLaw)
    end

    local outputFile = outputStem .. ".vstpreset"
    local saved, saveErr = hb.save_layer_preset(ctx, layer, outputFile)
    if not saved then return hb.fail(saveErr) end
    return hb.ok("Built " .. outputFile, 1)
end

return M
