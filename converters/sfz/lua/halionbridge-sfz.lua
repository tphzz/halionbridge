-- Generated SFZ helper for halionbridge convert sfz.
--
-- This module is converter-owned support code, not part of the generic
-- halionbridge Lua builder API. Generated SFZ build scripts may require it
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
    filter_cutoff = true,

    sample_offset = false,
    sample_end = false,
    transpose = false,
    tune = false,
    volume = false,
    pan = false,
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
    if type(Layer) ~= "function" then
        return nil, "HALion Layer constructor is not available"
    end

    local layer = Layer()
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

    return layer
end

function hb.create_sample_zone(ctx, layer, name)
    if type(Zone) ~= "function" then
        return nil, "HALion Zone constructor is not available"
    end
    if layer == nil or not has_method(layer, "appendZone") then
        return nil, "HALion layer cannot accept sample zones"
    end

    local zone = Zone()
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

function hb.apply_required_sample_fields(zone, region)
    local ok, err = hb.set_parameter_required(zone, "SampleOsc.Filename", region.sample)
    if not ok then return false, err end

    ok, err = hb.set_parameter_required(zone, "SampleOsc.Rootkey", region.pitch_keycenter)
    if not ok then return false, err end

    ok, err = hb.set_amp_envelope_required(zone, region.amp_envelope)
    if not ok then return false, err end

    return true
end

function hb.apply_optional_sample_fields(ctx, zone, region)
    if region.loop_start and region.loop_end then
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopModeA", 1)
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopStartA", region.loop_start)
        hb.set_parameter_if_available(ctx, zone, "SampleOsc.SustainLoopEndA", region.loop_end)
    end

    if region.amp_velocity_to_level then
        hb.set_parameter_if_available(ctx, zone, "Amp Env.VelocityToLevel", region.amp_velocity_to_level)
    end

    if region.filter_cutoff then
        hb.set_parameter_if_available(ctx, zone, "Filter.Cutoff", region.filter_cutoff)
    end

    return true
end

function hb.apply_required_mapping_fields(zone, region)
    local ok, err = hb.assign_field_required(zone, "keyLow", region.lokey)
    if not ok then return false, err end

    ok, err = hb.assign_field_required(zone, "keyHigh", region.hikey)
    if not ok then return false, err end

    ok, err = hb.assign_field_required(zone, "velLow", region.lovel)
    if not ok then return false, err end

    ok, err = hb.assign_field_required(zone, "velHigh", region.hivel)
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
    return hb.unsupported(ctx, "pitch", "pitch mapping behavior has not been verified")
end

return hb
