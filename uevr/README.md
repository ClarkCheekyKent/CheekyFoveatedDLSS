# Cheeky Foveated DLSS: UEVR preview

This preview runs Cheeky's DLSS processing from a UEVR plugin and puts its
controls in UEVR's LuaLoader menu. ReShade is not required. The original ReShade
add-on remains a separate build. Actual UEVR/game/headset validation is pending.

## Install for the first Hogwarts Legacy test

1. Close the game. Use a UEVR build with plugin API **2.39.0 or compatible newer
   2.x**. Older API versions are rejected with a plugin-load error. The SDK is
   pinned in `third_party/uevr/REVISION`; UEVR's release name is not its API version.
2. Move `CheekyFoveatedDLSS.addon64` out of the game's ReShade add-on search
   locations for this test. Keep it somewhere safe for rollback. ReShade itself
   and unrelated effects can remain installed. Do not run both Cheeky integrations
   in one process. The new builds guard against this; the plugin also checks for
   the old add-on's normal filename.
3. Open the **per-game configuration directory** from UEVR. It is normally
   `%APPDATA%\UnrealVRMod\<game-executable-name>`. Extract the ZIP directly into
   that directory, retaining the folder structure below. Do not put it in the
   game executable folder or UEVR's own installation folder.
4. Keep the matching Cheeky OpenXR layer installed if using OpenXR automatic
   alignment or eye tracking. This preview uses the existing ABI 4 layer; it
   does not register a new layer or replace it with UEVR gaze handling. The
   existing matching `CheekyEyeTrackingSetup.exe` installs it if needed. Native
   OpenVR uses the existing built-in adapter. Manual fixed placement can work
   without the OpenXR layer.
5. Start Hogwarts Legacy with DX12 and DLSS enabled, then inject UEVR using your
   normal sequence. Start with **Native Stereo** and **DLSS-NR off**.
6. Open UEVR's menu, then **LuaLoader > Cheeky Foveated DLSS**. If scripts are
   shown individually, expand `cheeky_foveated_dlss.lua` first. Confirm the menu
   is visible and usable in the headset, and the status says Ready.

```text
<per-game UEVR configuration directory>/
  plugins/
    CheekyFoveatedDLSS.dll
    CheekyFoveatedDLSS/
      CheekyFoveatedDLSSRuntime.dll
  scripts/
    cheeky_foveated_dlss.lua
```

The nested runtime DLL is intentional: PluginLoader should load only the thin
adapter. Always install the two DLLs and Lua file from the same package.

## Test in the headset

Sliders update their displayed value while dragging and apply **once on release**.
Holding a slider still while pressed does not apply it. Checkboxes and selections
apply immediately. Settings save automatically after an accepted change. Apply
and Discard are available for pending or rejected edits. Alt+Shift+/ toggles SR.

1. Enable the red alignment border, keep Fixed placement and automatic stereo
   alignment, and check that both eyes show the correct region. Inspect left/right
   mappings and the alignment source under Diagnostics and support. A missing
   eye tracker is expected with fixed placement.
2. Drag width, height and supersampling, hold each still, then release. Check that
   the image changes only on release and that there is no repeated recreation
   or stutter during the drag. Test a controller and mouse if both are available.
3. Toggle SR off/on and verify the effect in both eyes. Check that evaluation and
   active counters increase. Compare GPU timings after the scene settles; zero
   timing samples can mean unavailable measurements.
4. If the plugin reports no evaluations after late injection, toggle the game's
   DLSS off/on to recreate its feature. Injection after DLSS creation can miss
   metadata; this must be validated in Hogwarts rather than assumed supported.
5. Restart the game and confirm settings persist. Then test pause menus and
   loading transitions. Try real/simulated gaze only after fixed alignment works.

If anything is wrong, select **Write diagnostic report** and retain these files
from the same per-game UEVR directory:

