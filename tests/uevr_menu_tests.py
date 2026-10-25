"""Run the real menu against a mocked UEVR host (requires test-only lupa).

python -m pip install --target build/test-python lupa==2.8
python tests/uevr_menu_tests.py
This validates Lua behavior, not UEVR's renderer, input routing, or headset UI.
"""
import copy
import importlib
import json
import os
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build/test-python"))
reports = list((Path(os.environ.get("CHEEKY_UEVR_TEST_BIN", str(ROOT / "bin/Release"))) / "uevr-test-data").glob("*/CheekyFoveatedDLSS-diagnostics.json"))
if not reports:
    raise SystemExit("Run CheekyUEVRTests.exe first to produce a real runtime snapshot.")
baseline_path = max(reports, key=lambda p: p.stat().st_mtime)
baseline = json.loads(baseline_path.read_text())
assert baseline['eye_calibration']['backend'] == 'Waiting for VR'
assert not baseline['eye_calibration']['active']
assert 'corrections' in baseline['eye_calibration']
assert baseline['settings']['NrProcessingOrder'] in (0, 1)
assert baseline['setting_groups']['NrProcessingOrder'] == 'nr'
assert {'processing_order', 'processing_width', 'processing_height', 'skip_reason'} <= baseline['nr_details'].keys()
for api in baseline['apis']:
    assert {'before_nr_full_ms', 'before_nr_foveated_ms', 'before_pipeline_ms',
            'after_pipeline_ms', 'nr_full_ms', 'nr_foveated_ms'} <= api.keys()
bundles = list((baseline_path.parent / "support").glob("*.zip"))
assert bundles, "Native host test must produce a support ZIP"
with zipfile.ZipFile(max(bundles, key=lambda p: p.stat().st_mtime)) as bundle:
    assert bundle.testzip() is None, "Corrupt support ZIP"
    assert {"diagnostics.json", "settings.ini", "issue-report.md", "README.txt", "CheekyFoveatedDLSS-UEVR.log"} <= set(bundle.namelist())
    assert json.loads(bundle.read("diagnostics.json")) == baseline
    saved_settings = bundle.read("settings.ini").decode()
    assert "NrProcessingOrder=" + str(baseline["settings"]["NrProcessingOrder"]) in saved_settings
    assert "SchemaVersion=1" in saved_settings
    assert all("/" not in name and "\\" not in name for name in bundle.namelist())


