-- Cheeky Foveated DLSS UEVR plugin. The C++ runtime owns validation and all GPU work.
local protocol = 1
local command_event = "cheeky.foveated_dlss.command.v1"
local snapshot_event = "cheeky.foveated_dlss.snapshot.v1"
local status, draft, dirty = nil, {}, {}
local request, pending_apply, frame = 0, nil, 0
local error_text = nil
local last_snapshot_frame = 0
local ready_edits, slider_edits = {}, {}

local load_event = "cheeky.foveated_dlss.load.v1"
local load_state_event = "cheeky.foveated_dlss.load_state.v1"
local config_filename = "cheeky.txt"
local loader_state, loader_error = nil, nil
local enabled = false
local ok, config = pcall(fs.read, config_filename)
if ok and type(config) == "string" then
    -- Only an explicit valid opt-in enables loading. Malformed values stay off.
    enabled = config:match("^%s*disableMod%s*=%s*([01])%s*$") == "0"
end

local function request_load()
    uevr.api:dispatch_custom_event(load_event, enabled and "true" or "false")
end

local function clear_runtime_ui()
    status, draft, dirty = nil, {}, {}
    ready_edits, slider_edits, pending_apply = {}, {}, nil
    error_text = nil
end

local function set_enabled(value)
    local contents = value and "disableMod=0\n" or "disableMod=1\n"
    local written = pcall(fs.write, config_filename, contents)
    local read_ok, saved = pcall(fs.read, config_filename)
    if not written or not read_ok or saved ~= contents then
        loader_error = "Could not save this game's Cheeky enable setting. Check the UEVR profile directory."
        if not value then
            -- Always allow stopping this session, even on a read-only profile.
            enabled, loader_state = false, nil
            clear_runtime_ui()
            request_load()
            loader_error = loader_error .. " Disabled for this session only."
        end
        return
    end
    enabled, loader_state, loader_error = value, nil, nil
    clear_runtime_ui()
    request_load()
end

