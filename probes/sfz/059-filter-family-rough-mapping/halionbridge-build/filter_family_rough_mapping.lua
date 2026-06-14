local hb = require("halionbridge-sfz")

local samplePath = "C:/develop/halionbridge/probes/sfz/059-filter-family-rough-mapping/source/samples/saw_A3_single_cycle.wav"

local cases = {
    { id = "000", name = "no_filter", label = "No Filter" },
    { id = "001", name = "lpf_1p", label = "LPF 1P", type = 1, mode = 0, shape = 3, cutoff = 1000, resonance = 32 },
    { id = "002", name = "lpf_2p", label = "LPF 2P", type = 1, mode = 0, shape = 2, cutoff = 1000, resonance = 48 },
    { id = "003", name = "lpf_4p", label = "LPF 4P", type = 1, mode = 0, shape = 0, cutoff = 1000, resonance = 48 },
    { id = "004", name = "lpf_6p", label = "LPF 6P", type = 1, mode = 0, shape = 0, cutoff = 1000, resonance = 48 },
    { id = "005", name = "hpf_1p", label = "HPF 1P", type = 1, mode = 0, shape = 11, cutoff = 1000, resonance = 48 },
    { id = "006", name = "hpf_2p", label = "HPF 2P", type = 1, mode = 0, shape = 10, cutoff = 1000, resonance = 48 },
    { id = "007", name = "hpf_4p", label = "HPF 4P", type = 1, mode = 0, shape = 8, cutoff = 1000, resonance = 48 },
    { id = "008", name = "hpf_6p", label = "HPF 6P", type = 1, mode = 0, shape = 8, cutoff = 1000, resonance = 48 },
    { id = "009", name = "bpf_1p", label = "BPF 1P", type = 1, mode = 0, shape = 4, cutoff = 1000, resonance = 48 },
    { id = "010", name = "bpf_2p", label = "BPF 2P", type = 1, mode = 0, shape = 4, cutoff = 1000, resonance = 48 },
    { id = "011", name = "brf_1p", label = "BRF 1P", type = 1, mode = 0, shape = 12, cutoff = 1000, resonance = 48 },
    { id = "012", name = "brf_2p", label = "BRF 2P", type = 1, mode = 0, shape = 12, cutoff = 1000, resonance = 48 },
    { id = "013", name = "lpf_2p_sv", label = "LPF 2P SV", type = 1, mode = 0, shape = 2, cutoff = 1000, resonance = 48 },
    { id = "014", name = "hpf_2p_sv", label = "HPF 2P SV", type = 1, mode = 0, shape = 10, cutoff = 1000, resonance = 48 },
    { id = "015", name = "bpf_2p_sv", label = "BPF 2P SV", type = 1, mode = 0, shape = 4, cutoff = 1000, resonance = 48 },
    { id = "016", name = "brf_2p_sv", label = "BRF 2P SV", type = 1, mode = 0, shape = 12, cutoff = 1000, resonance = 48 },
    { id = "017", name = "lsh", label = "LSH weak baseline", type = 1, mode = 0, shape = 3, cutoff = 1000, resonance = 0 },
    { id = "018", name = "hsh", label = "HSH weak baseline", type = 1, mode = 0, shape = 11, cutoff = 1000, resonance = 0 },
    { id = "019", name = "peq", label = "PEQ weak baseline", type = 1, mode = 0, shape = 16, cutoff = 1000, resonance = 48 },
    { id = "020", name = "pink", label = "Pink weak baseline", type = 1, mode = 0, shape = 3, cutoff = 1000, resonance = 0 },
}

local function filter_params(case)
    local params = {}
    if case.type ~= nil then
        params[#params + 1] = { "Filter.Type", case.type }
    end
    if case.mode ~= nil then
        params[#params + 1] = { "Filter.Mode", case.mode }
    end
    if case.shape ~= nil then
        params[#params + 1] = { "Filter.ShapeA", case.shape }
    end
    if case.cutoff ~= nil then
        params[#params + 1] = { "Filter.Cutoff", case.cutoff }
    end
    if case.resonance ~= nil then
        params[#params + 1] = { "Filter.Resonance", case.resonance }
    end
    return params
end

local function region_for(case)
    return {
        name = case.name,
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
    }
end

return function(ctx)
    ctx.log("Building 059 rough filter-family mapping presets")

    local savedCount = 0
    for i, case in ipairs(cases) do
        ctx.progress(i - 1, #cases, "Building " .. case.id .. " " .. case.name)

        local layer, layerErr = hb.create_layer(ctx, "059 " .. case.id .. " " .. case.label)
        if not layer then
            return hb.fail(layerErr)
        end

        local region = region_for(case)
        local zone, zoneErr = hb.append_sample_zone(ctx, layer, region)
        if not zone then
            return hb.fail(zoneErr)
        end

        for _, param in ipairs(filter_params(case)) do
            hb.set_parameter_if_available(ctx, zone, param[1], param[2])
        end

        local outputFile = case.id .. "_" .. case.name .. ".vstpreset"
        local saved, saveErr = hb.save_layer_preset(ctx, layer, outputFile)
        if not saved then
            return hb.fail(saveErr)
        end
        savedCount = savedCount + 1
    end

    ctx.progress(#cases, #cases, "Saved " .. tostring(savedCount) .. " rough filter-family presets")
    return hb.ok("Built " .. tostring(savedCount) .. " rough filter-family presets", savedCount)
end
