local hb = require("halionbridge-sfz")

local sampleFile = "C:/develop/halionbridge/probes/sfz/030-main-velocity-curve-sweep-diagnostic/source/samples/sine_A3_single_cycle.wav"

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

function M.build(ctx, curveIndex)
    local label = string.format("%03d Main Velocity Curve %d", curveIndex, curveIndex)
    local outputFile = string.format("%03d_main_velocity_curve_%d.vstpreset", curveIndex, curveIndex)

    ctx.log("Building " .. label)
    local layer, layerErr = hb.create_layer(ctx, label)
    if not layer then return hb.fail(layerErr) end

    -- The Main-section Level Velocity Curve exists on programs and layers.
    -- Disable inheritance so this layer-local curve index is the one heard.
    hb.set_parameter_if_available(ctx, layer, "InheritVelocitySettings", false)
    hb.set_parameter_if_available(ctx, layer, "VelocityToLevelCurve", curveIndex)

    local zone, zoneErr = hb.append_sample_zone(ctx, layer, make_region())
    if not zone then return hb.fail(zoneErr) end

    -- Keep the amp envelope velocity curve linear here. This probe isolates the
    -- Main-section velocity remap shown in HALion's program/layer Main section.
    hb.set_parameter_if_available(ctx, zone, "Amp Env.VelocityToLevel", 100)
    hb.set_parameter_if_available(ctx, zone, "Amp Env.VelocityToLevelCurve", 0)

    local saved, saveErr = hb.save_layer_preset(ctx, layer, outputFile)
    if not saved then return hb.fail(saveErr) end
    return hb.ok("Built " .. outputFile, 1)
end

return M
