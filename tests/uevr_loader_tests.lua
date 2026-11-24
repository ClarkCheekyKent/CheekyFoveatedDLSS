-- Run with Lua 5.4+: lua tests/uevr_loader_tests.lua [path/to/cheeky_foveated_dlss.lua]
local script = arg[1] or "uevr/scripts/cheeky_foveated_dlss.lua"
local function fixture(config, read_failure)
    local f = {config = config or "", events = {}, messages = {}}
    local env = setmetatable({}, {__index = _G})
    env.fs = {
        read = function()
            if read_failure then error("unreadable profile") end
            return f.config
        end,
        write = function(_, data) if not f.write_failure then f.config = data end end,
    }
    env.uevr = {
        api = {dispatch_custom_event = function(_, event, data)
            f.events[#f.events + 1] = {event, data}
        end},
        sdk = {callbacks = {
            on_lua_event = function(fn) f.receive = fn end,
            on_frame = function(fn) f.frame = fn end,
        }},
        lua = {add_script_panel = function(_, fn) f.panel = fn end},
    }
    env.imgui = {
        checkbox = function(label, current)
            f.checks = f.checks or {}; f.checks[label] = current
            if label == "Enable Cheeky for this game" then
                if f.toggle ~= nil then local value = f.toggle; f.toggle = nil; return true, value end
                f.checked = current
            elseif f.edits and f.edits[label] ~= nil then
                local value = f.edits[label]; f.edits[label] = nil; return true, value
            end
            return false, current
        end,
        text = function(text) f.messages[#f.messages + 1] = text end,
        tree_node = function(label) return f.trees and f.trees[label] or false end,
        tree_pop = function() end,
        button = function() return false end,
        same_line = function() end,
        begin_disabled = function() end,
        end_disabled = function() end,
        begin_table = function() return false end,
        separator = function() end,
        spacing = function() end,
        text_colored = function() end,
    }
    env.json = {load_string = function() return f.snapshot end}
    assert(loadfile(script, "t", env))()
    return f
end
local request_event = "cheeky.foveated_dlss.load.v1"
local state_event = "cheeky.foveated_dlss.load_state.v1"
local snapshot_event = "cheeky.foveated_dlss.snapshot.v1"
for _, config in ipairs({"", "disableMod=1\n", "disableMod=banana\n", "disableMod=2\n"}) do
    local f = fixture(config)
    assert(#f.events == 0, "Must register callbacks before first request")
    f.frame()
    assert(f.events[1][1] == request_event and f.events[1][2] == "false", "Default/malformed settings stay disabled")
end
local unreadable = fixture(nil, true); unreadable.frame()
assert(unreadable.events[1][2] == "false", "Read failure must remain disabled")
local f = fixture("disableMod=0\r\n")
f.frame()
assert(f.events[1][2] == "true", "Saved per-game enable must be restored")
for _ = 1, 120 do f.frame() end
assert(#f.events == 2 and f.events[2][2] == "true", "Lost startup request must be retried")
f.receive(state_event, "enabled")
f.snapshot = {protocol = 1, settings = {Width = .55}, request = 0}
f.receive(snapshot_event, "snapshot")
f.toggle = false; f.panel()
assert(f.config == "disableMod=1\n" and f.events[#f.events][2] == "false", "Disable persists and dispatches")
f.receive(snapshot_event, "late snapshot")
local before = #f.events
for _ = 1, 120 do f.frame() end
for i = before + 1, #f.events do
    assert(f.events[i][1] == request_event and f.events[i][2] == "false", "Disabled UI must not send stale edits or get commands")
end
f.toggle = true; f.panel()
assert(f.config == "disableMod=0\n" and f.events[#f.events][2] == "true", "Re-enable persists and dispatches")
f.receive(state_event, "failed"); f.panel()
assert(f.messages[#f.messages]:find("could not start"), "Startup failure must be visible without a runtime snapshot")
local bad_write = fixture()
bad_write.write_failure = true; bad_write.toggle = true; bad_write.panel(); bad_write.frame()
assert(bad_write.events[#bad_write.events][2] == "false", "Silent write failure must not change the saved enable state")
assert(table.concat(bad_write.messages, "\n"):find("Could not save"), "Persistence failure must be visible")
local cannot_save_disable = fixture("disableMod=0\n")
cannot_save_disable.write_failure = true; cannot_save_disable.toggle = false; cannot_save_disable.panel()
assert(cannot_save_disable.events[#cannot_save_disable.events][2] == "false", "Read-only profile must not prevent disabling this session")
assert(cannot_save_disable.config == "disableMod=0\n", "Failed save must be reported as session-only")
local crop_label = "UEVR full-eye crop compatibility (experimental)"
local compact_label = "Compact full-eye capture (experimental)"
local crop = fixture("disableMod=0\n")
crop.receive(state_event, "enabled")
crop.snapshot = {protocol = 1, ready = true, settings = {
    FgEnabled = false, FgFullEyeCropCompatibility = false, FgFullEyeCompactCapture = false},
    afw_experiment = {rendering_mode_known = true, rendering_mode = 0}}
crop.receive(snapshot_event, "snapshot")
crop.trees = {["Cheeky Foveated DLSS"] = true, ["Frame Gen (Experimental)"] = true, ["Advanced settings"] = true}
crop.panel()
assert(crop.checks[crop_label] == false and crop.checks[compact_label] == nil,
    "Compact capture must be hidden until compatibility is enabled")
crop.edits = {[crop_label] = true, [compact_label] = true}
crop.panel()
assert(crop.checks[compact_label] == false, "Compact capture appears as soon as compatibility is enabled")
local applied = crop.events[#crop.events][2]
assert(applied:find("FgFullEyeCropCompatibility=true", 1, true) and applied:find("FgFullEyeCompactCapture=true", 1, true),
    "Both crop options must reach the native apply transaction")
-- Reconnecting to an older runtime must discard unsupported pending/draft edits.
crop.snapshot.settings.FgFullEyeCropCompatibility = nil
crop.snapshot.settings.FgFullEyeCompactCapture = nil
crop.receive(snapshot_event, "older runtime")
crop.checks = {}; crop.panel()
assert(crop.checks[crop_label] == nil and crop.checks[compact_label] == nil,
    "Unsupported full-eye options must be removed from drafts on reconnect")
crop.snapshot.settings.FgEnabled = true
crop.snapshot.fg = {performance = {full_eye_requested = true, full_eye_compact_requested = false, source_eye = {
    {active = true, compact = false, source_width = 2040, source_height = 2080,
        visible_x = 496, visible_y = 0, visible_width = 1544, visible_height = 2080, mapping_revision = 7},
    {active = true, compact = true, source_width = 2040, source_height = 2080,
        visible_x = 0, visible_y = 0, visible_width = 1545, visible_height = 2080, mapping_revision = 8}}}}
crop.trees["FG capture and presentation diagnostics"] = true
crop.receive(snapshot_event, "diagnostics"); crop.panel()
local diagnostics = table.concat(crop.messages, "\n")
assert(diagnostics:find("Full-eye compatibility requested: true", 1, true) and
    diagnostics:find("Compact capture requested: false", 1, true) and
    diagnostics:find("Eye 1 capture: Full source-eye baseline", 1, true) and
    diagnostics:find("Eye 2 capture: Compact visible crop", 1, true) and
    diagnostics:find("2040x2080 / (496, 0) 1544x2080; mapping 7", 1, true) and
    diagnostics:find("2040x2080 / (0, 0) 1545x2080; mapping 8", 1, true),
    "Diagnostics must distinguish requested/active capture modes and show per-eye numeric crop geometry")
print("PASS: UEVR Lua loader and full-eye crop visibility, draft/apply and reconnect handling")
