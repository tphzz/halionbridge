local hb = require("halionbridge-sfz")

local samplePath = "C:/develop/halionbridge/probes/sfz/050-amplitude-decay-shape-diagnostic/source/samples/amp_env_loop.wav"

local function release_points(config)
    if config.release_bump then
        return {
            { level = 0.35, duration = 0.0679, curve = -0.24 },
            { level = 0, duration = 0.3549, curve = -1.0 },
        }
    end

    return {
        { level = 0, duration = 0, curve = 0 },
    }
end

local function envelope_points(config)
    local points = {
        { level = config.start_level or 0, duration = 0, curve = 0 },
        { level = 1, duration = config.attack or 0.01, curve = 0 },
    }

    if config.decay_points then
        for _, point in ipairs(config.decay_points) do
            points[#points + 1] = point
        end
    else
        points[#points + 1] = {
            level = config.sustain_level,
            duration = config.decay_duration,
            curve = config.decay_curve or 0,
        }
    end

    local sustainIndex = #points
    for _, point in ipairs(release_points(config)) do
        points[#points + 1] = point
    end

    return points, sustainIndex
end

return function(config)
    local points, sustainIndex = envelope_points(config)

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
                points = points,
                sustain_index = sustainIndex,
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
        ctx.log("Building " .. config.layer_name)

        local layer, layerErr = hb.create_layer(ctx, config.layer_name)
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

        ctx.progress(#regions, #regions + 1, "Saving " .. config.output_file)
        local saved, saveErr = hb.save_layer_preset(ctx, layer, config.output_file)
        if not saved then
            return hb.fail(saveErr)
        end

        ctx.progress(#regions + 1, #regions + 1, "Saved " .. config.output_file)
        return hb.ok("Built " .. config.output_file, 1)
    end
end
