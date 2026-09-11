"""Run the real menu against a mocked UEVR host (requires test-only lupa).

python -m pip install --target build/test-python lupa==2.8
python tests/uevr_menu_tests.py
This validates Lua behavior, not UEVR's renderer, input routing, or headset UI.
"""
import copy
import importlib
import json
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build/test-python"))
reports = list((ROOT / "bin/Release/uevr-test-data").glob("*/CheekyFoveatedDLSS-diagnostics.json"))
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
        tree_stack, tree_order, tree_parents = {}, {}, {}
        uevr = {api = {}, sdk = {callbacks = {}}}
        for _, name in ipairs({'on_lua_event', 'on_frame', 'on_draw_ui'}) do
            uevr.sdk.callbacks[name] = function(fn) callbacks[name] = fn end
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
            tree_node = function(label)
                trees[label] = true; tree_parents[label] = tree_stack[#tree_stack]
                table.insert(tree_order, label); table.insert(tree_stack, label); return true
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
            checkbox = widget, slider_int = widget, slider_float = widget,
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

    def draw(click=None, changes=None):
        g.drawn = lua.table()
        g["values"] = lua.table()
        g.trees = lua.table()
        g.tree_order, g.tree_parents = lua.table(), lua.table()
        if click:
            g.clicked[click] = True
        for key, value in (changes or {}).items():
            g.changes[key] = value
        g.callbacks.on_draw_ui()
        assert len(g.disabled) == 1, "Unbalanced disabled scope"
        assert len(g.tree_stack) == 0, "Unbalanced tree scope"

    def receive(value):
        g.callbacks.on_lua_event("cheeky.foveated_dlss.snapshot.v1", json.dumps(value))

    def last():
        return g.sent[len(g.sent)].text

    draw()
    assert any("Waiting" in t for t in g.drawn.values())
    g.callbacks.on_frame()
    assert last().endswith("\nget")
    state = copy.deepcopy(baseline)
    state["settings"]["Enabled"] = True
    receive(state)
    draw()  # Open every tree, validate all widget types and enum keys.
    order = list(g.tree_order.values())
    assert order.index("Stereo and gaze") < order.index("DLSS-SR") < order.index("DLSS-NR (experimental)")
    assert g.tree_parents["Frame rate comparison"] == "DLSS-SR"
    assert g.tree_parents["Eye calibration"] == "Diagnostics and support"
    assert any("Waiting for VR" in str(t) for t in g.drawn.values())
    draw("Reset eye calibration counters")
    assert last().endswith("\ncalibration_reset")
    draw(changes={"Automatic eye calibration (this session)": False})
    assert last().endswith("\ncalibration_disable")
    draw(changes={"Automatic eye calibration (this session)": True})
    assert last().endswith("\ncalibration_enable")
    older = copy.deepcopy(state)
    older.pop("eye_calibration")
    receive(older)
    draw()
    assert any("Unavailable in this runtime" in str(t) for t in g.drawn.values())
    receive(state)
    state["support"] = {"busy": False, "zip": "C:/test/support/report.zip"}
    receive(state)
    for label, action in (("Report an issue...", "report_issue"), ("Create support ZIP only", "report"),
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
        ("Fovea width", "Width", (0.35, 0.45, 0.55, 0.75)),
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
    assert g.trees["DLSS-SR"] and g.trees["SR resolution and GPU timing"]
    assert g["values"]["Fovea width"] is None and g["values"]["NR intensity"] is None
    state["settings"].update(Enabled=True, NrEnabled=True, NrFoveated=False)
    receive(state)
    draw()
    assert g["values"]["NR width"] is None and g["values"]["Use SR size and shape"] is None
    assert g["values"]["NR intensity"] is not None
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
    assert g.combos["DLSS-NR preset"][7] == "Preset G"
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
    assert any("+50.0 FPS (+100.0%)" in t for t in g.drawn.values())
    assert any("Not sampled / unavailable" in t for t in g.drawn.values())
    state["renderer"] = 0
    receive(state)
    draw()
    assert any("unavailable on the DX11" in t for t in g.drawn.values())
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
    print(engine + ": menu, protocol, drafts, acknowledgements, slider release and reconnect passed")


for engine in ("luajit21", "lua54"):
    run(engine)
