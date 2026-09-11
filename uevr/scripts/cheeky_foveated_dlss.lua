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
    -- A reconnect can reach an older runtime. Drop unsupported order drafts
    -- before automatic flush or Apply can resend them to that runtime.
    if value.settings.NrProcessingOrder == nil then
        draft.NrProcessingOrder, dirty.NrProcessingOrder = nil, nil
        ready_edits.NrProcessingOrder, slider_edits.NrProcessingOrder = nil, nil
        if pending_apply and pending_apply.values.NrProcessingOrder ~= nil then
            pending_apply.values.NrProcessingOrder = nil
            if not next(pending_apply.values) and not pending_apply.reset_group then pending_apply = nil end
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
local function result(value) return string.format("0x%08X", value or 0) end
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

local function sr_diagnostics(d)
    local c = d.crop or {}
    section("Resolution")
    rows("sr_resolution", {
        {"Game input", size(d.input_width, d.input_height)},
        {"Game output", size(d.output_width, d.output_height)},
        {"Private DLSS input", size(c.input_width, c.input_height)},
        {"Private DLSS output", size(c.output_width, c.output_height)},
        {"Crop input / output origin", string.format("%d,%d / %d,%d", c.input_x or 0, c.input_y or 0, c.output_x or 0, c.output_y or 0)},
        {"Motion vectors", size(d.motion_width, d.motion_height) .. " / " .. (d.motion_space or "Unknown")}
    })
    section("DLSS-SR GPU timing (250 ms average)")
    rows("sr_timing", {
        {"Native DLSS call", timing(d.native_ms)}, {"Foveated center DLSS", timing(d.foveated_ms)},
        {"Peripheral DLAA", timing(d.peripheral_ms)}, {"Center call savings", saving(d.native_ms, d.foveated_ms)}
    })
    text("GPU call timings exclude other game work; samples are retained when a mode is disabled.")
    local gpu = status.gpu_timing
    if gpu and status.renderer == 1 then
        if (gpu.waiting_submission or 0) > 0 then
            text("Timestamp queries are waiting for their command-list submission.")
        elseif (gpu.waiting_gpu or 0) > 0 then
            text("Timestamp readback is waiting for GPU completion.")
        end
        if (gpu.failures or 0) > 0 then text("GPU timing error: " .. result(gpu.last_error) .. ". Save a diagnostic report.") end
    end
