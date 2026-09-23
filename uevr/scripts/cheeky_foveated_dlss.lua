-- Cheeky Foveated DLSS UEVR plugin. The C++ runtime owns validation and all GPU work.
local protocol = 1
local command_event = "cheeky.foveated_dlss.command.v1"
local snapshot_event = "cheeky.foveated_dlss.snapshot.v1"
local status, draft, dirty = nil, {}, {}
local request, pending_apply, frame = 0, nil, 0
local error_text = nil
local last_snapshot_frame = 0
local ready_edits, slider_edits = {}, {}

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
    if event ~= snapshot_event then return end
    local ok, value = pcall(json.load_string, text)
    if not ok or type(value) ~= "table" or value.protocol ~= protocol or type(value.settings) ~= "table" then
        error_text = "Cheeky UI/runtime version mismatch or invalid response. Install the matching files."
        return
    end
    status = value
    -- A reconnect can reach an older runtime. Drop unsupported optional drafts
    -- before automatic flush or Apply can resend them to that runtime.
    for _, key in ipairs({"NrProcessingOrder", "AfwManualCoverage", "AfwAutomaticCoverage", "AfwWarpMargin", "EyeCalibrationContinuous"}) do
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
local alignment = {[0]="Manual fallback", [1]="Streamline projection", [2]="OpenXR", [3]="OpenVR"}

local function performance(d, f)
    section("Frame rate comparison")
    rows("fps", {{"SR enabled", fps(f.sr_enabled_ms)}, {"SR disabled", fps(f.sr_disabled_ms)},
        {"Frame time savings", saving(f.sr_disabled_ms, f.sr_enabled_ms)}})
    text("Toggle SR in the same scene. These measure UEVR present callbacks; headset FPS may differ.")

    section("GPU time (250 ms average)")
    local times = {{"Native DLSS", timing(d.native_ms)}, {"Foveated center DLSS", timing(d.foveated_ms)},
        {"Peripheral DLAA", timing(d.peripheral_ms)}}
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
    local choices = {[0]="Centered (70% minimum)",[1]="Manual"}
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
        slider(gaze and "Fallback height offset" or "Height offset", "AlignedHeightOffset", -1, 1)
    elseif mode == 1 then
        slider(gaze and "Fallback stereo X offset" or "Stereo X offset", "XOffset", -1, 1)
        slider(gaze and "Fallback height offset" or "Height offset", "HeightOffset", -1, 1)
    end
    if gaze then text("Gaze follows both eyes. The fallback above applies when tracking is unavailable.")
    elseif mode == 0 then text("Centered coverage uses at least 70% of the image width and height.") end
    if (gaze or mode ~= 0) and draft.AfwWarpMargin ~= nil and imgui.tree_node("Advanced AFW") then
        slider("Extra margin per edge", "AfwWarpMargin", 0, 0.25)
        text("Adds a fixed fraction of the full image around each eye's region. Larger margins cost more GPU time.")
        imgui.tree_pop()
    end
end

