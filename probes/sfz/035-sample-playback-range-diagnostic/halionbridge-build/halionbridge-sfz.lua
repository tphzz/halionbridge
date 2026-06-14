-- Generated SFZ helper for halionbridge convert sfz.
--
-- This module is converter-owned support code, not part of the generic
-- halionbridge Lua builder API. Generated SFZ build scripts require it
-- to create HALion layers and sample zones defensively while keeping the
-- generated entrypoint source compact and inspectable.

local hb = {}

hb.version = 1

hb.capabilities = {
    sample_zones = true,
    key_range = true,
    velocity_range = true,
    root_key = true,
    sustain_loop = true,
    amp_envelope = true,
    amp_velocity_to_level = true,
    volume = true,
    pan = true,
    filter_cutoff = true,
    transpose = true,
    tune = true,
    pitch_keytrack = true,

    sample_offset = false,
    sample_end = false,
    pitch_bend = false,
    crossfade = false,
    random_selection = false,
    sequence_selection = false,
    filter_envelope = false,
    pitch_envelope = false,
}

local sampleZoneType = 1

local function has_method(value, name)
    if value == nil then
        return false
    end

    local ok, method = pcall(function()
        return value[name]
    end)
    return ok and type(method) == "function"
end

local function call_with_error(prefix, fn)
    local ok, err = pcall(fn)
    if ok then
        return true
    end

    return false, prefix .. ": " .. tostring(err)
end

local function sfz_inclusive_end_to_halion_marker(sample_index)
    if type(sample_index) == "number" then
        return sample_index + 1
    end

    return sample_index
end

function hb.ok(message, saved)
    return {
        ok = true,
        saved = saved or 1,
        failed = 0,
        message = message or "Built SFZ preset",
    }
end

function hb.fail(message, failed)
    return {
        ok = false,
        saved = 0,
        failed = failed or 1,
        message = message or "SFZ build failed",
    }
end

function hb.warn(ctx, message)
    if ctx and type(ctx.log) == "function" then
        ctx.log("Warning: " .. tostring(message))
    end
end

function hb.unsupported(ctx, field, reason)
    local message = "Unsupported SFZ field " .. tostring(field)
    if reason and reason ~= "" then
        message = message .. ": " .. tostring(reason)
    end

    hb.warn(ctx, message)
    return false, message
end

function hb.path_join(ctx, root, rel)
    if ctx and type(ctx.path_join) == "function" then
        return ctx.path_join(root, rel)
    end

    local base = tostring(root or "")
    local child = tostring(rel or "")
    if base == "" then
        return child
    end
    if child == "" then
        return base
    end
    if base:sub(-1) == "/" or base:sub(-1) == "\\" then
        return base .. child
    end
    return base .. "/" .. child
end

function hb.set_name_if_available(ctx, element, name)
    if not name or not has_method(element, "setName") then
        return true
    end

    local ok, err = call_with_error("Skipped name assignment", function()
        element:setName(name)
    end)
    if not ok then
        hb.warn(ctx, err)
    end
    return true
end

function hb.set_parameter_required(target, name, value)
    if target == nil then
        return false, "Failed to set required parameter " .. tostring(name) .. ": target is nil"
    end
    if name == nil or name == "" then
        return false, "Failed to set required parameter: name is empty"
    end
    if value == nil then
        return false, "Failed to set required parameter " .. tostring(name) .. ": value is nil"
    end

    return call_with_error("Failed to set required parameter " .. tostring(name), function()
        target:setParameter(name, value)
    end)
end

function hb.set_parameter_if_available(ctx, target, name, value)
    if target == nil or name == nil or name == "" or value == nil then
        return true
    end

    local ok, err = call_with_error("Skipped " .. tostring(name), function()
        target:setParameter(name, value)
    end)
    if not ok then
        hb.warn(ctx, err)
    end
    return true
end

function hb.assign_field_required(target, name, value)
    if target == nil then
        return false, "Failed to assign required field " .. tostring(name) .. ": target is nil"
    end
    if name == nil or name == "" then
        return false, "Failed to assign required field: name is empty"
    end
    if value == nil then
        return false, "Failed to assign required field " .. tostring(name) .. ": value is nil"
    end

    return call_with_error("Failed to assign required field " .. tostring(name), function()
        target[name] = value
    end)
end

