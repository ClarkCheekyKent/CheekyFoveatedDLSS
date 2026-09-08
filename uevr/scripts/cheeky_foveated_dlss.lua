-- Cheeky Foveated DLSS UEVR preview. The C++ runtime owns validation and all GPU work.
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
    text("Sliders apply when released. Checkboxes and selections apply immediately; settings save automatically.")
    text("Alt+Shift+/ toggles SR immediately.")
    apply_buttons("top")

    check("Enable foveated DLSS-SR", "Enabled")
    combo("Center preset", "CenterPreset", {[0]="Game/default",[5]="E",[11]="K",[12]="L",[13]="M"})
    slider("Center supersampling", "CenterSupersampling", 1, 2)
    slider("Fovea width", "Width", 0.2, 1)
    slider("Fovea height", "Height", 0.2, 1)
    slider("Roundness", "Roundness", 0, 1)
    slider("Transition width", "TransitionWidth", 0, 0.3)
    check("Show red alignment border", "AlignmentBorder")
    check("Peripheral DLAA", "PeripheralDlaa")
    combo("Peripheral preset", "PeripheralDlaaPreset", {[5]="E",[11]="K",[12]="L",[13]="M"})
    slider("Periphery scale", "PeripheralDlaaScale", 0.2, 1)

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
            slider("Simulation pattern (0-5)", "SimulationPattern", 0, 5, true)
            check("Show next jump target", "ShowNextJumpTarget")
        end
        slider("Gaze smoothing (ms)", "GazeSmoothingMs", 0, 100)
        slider("Crop quantization (pixels)", "GazeQuantizationPixels", 1, 64, true)
        slider("Jump reset threshold", "GazeJumpResetRatio", 0.01, 1)
        text("OpenXR alignment/gaze uses the matching Cheeky OpenXR layer. No tracker is needed for fixed alignment.")
        text("Missing or ambiguous eye data falls back to fixed placement.")
        imgui.tree_pop()
    end

    if imgui.tree_node("DLSS-NR (experimental)") then
        if status.renderer == 0 then
            text("DLSS-NR / DX12 transport is unavailable on the DX11 path in this preview.")
        else
            check("Enable DLSS-NR", "NrEnabled")
            check("Foveated NR", "NrFoveated")
            check("Use SR size and shape", "NrUseSrFoveation")
            check("Show NR alignment border", "NrAlignmentBorder")
            slider("NR width", "NrWidth", 0.2, 1)
            slider("NR height", "NrHeight", 0.2, 1)
            slider("NR roundness", "NrRoundness", 0, 1)
            slider("NR transition", "NrTransitionWidth", 0, 0.3)
            slider("NR working scale", "NrWorkingScale", 0.1, 1)
            slider("NR preset (0 default)", "NrPreset", 0, 7, true)
            slider("NR intensity", "NrIntensity", 0, 2)
            if imgui.tree_node("Advanced NR") then
                slider("Local tone", "NrLocalToneStrength", 0, 2)
                slider("Local structure", "NrLocalStructureStrength", 0, 2)
                slider("Skin structure", "NrSkinStructureStrength", 0, 2)
                check("Automatic mask", "NrAutomaticMask")
                check("UI correction", "NrUiCorrection")
                slider("Paper white", "NrPaperWhiteScale", 0.01, 8)
                slider("HDR transfer", "NrHdrTransferStrength", 0, 2)
                slider("Color strength", "NrColorStrength", 0, 2)
                combo("Depth convention", "NrDepthConvention", {[0]="Game/default",[1]="Normal",[2]="Reversed"})
                slider("Motion scale X", "NrMotionScaleXMultiplier", -4, 4)
                slider("Motion scale Y", "NrMotionScaleYMultiplier", -4, 4)
                imgui.tree_pop()
            end
            if imgui.button("Reset NR history / retry") then send("reset_nr") end
            text("Supply compatible NVIDIA components yourself; see the package README.")
        end
        text("NR: " .. tostring(status.nr))
        imgui.tree_pop()
    end
    apply_buttons("bottom")

    if imgui.tree_node("Diagnostics and support") then
        text("Renderer: " .. (status.renderer == 1 and "DX12" or "DX11 direct"))
        text("Settings revision " .. tostring(status.revision) .. "; saved " .. tostring(status.saved_revision))
        local o, g = status.observer or {}, status.gaze or {}
        text("D3D12 observer ready: " .. tostring(o.ready) .. "; submissions: " .. tostring(o.submissions) .. "; copies: " .. tostring(o.copies))
        text("Eye views: " .. tostring(g.views) .. "; left mapped: " .. tostring(g.left_mapped) .. "; right mapped: " .. tostring(g.right_mapped))
        local alignment = {[0]="Manual",[1]="Streamline projection",[2]="OpenXR",[3]="OpenVR"}
        text("Alignment: " .. tostring(alignment[g.alignment]) .. "; gaze active: " .. tostring(g.using_gaze))
        text("OpenXR layer: " .. tostring(g.layer) .. "; matching ABI: " .. tostring(g.abi) .. "; ambiguous mapping: " .. tostring(g.ambiguous))
        for i, d in ipairs(status.apis or {}) do
            text((i == 1 and "DX11: " or "DX12: ") .. tostring(d.state))
            text("Evaluations: " .. tostring(d.evaluations) .. "; active: " .. tostring(d.active) .. "; feature creates observed: " .. tostring(d.creates))
            text(tostring(d.input_width) .. "x" .. tostring(d.input_height) .. " -> " .. tostring(d.output_width) .. "x" .. tostring(d.output_height))
            text(string.format("DLSS %.3f ms; native %.3f ms; peripheral %.3f ms", d.foveated_ms or 0, d.native_ms or 0, d.peripheral_ms or 0))
        end
        text("Zero timings may mean unavailable samples. Waiting after late injection: try toggling the game's DLSS.")
        if imgui.button("Write diagnostic report") then send("report") end
        if imgui.button("Refresh status") then send("get") end
        text("Files are saved in this game's UEVR configuration folder.")
        text("Plugin reload disables processing then reconnects. Runtime updates require restarting the game.")
        imgui.tree_pop()
    end
    if imgui.tree_node("Reset settings") then
        if imgui.button("Restore all Cheeky defaults") then
            dirty, ready_edits, slider_edits, pending_apply = {}, {}, {}, nil
            send("defaults")
        end
        imgui.tree_pop()
    end
    imgui.tree_pop()
    flush_edits()
end)