def run(engine):
    lua = importlib.import_module("lupa." + engine).LuaRuntime(unpack_returned_tuples=True)
    lua.execute("""
        callbacks, sent, clicked, changes, drawn, values, combos, trees = {}, {}, {}, {}, {}, {}, {}, {}
        disabled = {false}
        slider_ranges = {}
        tree_stack, tree_order, tree_parents, closed_trees = {}, {}, {}, {}
        fs = {read = function() return "disableMod=0" end}
        uevr = {api = {}, lua = {}, sdk = {callbacks = {}}}
        for _, name in ipairs({'on_lua_event', 'on_frame'}) do
            uevr.sdk.callbacks[name] = function(fn) callbacks[name] = fn end
        end
        function uevr.lua.add_script_panel(name, fn)
            assert(name == "Cheeky Foveated DLSS", "Unexpected script panel name")
            callbacks.script_panel = fn
        end
        function uevr.api:dispatch_custom_event(event, text)
            table.insert(sent, {event=event, text=text})
        end
        local function widget(label, current)
            last_item = label
            values[label] = current
            local value = changes[label]
            changes[label] = nil
            if value ~= nil and not disabled[#disabled] then return true, value end
            return false, current
        end
        imgui = {
            ColumnFlags = {WidthStretch=8, WidthFixed=16},
            ImGuiStyleVar = {FrameRounding=12},
            push_style_color = function() style_colors = (style_colors or 0) + 1 end,
            pop_style_color = function(n) style_colors = style_colors - n; assert(style_colors >= 0) end,
            push_style_var = function() style_vars = (style_vars or 0) + 1 end,
            pop_style_var = function(n) style_vars = style_vars - n; assert(style_vars >= 0) end,
            calc_text_size = function(s) return {x=#s*7,y=14} end,
            get_cursor_screen_pos = function() return {x=10,y=10} end,
            draw_list_path_clear = function() end,
            draw_list_path_line_to = function(p) assert(p[1]==p[1] and p[2]==p[2]) end,
            draw_list_path_stroke = function() end,
            table_setup_column = function() end,
            tree_node = function(label)
                trees[label] = true; tree_parents[label] = tree_stack[#tree_stack]
                table.insert(tree_order, label)
                if closed_trees[label] then return false end
                table.insert(tree_stack, label); return true
            end,
            tree_pop = function() assert(#tree_stack > 0); table.remove(tree_stack) end,
            text = function(text) table.insert(drawn, text) end,
            text_colored = function(text) table.insert(drawn, text) end,
            spacing = function() end,
            begin_disabled = function(value) table.insert(disabled, value or disabled[#disabled]) end,
            end_disabled = function() table.remove(disabled) end,
            begin_table = function() return true end, end_table = function() end,
            table_next_row = function() end, table_next_column = function() end,
            same_line = function() end,
            checkbox = widget, slider_int = widget,
            slider_float = function(label, current, minimum, maximum)
                slider_ranges[label] = {minimum, maximum}
                return widget(label, current)
            end,
            is_item_active = function() return last_item == active_label end,
            combo = function(label, value, items)
                combos[label] = items
                assert(items[value] ~= nil, 'Unknown combo key: ' .. label)
                return widget(label, value)
            end,
            button = function(label)
                local hit = clicked[label]; clicked[label] = nil; return hit == true and not disabled[#disabled]
            end
        }
        json = {}
    """)
    g = lua.globals()

    def to_lua(value):
        if isinstance(value, dict):
            return lua.table_from({k: to_lua(v) for k, v in value.items()})
        if isinstance(value, list):
            return lua.table_from([to_lua(v) for v in value])
        return value

    g.json.load_string = lambda text: to_lua(json.loads(text))
    lua.execute((ROOT / "uevr/scripts/cheeky_foveated_dlss.lua").read_text())

    g.callbacks.on_lua_event("cheeky.foveated_dlss.load_state.v1", "enabled")

    def draw(click=None, changes=None):
        g.drawn = lua.table()
        g["values"] = lua.table()
        g.trees = lua.table()
        g.tree_order, g.tree_parents = lua.table(), lua.table()
        if click:
            g.clicked[click] = True
        for key, value in (changes or {}).items():
            g.changes[key] = value
        g.callbacks.script_panel()
        assert len(g.disabled) == 1, "Unbalanced disabled scope"
        assert len(g.tree_stack) == 0, "Unbalanced tree scope"
        assert (g.style_colors or 0) == 0 and (g.style_vars or 0) == 0, "Donate button leaked its style into the UEVR menu"

    def receive(value):
        g.callbacks.on_lua_event("cheeky.foveated_dlss.snapshot.v1", json.dumps(value))

    def last():
        return g.sent[len(g.sent)].text

    draw()
    assert any("Waiting" in t for t in g.drawn.values())
    g.callbacks.on_frame()
    assert last().endswith("\nget")
    state = copy.deepcopy(baseline)
    # Native host snapshots have no live UEVR rendering mode. Exercise the
    # Native Stereo menu explicitly, then test other modes below.
    state["settings"]["FgEnabled"] = False
    state["settings"]["FgMatchSrRegion"] = False
    state["afw_experiment"]["rendering_mode_known"] = True
    state["afw_experiment"]["rendering_mode"] = 0
    if "--fg-center" in sys.argv:
        state["settings"]["FgEnabled"] = False
        state["settings"]["FgCenterMethod"] = 0
        state["settings"]["FgMatchSrRegion"] = False
        state["afw_experiment"]["rendering_mode_known"] = True
        state["afw_experiment"]["rendering_mode"] = 0
    state["settings"]["Enabled"] = True
    state["settings"]["D3D12LowerHook"] = True
    state["setting_groups"]["D3D12LowerHook"] = "sr"
    state["d3d12_lower_hook_active"] = True
    state["d3d12_hook_restart_required"] = False
    receive(state)
    draw()
    assert g["values"]["Use lower DLSS hook (DX12)"] is True
    assert "DLSS-FG (experimental)" in g.trees
    assert g["values"]["Enable Frame Generation"] is False
    state["settings"]["FgMultiplier"] = 2
    receive(state)
    draw(changes={"FG multiplier": 4})
    assert "FgMultiplier=4" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["FgMultiplier"] = 4
    receive(state)
    state["settings"]["FgAutoMultiplier"] = False
    receive(state)
    draw(changes={"Automatic FG multiplier": True})
    assert "FgAutoMultiplier=true" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["FgAutoMultiplier"] = True
    state["fg"] = {"automatic": {"supported": True, "selected": 3, "presented": 2, "maximum": 4, "valid": True}}
    receive(state)
    draw()
    assert any("Auto: 3x selected / 2x presenting (maximum 4x)" in t for t in g.drawn.values())
    draw(changes={"Automatic FG multiplier": False})
    assert "FgAutoMultiplier=false" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["FgAutoMultiplier"] = False
    receive(state)
    draw(changes={"Enable Frame Generation": True})
    assert "FgEnabled=true" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["FgEnabled"] = True
    state["fg"] = {"layer_present": True, "controls_supported": True, "compatible_session": True,
                   "active": True, "state": "Generating frames", "native_frames": 5,
                   "generated_frames": 3, "fallback_frames": 1,
                   "captured_guides": 38, "eligible_guides": 0, "pending_guides": 2,
                   "match_reason": "Guides await submission"}
    receive(state)
    draw()
    if "--fg-center" in sys.argv:
        for method in (1, 2, 4, 5, 0):
            draw(changes={"FG center method": method})
            assert f"FgCenterMethod={method}" in last()
            state["request"] = state["applied_request"] = int(last().splitlines()[1])
            state["settings"]["FgCenterMethod"] = method
            receive(state)
            draw()
            assert g["values"]["FG center method"] == method
        state["renderer"] = 0
        state["settings"]["FgCenterMethod"] = 2
        state["settings"]["FgPeripheralMethod"] = 1
        receive(state)
        draw()
        assert g["values"]["FG center method"] == 2
        assert g["values"]["FG periphery method"] == 1
        assert g["values"]["FG GPU queue"] is None
        assert g["values"]["Release game rendering"] == state["settings"]["FgReleasePoint"]
        assert not any("Native D3D11: shader FG" in text for text in g.drawn.values())
        state["settings"]["FgPeripheralMethod"] = 3
        receive(state)
        draw()
        assert g["values"]["FG GPU queue"] is not None
        state["settings"].update(FgCenterMethod=4, FgPeripheralMethod=4, FgOpticalCenterPreset=1,
                                 FgOpticalPeripheralPreset=2, FgOpticalCenterPercent=100, FgOpticalPeripheralPercent=50,
                                 FgOpticalCenterGrid=4, FgOpticalPeripheralGrid=4)
        receive(state)
        draw()
        assert g["values"]["FG GPU queue"] is None
        for label, key, value in (("Center optical flow preset", "FgOpticalCenterPreset", 0),
                                  ("Periphery optical flow preset", "FgOpticalPeripheralPreset", 1),
                                  ("Center optical flow resolution", "FgOpticalCenterPercent", 75),
                                  ("Periphery optical flow resolution", "FgOpticalPeripheralPercent", 25),
                                  ("Center optical flow vector grid", "FgOpticalCenterGrid", 1),
                                  ("Periphery optical flow vector grid", "FgOpticalPeripheralGrid", 2)):
            draw(changes={label: value})
            assert f"{key}={value}" in last()
            state["request"] = state["applied_request"] = int(last().splitlines()[1])
            state["settings"][key] = value
            receive(state)
        # The depth-assisted variant keeps the same optical flow controls.
        state["settings"].update(FgCenterMethod=5, FgPeripheralMethod=5)
        receive(state)
        draw()
        assert g["values"]["Center optical flow preset"] == 0
        assert g["values"]["Periphery optical flow resolution"] == 25
        del state["settings"]["FgCenterMethod"]
        receive(state)
        draw()
        assert g["values"]["FG center method"] is None
        print("PASS FG center menu:", engine)
        return
    assert g["values"]["FG region width"] == state["settings"]["FgWidth"]
    assert g["values"]["Release game rendering"] == 3
    for retired in ("Submit repeat frames", "FG color capture", "FG OpenXR uploads", "FG guide capture",
                    "FG frame storage", "Direct periphery shading", "FG diagnostic mode", "FG periphery depth"):
        assert g["values"][retired] is None, "Fixed FG option must not be drawn: " + retired
    assert g.tree_parents["Advanced settings"] == "DLSS-FG (experimental)"
    assert g.tree_parents["Advanced settings"] is not None and g["values"]["FG pacing clock"] is not None
    assert "FG resource memory breakdown##fg" not in g.trees, "GPU memory belongs to Performance, not the FG menu"
    state["gpu_memory"] = {"supported":True,"scope":"process","adapter":"Test GPU","dedicated_capacity_bytes":12288*1048576,
        "local":{"valid":True,"usage_bytes":9000*1048576,"budget_bytes":8192*1048576,"headroom_bytes":0,
                 "over_budget_bytes":808*1048576,"peak_bytes":10000*1048576,"over_budget_samples":3},
        "nonlocal":{"valid":True,"usage_bytes":512*1048576,"budget_bytes":16384*1048576},
        "baseline":{"valid":True,"nonlocal_valid":True,"fg_enabled":False,"local_delta_bytes":500*1048576,"nonlocal_delta_bytes":-256*1048576}}
    receive(state)
    draw()
    for value in ("9000.0 MiB / 8192.0 MiB", "OVER LOCAL BUDGET by 808.0 MiB", "+500.0 MiB", "-256.0 MiB"):
        assert value in list(g.drawn.values()), "Missing memory diagnostic: " + value
    draw("Mark VRAM baseline##performance")
    assert last().endswith("\nmark_vram_baseline"), "VRAM baseline button did not reach the runtime"
    state["settings"]["FgEnabled"] = False
    receive(state)
    draw("Mark VRAM baseline##performance")
    assert last().endswith("\nmark_vram_baseline"), "VRAM baseline must remain accessible with FG OFF"
    state["settings"]["FgEnabled"] = True
    state["gpu_memory"]["local"]["valid"] = False
    state["gpu_memory"]["nonlocal"]["valid"] = False
    receive(state)
    draw()
    assert "OVER LOCAL BUDGET by 808.0 MiB" not in list(g.drawn.values())
    assert "+500.0 MiB" not in list(g.drawn.values()), "Unavailable queries must not display stale memory deltas"
    state["gpu_memory"]["supported"] = False
    receive(state)
    draw()
    assert any("GPU memory query unavailable" in t for t in g.drawn.values())
    state["gpu_memory"]["supported"] = True
    state["gpu_memory"]["local"]["valid"] = True
    state["gpu_memory"]["nonlocal"]["valid"] = True
    state["gpu_memory"]["fg_resources"] = {"layer_supported":True,"guides_supported":True,"presenter_present":True,
        "capture_tick":1,"worker_tick":1,"presenter_tick":1,"app_resource_bytes":320*1048576,
        "swapchain_estimated_bytes":128*1048576,"known_resource_bytes":448*1048576,"categories":{
            "capture":{"bytes":128*1048576,"resources":4,"reusable_bytes":32*1048576},
            "generated":{"bytes":64*1048576,"resources":2,"reusable_bytes":16*1048576}}}
    receive(state)
    draw()
    for value in ("128.0 MiB / 4 resources","64.0 MiB / 2 resources","448.0 MiB","32.0 MiB / 16.0 MiB","Not measured"):
        assert value in list(g.drawn.values()), "Missing per-category FG memory: " + value
    state["gpu_memory"]["fg_resources"]["layer_supported"] = False
    receive(state)
    draw()
    assert any("Partial breakdown: update OpenXR" in t for t in g.drawn.values())
    state["gpu_memory"]["fg_resources"]["layer_supported"] = True
    receive(state)
    draw()
    assert g.combos["FG GPU queue"][2] == "Shared game queue"
    for option in (2, 0, 1):
        draw(changes={"FG GPU queue": option})
        assert "FgQueuePriority=" + str(option) in last()
        state["request"] = state["applied_request"] = int(last().splitlines()[1])
        state["settings"]["FgQueuePriority"] = option
        receive(state)
        draw()
        assert g["values"]["FG GPU queue"] == option
    for point in (1, 2, 3, 4, 5, 0):
        draw(changes={"Release game rendering": point})
        assert "FgReleasePoint=" + str(point) in last()
        state["request"] = state["applied_request"] = int(last().splitlines()[1])
        state["settings"]["FgReleasePoint"] = point
        receive(state)
        draw()
        assert g["values"]["Release game rendering"] == point
    assert any("Generating frames" in t for t in g.drawn.values())
    for label, key in (("FG latency diagnostics","FgLatencyDiagnostics"),):
        for option in (1,0):
            draw(changes={label:option})
            assert key + "=" + str(option) in last()
            state["request"] = state["applied_request"] = int(last().splitlines()[1])
            state["settings"][key] = option
            receive(state)
            draw()
            assert g["values"][label] == option
    assert "Frame ID source" not in list(g.drawn.values())
    for text in ("Captured guides: 38","Guide matching: Guides await submission","Direct output fallback: unavailable"):
        assert text in list(g.drawn.values()), "Missing capture diagnostic: " + text
    assert g.tree_parents["CPU waits and pacing"] == "FG capture and presentation diagnostics"
    assert "Game waits for credit" in list(g.drawn.values())
    count = len(g.sent)
    draw("Copy FG diagnostic JSON")
    assert last().endswith("\ncopy_fg_diagnostics"), "Copy FG diagnostic JSON did not reach the runtime"
    assert "Install the matching OpenXR layer update." in list(g.drawn.values())
    performance = {"supported": True, "gpu_supported": True, "gpu_valid": True, "gpu_samples": 42,
                   "window_samples": 42, "foveated_eyes": 2, "center_ms": 2.75, "center_last_ms": 2.5,
                   "periphery_ms": .4, "periphery_last_ms": .35, "preparation_ms": .6,
                   "preparation_last_ms": .55, "total_ms": 3.9, "total_last_ms": 3.5,
                   "completion_wait_ms": 4.2, "copy_wait_ms": 1.1, "xr_wait_ms": 18.2, "xr_period_ms": 11.111,
                   "center_width": 280, "center_height": 232, "peripheral_width": 256, "peripheral_height": 256,
                   "output_width": 512, "output_height": 512}
    state["fg"]["performance"] = performance
    count = len(g.sent)
    receive(state)
    draw()
    count = len(g.sent)
    for text in ("DLSS-FG / shader center", "Periphery + output assembly", "2.750 / 2.500", "0.400 / 0.350",
                 "FG GPU total (per generated frame)", "280 x 232", "256 x 256", "512 x 512"):
        assert text in list(g.drawn.values()), "Missing FG timing: " + text
    for text in ("4.200 ms", "18.200 ms", "Headset frame period"):
        assert text not in list(g.drawn.values()), "Standalone does not show CPU wait rows: " + text
    assert len(g.sent) == count, "Reading FG performance must not change settings"
    state["fg"]["experiments"] = {"supported":True,"latency_enabled":True,"latency_valid":True,
        "latency_samples":12,"capture_dependency":{"mean":9.5,"last":9.0}}
    receive(state)
    draw()
    for label in ("Capture-fence dependency","9.500 / 9.000","Native GPU tail (excluded)"):
        assert label not in list(g.drawn.values()), "Standalone does not show calibrated latency rows: " + label
    state["fg"]["overhead"] = {"supported":True,"guide_samples":10,"capture_samples":10,
        "publication_samples":10,"upload_samples":20,"guide_ms":.2,"guide_last_ms":.1,
        "capture_ms":1.3,"capture_last_ms":1.2,"publication_ms":.3,"publication_last_ms":.2,
        "upload_ms":.4,"upload_last_ms":.3}
    receive(state)
    draw()
    for label in ("  Generation subtotal (per generated frame)","Guide capture (per real frame)","Color capture + guide handoff (per real frame)",
                  "  Capture subtotal","1.500 / 1.300","Generated output copy (per generated frame)","OpenXR uploads (per presentation)",
                  "  Presentation subtotal","0.700 / 0.500","Measured GPU stage sum","6.100 / 5.300","1.300 / 1.200"):
        assert label in list(g.drawn.values()), "Missing separately measured GPU stage: " + label
    assert "FG GPU total (per generated frame)" not in list(g.drawn.values()), "Do not label a partial processing interval as complete FG overhead"
    assert len(g.sent) == count, "Reading additional GPU stages must not change settings"
    state["fg"]["batch_timing"] = {"supported": True, "valid": True, "generated_count": 3,
        "optical_regions": 4, "batch_ms": 7.2, "batch_last_ms": 7.0,
        "phases": [{"index": 1, "ms": 5.0, "last_ms": 4.8}, {"index": 2, "ms": 1.0, "last_ms": 1.0},
                   {"index": 3, "ms": 1.0, "last_ms": 1.0}]}
    receive(state)
    draw()
    for label in ("Whole batch incl. output copies (per real frame)", "Generated stereo frame 1 (includes optical flow)",
                  "Generated stereo frame 2", "Generated stereo frame 3", "7.200 / 7.000", "5.000 / 4.800"):
        assert label in list(g.drawn.values()), "Missing distinct batch/phase scope: " + label
    assert len(g.sent) == count, "Reading batch timings must not change settings"
    state["fg"]["batch_timing"]["valid"] = False
    receive(state)
    draw()
    assert "Generated stereo frame 1 (includes optical flow)" not in list(g.drawn.values()), "Hide invalid batch timings"
    performance["asynchronous_presenter"] = True
    performance["producer_cpu_ms"] = .25
    performance["pacing_wait_ms"] = 12
    performance["queued_native"] = 2
    performance["skipped_native"] = 7
    performance["color_token"] = performance["guide_token"] = 42
    performance["inference_color_format"] = 28
    performance["shared_gpu_queue"] = 1
    performance["mismatched_guides"] = 3
    receive(state)
    draw()
    for label in ("FG completion wait (background)", "Game frame handoff (CPU)", "Rejected guide matches", "RGBA8 SDR"):
        assert label not in list(g.drawn.values()), "Standalone does not show buffered presenter rows: " + label
    state["fg"]["cadence"] = {"supported":True,"valid":True,"game_fps":32,"native_target_fps":60,
        "native_fps":32,"generated_fps":32,"unique_fps":64,"submission_fps":120,"refresh_hz":120,
        "app_image_wait_ms":4,"present_image_wait_ms":5,"begin_ms":6,"end_ms":7,"present_cpu_ms":8}
    count = len(g.sent)
    receive(state)
    draw()
    for label in ("FPS (game / native / generated)","32.0 / 32.0 / 32.0","Unique FPS output","64.0 FPS"):
        assert label in list(g.drawn.values()), "Missing measured cadence: " + label
    assert "Game image wait / call" not in list(g.drawn.values())
    assert len(g.sent) == count, "Reading cadence must not change settings"
    assert "Real frame" not in list(g.drawn.values()), "Older layers must not show zero as a measured frame age"
    state["fg"]["presentation"] = {"supported":True,"native_samples":10,"generated_samples":9,
        "native_age_ms":30,"native_age_last_ms":31,"generated_age_ms":22,"generated_age_last_ms":23,
        "newest_input_age_ms":12,"newest_input_age_last_ms":13}
    receive(state)
    draw()
    for label in ("Real frame","30.00 ms",
                  "Generated frame (at its interpolated time)","22.00 ms",
                  "Newest input used for the generated frame","12.00 ms"):
        assert label in list(g.drawn.values()), "Missing frame latency telemetry: " + label
    assert len(g.sent) == count, "Frame-age telemetry must never apply settings"
    state["fg"]["presentation"]["generated_samples"] = 0
    receive(state)
    draw()
    assert "Real frame" in list(g.drawn.values()) and "22.00 ms" not in list(g.drawn.values())
    del state["fg"]["presentation"]
    receive(state)
    draw()
    assert "Real frame" not in list(g.drawn.values()), "Missing optional export must preserve older layer compatibility"
    assert "No content-age samples yet." in list(g.drawn.values())
    performance["asynchronous_presenter"] = False
    performance["foveated_eyes"] = 0
    performance["center_width"] = performance["center_height"] = 512
    receive(state)
    draw()
    assert "FG full frame" in list(g.drawn.values())
    assert "0.400 / 0.350" not in list(g.drawn.values()), "Do not present full-frame FG as a measured periphery pass"
    performance["gpu_valid"] = False
    receive(state)
    draw()
    assert "Waiting for a complete GPU timing sample." in list(g.drawn.values())
    assert "2.750 / 2.500" not in list(g.drawn.values()), "Do not show stale/invalid GPU samples as current"
    performance["gpu_valid"] = True
    assert g.slider_ranges["FG transition width"][1] == 0
    for label,key,value in (("FG scale","FgResolutionPercent",50),
                            ("FG periphery scale","FgPeripheralPercent",25),
                            ("FG center method","FgCenterMethod",2),
                            ("FG periphery method","FgPeripheralMethod",3)):
        draw(changes={label:value})
        assert f"{key}={value}" in last(), "FG control must send its actual setting: " + label
        state["request"] = state["applied_request"] = int(last().splitlines()[1])
        state["settings"][key] = value
        receive(state)
    draw(changes={"Foveated frame generation": False})
    assert "FgFoveated=false" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["FgFoveated"] = False
    receive(state)
    draw()
    assert g["values"]["FG region width"] is None
    old_layer = copy.deepcopy(state)
    old_layer["fg"]["controls_supported"] = False
    receive(old_layer)
    draw()
    assert "Update the OpenXR layer and restart the game." in list(g.drawn.values())
    receive(state)
    draw("Reset FG defaults")
    assert last().endswith("\ndefaults_fg")
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    for key, value in baseline["settings"].items():
        if key.startswith("Fg"):
            state["settings"][key] = value
    receive(state)
    draw()
    assert g["values"]["Enable Frame Generation"] is False
    draw(changes={"Use lower DLSS hook (DX12)": False})
    assert "D3D12LowerHook=false" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["D3D12LowerHook"] = False
    state["d3d12_hook_restart_required"] = True
    receive(state)
    draw()
    assert g["values"]["Use lower DLSS hook (DX12)"] is False
    assert any("Active DLSS hook: Lower" in t for t in g.drawn.values())
    assert any("saved for next game restart" in t for t in g.drawn.values())
    draw(changes={"Use lower DLSS hook (DX12)": True})
    assert "D3D12LowerHook=true" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["D3D12LowerHook"] = True
    state["d3d12_hook_restart_required"] = False
    receive(state)
    draw()
    # Mapping selection is one transaction; manual-only eye order cannot edit Auto.
    count = len(g.sent)
    draw(changes={"First DLSS view is right eye": True})
    assert len(g.sent) == count
    draw(changes={"Mapping method": 4})
    assert "ManualStereoMapping=true" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["ManualStereoMapping"] = True
    receive(state)
    draw(changes={"First DLSS view is right eye": True})
    assert "ManualStereoRightFirst=true" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["ManualStereoRightFirst"] = True
    receive(state)
    draw(changes={"Mapping method": 2})
    assert "ManualStereoMapping=false" in last() and "EyeCalibrationMethod=2" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"].update(ManualStereoMapping=False, EyeCalibrationMethod=2)
    receive(state)
    for center_mode in (0, 1, 2):
        height_state = copy.deepcopy(state)
        height_state["settings"].update(CenterMode=center_mode, AlignedHeightOffset=.2, AutoStereoAlignment=True)
        receive(height_state)
        draw()
        assert g["values"]["Height offset"] == .2
        assert "Fallback height offset" not in g["values"]
    receive(state)
    rr = copy.deepcopy(state)
    rr["renderer"] = 1
    rr["apis"] = [{}, {"reconstruction_feature": 13}]
    rr["settings"].update(RrCenterPreset=6, RrPeripheralPreset=4, PeripheralDlaa=True)
    receive(rr)
    draw()
    assert g["values"]["Center RR preset"] == 6 and g["values"]["Peripheral RR preset"] == 4
    assert set(g.combos["Center RR preset"].keys()) == {0, 4, 5, 6}
    assert g["values"]["Center preset"] is None, "RR exposed SR-only presets"
    receive(state)
    draw()
    assert g["values"]["Center preset"] is not None and g["values"]["Center RR preset"] is None
    assert g.combos["Center preset"][10] == "J"
    assert g["values"]["Fix motion-vector blur"] is False
    draw(changes={"Fix motion-vector blur": True})
    assert "FixMotionBlur=true" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["FixMotionBlur"] = True
    receive(state)
    draw()
    assert g["values"]["Fix motion-vector blur"] is True
    receive(rr)
    draw()
    assert g["values"]["Fix motion-vector blur"] is None, "RR exposed the SR motion workaround"
    state["settings"]["FixMotionBlur"] = False
    receive(state)
    afw = copy.deepcopy(state)
    afw["renderer"] = 1  # AFW supports DX12; the newest ordinary host report may be DX11.
    afw["afw_experiment"] = {"enabled": True, "core_calls": 100, "lower_calls": 0,
                             "missing_lower_calls": 100, "rejected_core_reentry": 0,
                             "rendering_mode_known": True, "rendering_mode": 3}
    receive(afw)
    count = len(g.sent)
    draw()
    assert any(t == "Selected" for t in g.drawn.values())
    assert not g.trees["AFW details"] and g.trees["Support"] and g.trees["Performance"]
    assert g.trees["Stereo Eye Mapping"] and g.tree_parents["Stereo Eye Mapping"] == "Stereo and gaze"
    assert len(g.sent) == count, "AFW effective overrides must not rewrite saved preferences"
    assert "Foveation center" in g["values"] and "Enable DLSS-NR" in g["values"]
    assert "Automatic stereo alignment" in g["values"]
    assert ("Invert stereo eye order" in g["values"]) == (not afw["settings"]["AutoStereoAlignment"])
    assert "Automatic eye calibration (this session)" not in g["values"]
    assert "Center supersampling" in g["values"]
    afw["settings"].update(NrEnabled=True, NrFoveated=True, NrUseSrFoveation=False)
    receive(afw)
    draw()
    assert "Rendering order" in g["values"] and "NR width" in g["values"]
    assert "NR roundness" in g["values"] and "NR working scale" in g["values"]
    assert not any("after AFW motion preparation" in t for t in g.drawn.values())
    for label in ("Fovea width", "Fovea height", "NR width", "NR height"):
        assert g.slider_ranges[label][1] == 0.2 and g.slider_ranges[label][2] == 1
    afw["afw_experiment"].update(lower_calls=100, missing_lower_calls=0)
    afw["afw_experiment"].update(runtime_candidates=1, runtime_selected=True)
    receive(afw)
    draw()
    assert not any("ordinary DLSS passes through" in t for t in g.drawn.values())
    afw["afw_experiment"].update(warp_observer_ready=True, warp_calls=0, last_warp_age_ms=-1)
    receive(afw)
    draw()
    afw["settings"]["AfwManualCoverage"] = True
    afw["afw_experiment"].update(manual_coverage=True, warp_calls=3, last_warp_age_ms=20,
                                  effective_width=0.8, effective_height=0.6, effective_center_scale=1.25, coverage_observed=True)
    receive(afw)
    draw()
    assert "Stereo X offset" in g["values"] and "Extra margin per edge" in g["values"]
    assert "Roundness" in g["values"]
    assert "Depth-adaptive warp padding" not in g["values"]
    afw["afw_experiment"]["last_warp_age_ms"] = 1500
    receive(afw)
    draw()
    assert len(g.sent) == count, "Drawing AFW status and overrides must not write settings"
    afw["settings"]["AfwAutomaticCoverage"] = True
    afw["afw_experiment"].update(coverage_mode=2, projection_valid=True, warp_metadata_supported=True,
                                  last_evaluation_eye=1, last_warp_source_eye=1, last_warp_mode=3, source_left_calls=10, source_right_calls=12)
    receive(afw)
    draw()
    assert "Height offset" in g["values"] and "Stereo X offset" not in g["values"]
    assert g["values"]["AFW stereo coverage"] == 2
    afw["afw_experiment"]["last_warp_mode"] = 2
    receive(afw)
    draw()
    afw["afw_experiment"].update(coverage_mode=0, projection_valid=False)
    receive(afw)
    draw()
    assert any("waiting for matching UEVR projections" in t for t in g.drawn.values())
    afw["settings"]["CenterMode"] = 2
    afw["settings"]["SimulationPattern"] = 2
    afw["gaze"].update(using_gaze=True, afw_bilateral=True, afw_fresh_sample=True)
    afw["afw_experiment"].update(coverage_mode=3, projection_valid=True)
    receive(afw)
    draw()
    assert "Simulation pattern" in g["values"] and "Gaze smoothing (ms)" in g["values"]
    assert "Show next jump target" in g["values"] and "Roundness" in g["values"]
    assert g["values"]["AFW tracking-loss fallback"] == 2
    assert not any("waiting for matching UEVR projections" in t for t in g.drawn.values())
    assert len(g.sent) == count, "Gaze diagnostics must not rewrite preferences"
    afw["settings"]["Enabled"] = False
    receive(afw)
    draw()
    assert "Extra margin per edge" in g["values"]
    assert "Enable DLSS-NR" in g["values"] and "Fovea width" not in g["values"]
    afw["settings"]["Enabled"] = True
    afw["afw_experiment"].update(coverage_enabled=False, rendering_mode=2)
    receive(afw)
    draw()
    assert "Automatic stereo alignment" in g["values"]
    assert ("Invert stereo eye order" in g["values"]) == (not afw["settings"]["AutoStereoAlignment"])
    assert "Automatic eye calibration (this session)" not in g["values"]
    assert "AFW stereo coverage" not in g["values"]
    assert len(g.sent) == count, "Live AFW off/on must not rewrite preferences"
    afw["afw_experiment"].update(coverage_enabled=True, rendering_mode=3)
    afw["settings"]["CenterMode"] = 0
    # Collapsed diagnostics must keep the AFW dump out of the normal menu.
    receive(afw)
    g.closed_trees["Support"] = True
    g.closed_trees["Advanced AFW"] = True
    draw()
    assert not g.trees["AFW details"]
    assert not any("Full-frame core" in t or "Last warp call" in t for t in g.drawn.values())
    assert "Extra margin per edge" not in g["values"]
    assert g["values"]["AFW stereo coverage"] == 2
    assert any(t == "Selected" for t in g.drawn.values())
    assert len(g.sent) == count
    g.closed_trees["Support"] = False
    g.closed_trees["Advanced AFW"] = False
    draw()
    assert g.tree_parents["Advanced AFW"] == "Stereo and gaze"
    # Each mode switch is one transaction setting both legacy flags. Loading
    # an old file with both enabled must select Automatic without rewriting it.
    for mode in (1, 0, 2):
        count = len(g.sent)
        draw(changes={"AFW stereo coverage": mode})
        assert len(g.sent) == count + 1
        assert "AfwManualCoverage=" + str(mode == 1).lower() in last()
        assert "AfwAutomaticCoverage=" + str(mode == 2).lower() in last()
        afw["request"] = afw["applied_request"] = int(last().splitlines()[1])
        afw["settings"].update(AfwManualCoverage=mode == 1, AfwAutomaticCoverage=mode == 2)
        receive(afw)
        draw()
        assert g["values"]["AFW stereo coverage"] == mode
        assert len(g.sent) == count + 1
    afw["afw_experiment"].update(rendering_mode_known=False)
    receive(afw)
    draw()
    assert any(t == "Mode unavailable" for t in g.drawn.values())
    assert not any(t == "Selected" for t in g.drawn.values())
    legacy_afw = copy.deepcopy(afw)
    del legacy_afw["settings"]["AfwManualCoverage"]
    del legacy_afw["settings"]["AfwAutomaticCoverage"]
    del legacy_afw["settings"]["AfwWarpMargin"]
    receive(legacy_afw)
    draw()
    assert "AFW stereo coverage" not in g["values"] and "Extra margin per edge" not in g["values"]
    receive(state)
    draw()  # Open every tree, validate all widget types and enum keys.
    order = list(g.tree_order.values())
    assert order.index("Stereo and gaze") < order.index("DLSS-SR") < order.index("DLSS-NR (experimental)")
    assert g.tree_parents["Performance"] == "Cheeky Foveated DLSS"
    assert g.trees["Stereo Eye Mapping"] and g.tree_parents["Stereo Eye Mapping"] == "Stereo and gaze"
    for removed in ("AFW details", "GPU timestamp collection", "Eye mapping details", "DLSS view details",
                    "DX11 interception", "DX12 interception", "SR resolution and GPU timing", "NR status and GPU timing"):
        assert not g.trees[removed], "Developer diagnostics must stay in support reports: " + removed
    assert not any(t in ("Last result", "Settings revision / saved", "Crop input / output origin",
                         "Recorded / submitted", "Layer or adapter / matching ABI") for t in g.drawn.values())
    # Performance selects the actual NR order/shape, including a missing sample
    # in that mode. It must not accidentally display another mode's saved time.
    perf = copy.deepcopy(state)
    perf["renderer"] = 1
    perf["settings"]["NrEnabled"] = True
    perf["apis"][1].update(before_nr_full_ms=11, before_nr_foveated_ms=12, nr_full_ms=21, nr_foveated_ms=22)
    count = len(g.sent)
    for before, foveated, expected in ((True, False, 11), (True, True, 12), (False, False, 21), (False, True, 22)):
        perf["settings"].update(NrProcessingOrder=int(before), NrFoveated=foveated)
        receive(perf)
        draw()
        assert ("NR + preparation (Before)" if before else "NR (After)") in list(g.drawn.values())
        assert f"{expected:.3f} ms" in list(g.drawn.values())
    perf["apis"][1]["nr_foveated_ms"] = None
    receive(perf)
    draw()
    assert "21.000 ms" not in list(g.drawn.values()), "No full-frame fallback for an unsampled foveated NR call"
    assert len(g.sent) == count, "Reading performance must not write settings"
    receive(state)
    draw()
    assert any("Waiting for VR" in str(t) for t in g.drawn.values())
    assert "Automatic eye calibration (this session)" not in g["values"]
    for automatic in (True, False):
        visibility = copy.deepcopy(state)
        visibility["settings"].update(AutoStereoAlignment=automatic, AfwManualCoverage=False, AfwAutomaticCoverage=False)
        receive(visibility)
        draw()
        assert ("Invert stereo eye order" in g["values"]) == (not automatic)
        assert "Mapping method" in g["values"] and g.trees["Stereo Eye Mapping"]
    receive(state)
    for method in (1, 2):
        state["settings"]["EyeCalibrationMethod"] = method
        receive(state)
        count = len(g.sent)
        draw(changes={"Continuously validate eye calibration": False})
        g.callbacks.on_frame()
        assert len(g.sent) == count, "Corner methods must hide the recalibration policy"
    state["settings"]["EyeCalibrationMethod"] = 3
    receive(state)
    draw(changes={"Continuously validate eye calibration": False})
    g.callbacks.on_frame()
    assert "EyeCalibrationContinuous=false" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["EyeCalibrationContinuous"] = False
    receive(state)
    draw("Recalibrate now")
    assert last().endswith("\ncalibration_recalibrate")
    older = copy.deepcopy(state)
    older.pop("eye_calibration")
    receive(older)
    draw()
    assert any("Unavailable in this runtime" in str(t) for t in g.drawn.values())
    receive(state)
    state["support"] = {"busy": False, "zip": "C:/test/support/report.zip"}
    receive(state)
    for label, action in (("Donate", "donate"), ("Report an issue...", "report_issue"), ("Create support ZIP only", "report"),
                          ("Show ZIP", "show_report"), ("Open GitHub issue", "open_issue")):
        draw(label)
        assert last().endswith("\n" + action), "Wrong report action"
    state["support"]["busy"] = True
    receive(state)
    count = len(g.sent)
    draw("Report an issue...")
    assert len(g.sent) == count, "Busy report action should be disabled"
    state["support"]["busy"] = False
    receive(state)
    draw(changes={"Enable foveated DLSS-SR": False})
    receive(state)  # A periodic update must not overwrite a dirty false value.
    draw("Apply changes##bottom")
    assert "Enabled=false" in last()
    applied_id = int(last().splitlines()[1])
    draw(changes={"Enable foveated DLSS-SR": True})
    state["request"] = state["applied_request"] = applied_id
    state["settings"]["Enabled"] = False
    state["message"] = "An asynchronous save status can replace the message"
    receive(state)
    draw("Apply changes##bottom")
    assert "Enabled=true" in last(), "Newer edits were discarded by an older acknowledgement"
    rejected_id = int(last().splitlines()[1])
    state["request"] = rejected_id  # Keep applied_request old: rejected transaction.
    receive(state)
    draw("Apply changes##bottom")
    assert "Enabled=true" in last(), "Rejected edits should remain correctable"
    draw("Discard edits##bottom")
    draw()
    assert g["values"]["Enable foveated DLSS-SR"] is False
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["Enabled"] = True
    receive(state)
    # Drag across many values, then hold still. Neither movement nor time may
    # send GPU-setting updates until ImGui reports the slider inactive.
    for label, key, samples in (
        ("Fovea width", "Width", (0.35, 0.45, 0.55, 0.2)),
        ("Crop quantization (pixels)", "GazeQuantizationPixels", (2, 4, 8, 16)),
    ):
        count = len(g.sent)
        g.active_label = label
        for value in samples:
            draw(changes={label: value})
            g.callbacks.on_frame()
            assert len(g.sent) == count, "Slider applied during drag"
        for _ in range(20):
            draw()
            g.callbacks.on_frame()
        assert len(g.sent) == count, "Stationary held slider applied prematurely"
        g.active_label = None
        draw()  # Release with changed=false is still a commit.
        assert len(g.sent) == count + 1, "Release must send exactly one update"
        assert key + "=" + str(samples[-1]) in last()
        state["request"] = state["applied_request"] = int(last().splitlines()[1])
        state["settings"][key] = samples[-1]
        receive(state)
        draw()
        assert len(g.sent) == count + 1, "Release committed twice"
    count = len(g.sent)
    draw(changes={"Fovea width": 0.6})  # Keyboard edit ending without an active drag.
    assert len(g.sent) == count + 1 and "Width=0.6" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["Width"] = 0.6
    receive(state)
    # Hidden controls must not retain editable widgets; diagnostic trees stay available.
    state["renderer"] = 1
    state["settings"].update(Enabled=False, NrEnabled=False)
    receive(state)
    draw()
    assert g.trees["DLSS-SR"] and g.trees["Performance"]
    assert g["values"]["Fovea width"] is None and g["values"]["NR intensity"] is None
    state["settings"].update(Enabled=True, NrEnabled=True, NrFoveated=False, NrAutomaticMask=False)
    receive(state)
    draw()
    assert g["values"]["NR width"] is None and g["values"]["Use SR size and shape"] is None
    assert g["values"]["NR intensity"] is not None
    assert g.slider_ranges["NR intensity"][1] == 0 and g.slider_ranges["NR intensity"][2] == 1
    assert g["values"]["Skin structure"] is None, "Hide skin tuning without automatic masking"
    assert g["values"]["DLSS-NR preset"] is None and g["values"]["UI correction"] is None
    state["nr_details"]["hdr_input"] = False
    receive(state)
    draw()
    assert g["values"]["Paper white"] is None, "SDR has no paper-white conversion"
    state["nr_details"]["hdr_input"] = True
    receive(state)
    draw()
    assert g["values"]["Paper white"] is not None, "HDR must expose paper-white conversion"
    state["settings"]["NrAutomaticMask"] = True
    receive(state)
    draw()
    assert g["values"]["Skin structure"] is not None, "Automatic masking must expose skin tuning"
    state["settings"].update(NrFoveated=True, NrUseSrFoveation=True)
    receive(state)
    draw()
    assert g["values"]["NR width"] is None and g["values"]["Use SR size and shape"] is True
    state["settings"].update(NrUseSrFoveation=False, CenterMode=2)
    receive(state)
    draw()
    assert g["values"]["NR width"] is not None
    assert g.combos["Simulation pattern"][0] == "Figure eight (8 s)"
    assert g.combos["Simulation pattern"][5] == "Hold center"
    assert g.combos["DLSS-NR style"][2] == "Cinematic"
    count = len(g.sent)
    draw(changes={"DLSS-NR style": 2})
    assert len(g.sent) == count + 1 and "NrStyle=2" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["NrStyle"] = 2
    receive(state)
    draw()
    assert g["values"]["DLSS-NR style"] == 2, "Style acknowledgement was not retained"
    # Rendering order follows the same immediate apply and acknowledgement
    # flow as the existing selectors, while preserving working scale.
    state["settings"].update(NrProcessingOrder=0, NrWorkingScale=0.37)
    receive(state)
    draw()
    assert g.combos["Rendering order"][0] == "After upscaling"
    assert g.combos["Rendering order"][1] == "Before upscaling"
    assert g["values"]["Rendering order"] == 0
    count = len(g.sent)
    draw(changes={"Rendering order": 1})
    assert len(g.sent) == count + 1 and "NrProcessingOrder=1" in last()
    assert "NrWorkingScale=" not in last()
    order_request = int(last().splitlines()[1])
    draw(changes={"Rendering order": 0})  # New edit while the first is in flight.
    assert len(g.sent) == count + 1
    state["request"] = state["applied_request"] = order_request
    state["settings"]["NrProcessingOrder"] = 1
    receive(state)
    draw()
    assert len(g.sent) == count + 2 and "NrProcessingOrder=0" in last()
    assert g["values"]["Rendering order"] == 0, "Older ack overwrote a newer order draft"
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["NrProcessingOrder"] = 0
    receive(state)
    draw()
    assert len(g.sent) == count + 2 and abs(g["values"]["NR working scale"] - 0.37) < 0.0001
    draw(changes={"Rendering order": 1})
    state["request"] = int(last().splitlines()[1])  # Rejected: applied_request stays old.
    receive(state)
    draw("Apply changes##bottom")
    assert "NrProcessingOrder=1" in last(), "Rejected order draft could not be retried"
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["NrProcessingOrder"] = 1
    receive(state)
    # A reset clears only its group's held edits; unrelated drafts survive its acknowledgement.
    g.active_label = "Gaze smoothing (ms)"
    draw(changes={"Gaze smoothing (ms)": 35})
    count = len(g.sent)
    draw("Reset DLSS-NR defaults")
    assert len(g.sent) == count + 1 and last().endswith("\ndefaults_nr")
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["NrEnabled"] = False
    state["settings"]["NrProcessingOrder"] = 0
    receive(state)
    g.active_label = None
    draw()
    assert "GazeSmoothingMs=35" in last(), "Unrelated held edit was lost by group reset"
    assert "NrProcessingOrder=" not in last(), "NR reset resurrected the rendering order draft"
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["GazeSmoothingMs"] = 35
    receive(state)
    g.active_label = "Fovea width"
    draw(changes={"Fovea width": 0.8})
    draw("Reset DLSS-SR defaults")
    assert last().endswith("\ndefaults_sr")
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["Width"] = 0.55
    receive(state)
    g.active_label = None
    count = len(g.sent)
    draw()
    assert len(g.sent) == count and g["values"]["Fovea width"] == 0.55, "Reset resurrected an old slider draft"
    draw("Reset Stereo / gaze defaults")
    assert last().endswith("\ndefaults_gaze")
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    receive(state)
    state["frame"] = {"present_ms": 10, "sr_enabled_ms": 10, "sr_disabled_ms": 20}
    receive(state)
    draw()
    assert any("100.0 FPS" in t for t in g.drawn.values())
    assert any("10.000 ms (50.0%)" in t for t in g.drawn.values())
    assert any("Not sampled / unavailable" in t for t in g.drawn.values())
    state["renderer"] = 0
    receive(state)
    draw()
    assert any("DLSS-NR on DX11 requires DX12 transport." in t for t in g.drawn.values())
    state["settings"]["CenterMode"] = 2
    state["settings"]["AutoStereoAlignment"] = False
    receive(state)
    draw()  # Simulation and manual alignment branches.
    # Reaching an older runtime must remove unsupported queued edits, including
    # a newer draft waiting for an outstanding selector acknowledgement.
    state["renderer"] = 1
    state["settings"].update(NrEnabled=True, NrProcessingOrder=0)
    receive(state)
    draw()
    assert g["values"]["Rendering order"] == 0, "NR defaults did not reset the selector"
    draw(changes={"Rendering order": 1})
    order_request = int(last().splitlines()[1])
    draw(changes={"Rendering order": 0})
    older = copy.deepcopy(state)
    older["settings"].pop("NrProcessingOrder")
    older["setting_groups"].pop("NrProcessingOrder")
    older["request"] = older["applied_request"] = 0  # A freshly connected older runtime.
    receive(older)
    count = len(g.sent)
    draw("Apply changes##bottom", changes={"Rendering order": 1})
    g.changes["Rendering order"] = None  # No such widget existed in this draw.
    g.callbacks.on_frame()
    assert len(g.sent) == count, "Older runtime received an unsupported rendering-order setting"
    assert g["values"]["Rendering order"] is None
    assert not any("Waiting for settings acknowledgement" in t for t in g.drawn.values()), "Unsupported in-flight order blocked reconnect"
    assert any("Rendering order: Unavailable in this runtime" in t for t in g.drawn.values())
    receive({"protocol": 999, "settings": {}})
    draw()
    assert any("version mismatch" in t for t in g.drawn.values())
    g.callbacks.on_lua_event("cheeky.foveated_dlss.snapshot.v1", "{bad json")
    draw()
    assert any("invalid response" in t for t in g.drawn.values())
    for _ in range(601):
        g.callbacks.on_frame()
    draw()
    assert any("Waiting" in t for t in g.drawn.values()), "Stale host must show disconnected"
    state["settings"].update(NrProcessingOrder=1, NrWorkingScale=0.37)
    receive(state)
    count = len(g.sent)
    draw()
    assert g["values"]["Rendering order"] == 1 and g["values"]["NR working scale"] == 0.37
    assert len(g.sent) == count, "Reconnect resent a discarded rendering-order edit"
    state["renderer"] = 0
    state["revision"] += 1
    state["settings"].update(D3D11D3D12Transport=False, NrEnabled=False)
    receive(state)
    draw()
    assert g["values"]["DX11 -> DX12 transport"] is False
    assert g["values"]["Enable DLSS-NR"] is None
    draw(changes={"DX11 -> DX12 transport": True})
    assert "D3D11D3D12Transport=true" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["D3D11D3D12Transport"] = True
    receive(state)
    draw()
    assert g["values"]["Enable DLSS-NR"] is False
    draw(changes={"Enable DLSS-NR": True})
    assert "NrEnabled=true" in last()
    state["request"] = state["applied_request"] = int(last().splitlines()[1])
    state["settings"]["NrEnabled"] = True
    receive(state)
    state["renderer"] = 1
    receive(state)
    draw()
    assert g["values"]["DX11 -> DX12 transport"] is None
    assert g["values"]["Enable DLSS-NR"] is True
    print(engine + ": menu, protocol, drafts, acknowledgements, slider release and reconnect passed")


for engine in ("luajit21", "lua54"):
    run(engine)
