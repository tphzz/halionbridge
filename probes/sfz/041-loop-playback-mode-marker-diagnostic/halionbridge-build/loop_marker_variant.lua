local hb = require("halionbridge-sfz")

local probeRoot = "C:/develop/halionbridge/probes/sfz/041-loop-playback-mode-marker-diagnostic/source/samples/"

return function(config)
    local layerName = config.layer_name
    local outputFile = config.output_file

    local region = {
        name = layerName,
        sample_playback = {
            sample = probeRoot .. "pre_loop_tail_clean.wav",
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
                { level = 0, duration = 3.0, curve = 0 },
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

        if config.playback_mode ~= nil then
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.PlaybackMode", config.playback_mode)
        end

        if config.loop_select ~= nil then
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.LoopSelect", config.loop_select)
        end

        if config.sample_end ~= nil then
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SampleEnd", config.sample_end)
        end

        if config.sample_start ~= nil then
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SampleStart", config.sample_start)
        end

        if config.release_start ~= nil then
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.ReleaseStart", config.release_start)
        end

        if config.loop_mode ~= nil then
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopModeA", config.loop_mode)
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopStartA", config.loop_start or 11000)
            hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopEndA", config.loop_end or 22000)
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
