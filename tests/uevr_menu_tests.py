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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build/test-python"))
reports = list((ROOT / "bin/Release/uevr-test-data").glob("*/CheekyFoveatedDLSS-diagnostics.json"))
if not reports:
    raise SystemExit("Run CheekyUEVRTests.exe first to produce a real runtime snapshot.")
baseline = json.loads(max(reports, key=lambda p: p.stat().st_mtime).read_text())


def run(engine):
    lua = importlib.import_module("lupa." + engine).LuaRuntime(unpack_returned_tuples=True)
    lua.execute("""
        callbacks, sent, clicked, changes, drawn, values = {}, {}, {}, {}, {}, {}
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
            if value ~= nil then return true, value end
            return false, current
        end
        imgui = {
            tree_node = function() return true end, tree_pop = function() end,
            text = function(text) table.insert(drawn, text) end,
            same_line = function() end,
            checkbox = widget, slider_int = widget, slider_float = widget,
            is_item_active = function() return last_item == active_label end,
            combo = function(label, value, items)
                assert(items[value] ~= nil, 'Unknown combo key: ' .. label)
                return widget(label, value)
            end,
            button = function(label)
                local hit = clicked[label]; clicked[label] = nil; return hit == true
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
        if click:
            g.clicked[click] = True
        for key, value in (changes or {}).items():
            g.changes[key] = value
        g.callbacks.on_draw_ui()

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
    state["renderer"] = 0
    receive(state)
    draw()
    assert any("unavailable on the DX11" in t for t in g.drawn.values())
    state["settings"]["CenterMode"] = 2
    state["settings"]["AutoStereoAlignment"] = False
    receive(state)
    draw()  # Simulation and manual alignment branches.
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
    print(engine + ": menu, protocol, drafts, acknowledgements, slider release and reconnect passed")


for engine in ("luajit21", "lua54"):
    run(engine)