local function send(action, changes)
    request = request + 1
    local lines = {tostring(protocol), tostring(request), action}
    if changes then
        for key, value in pairs(changes) do
            local text = type(value) == "boolean" and (value and "true" or "false") or tostring(value)
            lines[#lines + 1] = key .. "=" .. text
        end
    end
    uevr.api:dispatch_custom_event(command_event, table.concat(lines, "\n"))
    return request
end

local function flush_edits()
    if pending_apply or not status or not next(ready_edits) then return end
    local submitted = ready_edits
    ready_edits = {}
    pending_apply = {id = send("set", submitted), values = submitted}
end

uevr.sdk.callbacks.on_lua_event(function(event, text)
    if event == load_state_event then
        if text ~= "enabled" and text ~= "disabled" and text ~= "failed" then return end
        loader_state = text
        if text == "disabled" then clear_runtime_ui() end
        return
    end
    if event ~= snapshot_event or not enabled then return end
    local ok, value = pcall(json.load_string, text)
    if not ok or type(value) ~= "table" or value.protocol ~= protocol or type(value.settings) ~= "table" then
        error_text = "Cheeky UI/runtime version mismatch or invalid response. Install the matching files."
        return
    end
    status = value
    -- A reconnect can reach an older runtime. Drop unsupported optional drafts
    -- before automatic flush or Apply can resend them to that runtime.
    for _, key in ipairs({"FgReleasePoint", "FgPacingClock", "FgLatencyDiagnostics", "FgDirectXrOutput", "FgWaitForCompletion", "FgResolutionPercent", "FgPeripheralPercent", "FgPeripheralMethod", "FgCenterMethod", "FgEnabled", "FgFoveated", "FgMatchSrRegion", "FgShowBorder", "FgWidth", "FgHeight", "FgXOffset", "FgYOffset", "FgFeather", "FixMotionBlur", "NrProcessingOrder", "AfwManualCoverage", "AfwAutomaticCoverage", "AfwWarpMargin", "EyeCalibrationContinuous", "EyeCalibrationMethod"}) do
        if value.settings[key] == nil then
            draft[key], dirty[key] = nil, nil
            ready_edits[key], slider_edits[key] = nil, nil
            if pending_apply and pending_apply.values[key] ~= nil then
                pending_apply.values[key] = nil
                if not next(pending_apply.values) and not pending_apply.reset_group then pending_apply = nil end
            end
        end
    end
    last_snapshot_frame = frame
    error_text = nil
    if pending_apply and value.request == pending_apply.id then
        -- Keep drafts after a rejected transaction so the user can correct it.
        if value.applied_request == pending_apply.id then
            for key, sent in pairs(pending_apply.values) do
                if dirty[key] == sent then dirty[key] = nil end
            end
        end
        pending_apply = nil
    end
    for key, v in pairs(value.settings) do
        if dirty[key] == nil then draft[key] = v end
    end
end)

uevr.sdk.callbacks.on_frame(function()
    frame = frame + 1
    -- Retry/acknowledge the desired state even if Lua started before the plugin,
    -- or the thin adapter was reloaded. Duplicate requests do not reload it.
    if frame % 120 == 1 then request_load() end
    if not enabled or loader_state ~= "enabled" then return end
    if status and frame - last_snapshot_frame > 600 then
        status, pending_apply = nil, nil
    end
    if not status and frame % 120 == 1 then send("get") end
    flush_edits()
end)

local function check(label, key)
    local changed, value = imgui.checkbox(label, draft[key] == true)
    if changed then draft[key], dirty[key], ready_edits[key] = value, value, value end
end
local function slider(label, key, minimum, maximum, integer)
    local changed, value
    if integer then changed, value = imgui.slider_int(label, draft[key] or minimum, minimum, maximum)
    else changed, value = imgui.slider_float(label, draft[key] or minimum, minimum, maximum, "%.3f") end
    -- UEVR exposes is_item_active, but not IsItemDeactivatedAfterEdit.
    -- Read it immediately after this slider. A stationary held slider remains
    -- active: neither a changed-value event nor an idle timer means release.
    local active = imgui.is_item_active()
    if changed then
        draft[key], dirty[key], slider_edits[key] = value, value, true
        ready_edits[key] = nil
    end
    if slider_edits[key] and not active then
        ready_edits[key], slider_edits[key] = draft[key], nil
    end
end
local function combo(label, key, values)
    -- UEVR's Lua combo preserves table keys: use the actual settings enum values.
    local changed, value = imgui.combo(label, draft[key], values)
    if changed then draft[key], dirty[key], ready_edits[key] = value, value, value end
end
local function text(s) imgui.text(tostring(s)) end
local function donate_button()
    -- Do not remove this donation button or change its original destination when
    -- this project is forked or integrated into another project.
    -- The native donate command opens https://linktr.ee/clarkcheekykent.
    local styled = imgui.push_style_color and imgui.pop_style_color
    if styled then
        -- UEVR accepts ImGui color indices and signed packed ABGR colors.
        imgui.push_style_color(21, 0xFF553DD6 - 0x100000000) -- Button
        imgui.push_style_color(22, 0xFF664DE8 - 0x100000000) -- ButtonHovered
        imgui.push_style_color(23, 0xFF422BB8 - 0x100000000) -- ButtonActive
        imgui.push_style_color(0, -1) -- Text: white
    end
    local rounded = imgui.push_style_var and imgui.pop_style_var and imgui.ImGuiStyleVar
    local font = imgui.calc_text_size and imgui.calc_text_size("Donate").y or 17
    if rounded then imgui.push_style_var(imgui.ImGuiStyleVar.FrameRounding, font * 0.3) end
    local heart = imgui.get_cursor_screen_pos and imgui.draw_list_path_clear and
        imgui.draw_list_path_line_to and imgui.draw_list_path_stroke
    local origin = heart and imgui.get_cursor_screen_pos()
    local clicked = imgui.button("Donate", {font * 6.6, font * 1.7})
    if heart then
        local size = font * 0.95
        imgui.draw_list_path_clear()
        for i = 0, 39 do
            local t = i * math.pi * 2 / 40
            local x = 0.5 + 0.5 * math.sin(t)^3
            local y = (12 - (13 * math.cos(t) - 5 * math.cos(2*t) - 2 * math.cos(3*t) - math.cos(4*t))) / 29
            imgui.draw_list_path_line_to({origin.x + font * 0.4 + size * x,
                origin.y + (font * 1.7 - size) * 0.5 + size * y})
        end
        imgui.draw_list_path_stroke(0xFFFFFFFF, true, font * 0.11)
    end
    if rounded then imgui.pop_style_var(1) end
    if styled then imgui.pop_style_color(4) end
    if imgui.is_item_hovered and imgui.is_item_hovered() then
        imgui.set_tooltip("https://linktr.ee/clarkcheekykent")
    end
    if clicked then send("donate") end
end
local function apply_buttons(id)
    if imgui.button("Apply changes##" .. id) then
        if next(dirty) and not pending_apply then
            local submitted = {}
            for key, value in pairs(dirty) do submitted[key] = value end
            ready_edits, slider_edits = {}, {}
            pending_apply = {id = send("set", submitted), values = submitted}
        end
    end
    imgui.same_line()
    if imgui.button("Discard edits##" .. id) then
        dirty, ready_edits, slider_edits = {}, {}, {}
        if status then for k, v in pairs(status.settings) do draft[k] = v end end
    end
    if next(dirty) then text("Edits are pending. Sliders apply on release; failed edits can be retried with Apply.") end
    if pending_apply then text("Waiting for settings acknowledgement...") end
end


local function section(label)
    imgui.spacing()
    imgui.text_colored(label, 0xFF8FC9EC)
end
local function rows(id, entries)
    -- UEVR's documented table binding; keep a plain-text fallback for older hosts.
    if not imgui.begin_table then
        for _, row in ipairs(entries) do text(row[1] .. ": " .. tostring(row[2])) end
    elseif imgui.begin_table(id, 2, 0, {0, 0}, 0) then
        for _, row in ipairs(entries) do
            imgui.table_next_row(0, 0)
            imgui.table_next_column(); text(row[1])
            imgui.table_next_column(); text(row[2])
        end
        imgui.end_table()
    end
end
local function yes(value) return value and "Yes" or "No" end
local function size(w, h)
    if not w or not h or w == 0 or h == 0 then return "Not sampled yet" end
    return string.format("%d x %d", w, h)
end
local function timing(ms)
    return ms and ms > 0 and string.format("%.3f ms", ms) or "Not sampled / unavailable"
end
local function fps(ms)
    return ms and ms > 0 and string.format("%.1f FPS  |  %.2f ms", 1000 / ms, ms) or "Not sampled yet"
end
local function saving(full, reduced)
    if not full or not reduced or full <= 0 or reduced <= 0 then return "Sample both modes to compare" end
    return string.format("%.3f ms (%.1f%%)", full - reduced, 100 * (full - reduced) / full)
end
local function reset_group(label, group)
    imgui.begin_disabled(pending_apply ~= nil or (group ~= "all" and not status.setting_groups))
    if imgui.button(label) and not pending_apply then
        local submitted = {}
        for key, value in pairs(dirty) do
            if group == "all" or status.setting_groups[key] == group then submitted[key] = value end
        end
        for key in pairs(draft) do
            if group == "all" or status.setting_groups[key] == group then
                ready_edits[key], slider_edits[key] = nil, nil
            end
        end
        pending_apply = {id = send(group == "all" and "defaults" or "defaults_" .. group),
            values = submitted, reset_group = group}
    end
    imgui.end_disabled()
end
local alignment = {[0]="Manual fallback", [1]="Streamline projection", [2]="OpenXR", [3]="OpenVR", [4]="LibOVR"}

local function fg_resource_memory(id, r)
    if not r then text("Update the plugin/runtime for the FG resource memory breakdown."); return end
    if not imgui.tree_node("FG resource memory breakdown##" .. id) then return end
    local c = r.categories or {}
    local function mib(bytes) return string.format("%.1f MiB", (bytes or 0) / 1048576) end
    local entries = {}
    local function item(label, key, available)
        local b = c[key] or {}
        local value = available and (mib(b.bytes) .. string.format(" / %.0f resources",b.resources or 0)) or "Unavailable / awaiting sample"
        if available and (b.unknown_resources or 0)>0 then value=value .. string.format("; %.0f unsized",b.unknown_resources) end
        entries[#entries+1] = {label,value}
    end
    local capture = r.layer_supported and (not r.presenter_present or (r.capture_tick or 0)>0)
    local worker = r.layer_supported and (not r.presenter_present or (r.worker_tick or 0)>0)
    local presenter = r.layer_supported and (not r.presenter_present or (r.presenter_tick or 0)>0)
    item("Native color / capture pool","capture",capture)
    item("Generated output pool","generated",worker)
    item("Upload transport pool","upload_pool",presenter)
    item("Captured depth guides (raw/prepared)","guide_depth",r.guides_supported)
    item("Captured motion guides (raw/prepared)","guide_motion",r.guides_supported)
    item("Prepared FG color","prepared_color",worker)
    item("Worker-owned prepared depth","prepared_depth",worker)
    item("Worker-owned prepared motion","prepared_motion",worker)
    item("NGX output textures + flags","ngx_outputs",worker)
    item("Periphery + depth history","periphery",worker)
    item("Composition scratch","composition",worker)
    item("Shared scene XR images (estimated)","swapchain_native",presenter)
    item("Separate generated XR images (estimated)","swapchain_generated",presenter)
    item("Auxiliary XR images (estimated)","swapchain_auxiliary",presenter)
    rows("fg_memory_categories_" .. id,entries)
    rows("fg_memory_totals_" .. id,{
        {"Known app FG resources",mib(r.app_resource_bytes)},
        {"XR image allocation estimates",mib(r.swapchain_estimated_bytes)},
        {"Known resources + XR estimates",mib(r.known_resource_bytes)},
        {"Reusable capture / output pool",mib((c.capture or {}).reusable_bytes) .. " / " .. mib((c.generated or {}).reusable_bytes)},
        {"NVIDIA internal / driver memory","Not measured"}
    })
    if not r.layer_supported then text("Partial breakdown: update OpenXR support for presenter resources.") end
    if (r.unknown_resources or 0)>0 then text("Some resource sizes are unavailable; totals omit those sizes.") end
    text("Unique resource allocation sizes, not resident VRAM. XR images are estimates; borrowed game textures are excluded.")
    text("Prepared guide captures are counted in captured rows; borrowed guides add no worker-owned allocation.")
    text("Reusable pools are a subset of the totals. They are retained allocations, not extra copies per reference.")
    text("Does not measure NVIDIA internal workspace, command/descriptor storage or driver overhead; do not subtract from DXGI usage to infer NVIDIA's share.")
    imgui.tree_pop()
end

local function gpu_memory(id)
    section("GPU memory (game process)")
    local m = status.gpu_memory or {}
    local l, n, b = m["local"] or {}, m.nonlocal or {}, m.baseline or {}
    local function mib(bytes) return string.format("%.1f MiB", (bytes or 0) / 1048576) end
    local function delta(bytes) return string.format("%+.1f MiB", (bytes or 0) / 1048576) end
    if not m.supported then
        text("GPU memory query unavailable in this runtime or renderer.")
        return
    end
    rows("gpu_memory_" .. id, {
        {"GPU adapter", m.adapter or "Unknown"},
        {"Local usage / budget", l.valid and (mib(l.usage_bytes) .. " / " .. mib(l.budget_bytes)) or "Unavailable"},
        {"Local budget headroom", l.valid and mib(l.headroom_bytes) or "Unavailable"},
        {"Local sampled peak", l.valid and mib(l.peak_bytes) or "Unavailable"},
        {"Samples over local budget", l.valid and string.format("%.0f",l.over_budget_samples or 0) or "Unavailable"},
        {"Shared / nonlocal usage", n.valid and mib(n.usage_bytes) or "Unavailable"},
        {"Shared / nonlocal budget", n.valid and mib(n.budget_bytes) or "Unavailable"},
        {"Dedicated adapter capacity", (m.dedicated_capacity_bytes or 0)>0 and mib(m.dedicated_capacity_bytes) or "Unavailable / shared-memory GPU"}
    })
    if l.valid and (l.over_budget_bytes or 0)>0 then text("OVER LOCAL BUDGET by " .. mib(l.over_budget_bytes)) end
    if b.valid then
        rows("gpu_memory_baseline_" .. id, {
            {"Baseline FG setting", b.fg_enabled and "ON" or "OFF"},
            {"Local change from baseline", l.valid and delta(b.local_delta_bytes) or "Unavailable"},
            {"Shared change from baseline", n.valid and b.nonlocal_valid and delta(b.nonlocal_delta_bytes) or "Unavailable"}
        })
    else text("No VRAM baseline marked.") end
    imgui.begin_disabled(not l.valid)
    if imgui.button("Mark VRAM baseline##" .. id) then send("mark_vram_baseline") end
    imgui.end_disabled()
    text("Sampled once per second. Local memory is VRAM on discrete GPUs; budget is assigned by Windows.")
    text("Includes the game, Cheeky and in-process runtime allocations. Excludes other processes; capacity is not free VRAM.")
    text("Mark a baseline with FG OFF, then enable FG in the same scene. The change also includes game allocations.")
    fg_resource_memory(id,m.fg_resources)
end

local function performance(d, f)
    gpu_memory("performance")
    section("Frame rate comparison")
    rows("fps", {{"SR enabled", fps(f.sr_enabled_ms)}, {"SR disabled", fps(f.sr_disabled_ms)},
        {"Frame time savings", saving(f.sr_disabled_ms, f.sr_enabled_ms)}})
    text("Toggle SR in the same scene. These measure UEVR present callbacks; headset FPS may differ.")

    section("GPU time (250 ms average)")
    local quad = status.quad_views and status.quad_views.active
    local times = {{quad and "Original DLSS" or "Full DLSS", timing(d.native_ms)}}
    if quad then table.insert(times, {"Original peripheral DLSS", timing(d.native_peripheral_ms)}) end
    table.insert(times, {"Foveated DLSS", timing(d.foveated_ms)})
    table.insert(times, {"Peripheral DLSS", timing(d.peripheral_ms)})
    local settings = status.settings or {}
    if status.renderer == 1 and settings.NrEnabled then
        local before = settings.NrProcessingOrder == 1
        local metric = settings.NrFoveated and "nr_foveated_ms" or "nr_full_ms"
        if before then metric = settings.NrFoveated and "before_nr_foveated_ms" or "before_nr_full_ms" end
        times[#times + 1] = {before and "NR + preparation (Before)" or "NR (After)", timing(d[metric])}
    end
    rows("gpu_time", times)
    text("Samples are retained when a mode is disabled. DLSS timings exclude other game work.")

    section("Resolution")
    local c = d.crop or {}
    rows("resolution", {
        {"Game input / output", size(d.input_width, d.input_height) .. " / " .. size(d.output_width, d.output_height)},
        {"Center DLSS input / output", size(c.input_width, c.input_height) .. " / " .. size(c.output_width, c.output_height)}
    })
    if status.renderer == 1 and settings.NrEnabled then
        local n = status.nr_details or {}
        rows("nr_resolution", {{"NR working size", size(n.working_width, n.working_height)}})
    end
end

local function afw_mode(afw)
    if afw.rendering_mode_known then return afw.rendering_mode == 3 and "Selected" or "Not selected" end
    return afw.coverage_enabled == false and "Not selected" or "Mode unavailable"
end

local function afw_controls()
    if draft.AfwManualCoverage == nil then return end
    local gaze = draft.CenterMode ~= 0
    local mode = draft.AfwAutomaticCoverage and 2 or draft.AfwManualCoverage and 1 or 0
    local choices = {[0]="Per-eye (calibrated)",[1]="Manual"}
    if draft.AfwAutomaticCoverage ~= nil then choices[2] = "Automatic" end
    local changed, value = imgui.combo(gaze and "AFW tracking-loss fallback" or "AFW stereo coverage", mode, choices)
    if changed then
        -- Commit both legacy flags together, preserving existing preferences
        -- until the user selects a mode. Automatic still wins in older INIs.
        mode = value
        local edits = {AfwManualCoverage = value == 1}
        if draft.AfwAutomaticCoverage ~= nil then edits.AfwAutomaticCoverage = value == 2 end
        for key, enabled in pairs(edits) do draft[key], dirty[key], ready_edits[key] = enabled, enabled, enabled end
    end
    if mode == 2 then
        -- Height is edited once in Foveation placement below.
    elseif mode == 1 then
        slider(gaze and "Fallback stereo X offset" or "Stereo X offset", "XOffset", -1, 1)
    end
    if mode == 0 then text("Uses the configured region and calibrated eye alignment.")
    elseif gaze then text("Gaze follows both eyes. The fallback above applies when tracking is unavailable.") end
    if (gaze or mode ~= 0) and draft.AfwWarpMargin ~= nil and imgui.tree_node("Advanced AFW") then
        slider("Extra margin per edge", "AfwWarpMargin", 0, 0.25)
        text("Adds a fixed fraction of the full image around each eye's region. Larger margins cost more GPU time.")
        imgui.tree_pop()
    end
end

uevr.lua.add_script_panel("Cheeky Foveated DLSS", function()
    local changed, value = imgui.checkbox("Enable Cheeky for this game", enabled)
    if changed then set_enabled(value) end
    if loader_error then text(loader_error) end
    if not enabled then
        text("Disabled. After first activation, hook code stays resident until application exit; processing is detached.")
        return
    end
    if loader_state == "failed" then
        text("Cheeky could not start. Check the runtime message below and the log; missing files require the complete matching package.")
        if not status then return end
    elseif loader_state ~= "enabled" then
        text("Waiting for the Cheeky plugin. Install the matching plugin and Lua script.")
        return
    end

    if not imgui.tree_node("Cheeky Foveated DLSS") then return end
    if error_text then text(error_text) end
    if not status then
        text("Waiting for the Cheeky native plugin.")
        text("Extract the complete UEVR package into this game's configuration folder.")
        text("Check PluginLoader and the UEVR log for DLL/API errors.")
        if imgui.button("Reconnect") then send("get") end
        imgui.tree_pop()
        return
    end
    if imgui.table_setup_column and imgui.ColumnFlags and imgui.begin_table("cheeky_header", 2, 0, {0, 0}, 0) then
        imgui.table_setup_column("Version", imgui.ColumnFlags.WidthStretch, 1.0, 0)
        local font = imgui.calc_text_size("Donate").y
        imgui.table_setup_column("Support", imgui.ColumnFlags.WidthFixed, font * 6.6, 0)
        imgui.table_next_row(0, 0)
        imgui.table_next_column(); text("Cheeky " .. tostring(status.version))
        imgui.table_next_column(); donate_button()
        imgui.end_table()
    else
        text("Cheeky " .. tostring(status.version))
        imgui.same_line(); donate_button()
    end
    local message = tostring(status.message or "")
    if message ~= "" and message ~= "Settings applied" and not message:match("^Ready%.") then text(message) end
    local afw = status.afw_experiment or {}
    local afw_available = afw.enabled and afw.coverage_enabled ~= false
    local afw_active = afw_available and (draft.AfwAutomaticCoverage or draft.AfwManualCoverage)
    if not status.ready then text("Processing is paused.") end
    text("Alt+Shift+/ toggles SR.")
    local d = (status.apis or {})[status.renderer == 1 and 2 or 1] or {}
    local f = status.frame or {}
    local fg_state = status.fg or {}
    local fg_timeline = fg_state.pacing_timeline or {}
    local fg_cadence = fg_state.cadence or {}
    local native_fg_mode = afw.rendering_mode_known and afw.rendering_mode == 0
    local fg_status_text = "Disabled"
    if not native_fg_mode then
        fg_status_text = afw.rendering_mode_known and "Paused - Native Stereo required" or "Waiting for UEVR rendering mode"
    elseif draft.FgEnabled then
        fg_status_text = (fg_timeline.supported and fg_timeline.recent_generated) and
            string.format("Generating (%.1f FG FPS)",fg_cadence.generated_fps or 0) or "No recent generated frames"
    end
    rows("overview", {{"Renderer", status.renderer == 1 and "DX12" or "DX11 direct"},
        {"SR status", d.state or "Waiting"}, {"NR status", status.nr or "Waiting"},
        {"DLSS-FG status", fg_status_text},
        {"UEVR present cadence", fps(f.present_ms)}})
    if afw.enabled then rows("afw_overview", {{"AFW", afw_mode(afw)}}) end
    -- A reset is a native transaction. Wait for its authoritative snapshot before editing again.
    imgui.begin_disabled(pending_apply ~= nil and pending_apply.reset_group ~= nil)

    if imgui.tree_node("Stereo and gaze") then
        combo("Foveation center", "CenterMode", {[0]="Fixed",[1]="Runtime gaze (OpenXR / OpenVR)",[2]="Simulated gaze"})
        if afw_available then
            afw_controls()
            afw_active = draft.AfwAutomaticCoverage or draft.AfwManualCoverage
        end
        section("Foveation placement")
        local aligned_height = afw_active and draft.AfwAutomaticCoverage or (not afw_active and draft.AutoStereoAlignment)
        slider("Height offset", aligned_height and "AlignedHeightOffset" or "HeightOffset", -1, 1)
        text("Moves fixed and eye-tracked placement. Negative = up; positive = down.")
        if afw_active then
            if draft.AfwAutomaticCoverage and afw.coverage_mode ~= 2 and afw.coverage_mode ~= 3 then
                text("Automatic coverage is waiting for matching UEVR projections; using centered fallback.")
            end
        else
            check("Automatic stereo alignment", "AutoStereoAlignment")
            if not draft.AutoStereoAlignment then slider("Manual stereo X offset", "XOffset", -1, 1) end
            if not draft.AutoStereoAlignment then check("Invert stereo eye order", "InvertStereoXOffset") end
        end
        if imgui.tree_node("Stereo Eye Mapping") then
            local c = status.eye_calibration or {}
            if status.settings.EyeCalibrationMethod ~= nil then
                local methods = {[0]="Auto",[1]="Standard corners",[2]="Timing tolerant corners",[3]="Full crop search",[4]="Manual"}
                local selected = draft.ManualStereoMapping and 4 or draft.EyeCalibrationMethod
                local changed, method = imgui.combo("Mapping method", selected, methods)
                if changed then
                    local edits = {ManualStereoMapping = method == 4}
                    if method ~= 4 then edits.EyeCalibrationMethod = method end
                    for key, value in pairs(edits) do draft[key], dirty[key], ready_edits[key] = value, value, value end
                end
                imgui.begin_disabled(not draft.ManualStereoMapping)
                check("First DLSS view is right eye", "ManualStereoRightFirst")
                imgui.end_disabled()
                if draft.ManualStereoMapping then
                    text("Manual: assumes full-eye images with matching XR coordinates.")
                    text("Off means left eye first. No crop, flip or projection changes.")
                    text("Check both eyes and image edges with the red alignment border.")
                end
            end
            if not draft.ManualStereoMapping then
                local methods = {[0]="Auto",[1]="Standard corners",[2]="Timing tolerant corners",[3]="Full crop search"}
                local learned = status.settings.EyeCalibrationLearnedMethod or 0
                text("Learned starting method: " .. (learned > 0 and methods[learned] or "Not learned yet"))
                text("Active method: " .. (c.active_method or "Waiting"))
                if status.settings.EyeCalibrationLearnedMethod == 2 and (status.settings.EyeCalibrationLearnedSessions or 0) < 2 then
                    text("Timing preference needs confirmation on another launch.")
                end
                if imgui.button("Reset learned calibration method") then send("calibration_forget") end
            end
            if not draft.ManualStereoMapping and status.settings.EyeCalibrationContinuous ~= nil then
                if draft.EyeCalibrationMethod == 3 or (draft.EyeCalibrationMethod == 0 and c.active_method == "Full crop search") then
                    check("Continuously validate eye calibration", "EyeCalibrationContinuous")
                    if draft.EyeCalibrationContinuous == false then
                        text("Recalibrates only when views, dimensions, submission bounds, or the VR session change. Same-view eye swaps and image crop changes are not detected.")
                    end
                end
                if c.enabled and imgui.button("Recalibrate now") then send("calibration_recalibrate") end
            end
            rows("eye_calibration", {{"Runtime", c.backend or "Waiting for VR"},
                {"Status", draft.ManualStereoMapping and "Manual (unverified full-eye layout)" or c.status or "Unavailable in this runtime"}})
            imgui.tree_pop()
        end
        section("Eye tracking")
        if draft.CenterMode == 2 then
            combo("Simulation pattern", "SimulationPattern", {[0]="Figure eight (8 s)",[1]="Slow sweep (20 s)",
                [2]="Jump every 2 s",[3]="Jump every 8 s",[4]="Tracking loss",[5]="Hold center"})
            text("Patterns restart when changed. Enable the red border to inspect motion.")
            if draft.SimulationPattern == 2 or draft.SimulationPattern == 3 then check("Show next jump target", "ShowNextJumpTarget") end
            if draft.SimulationPattern == 4 then text("Moves for 4 s, loses tracking for 1 s, then recovers.") end
        end
        if imgui.tree_node("OpenXR input compatibility") then
            check("Independent OpenXR gaze (restart application required)", "IndependentOpenXRGaze")
            text("For hosts without OpenXR input only. Attaches gaze after 90 frames.")
            text("Later host input initialization cannot attach until restart.")
            imgui.tree_pop()
        end
        if imgui.tree_node("Advanced eye tracking") then
            slider("Gaze smoothing (ms)", "GazeSmoothingMs", 0, 100)
            slider("Hold gaze on signal loss (ms)", "GazeHoldMs", 0, 1000)
            slider("Crop quantization (pixels)", "GazeQuantizationPixels", 1, 64, true)
            slider("Jump reset threshold", "GazeJumpResetRatio", 0.01, 1)
            imgui.tree_pop()
        end
        local g = status.gaze or {}
        if afw_active then
            if draft.CenterMode ~= 0 then rows("afw_gaze_summary", {{"Gaze driving foveation", yes(g.using_gaze)}}) end
        else
            rows("gaze_summary", {{"Alignment", alignment[g.alignment] or "Unknown"}, {"Gaze driving foveation", yes(g.using_gaze)},
                {"Mapped left / right", yes(g.left_mapped) .. " / " .. yes(g.right_mapped)}})
        end
        if draft.CenterMode == 1 and not g.using_gaze then
            imgui.text_colored(afw_active and "Eye tracking or UEVR projections unavailable; using fixed fallback." or "Eye tracking unavailable or awaiting mapping; using fixed fallback.", 0xFFFFBC70)
        end
        if draft.CenterMode ~= 0 or not afw_active then
            text("OpenXR alignment/gaze uses the matching Cheeky layer. Fixed alignment needs no eye tracker.")
        end
        reset_group("Reset Stereo / gaze defaults", "gaze")
        imgui.tree_pop()
    end

    local rr = d.reconstruction_feature == 13
    if imgui.tree_node(rr and "DLSS-RR" or "DLSS-SR") then
        if draft.D3D12LowerHook ~= nil then
            check("Use lower DLSS hook (DX12)", "D3D12LowerHook")
            text("Off selects the higher call. Restart the game after changing this.")
            text("Active DLSS hook: " .. (status.d3d12_lower_hook_active and "Lower" or "Higher"))
            if status.d3d12_hook_restart_required then text("DLSS hook change saved for next game restart.") end
        end
        check("Enable foveated DLSS-SR", "Enabled")
        if draft.Enabled then
            section("Center")
            if rr then combo("Center RR preset", "RrCenterPreset", {[0]="Game/default",[4]="D",[5]="E",[6]="F"})
            else combo("Center preset", "CenterPreset", {[0]="Game/default",[5]="E",[10]="J",[11]="K",[12]="L",[13]="M"}) end
            if not rr and draft.FixMotionBlur ~= nil then
                check("Fix motion-vector blur", "FixMotionBlur")
                text("(Use if preset causes blurriness)")
                text("DX12 / input-resolution motion vectors. Adds a GPU motion copy.")
            end
            slider("Center supersampling", "CenterSupersampling", 1, 2)
            slider("Fovea width", "Width", 0.2, 1)
            slider("Fovea height", "Height", 0.2, 1)
            if afw_active then text("AFW sizes are per eye. Stereo coverage and extra margin can enlarge the visible region.") end
            slider("Roundness", "Roundness", 0, 1)
            text("Cosmetic only; no performance impact.")
            slider("Transition width", "TransitionWidth", 0, 0.3)
            check("Show red alignment border", "AlignmentBorder")
            section("Periphery")
            check("Peripheral DLSS", "PeripheralDlaa")
            if draft.PeripheralDlaa then
                if rr then combo("Peripheral RR preset", "RrPeripheralPreset", {[0]="Game/default",[4]="D",[5]="E",[6]="F"})
                else combo("Peripheral preset", "PeripheralDlaaPreset", {[5]="E",[10]="J",[11]="K",[12]="L",[13]="M"}) end
                slider("Periphery scale", "PeripheralDlaaScale", 0.2, 1)
            end
        end
        reset_group("Reset DLSS-SR defaults", "sr")
        imgui.tree_pop()
    end

    if imgui.tree_node("DLSS-FG (experimental)") then
        if draft.FgEnabled == nil then
            text("Install the matching DLSS-FG plugin, runtime and Lua script.")
        elseif not native_fg_mode then
            text("DLSS-FG controls are available only in UEVR Native Stereo.")
            text("The enable preference is preserved and FG will resume automatically on return to Native Stereo.")
        else
            check("Enable DLSS Frame Generation (2x)", "FgEnabled")
            local fg = status.fg or {}
            if not fg.layer_present then
                text("OpenXR layer not loaded. Install the updated OpenXR support and restart the game.")
            elseif not fg.controls_supported then
                text("OpenXR layer needs updating. Run the matching installer and restart the game.")
            elseif not status.settings.FgEnabled then
                text("Status: Off")
            elseif not fg.compatible_session then
                text("Status: Waiting for a running D3D12 OpenXR stereo session")
            elseif not fg.active and fg.state == "Off" then
                text("Status: Starting at the next frame")
            else
                text("Status: " .. tostring(fg.state or "Waiting"))
                if fg.state ~= "Generating frames" and fg.diagnostic and fg.diagnostic ~= "" then
                    text(tostring(fg.diagnostic))
                end
            end
            do -- Settings stay editable while FG is off; measurements only exist while it runs.
                if draft.FgCenterMethod ~= nil then
                    combo("FG center method", "FgCenterMethod", {[0]="DLSS", [1]="Simple motion-vector warping", [2]="Bidirectional interpolation"})
                end
                local native_shader11 = status and status.renderer == 0 and (draft.FgCenterMethod or 0) > 0 and (not draft.FgFoveated or (draft.FgPeripheralMethod or 1) < 3)
                if draft.FgResolutionPercent ~= nil then
                    combo("FG scale", "FgResolutionPercent", {[25]="25%", [50]="50%", [75]="75%", [100]="100% native"})
                end
                check("Foveated frame generation", "FgFoveated")
                if draft.FgFoveated then
                    if draft.FgMatchSrRegion ~= nil then
                        check("Match DLSS-SR foveation region", "FgMatchSrRegion")
                        if draft.FgMatchSrRegion then
                            text("Uses SR region size, transition and per-eye placement (including gaze when resolved).")
                        end
                    end
                    if not draft.FgMatchSrRegion then
                        slider("FG region width", "FgWidth", 0.2, 1)
                        slider("FG region height", "FgHeight", 0.2, 1)
                        slider("FG horizontal offset", "FgXOffset", -0.5, 0.5)
                        slider("FG vertical offset", "FgYOffset", -1, 1)
                        slider("FG transition width", "FgFeather", 0, 0.3)
                    end
                    if draft.FgPeripheralPercent ~= nil then
                        combo("FG periphery scale", "FgPeripheralPercent", {[25]="25%", [50]="50%", [75]="75%", [100]="100% native"})
                        combo("FG periphery method", "FgPeripheralMethod", {[0]="None (previous frame)", [1]="Simple motion-vector warping", [2]="Bidirectional interpolation", [3]="DLSS"})
                    end
                    if draft.FgShowBorder ~= nil then check("Show blue FG region border", "FgShowBorder") end
                end
                if imgui.tree_node("Advanced settings") then
                    if draft.FgPacingClock ~= nil then
                        combo("FG pacing clock", "FgPacingClock", {
                            [0]="Independent display clock", [1]="Follow presented frames", [2]="Follow presented frames + vsync lock"})
                        if draft.FgPacingClock ~= 0 then
                            text("Experimental: the game is slowed to the rate frames actually reach the headset, so it does not render frames that would be dropped.")
                        end
                    end
                    if not native_shader11 then
                        if draft.FgQueuePriority ~= nil then
                            combo("FG GPU queue", "FgQueuePriority", {
                                [0]="Separate NORMAL queue", [1]="Separate HIGH queue", [2]="Shared game queue"})
                            text("Switches live after the previous FG job retires; recreates NGX histories and resets FG timings and FPS averages.")
                        end
                    end
                    if draft.FgReleasePoint ~= nil then
                        combo("Release game rendering", "FgReleasePoint", {
                            [0]="After FG submission", [4]="4 ms after FG submission", [1]="After GPU preparation", [2]="After FG completion", [3]="After generated frame submission", [5]="Never (display clock only)"})
                        if draft.FgReleasePoint == 5 then
                            text("Experimental: the game is paced only by the headset clock and never waits for frame generation.")
                        elseif draft.FgReleasePoint ~= 3 then
                            text("Experimental overlap: use After generated frame submission for serialized matching.")
                        end
                    elseif draft.FgWaitForCompletion ~= nil then
                        check("Wait for FG completion before next frame", "FgWaitForCompletion")
                        text("A/B test: holds the next native frame until both FG eyes finish. May reduce FPS.")
                    end
                    if not native_shader11 and draft.FgDirectXrOutput ~= nil then
                        combo("FG output destination", "FgDirectXrOutput", {[0]="Compact intermediate", [1]="Direct OpenXR image (experimental)"})
                        local direct = status and status.fg and status.fg.direct_output
                        if draft.FgDirectXrOutput == 1 and direct and direct.supported then
                            local paths = {[0]="Baseline / waiting", [1]="NGX + assembly direct to XR", [2]="NGX scratch; assembly direct to XR", [3]="Compact fallback"}
                            rows("fg_direct_output", {{"Last output path", paths[direct.last_path] or "Waiting"},
                                {"Direct frames / fallback jobs",string.format("%.0f / %.0f",direct.frames or 0,direct.fallbacks or 0)},
                                {"Last fallback reason",direct.reason_text or "None"}})
                        end
                    end
                    if not native_shader11 and draft.FgLatencyDiagnostics ~= nil then
                        combo("FG latency diagnostics", "FgLatencyDiagnostics", {[0]="Off", [1]="Calibrated timeline (1 in 17 jobs)"})
                    end
                    imgui.tree_pop()
                end
                if draft.FgEnabled then
                rows("fg_frames", {{"Total frames (native / generated / fallback)",
                    string.format("%.0f / %.0f / %.0f",fg.native_frames or 0,fg.generated_frames or 0,fg.fallback_frames or 0)}})
                local p = fg.performance
                local cadence = fg.cadence
                if cadence and cadence.supported then
                    if cadence.valid then
                        rows("fg_cadence", {{"FPS (game / native / generated)", string.format("%.1f / %.1f / %.1f",cadence.game_fps or 0,cadence.native_fps or 0,cadence.generated_fps or 0)},
                            {"Unique FPS output",string.format("%.1f FPS",cadence.unique_fps or 0)}})
                    else text("Measuring actual presentation cadence...") end
                elseif p and p.asynchronous_presenter then
                    text("Update the OpenXR layer for measured unique-output FPS.")
                end
                local presentation = fg.presentation
                if presentation and presentation.supported then
                    local function age(samples, mean, last)
                        return (samples or 0)>0 and string.format("%.3f / %.3f ms",mean or 0,last or 0) or "No samples yet"
                    end
                    section("Content age at OpenXR submission (average / last ms)")
                    text("Capture-to-submit age, not rendering time or headset display latency.")
                    text("Reference values for comparing modes; not a measurement of added FG delay.")
                    rows("fg_frame_age", {
                        {"Real frame",age(presentation.native_samples,presentation.native_age_ms,presentation.native_age_last_ms)},
                        {"Generated frame (from the midpoint of its two real frames)",age(presentation.generated_samples,presentation.generated_age_ms,presentation.generated_age_last_ms)},
                        {"Newest input used for the generated frame",age(presentation.generated_samples,presentation.newest_input_age_ms,presentation.newest_input_age_last_ms)}})
                end
                if fg.match_reason then
                    rows("fg_source_match",{{"Guides captured",string.format("%.0f",fg.captured_guides or 0)},
                        {"Matching guides (ready / pending)",string.format("%.0f / %.0f",fg.eligible_guides or 0,fg.pending_guides or 0)},
                        {"Guide matching",fg.match_reason}})
                end
                section("GPU timings, both eyes (average / last ms)")
                local extra = fg.overhead
                if not p or not p.supported then
                    text("Install the matching OpenXR update for FG timings.")
                else
                    local function gpu_ms(mean, last)
                        return string.format("%.3f / %.3f", mean or 0, last or 0)
                    end
                    if p.gpu_valid then
                        local focused = (p.foveated_eyes or 0) > 0
                        local resized = not focused and (p.center_width or 0)>0 and
                            (p.center_width~=p.output_width or p.center_height~=p.output_height)
                        if native_shader11 and (p.output_width or 0)>0 then
                            rows("fg_resolutions", {{"Output per eye", size(p.output_width,p.output_height)}})
                        elseif (p.center_width or 0) > 0 then
                            rows("fg_resolutions", {{focused and "Center FG input" or "Full-frame FG input", size(p.center_width,p.center_height)},
                                {"Periphery input", focused and size(p.peripheral_width,p.peripheral_height) or "Not used"},
                                {"Output per eye", size(p.output_width,p.output_height)}})
                        end
                        -- Pipeline order with inline subtotals, matching the standalone menu.
                        local stages = extra and extra.supported
                        local entries, sum, sum_last = {}, 0, 0
                        local function stage(label, samples, mean, last, group)
                            if (samples or 0) > 0 then
                                group[1], group[2] = group[1] + (mean or 0), group[2] + (last or 0)
                                entries[#entries+1] = {label, gpu_ms(mean, last)}
                            else entries[#entries+1] = {label, "Waiting for GPU samples"} end
                        end
                        if stages then
                            local capture = {0, 0}
                            stage("Guide capture (per real frame)", extra.guide_samples, extra.guide_ms, extra.guide_last_ms, capture)
                            stage(native_shader11 and "Color capture / eye crop (per copy)" or "Color capture + guide handoff (per real frame)", extra.capture_samples, extra.capture_ms, extra.capture_last_ms, capture)
                            if not native_shader11 then entries[#entries+1] = {"  Capture subtotal", (extra.guide_samples or 0)>0 and (extra.capture_samples or 0)>0 and gpu_ms(capture[1], capture[2]) or "Waiting for GPU samples"} end
                            sum, sum_last = sum + capture[1], sum_last + capture[2]
                        end
                        if native_shader11 then
                            entries[#entries+1] = {"Shader generation (center + periphery)",gpu_ms(p.total_ms,p.total_last_ms)}
                        else
                        entries[#entries+1] = {"Color / guide preparation", gpu_ms(p.preparation_ms, p.preparation_last_ms)}
                        entries[#entries+1] = {focused and "DLSS-FG center" or "DLSS-FG full frame", gpu_ms(p.center_ms, p.center_last_ms)}
                        if focused or resized then
                            entries[#entries+1] = {resized and "Output resize" or "Periphery + output assembly", gpu_ms(p.periphery_ms,p.periphery_last_ms)}
                        end
                        entries[#entries+1] = {stages and "  Generation subtotal" or "FG GPU total", gpu_ms(p.total_ms, p.total_last_ms)}
                        end
                        sum, sum_last = sum + (p.total_ms or 0), sum_last + (p.total_last_ms or 0)
                        if stages then
                            local present = {0, 0}
                            if not native_shader11 then stage("Generated output copy (per generated frame)", extra.publication_samples, extra.publication_ms, extra.publication_last_ms, present) end
                            stage(native_shader11 and "OpenXR image copy (per copy)" or "OpenXR uploads (per presentation)", extra.upload_samples, extra.upload_ms, extra.upload_last_ms, present)
                            if not native_shader11 then
                            entries[#entries+1] = {"  Presentation subtotal", (extra.publication_samples or 0)>0 and (extra.upload_samples or 0)>0 and gpu_ms(present[1], present[2]) or "Waiting for GPU samples"}
                            sum, sum_last = sum + present[1], sum_last + present[2]
                            local complete = (extra.guide_samples or 0)>0 and (extra.capture_samples or 0)>0 and (extra.publication_samples or 0)>0 and (extra.upload_samples or 0)>0
                            entries[#entries+1] = {"Measured GPU stage sum", complete and gpu_ms(sum, sum_last) or "Waiting for GPU samples"}
                            end
                        end
                        rows("fg_gpu_timings", entries)
                        text(native_shader11 and "Shader time covers both eyes. Copy times are per copy. Runtime work is excluded." or
                            "Stage samples use different frame cadences and can overlap. Their sum is not total frame time; compositor work is excluded.")
                    elseif not p.gpu_supported and (p.gpu_samples or 0) == 0 then
                        text("GPU timestamps unavailable or FG has not started.")
                    else
                        text("Waiting for a complete GPU timing sample.")
                    end
                    rows("fg_wait_timings", {
                        {p.asynchronous_presenter and "FG completion wait (background)" or "FG completion wait (CPU)", string.format("%.3f ms", p.completion_wait_ms or 0)},
                        {p.asynchronous_presenter and "Game GPU-fence waits" or "Capture / upload waits (CPU)", string.format("%.3f ms", p.copy_wait_ms or 0)},
                        {p.asynchronous_presenter and "OpenXR waits (presenter)" or "OpenXR frame waits (CPU)", string.format("%.3f ms", p.xr_wait_ms or 0)},
                        {"Headset frame period", string.format("%.3f ms", p.xr_period_ms or 0)}})
                    text("CPU waits overlap GPU work; do not add them.")
                    text("GPU timings are elapsed intervals; contention can increase them.")
                    local exp = fg.experiments
                    if exp and exp.supported then
                        text("FG delay from GPU native-color capture: average / last (ms)")
                        if exp.latency_enabled and (exp.gpu_ready_samples or 0)>0 then
                            local function corrected(key)
                                local value=exp[key] or {}
                                return gpu_ms(value.mean,value.last)
                            end
                            rows("fg_gpu_ready_delay", {
                                {"Native GPU tail (excluded)",corrected("native_gpu_tail")},
                                {"GPU color-ready to generated submit",corrected("gpu_ready_to_submit")},
                                {"GPU color-ready to predicted display",(exp.predicted_samples or 0)>0 and corrected("gpu_ready_to_display") or
                                    (exp.display_time_supported and "Waiting for a usable display target" or "Runtime clock conversion unavailable")},
                                {"GPU-ready samples / late display targets",string.format("%.0f / %.0f",exp.gpu_ready_samples or 0,exp.prediction_late or 0)}})
                            text("Starts when the GPU reaches color capture, after queued native rendering. Latest input, not the content midpoint.")
                            text("Submit is measured; display is the runtime's predicted target, not confirmed physical display.")
                        elseif not exp.latency_enabled then text("GPU-ready latency diagnostics OFF.")
                        else text("Waiting for calibrated color timestamps on a generated submission.") end
                        text("FG latency breakdown: average / last (ms), sampled 1 in 17 jobs")
                        if exp.latency_enabled and exp.latency_valid then
                            local function latency(key)
                                local value = exp[key] or {}
                                return gpu_ms(value.mean, value.last)
                            end
                            rows("fg_latency_breakdown", {
                                {"Queue entry delay",latency("queue_entry")},
                                {"Capture-fence dependency",latency("capture_dependency")},
                                {"Capture ready to FG start",latency("ready_to_fg")},
                                {"FG GPU work + publication",latency("gpu_work")},
                                {"GPU end to CPU observation",latency("gpu_to_observed")},
                                {"Enqueue to CPU observation",latency("enqueue_to_observed")},
                                {"FG submit to CPU observation",latency("submit_to_observed")}})
                            text("First five stages sum to enqueue time. Submit time starts later, after CPU recording.")
                            text("GPU end to CPU observation includes the query/fence tail and CPU wakeup.")
                        elseif not exp.latency_enabled then text("FG latency diagnostics OFF.")
                        else text("Waiting for a valid calibrated timeline sample.") end
                        rows("fg_transport_counts", {
                            {"Timeline samples / rejected",string.format("%.0f / %.0f",exp.latency_samples or 0,exp.latency_rejected or 0)},
                            {"Color copies (full / direct eye / crop)",string.format("%.0f / %.0f / %.0f",exp.full_color_copies or 0,exp.direct_eye_copies or 0,exp.eye_crop_copies or 0)},
                            {"Direct-capture fallbacks",string.format("%.0f",exp.capture_fallbacks or 0)},
                            {"XR upload copies / GPU batches",string.format("%.0f / %.0f",exp.upload_copies or 0,exp.upload_batches or 0)}})
                        if draft.FgReleasePoint == 3 then rows("fg_generated_gate", {
                            {"Generated-submission gate releases / fallbacks",string.format("%.0f / %.0f",exp.generated_gate_releases or 0,exp.generated_gate_skips or 0)}}) end
                        text("Copy counters are cumulative for this session; latency windows reset when options change.")
                    end
                    if cadence and cadence.supported then
                        rows("fg_runtime_waits", {{"Game image wait / call",string.format("%.3f ms",cadence.app_image_wait_ms or 0)},
                            {"Game begin / end calls",string.format("%.3f / %.3f ms",cadence.app_begin_ms or 0,cadence.app_end_ms or 0)},
                            {"Game image release / call",string.format("%.3f ms",cadence.app_release_ms or 0)},
                            {"Presenter image wait / call",string.format("%.3f ms",cadence.present_image_wait_ms or 0)},
                            {"Runtime begin / end",string.format("%.3f / %.3f ms",cadence.begin_ms or 0,cadence.end_ms or 0)},
                            {"Presenter work / submission",string.format("%.3f ms",cadence.present_cpu_ms or 0)}})
                    end
                    if p.asynchronous_presenter then
                        text("Pacing wait: game sleeps until the next native-frame slot.")
                        rows("fg_alignment", {{"Color / guide submission",string.format("%.0f / %.0f",p.color_token or 0,p.guide_token or 0)},
                            {"FG input color",p.inference_color_format==28 and "RGBA8 SDR" or p.inference_color_format==24 and "RGB10" or "FP16"},
                            {"FG GPU queue",p.shared_gpu_queue==1 and "Game queue; background CPU worker" or "Separate queue"},
                            {"Rejected guide matches",tostring(p.mismatched_guides or 0)}})
                        text("Buffered presentation: FG completion runs off the game thread.")
                        rows("fg_buffered", {{"Game frame handoff (CPU)", string.format("%.3f ms",p.producer_cpu_ms or 0)},
                            {"Game pacing wait", string.format("%.3f ms",p.pacing_wait_ms or 0)},
                            {"Native frames queued",p.queued_native or 0},{"Skipped FG jobs (busy)",p.skipped_native or 0}})
                    end
                end
                local timeline = fg.pacing_timeline
                if timeline and timeline.supported then
                    local function metric(mean, last)
                        return string.format("%.3f / %.3f ms",mean or 0,last or 0)
                    end
                    text("CPU pacing timeline: mean / latest (waits may overlap; not additive)")
                    rows("fg_pacing_timeline", {
                        {"Pacing clock lateness",metric(timeline.clock_late_ms,timeline.clock_late_last_ms)},
                        {"Deferred game credit",metric(timeline.credit_deferred_ms,timeline.credit_deferred_last_ms)},
                        {"Game xrWaitFrame credit",metric(timeline.game_wait_credit_ms,timeline.game_wait_credit_last_ms)},
                        {"Game xrBeginFrame gate",metric(timeline.game_begin_gate_ms,timeline.game_begin_gate_last_ms)},
                        {"Presenter fresh-content wait",metric(timeline.presenter_content_ms,timeline.presenter_content_last_ms)},
                        {"Native xrEndFrame",metric(timeline.native_xr_end_ms,timeline.native_xr_end_last_ms)},
                        {"Generated xrEndFrame",metric(timeline.generated_xr_end_ms,timeline.generated_xr_end_last_ms)},
                        {"FG submit-to-complete",metric(timeline.fg_submit_to_complete_ms,timeline.fg_submit_to_complete_last_ms)},
                        {"Deferred / released credits",string.format("%.0f / %.0f",timeline.deferred_events or 0,timeline.deferred_releases or 0)},
                    })
                    text("Fresh-content wait occurs before the presenter's private xrWaitFrame.")
                end
                local native_latency=fg.native_submission_latency
                if native_latency and native_latency.supported then
                    text("NATIVE image to OpenXR: mean / latest (ms), not motion-to-photon")
                    if not native_latency.enabled then
                        text("Enable FG latency diagnostics to collect calibrated GPU timestamps.")
                    elseif (native_latency.gpu_samples or 0)==0 then
                        text("Awaiting native color GPU-copy completion samples.")
                    else
                        local function metric(m,last) return string.format("%.3f / %.3f",m or 0,last or 0) end
                        rows("fg_native_submission_latency",{
                            {"GPU-ready to native xrEndFrame entry (signed)",metric(native_latency.gpu_ready_to_submit_ms,native_latency.gpu_ready_to_submit_last_ms)},
                            {"Held AFTER ready (when positive)",metric(native_latency.native_hold_ms,native_latency.native_hold_last_ms)},
                            {"Submitted BEFORE ready (when negative)",metric(native_latency.submit_ahead_ms,native_latency.submit_ahead_last_ms)},
                            {"CPU color capture to native xrEndFrame",metric(native_latency.capture_to_submit_ms,native_latency.capture_to_submit_last_ms)},
                            {"GPU samples / pending / unavailable",string.format("%.0f / %.0f / %.0f",native_latency.gpu_samples or 0,native_latency.pending or 0,native_latency.missing_stamps or 0)}})
                    end
                    text("Ready means both eyes and their final color assembly/crop finished on the GPU. Negative signed time means xrEndFrame was called earlier; runtime/GPU completion is separate.")
                    text("This is the FG-ON native delay, NOT added FG latency: no matching FG-OFF GPU-ready baseline is measured.")
                end
                text("If generation is unavailable, the original native frame is displayed.")
                end
            end
            gpu_memory("fg")
            reset_group("Reset DLSS-FG defaults", "fg")
        end
        imgui.tree_pop()
    end

    if imgui.tree_node("DLSS-NR (experimental)") then
        if status.renderer == 0 then
            check("DX11 -> DX12 transport", "D3D11D3D12Transport")
            text("DLSS-NR on DX11 requires DX12 transport.")
        end
        if status.renderer ~= 0 or draft.D3D11D3D12Transport then
            check("Enable DLSS-NR", "NrEnabled")
            text("Alt+Shift+> (period key): toggle DLSS-NR")
            if draft.NrEnabled then
                check("Foveated NR", "NrFoveated")
                if draft.NrFoveated then
                    text("Uses Stereo and gaze settings, even with SR disabled.")
                    check("Use SR size and shape", "NrUseSrFoveation")
                    if not draft.NrUseSrFoveation then
                        slider("NR width", "NrWidth", 0.2, 1)
                        slider("NR height", "NrHeight", 0.2, 1)
                        slider("NR roundness", "NrRoundness", 0, 1)
                        text("Cosmetic only; no performance impact.")
                        slider("NR transition", "NrTransitionWidth", 0, 0.3)
                    end
                    check("Show NR alignment border", "NrAlignmentBorder")
                end
                section("Neural rendering")
                if status.settings.NrProcessingOrder ~= nil then
                    combo("Rendering order", "NrProcessingOrder", {[0]="After upscaling",[1]="Before upscaling"})
                else
                    text("Rendering order: Unavailable in this runtime")
                end
                slider("NR working scale", "NrWorkingScale", 0.1, 1)
                combo("DLSS-NR style", "NrStyle", {[0]="Standard",[1]="Natural",[2]="Cinematic"})
                slider("NR intensity", "NrIntensity", 0, 1)
                text("Intensity: 0 = no model edit, 1 = full model edit.")
                if imgui.tree_node("Advanced NR") then
                    slider("Local tone", "NrLocalToneStrength", 0, 2)
                    slider("Local structure", "NrLocalStructureStrength", 0, 2)
                    check("Automatic mask", "NrAutomaticMask")
                    if draft.NrAutomaticMask == true then
                        slider("Skin structure", "NrSkinStructureStrength", 0, 2)
                    end
                    if (status.nr_details or {}).hdr_input == true then
                        slider("Paper white", "NrPaperWhiteScale", 0.01, 8)
                    end
                    slider("Transfer strength", "NrHdrTransferStrength", 0, 2)
                    slider("Color strength", "NrColorStrength", 0, 2)
                    combo("Depth convention", "NrDepthConvention", {[0]="Game/default",[1]="Normal",[2]="Reversed"})
                    slider("Motion scale X", "NrMotionScaleXMultiplier", -4, 4)
                    slider("Motion scale Y", "NrMotionScaleYMultiplier", -4, 4)
                    imgui.tree_pop()
                end
            end
            if imgui.button("Reset NR history / retry") then send("reset_nr") end
            text("nvngx_dlssnr.dll: beside the runtime DLL, or beside the running game executable.")
            text("After adding the DLL, use Reset NR history / retry. The log lists paths and loader errors.")
        end
        reset_group("Reset DLSS-NR defaults", "nr")
        imgui.tree_pop()
    end
    if next(dirty) or pending_apply then apply_buttons("bottom") end

    if imgui.tree_node("Performance") then performance(d, f); imgui.tree_pop() end

    if imgui.tree_node("Support") then
        text("Report an issue creates a ZIP, opens GitHub and selects the ZIP in Explorer.")
        text("Review before sharing; logs may contain personal paths. Attach the ZIP and submit yourself.")
        local support = status.support or {}
        imgui.begin_disabled(support.busy == true)
        if imgui.button("Report an issue...") then send("report_issue") end
        imgui.same_line()
        if imgui.button("Create support ZIP only") then send("report") end
        imgui.end_disabled()
        if support.busy then text("Preparing support ZIP...") end
        if support.zip and support.zip ~= "" then
            text(support.zip)
            if imgui.button("Show ZIP") then send("show_report") end
            imgui.same_line()
            if imgui.button("Open GitHub issue") then send("open_issue") end
        end
        imgui.same_line()
        if imgui.button("Refresh status") then send("get") end
        imgui.tree_pop()
    end
    if imgui.tree_node("Reset settings") then
        reset_group("Restore all Cheeky defaults", "all")
        imgui.tree_pop()
    end
    imgui.end_disabled()
    imgui.tree_pop()
    flush_edits()
end)
