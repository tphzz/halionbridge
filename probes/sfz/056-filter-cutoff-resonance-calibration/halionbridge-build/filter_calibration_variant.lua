local hb = require("halionbridge-sfz")

local samplePath = "C:/develop/halionbridge/probes/sfz/056-filter-cutoff-resonance-calibration/source/samples/saw_A3_single_cycle.wav"

local function filter_params(config)
    local params = {}
    if config.filter_type ~= nil then
        params[#params + 1] = { "Filter.Type", config.filter_type }
    end
    if config.filter_mode ~= nil then
        params[#params + 1] = { "Filter.Mode", config.filter_mode }
    end
    if config.filter_shape_a ~= nil then
        params[#params + 1] = { "Filter.ShapeA", config.filter_shape_a }
    end
    if config.cutoff ~= nil then
        params[#params + 1] = { "Filter.Cutoff", config.cutoff }
    end
    if config.resonance ~= nil then
        params[#params + 1] = { "Filter.Resonance", config.resonance }
    end
    return params
end

return function(config)
    local params = filter_params(config)
    local regions = {
        {
            name = "saw_A3_single_cycle",
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
            amp_envelope = {
                points = {
                    { level = 0, duration = 0, curve = 0 },
                    { level = 1, duration = 0, curve = 0 },
                    { level = 1, duration = 0, curve = 0 },
                    { level = 0.00349999988, duration = 0.000969999994, curve = -0.239999995 },
                    { level = 0, duration = 0.00506999996, curve = -1 },
                },
                sustain_index = 3,
            },
            loop = {
                mode = "continuous",
                start = 0,
                finish = 199,
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
            for _, param in ipairs(params) do
                hb.set_parameter_if_available(ctx, zone, param[1], param[2])
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
