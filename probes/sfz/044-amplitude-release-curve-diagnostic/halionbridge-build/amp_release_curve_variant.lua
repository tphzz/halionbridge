local hb = require("halionbridge-sfz")

local samplePath = "C:/develop/halionbridge/probes/sfz/044-amplitude-release-curve-diagnostic/source/samples/amp_env_loop.wav"

return function(config)
    local layerName = config.layer_name
    local outputFile = config.output_file
    local releaseDuration = config.release_duration
    local releaseCurve = config.release_curve

    local regions = {
        {
            name = "amp_env_loop",
            sample_playback = {
                sample = samplePath,
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
                    { level = 0, duration = releaseDuration, curve = releaseCurve },
                },
                sustain_index = 3,
            },
            loop = {
                mode = "continuous",
                start = 0,
                finish = 43999,
            },
            gain = {
                sample_osc_level_db = 7.80000019,
                amp_velocity_to_level = 100,
            },
        },
    }

    return function(ctx)
        ctx.log("Building " .. layerName)

        local layer, layerErr = hb.create_layer(ctx, layerName)
        if not layer then
            return hb.fail(layerErr)
        end

        for i, region in ipairs(regions) do
            local label = hb.region_label(region)
            ctx.progress(i - 1, #regions, "Mapping " .. label)
            local zone, zoneErr = hb.append_sample_zone(ctx, layer, region)
            if not zone then
                return hb.fail(zoneErr)
            end
            ctx.progress(i, #regions, "Mapped " .. label)
        end

        ctx.progress(#regions, #regions + 1, "Saving " .. outputFile)
        local saved, saveErr = hb.save_layer_preset(ctx, layer, outputFile)
        if not saved then
            return hb.fail(saveErr)
        end

        ctx.progress(#regions + 1, #regions + 1, "Saved " .. outputFile)
        return hb.ok("Built " .. outputFile, 1)
    end
end