function hb.assign_field_if_available(ctx, target, name, value)
    if target == nil or name == nil or name == "" or value == nil then
        return true
    end

    local ok, err = call_with_error("Skipped field " .. tostring(name), function()
        target[name] = value
    end)
    if not ok then
        hb.warn(ctx, err)
    end
    return true
end

function hb.create_layer(ctx, name)
    local layerOk, layer = pcall(function()
        return Layer()
    end)
    if not layerOk or layer == nil then
        return nil, "HALion Layer constructor is not available: " .. tostring(layer)
    end

    hb.set_name_if_available(ctx, layer, name)

    if not this or not this.parent or not has_method(this.parent, "appendLayer") then
        return nil, "HALion program tree parent is not available"
    end

    local ok, err = call_with_error("Failed to append layer", function()
        this.parent:appendLayer(layer)
    end)
    if not ok then
        return nil, err
    end

    -- SFZ's default amp velocity response uses a squared curve. In HALion this
    -- is controlled by the Main-section velocity curve on program/layer
    -- elements, not by the amp-envelope point curve. Probe 030 verified curve
    -- index 1 as HALion's Squared mode. Disable inheritance so generated SFZ
    -- layers do not depend on the parent program's current UI setting.
    hb.set_parameter_if_available(ctx, layer, "InheritVelocitySettings", false)
    hb.set_parameter_if_available(ctx, layer, "VelocityToLevelCurve", 1)

    return layer
end

function hb.create_sample_zone(ctx, layer, name)
    if layer == nil or not has_method(layer, "appendZone") then
        return nil, "HALion layer cannot accept sample zones"
    end

    local zoneOk, zone = pcall(function()
        return Zone()
    end)
    if not zoneOk or zone == nil then
        return nil, "HALion Zone constructor is not available: " .. tostring(zone)
    end

    hb.set_name_if_available(ctx, zone, name)

    local ok, err = hb.set_parameter_required(zone, "ZoneType", sampleZoneType)
    if not ok then
        return nil, err
    end

    ok, err = call_with_error("Failed to append sample zone", function()
        layer:appendZone(zone)
    end)
    if not ok then
        return nil, err
    end

    return zone
end

