local hb = require("halionbridge-sfz")

local layerName = "003 VelocityToLevel 200 Sample +8.0"
local outputFile = "003_velocity_to_level_200_sample_plus_8p0.vstpreset"
local sampleFile = "C:/develop/halionbridge/probes/sfz/028-amp-velocity-calibration-diagnostic/source/samples/sine_A3_single_cycle.wav"

local region = {
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

return function(ctx)
    ctx.log("Building " .. layerName)
    local layer, layerErr = hb.create_layer(ctx, layerName)
    if not layer then return hb.fail(layerErr) end

    local zone, zoneErr = hb.append_sample_zone(ctx, layer, region)
    if not zone then return hb.fail(zoneErr) end
    hb.set_parameter_if_available(ctx, zone, "Amp Env.VelocityToLevel", 200)
    hb.set_parameter_if_available(ctx, zone, "Amp Env.VelocityToLevelCurve", 0)
    hb.set_parameter_if_available(ctx, zone, "SampleOsc.Level", 8.0)

    local saved, saveErr = hb.save_layer_preset(ctx, layer, outputFile)
    if not saved then return hb.fail(saveErr) end
    return hb.ok("Built " .. outputFile, 1)
end
