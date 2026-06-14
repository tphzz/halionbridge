local hb = require("halionbridge-sfz")

local probeRoot = "C:/develop/halionbridge/probes/sfz/039-loop-variants-diagnostic/source/samples/"

return function(config)
    local layerName = config.layer_name
    local outputFile = config.output_file

    local region = {
        name = layerName,
        sample_playback = {
            sample = probeRoot .. "loop_segments.wav",
        },
        mapping = {
            key_low = 57,
            key_high = 57,
            velocity_low = 0,
            velocity_high = 127,
            root_key = 57,
        },
        amp_envelope = {
            points = {
                { level = 0, duration = 0, curve = 0 },
                { level = 1, duration = 0, curve = 0 },
                { level = 1, duration = 0, curve = 0 },
                { level = 0, duration = 1.2, curve = 0 },
            },
            sustain_index = 3,
        },
        gain = {
            sample_osc_level_db = 7.8,
            amp_velocity_to_level = 100,
        },
    }

    return function(ctx)
        ctx.log("Building " .. layerName)

        local layer, layerErr = hb.create_layer(ctx, layerName)
        if not layer then
            return hb.fail(layerErr)
        end

        ctx.progress(0, 2, "Mapping " .. region.name)
        local zone, zoneErr = hb.append_sample_zone(ctx, layer, region)
        if not zone then
            return hb.fail(zoneErr)
        end

        hb.set_parameter_if_available(ctx, zone, "Amp Env.VelocityToLevelCurve", 0)

        if config.sample_end ~= nil then
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SampleEnd", config.sample_end)
        end

        if config.loop_mode ~= nil then
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopModeA", config.loop_mode)
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopStartA", 11025)
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopEndA", 22050)
        end

        if config.loop_xfade_length ~= nil then
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopXFadeLengthA", config.loop_xfade_length)
        end

        if config.loop_xfade_curve ~= nil then
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopXFadeCurveA", config.loop_xfade_curve)
        end

        if config.loop_tuning ~= nil then
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopTuningA", config.loop_tuning)
        end

        ctx.progress(1, 2, "Saving " .. outputFile)
        local saved, saveErr = hb.save_layer_preset(ctx, layer, outputFile)
        if not saved then
            return hb.fail(saveErr)
        end

        ctx.progress(2, 2, "Saved " .. outputFile)
        return hb.ok("Built " .. outputFile, 1)
    end
end