uevr.sdk.callbacks.on_draw_ui(function()
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
    text("Cheeky " .. tostring(status.version))
    text(status.message)
    local afw = status.afw_experiment or {}
    local afw_active = afw.enabled and afw.coverage_enabled ~= false
    if not status.ready then text("Processing is paused.") end
    text("Sliders apply on release. Other controls apply immediately and save automatically.")
    text("Alt+Shift+/ toggles SR.")
    local d = (status.apis or {})[status.renderer == 1 and 2 or 1] or {}
    local f = status.frame or {}
    rows("overview", {{"Renderer", status.renderer == 1 and "DX12" or "DX11 direct"},
        {"SR status", d.state or "Waiting"}, {"NR status", status.nr or "Waiting"},
        {"UEVR present cadence", fps(f.present_ms)}})
    if afw.enabled then rows("afw_overview", {{"AFW", afw_mode(afw)}}) end
    -- A reset is a native transaction. Wait for its authoritative snapshot before editing again.
    imgui.begin_disabled(pending_apply ~= nil and pending_apply.reset_group ~= nil)
    apply_buttons("top")

    if imgui.tree_node("Stereo and gaze") then
        combo("Foveation center", "CenterMode", {[0]="Fixed",[1]="Runtime gaze (OpenXR / OpenVR)",[2]="Simulated gaze"})
        if afw_active then
            afw_controls()
            if draft.AfwAutomaticCoverage and afw.coverage_mode ~= 2 and afw.coverage_mode ~= 3 then
                text("Automatic coverage is waiting for matching UEVR projections; using centered fallback.")
            end
        else
            check("Automatic stereo alignment", "AutoStereoAlignment")
            if draft.AutoStereoAlignment then slider("Height offset / gaze fallback", "AlignedHeightOffset", -1, 1)
            else
                slider("Manual stereo X offset", "XOffset", -1, 1)
                slider("Manual height offset", "HeightOffset", -1, 1)
            end
            check("Invert stereo eye order", "InvertStereoXOffset")
        end
        if draft.CenterMode == 2 then
            combo("Simulation pattern", "SimulationPattern", {[0]="Figure eight (8 s)",[1]="Slow sweep (20 s)",
                [2]="Jump every 2 s",[3]="Jump every 8 s",[4]="Tracking loss",[5]="Hold center"})
            text("Patterns restart when changed. Enable the red border to inspect motion.")
            if draft.SimulationPattern == 2 or draft.SimulationPattern == 3 then check("Show next jump target", "ShowNextJumpTarget") end
            if draft.SimulationPattern == 4 then text("Moves for 4 s, loses tracking for 1 s, then recovers.") end
        end
        if imgui.tree_node("Advanced eye tracking") then
            slider("Gaze smoothing (ms)", "GazeSmoothingMs", 0, 100)
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
        if not afw_active and imgui.tree_node("Eye calibration") then
            local c = status.eye_calibration or {}
            local changed, enabled = imgui.checkbox("Automatic eye calibration (this session)", c.enabled == true)
            if changed then send(enabled and "calibration_enable" or "calibration_disable") end
            if status.settings.EyeCalibrationContinuous ~= nil then
                check("Continuously validate eye calibration", "EyeCalibrationContinuous")
                if draft.EyeCalibrationContinuous == false then
                    text("Recalibrates only when views, dimensions, submission bounds, or the VR session change. Same-view eye swaps and image crop changes are not detected.")
                end
                if c.enabled and imgui.button("Recalibrate now") then send("calibration_recalibrate") end
            end
            rows("eye_calibration", {{"Runtime", c.backend or "Waiting for VR"},
                {"Status", c.status or "Unavailable in this runtime"}})
            imgui.tree_pop()
        end
        reset_group("Reset Stereo / gaze defaults", "gaze")
        imgui.tree_pop()
    end

    if imgui.tree_node("DLSS-SR") then
        check("Enable foveated DLSS-SR", "Enabled")
        if draft.Enabled then
            section("Center")
            combo("Center preset", "CenterPreset", {[0]="Game/default",[5]="E",[11]="K",[12]="L",[13]="M"})
            slider("Center supersampling", "CenterSupersampling", 1, 2)
            slider("Fovea width", "Width", 0.2, 1)
            slider("Fovea height", "Height", 0.2, 1)
            if afw_active then text("AFW sizes are per eye. Stereo coverage and extra margin can enlarge the visible region.") end
            slider("Roundness", "Roundness", 0, 1)
            slider("Transition width", "TransitionWidth", 0, 0.3)
            check("Show red alignment border", "AlignmentBorder")
            section("Periphery")
            check("Peripheral DLAA", "PeripheralDlaa")
            if draft.PeripheralDlaa then
                combo("Peripheral preset", "PeripheralDlaaPreset", {[5]="E",[11]="K",[12]="L",[13]="M"})
                slider("Periphery scale", "PeripheralDlaaScale", 0.2, 1)
            end
        end
        reset_group("Reset DLSS-SR defaults", "sr")
        imgui.tree_pop()
    end

    if imgui.tree_node("DLSS-NR (experimental)") then
        if status.renderer == 0 then
            text("DLSS-NR / DX12 transport is unavailable on the DX11 path in the UEVR plugin.")
        else
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
    apply_buttons("bottom")

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
