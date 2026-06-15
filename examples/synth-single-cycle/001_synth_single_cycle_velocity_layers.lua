local layerName = "Synth Single Cycle - Velocity Layers"
local outputFile = "synth_single_cycle_velocity_layers.vstpreset"
local sampleZoneType = 1

-- These values mirror the <group> block in the source SFZ file. The example
-- maps the reliable HALion equivalents directly and leaves the source filter
-- intent readable for authors who want to extend the conversion.
local group = {
    amp_veltrack = 100,
    ampeg_attack = 0.000999942,
    cutoff = 4978,
    fil_type = "lpf_2p",
    fil_veltrack = 2400,
}

local regions = {
    {
        name = "Saw A3 Low Velocity",
        sample = "samples/saw_A3_single_cycle.wav",
        pitch_keycenter = 57,
        lokey = 36,
        hikey = 51,
        lovel = 0,
        hivel = 63,
        loop_start = 0,
        loop_end = 199,
    },
    {
        name = "Additive Organ A3 High Velocity",
        sample = "samples/additive_organ_A3_si.wav",
        pitch_keycenter = 57,
        lokey = 36,
        hikey = 51,
        lovel = 64,
        hivel = 127,
        loop_start = 0,
        loop_end = 199,
    },
    {
        name = "Sine A3 Low Velocity",
        sample = "samples/sine_A3_single_cycle.wav",
        pitch_keycenter = 57,
        lokey = 52,
        hikey = 67,
        lovel = 0,
        hivel = 63,
        loop_start = 0,
        loop_end = 199,
    },
    {
        name = "Pulse 25 A3 High Velocity",
        sample = "samples/pulse25_A3_single_cy.wav",
        pitch_keycenter = 57,
        lokey = 52,
        hikey = 67,
        lovel = 64,
        hivel = 127,
        loop_start = 0,
        loop_end = 199,
    },
    {
        name = "Triangle A3 Low Velocity",
        sample = "samples/triangle_A3_single_c.wav",
        pitch_keycenter = 57,
        lokey = 68,
        hikey = 82,
        lovel = 0,
        hivel = 63,
        loop_start = 0,
        loop_end = 199,
    },
    {
        name = "Sample And Hold A3 High Velocity",
        sample = "samples/sample_hold_A3_singl.wav",
        pitch_keycenter = 57,
        lokey = 68,
        hikey = 82,
        lovel = 64,
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

    -- The velocity split comes from the SFZ lovel/hivel fields. HALion keeps
    -- those as zone fields, so no extra MIDI module or script is needed for
    -- these two-layer ranges to respond to velocity. The root key is written
    -- through SampleOsc.Rootkey because HALion's bound Zone object does not
    -- accept an added zone.rootKey field. The SFZ only declares
    -- loop_start/loop_end, so playback stays at the full audio file while the
    -- sustain loop receives the translated loop points.
    setParameterIfAvailable(ctx, zone, "SampleOsc.Filename", ctx.path_join(ctx.sample_root, region.sample))
    setParameterIfAvailable(ctx, zone, "SampleOsc.Rootkey", region.pitch_keycenter)
    setParameterIfAvailable(ctx, zone, "SampleOsc.SustainLoopModeA", 1)
    setParameterIfAvailable(ctx, zone, "SampleOsc.SustainLoopStartA", region.loop_start)
    setParameterIfAvailable(ctx, zone, "SampleOsc.SustainLoopEndA", region.loop_end)

    -- The source ampeg_attack value is kept in the group table above. HALion's
    -- dumped amp envelope exposes point data instead of a simple attack scalar,
    -- so this example only writes the group settings that map cleanly.
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

    local outputPath = ctx.path_join(ctx.output_dir, outputFile)
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