function hb.set_amp_envelope_required(zone, envelope)
    if type(envelope) ~= "table" or type(envelope.points) ~= "table" then
        return false, "Failed to set required amp envelope: envelope points are missing"
    end

    return call_with_error("Failed to set required amp envelope", function()
        local points = zone:getParameter("Amp Env.EnvelopePoints")
        if type(points) ~= "table" or #points == 0 then
            error("Amp Env.EnvelopePoints did not return an editable point table")
        end

        while #points > #envelope.points do
            removeEnvelopePoint(points, #points)
        end

        while #points < #envelope.points do
            insertEnvelopePoint(points, #points, 0, 0, 0)
        end

        for i, source in ipairs(envelope.points) do
            points[i].level = source.level
            points[i].duration = source.duration
            points[i].curve = source.curve
        end

        zone:setParameter("Amp Env.EnvelopePoints", points)
        zone:setParameter("Amp Env.SustainIndex", envelope.sustain_index)
    end)
end

function hb.sample_path(region)
    local playback = region and region.sample_playback or nil
    if type(playback) == "table" and playback.sample then
        return playback.sample
    end

    return region and region.sample or nil
end

function hb.region_mapping(region)
    local mapping = region and region.mapping or nil
    if type(mapping) == "table" then
        return mapping
    end

    return {
        key_low = region and region.lokey or nil,
        key_high = region and region.hikey or nil,
        velocity_low = region and region.lovel or nil,
        velocity_high = region and region.hivel or nil,
        root_key = region and region.pitch_keycenter or nil,
    }
end

function hb.region_label(region)
    return tostring((region and region.name) or hb.sample_path(region) or "region")
end

function hb.apply_required_sample_fields(zone, region)
    local mapping = hb.region_mapping(region)

    local ok, err = hb.set_parameter_required(zone, "SampleOsc.Filename", hb.sample_path(region))
    if not ok then return false, err end

    ok, err = hb.set_parameter_required(zone, "SampleOsc.Rootkey", mapping.root_key)
    if not ok then return false, err end

    ok, err = hb.set_amp_envelope_required(zone, region.amp_envelope)
    if not ok then return false, err end

    return true
end

function hb.apply_optional_sample_fields(ctx, zone, region)
    local loop = region and region.loop or nil
    local loop_start = type(loop) == "table" and loop.start or region and region.loop_start
    local loop_end = type(loop) == "table" and (loop.finish or loop["end"]) or region and region.loop_end
    if loop_start and loop_end then
        local halion_loop_end = sfz_inclusive_end_to_halion_marker(loop_end)
        -- SFZ loop_end is the last sample played in the loop. HALion's sample
        -- and loop end fields are marker positions, so a 200-sample cycle with
        -- SFZ loop_end=199 must be written as end marker 200. Setting both
        -- SampleEnd and SustainLoopEndA avoids a correct-sounding loop marker
        -- escaping at the sample boundary.
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.SampleEnd", halion_loop_end)
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopModeA", 1)
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopStartA", loop_start)
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopEndA", halion_loop_end)
    end

    local gain = region and region.gain or nil
    local sample_osc_level_db = type(gain) == "table" and gain.sample_osc_level_db or region and region.sample_osc_level_db
    if sample_osc_level_db then
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.Level", sample_osc_level_db)
    end

    local amp_velocity_to_level = type(gain) == "table" and gain.amp_velocity_to_level or region and region.amp_velocity_to_level
    if amp_velocity_to_level then
        hb.set_parameter_if_available(ctx, zone, "Amp Env.VelocityToLevel", amp_velocity_to_level)
    end

    local amp_pan = type(gain) == "table" and gain.amp_pan or region and region.amp_pan
    if amp_pan then
        hb.set_parameter_if_available(ctx, zone, "Amp.Pan", amp_pan)
    end

    local filter = region and region.filter or nil
    local filter_cutoff = type(filter) == "table" and filter.cutoff or region and region.filter_cutoff
    if filter_cutoff then
        hb.set_parameter_if_available(ctx, zone, "Filter.Cutoff", filter_cutoff)
    end

    local pitch = region and region.pitch or nil
    if type(pitch) == "table" then
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.Tune", pitch.tune_cents)
        if pitch.keytrack ~= nil then
            local mapping = hb.region_mapping(region)
            hb.set_parameter_if_available(ctx, zone, "Pitch.CenterKey", mapping.root_key)
            hb.set_parameter_if_available(ctx, zone, "Pitch.KeyFollow", pitch.keytrack)
        end
    end

    return true
end

function hb.apply_required_mapping_fields(zone, region)
    local mapping = hb.region_mapping(region)

    local ok, err = hb.assign_field_required(zone, "keyLow", mapping.key_low)
    if not ok then return false, err end

    ok, err = hb.assign_field_required(zone, "keyHigh", mapping.key_high)
    if not ok then return false, err end

    ok, err = hb.assign_field_required(zone, "velLow", mapping.velocity_low)
    if not ok then return false, err end

    ok, err = hb.assign_field_required(zone, "velHigh", mapping.velocity_high)
    if not ok then return false, err end

    return true
end

function hb.append_sample_zone(ctx, layer, region)
    local zone, err = hb.create_sample_zone(ctx, layer, region.name)
    if not zone then return nil, err end

    local ok
    ok, err = hb.apply_required_sample_fields(zone, region)
    if not ok then return nil, err end

    hb.apply_optional_sample_fields(ctx, zone, region)

    -- HALion can read sampler metadata while assigning SampleOsc.Filename.
    -- Writing the playable ranges last keeps the generated SFZ mapping in
    -- control even when the audio file carries its own sampler chunk.
    ok, err = hb.apply_required_mapping_fields(zone, region)
    if not ok then return nil, err end

    return zone
end

function hb.save_layer_preset(ctx, layer, output_file)
    if not ctx or type(ctx.save_preset) ~= "function" then
        return false, "Build context cannot save presets"
    end

    local output_path = hb.path_join(ctx, ctx.script_dir, output_file)
    local saved = ctx.save_preset(output_path, layer, "H7")
    if not saved then
        return false, "Failed to save " .. output_path
    end

    return true, output_path
end

function hb.apply_crossfade(ctx)
    return hb.unsupported(ctx, "crossfade", "crossfade behavior has not been verified for HALion output")
end

function hb.apply_selection(ctx)
    return hb.unsupported(ctx, "selection", "random and sequence selection behavior has not been implemented")
end

function hb.apply_filter_envelope(ctx)
    return hb.unsupported(ctx, "filter_envelope", "filter envelope behavior has not been verified")
end

function hb.apply_pitch_features(ctx)
    return hb.unsupported(ctx, "pitch", "dynamic pitch mapping behavior has not been verified")
end

return hb