- `CheekyFoveatedDLSS-diagnostics.json`
- `CheekyFoveatedDLSS-UEVR.log`
- `CheekyFoveatedDLSS.ini`
- UEVR's own log, including PluginLoader/Lua errors

Report the UEVR build, rendering mode, runtime, GPU, and whether the failure
happens before injection, after injection, or after changing a particular control.

## Preview limits and rollback

- DX12 has a native submission/copy observer. It currently supports one detected
  queue/command-list implementation; changing to incompatible wrapper methods
  pauses processing. Shader blits and nonzero mip/slice copy routes are not
  tracked. Some game-specific eye mappings may therefore fall back.
- DX11 uses the direct processing path. DX11-to-DX12 transport and DX11 NR are
  unavailable in this preview. DX12 NR remains experimental and off by default.
  A compatible `nvngx_dlssnr.dll`, if used, belongs beside the **runtime DLL** in
  the nested plugin folder. NVIDIA binaries are not included.
- Start with Native Stereo. Synchronized Sequential and AFR need separate game
  validation; this preview does not establish their temporal-history correctness.
- Reloading the adapter pauses processing and reconnects to the resident runtime.
  **Updating runtime binaries requires restarting the game.** Runtime hooks stay
  installed until process exit; unloading the adapter is not complete unhooking.
- Existing ReShade + UEVR success in Hogwarts is useful evidence, but it does
  not prove this plugin's hook order, late injection, UI input or eye mapping.

For rollback, close the game, remove the two plugin files and the Lua script
listed above, restore the saved `.addon64`, and start a fresh game process.
The OpenXR layer and ReShade configuration do not need changing.

## Build and game-free validation

```powershell
./scripts/build.ps1 -Configuration Release
./bin/Release/CheekyUEVRTests.exe --hardware
./bin/Release/CheekyTests.exe --d3d12-composite
./bin/Release/CheekyTests.exe --motion-resample
./scripts/package-uevr.ps1
```

The build script runs core tests plus a fake UEVR host that loads the real plugin,
exercises its ABI, messages, persistence, reset, duplicate-owner rejection and
unload/reload, and uses WARP for native D3D12 observer checks. `--dx11` exercises
the DX11 host path; `--hardware` repeats native observer checks on the default GPU.
Neither starts UEVR or a game. CMake also builds these targets and registers the
host tests with CTest. These tests do not evaluate NVIDIA DLSS.

For Lua behavior tests, optionally install `lupa==2.8` into `build/test-python`
and run `python tests/uevr_menu_tests.py` after the Release host test. This tests
the actual script under LuaJIT and Lua 5.4 with mocked UI bindings, including
float/integer slider release, holding still, keyboard edits and acknowledgements.
Lupa is a test dependency only and is not packaged.

## Implementation boundaries

`uevr/plugin.cpp` is an unloadable UEVR adapter. It queues Lua commands and sends
status from present callbacks. `uevr/runtime.cpp` owns settings, logging and
persistence around the existing NGX/Streamline interception and rendering code.
It is pinned before detours start, so workers, detours and resource-lifetime
callbacks cannot jump into an unloaded DLL. Detach uses an attachment token and
atomic processing gate, without waiting for threads under the Windows loader lock.

`uevr/graphics_observer.cpp` supplies D3D12 submission, copy, reset and lifetime
observations previously supplied by ReShade. The runtime builds without ReShade
API or ImGui headers. The core settings API provides coherent configured/effective
snapshots shared by both builds. A standalone host can reuse these boundaries;
OptiScaler integration is deferred and no OptiScaler code is included.

Upstream references: [UEVR plugin loading](https://docs.uevr.io/plugins/getting_started.html),
[Lua callbacks](https://docs.uevr.io/plugins/lua/callbacks.html),
[ImGui bindings including is_item_active](https://docs.uevr.io/plugins/lua/additional-bindings/imgui.html),
[pinned public API](https://github.com/praydog/UEVR/blob/4ee5c6b6162dee2291fc75f9dfc57667f6d45a2d/include/uevr/API.h).
