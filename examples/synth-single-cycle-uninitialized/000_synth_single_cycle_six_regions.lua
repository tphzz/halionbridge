local layerName = "Synth Single Cycle - Six Regions"
local outputFile = "synth_single_cycle_six_regions.vstpreset"
local sampleZoneType = 1

-- These values mirror the <group> block in the source SFZ file. The zone
-- builder below maps the sample oscillator, key range, velocity range, root
-- key, and loop settings directly. The filter settings are kept here so a
-- converter author can see the source intent and decide how aggressively to
-- map SFZ filter semantics into a HALion-specific filter setup.
local group = {
    amp_veltrack = 100,
    ampeg_attack = 0.000999942,
    cutoff = 4978,
    fil_type = "lpf_2p",
    fil_veltrack = 2400,
    pitch_keycenter = 57,
}

local regions = {
    {
        name = "Saw A3",
        sample = "samples/saw_A3_single_cycle.wav",
        lokey = 36,
        hikey = 43,
        lovel = 0,
        hivel = 127,
        loop_start = 0,
        loop_end = 199,
    },
    {
        name = "Additive Organ A3",
        sample = "samples/additive_organ_A3_si.wav",
        lokey = 44,
        hikey = 51,
        lovel = 0,
        hivel = 127,
        loop_start = 0,
        loop_end = 199,
    },
    {
        name = "Sine A3",
        sample = "samples/sine_A3_single_cycle.wav",
        lokey = 52,
        hikey = 59,
        lovel = 0,
        hivel = 127,
        loop_start = 0,
        loop_end = 199,
    },
    {
        name = "Pulse 25 A3",
        sample = "samples/pulse25_A3_single_cy.wav",
        lokey = 60,
        hikey = 67,
        lovel = 0,
        hivel = 127,
        loop_start = 0,
        loop_end = 199,
    },
    {
        name = "Triangle A3",
        sample = "samples/triangle_A3_single_c.wav",
        lokey = 68,
        hikey = 75,
        lovel = 0,
        hivel = 127,
        loop_start = 0,
        loop_end = 199,
    },
    {
        name = "Sample And Hold A3",
        sample = "samples/sample_hold_A3_singl.wav",
        lokey = 76,
        hikey = 82,
        lovel = 0,
        hivel = 127,
        loop_start = 0,
        loop_end = 199,
    },
}

local function setNameIfAvailable(element, name)
    if element.setName then
        element:setName(name)
    end
end

local function setParameterIfAvailable(ctx, zone, name, value)
    local ok, err = pcall(function()
        zone:setParameter(name, value)
    end)

    if not ok then
        ctx.log("Skipped " .. name .. ": " .. tostring(err))
    end
end

local function appendSampleZone(ctx, layer, region)
    local zone = Zone()
    setNameIfAvailable(zone, region.name)

    -- A newly constructed HALion Zone defaults to a synth zone in the tested
    -- HALion 7 setup. `ZoneType = 1` is the sample-zone value discovered by
    -- the local HALion parameter/probe scripts, and it must be set before the
    -- sample oscillator assignment is expected to create an actual Sample Zone.
    setParameterIfAvailable(ctx, zone, "ZoneType", sampleZoneType)

    layer:appendZone(zone)

    zone.keyLow = region.lokey
    zone.keyHigh = region.hikey
    zone.velLow = region.lovel
    zone.velHigh = region.hivel

    -- HALion stores sample-file assignment and loop points as SampleOsc
    -- parameters, while key and velocity ranges are zone fields. The root key
    -- is also a SampleOsc parameter; assigning zone.rootKey would try to add a
    -- new Lua field to HALion's bound Zone object and HALion rejects that.
    -- The SFZ only declares loop_start/loop_end, so the playback range is left
    -- at the full audio file and only the sustain loop is written here.
    setParameterIfAvailable(ctx, zone, "SampleOsc.Filename", ctx.path_join(ctx.sample_root, region.sample))
    setParameterIfAvailable(ctx, zone, "SampleOsc.Rootkey", group.pitch_keycenter)
    setParameterIfAvailable(ctx, zone, "SampleOsc.SustainLoopModeA", 1)
    setParameterIfAvailable(ctx, zone, "SampleOsc.SustainLoopStartA", region.loop_start)
    setParameterIfAvailable(ctx, zone, "SampleOsc.SustainLoopEndA", region.loop_end)

    -- These source group values have direct HALion parameter names in the
    -- current parameter dump. The SFZ ampeg_attack value stays in the group
    -- table above because the dump exposes the amp envelope as point data
    -- rather than a simple "attack" scalar.
    setParameterIfAvailable(ctx, zone, "Amp Env.VelocityToLevel", group.amp_veltrack)
    setParameterIfAvailable(ctx, zone, "Filter.Cutoff", group.cutoff)
end

return function(ctx)
    ctx.log("Building " .. layerName)

    local layer = Layer()
    setNameIfAvailable(layer, layerName)
    this.parent:appendLayer(layer)

    for i, region in ipairs(regions) do
        ctx.progress(i - 1, #regions, "Mapping " .. region.sample)
        appendSampleZone(ctx, layer, region)
        ctx.progress(i, #regions, "Mapped " .. region.sample)
    end

    local outputPath = ctx.path_join(ctx.script_dir, outputFile)
    ctx.progress(#regions, #regions + 1, "Saving " .. outputFile)
    local saved = ctx.save_preset(outputPath, layer, "H7")

    if not saved then
        return {
            ok = false,
            saved = 0,
            failed = 1,
            message = "Failed to save " .. outputPath,
        }
    end

    ctx.progress(#regions + 1, #regions + 1, "Saved " .. outputFile)
    return {
        ok = true,
        saved = 1,
        failed = 0,
        message = "Built " .. outputFile,
    }
end