end
local function nr_diagnostics(d)
    local n = status.nr_details or {}
    rows("nr_status", {
        {"State", status.nr}, {"Route", n.route or "Unknown"},
        {"Candidate / evaluated / failed", string.format("%d / %d / %d", n.candidates or 0, n.evaluations or 0, n.failures or 0)},
        {"Last result", result(n.result)}, {"SR output", size(n.output_width, n.output_height)},
        {"NR processing size", size(n.processing_width, n.processing_height)},
        {"Skip reason", n.skip_reason or ""},
        {"NR region", size(n.region_width, n.region_height)},
        {"Region origin", string.format("%d,%d", n.region_x or 0, n.region_y or 0)},
        {"NR working size", size(n.working_width, n.working_height)},
        {"Intermediate VRAM", string.format("%.1f MiB", (n.vram_bytes or 0) / 1048576)}
    })
    section("DLSS-NR GPU timing (250 ms average)")
    rows("nr_timing", {
        {"Before: full NR + preparation", timing(d.before_nr_full_ms)},
        {"Before: foveated NR + preparation", timing(d.before_nr_foveated_ms)},
        {"Before: total intercepted pipeline", timing(d.before_pipeline_ms)},
        {"After: full NR call", timing(d.nr_full_ms)},
        {"After: foveated NR call", timing(d.nr_foveated_ms)},
        {"After: foveated savings", saving(d.nr_full_ms, d.nr_foveated_ms)},
        {"After: total intercepted pipeline", timing(d.after_pipeline_ms)}
    })
    text("Samples are retained when a mode is disabled; compare timings from the same processing order.")
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
    if not status.ready then text("Processing is paused. See the status and log before testing.") end
    text("Sliders apply on release. Other controls apply immediately and save automatically.")
    text("Alt+Shift+/ toggles SR.")
    local d = (status.apis or {})[status.renderer == 1 and 2 or 1] or {}
    local f = status.frame or {}
    rows("overview", {{"Renderer", status.renderer == 1 and "DX12" or "DX11 direct"},
        {"SR status", d.state or "Waiting"}, {"UEVR present cadence", fps(f.present_ms)}})
    -- A reset is a native transaction. Wait for its authoritative snapshot before editing again.
    imgui.begin_disabled(pending_apply ~= nil and pending_apply.reset_group ~= nil)
    apply_buttons("top")

    if imgui.tree_node("Stereo and gaze") then
        combo("Foveation center", "CenterMode", {[0]="Fixed",[1]="Runtime gaze (OpenXR / OpenVR)",[2]="Simulated gaze"})
        check("Automatic stereo alignment", "AutoStereoAlignment")
        if draft.AutoStereoAlignment then slider("Height offset / gaze fallback", "AlignedHeightOffset", -1, 1)
        else
            slider("Manual stereo X offset", "XOffset", -1, 1)
            slider("Manual height offset", "HeightOffset", -1, 1)
        end
        check("Invert stereo eye order", "InvertStereoXOffset")
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
        rows("gaze_summary", {{"Alignment", alignment[g.alignment] or "Unknown"}, {"Gaze driving foveation", yes(g.using_gaze)},
            {"Mapped left / right", yes(g.left_mapped) .. " / " .. yes(g.right_mapped)}})
        if draft.CenterMode == 1 and not g.using_gaze then
            imgui.text_colored("Eye tracking unavailable or awaiting mapping; using fixed fallback.", 0xFFFFBC70)
        end
        text("OpenXR alignment/gaze uses the matching Cheeky layer. Fixed alignment needs no eye tracker.")
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
        if imgui.tree_node("Frame rate comparison") then
            rows("fps", {{"Present cadence (250 ms avg)", fps(f.present_ms)},
                {"SR enabled", fps(f.sr_enabled_ms)}, {"SR disabled", fps(f.sr_disabled_ms)},
                {"Frame time savings", saving(f.sr_disabled_ms, f.sr_enabled_ms)}})
            if (f.sr_enabled_ms or 0) > 0 and (f.sr_disabled_ms or 0) > 0 then
                text(string.format("Change: %+.1f FPS (%+.1f%%)", 1000 / f.sr_enabled_ms - 1000 / f.sr_disabled_ms,
                    100 * (f.sr_disabled_ms / f.sr_enabled_ms - 1)))
            end
            text("Measures UEVR present callbacks; headset display/reprojection FPS may differ.")
            text("Toggle SR in the same scene. Samples settle for 1 s, then average over 250 ms.")
            text("Comparisons retain the last sample of each mode; scene and NR changes also affect frame rate.")
            imgui.tree_pop()
        end
        if imgui.tree_node("SR resolution and GPU timing") then sr_diagnostics(d); imgui.tree_pop() end
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
            text("NR: " .. tostring(status.nr))
            if imgui.button("Reset NR history / retry") then send("reset_nr") end
            text("nvngx_dlssnr.dll: beside the runtime DLL, or beside the running game executable.")
            text("After adding the DLL, use Reset NR history / retry. The log lists paths and loader errors.")
        end
        reset_group("Reset DLSS-NR defaults", "nr")
        if imgui.tree_node("NR status and GPU timing") then nr_diagnostics(d); imgui.tree_pop() end
        imgui.tree_pop()
    end
    apply_buttons("bottom")

    if imgui.tree_node("Diagnostics and support") then
        if imgui.tree_node("Eye calibration") then
            local c = status.eye_calibration or {}
            local changed, enabled = imgui.checkbox("Automatic eye calibration (this session)", c.enabled == true)
            if changed then send(enabled and "calibration_enable" or "calibration_disable") end
            rows("eye_calibration", {{"Backend", c.backend or "Waiting for VR"},
                {"Graphics API", c.graphics_api == 12 and "D3D12" or c.graphics_api == 11 and "D3D11" or "Waiting for DLSS"},
                {"Status", c.status or "Unavailable in this runtime"},
                {"Corrections applied", c.corrections or 0}, {"Confirmed mapping updates", c.applied or 0},
                {"Valid / completed samples", string.format("%d / %d", c.valid or 0, c.completed or 0)},
                {"Skipped / in flight", string.format("%d / %d", c.skipped or 0, c.in_flight or 0)},
                {"CPU work", string.format("%.2f us/frame", c.cpu_us_per_frame or 0)},
                {"GPU marker / copy work", (c.gpu_samples or 0) > 0 and string.format("%.2f us", c.gpu_us or 0) or "Not sampled / unavailable"},
                {"Readback latency", string.format("%.2f VR frames", c.latency_frames or 0)},
                {"Last recognized left / right", tostring(c.left_view or 0) .. " / " .. tostring(c.right_view or 0)}})
            text("Samples every 10 VR frames. Corrections count changes to an existing eye assignment; confirmations do not increment it.")
            text("GPU time covers marker and copy commands; CPU time excludes lock waiting.")
            if imgui.button("Reset eye calibration counters") then send("calibration_reset") end
            imgui.tree_pop()
        end
        local a, o, g = status.late_attach or {}, status.observer or {}, status.gaze or {}
        rows("host", {{"Settings revision / saved", tostring(status.revision) .. " / " .. tostring(status.saved_revision)},
            {"Streamline options hook / observed", yes(a.options_hooked) .. " / " .. yes(a.options_seen)},
            {"Native NGX fallback calls", a.fallback_calls or 0}, {"Submission observer ready", yes(o.ready)},
            {"Submissions / copies / resets", string.format("%d / %d / %d", o.submissions or 0, o.copies or 0, o.resets or 0)}})
        if a.native_fallback then text("Streamline is forwarding to native NGX. Increasing active counters confirm processing.") end
        if status.renderer == 1 and imgui.tree_node("GPU timestamp collection") then
            local gpu = status.gpu_timing or {}
            rows("gpu_queries", {{"Recorded / submitted", string.format("%d / %d", gpu.recorded or 0, gpu.submitted or 0)},
                {"Completed / valid samples", string.format("%d / %d", gpu.completed or 0, gpu.published or 0)},
                {"Waiting for submission / GPU", string.format("%d / %d", gpu.waiting_submission or 0, gpu.waiting_gpu or 0)},
                {"Discarded recordings", gpu.discarded or 0}, {"Failures / last error", tostring(gpu.failures or 0) .. " / " .. result(gpu.last_error)}})
            text("Save a diagnostic report if these counters stop advancing while DLSS evaluates.")
            imgui.tree_pop()
        end
        if imgui.tree_node("Eye mapping details") then
            rows("gaze_details", {{"Runtime", g.runtime or "Unknown"}, {"Layer or adapter / matching ABI", yes(g.layer) .. " / " .. yes(g.abi)},
                {"Active / peak / seen views", string.format("%d / %d / %d", g.views or 0, g.peak_views or 0, g.seen_views or 0)},
                {"Ambiguous mapping", yes(g.ambiguous)}, {"Sample age", timing(g.age_ms)}, {"Status flags", result(g.status_flags)},
                {"History reset", ({[0]="None",[1]="First valid gaze",[2]="Tracking reacquired",[3]="Eye remapped",
                    [4]="Crop size changed",[5]="Large gaze jump"})[g.reset_reason or 0] or "Unknown"}})
            for i, eye in ipairs(g.eyes or {}) do
                section(i == 1 and "Left eye" or "Right eye")
                rows("eye" .. i, {{"DLSS view", eye.view_id}, {"Mapped / stable matches", yes(eye.mapped) .. " / " .. tostring(eye.stable_matches)},
                    {"Center U / V", string.format("%.3f / %.3f", eye.center_u or 0, eye.center_v or 0)},
                    {"Crop movement X / Y", string.format("%d / %d px", eye.delta_x or 0, eye.delta_y or 0)},
                    {"Packed / copy / projection", yes(eye.packed) .. " / " .. yes(eye.copy) .. " / " .. yes(eye.projection)}})
                rows("eye_marker" .. i, {{"Pixel marker mapping", yes(eye.marker)}})
            end
            imgui.tree_pop()
        end
        if imgui.tree_node("DLSS view details") then
            for _, view in ipairs(status.view_details or {}) do
                section("View " .. view.id .. " / " .. view.eye)
                rows("view" .. view.id, {{"Evaluations", view.evaluations},
                    {"Input / output", size(view.input_width, view.input_height) .. " / " .. size(view.output_width, view.output_height)},
                    {"Foveated output", size(view.crop_width, view.crop_height)}})
            end
            if (status.view_details_total or 0) > 16 then text("Showing the first 16 views.") end
            imgui.tree_pop()
        end
        for i, api in ipairs(status.apis or {}) do
            if imgui.tree_node(i == 1 and "DX11 interception" or "DX12 interception") then
                rows("api" .. i, {{"State", api.state}, {"Runtime / hook / detour", yes(api.runtime_loaded) .. " / " .. yes(api.hook) .. " / " .. yes(api.direct_detour)},
                    {"Evaluations / active / creates", string.format("%d / %d / %d", api.evaluations or 0, api.active or 0, api.creates or 0)},
                    {"Last result", result(api.result)}, {"Private result", api.has_private_result and result(api.private_result) or "Not sampled yet"},
                    {"Route", i == 1 and (api.execution_path or "Unknown") or ({[0]="Unknown",[1]="Public NGX",[2]="Core NGX"})[api.ngx_route or 0]}})
                imgui.tree_pop()
            end
        end
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
        text("Reports, INI and log are in this game's UEVR configuration folder.")
        text("Runtime DLL updates require restarting the game.")
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
