# Cheeky Foveated DLSS: UEVR preview

This preview runs Cheeky's DLSS processing from a UEVR plugin and puts its
controls in UEVR's LuaLoader menu. ReShade is not required. The original ReShade
add-on remains a separate build. The user reports the preceding plugin working
in Hogwarts Legacy; this UI update still needs game/headset validation.

## UI parity update

Stereo and gaze is the first settings section. Frame-rate comparison is inside
DLSS-SR, alongside its resolution and GPU timings.

**Report an issue...** now builds a support ZIP, opens a prefilled GitHub issue
and selects the ZIP in Explorer. Add your description, drag the ZIP into the
issue and submit it yourself. Nothing is uploaded or submitted automatically.
The archive contains the current diagnostics JSON, configured settings, an
issue summary, recent Cheeky/UEVR log tails and UEVR configuration when available.
Each input log/config is capped at 2 MiB; missing files are listed in README.txt.
ZIPs are saved under `support/` in the game's UEVR configuration folder, and
their path is displayed in the menu. **Create support ZIP only** avoids opening
applications; **Show ZIP** and **Open GitHub issue** let you reopen them later
during the same runtime session. The original standalone diagnostics JSON is
still written beside the INI. Review the bundle before sharing; logs can contain
personal paths.

The subsequent GPU-timing update fixes abandoned timestamp slots after an
unsubmitted command list is reset, and matches forwarding graphics wrappers
to submitted native lists using object private data. It retains fencing on the
actual submission queue. **Diagnostics > GPU timestamp collection** and the
log now show recorded, submitted, completed and discarded queries, outstanding
work and errors. The log writes a timing summary every five seconds, so it is
useful even if the game closes before you save a report.

Standalone tests reproduce slot exhaustion in the old build and verify recovery,
wrapper/native matching and native/center/peripheral GPU readback on WARP and
the hardware GPU. These use mock NGX calls, not NVIDIA DLSS or a running game;
the exact cause of the reported Hogwarts timing failure remains unconfirmed.

- DLSS-SR, DLSS-NR and Stereo/gaze each have an expandable section and their
  own defaults button. Resets affect only that group's settings (including its
  enable switch); SR reset preserves gaze placement and NR settings.
- Disabled SR/NR processing hides its tuning controls. Full-frame NR hides
  foveation controls; using SR size/shape hides the independent NR geometry.
  Status, retained timings and reset buttons remain accessible.
- Simulation patterns and NR presets use named dropdowns. All float/integer
  sliders still send one transaction on release, including keyboard edits.
- Diagnostics use aligned label/value tables, with SR crop and motion-vector
  sizes, GPU timings, NR route/calls/results/VRAM, per-eye mapping, individual
  DLSS views and interception details. Zero GPU timings mean unsampled or
  unavailable, rather than a measured zero cost.
- FPS measures **UEVR present callback cadence**, averaged over 250 ms. It is
  not the headset's display/reprojection rate. The SR enabled/disabled comparison
  retains the last sample of each mode after a one-second settling period.
  Toggle SR in the same scene and keep other settings constant when comparing.
- The NR DLL loader also searches beside the running game executable; see
  the placement and retry instructions below.

Lua formatting uses UEVR's [documented ImGui bindings](https://docs.uevr.io/plugins/lua/additional-bindings/imgui.html).

## Checkpoint and broader build

The original preview is preserved at commit `0ed7d4d`, with the separately named
`CheekyFoveatedDLSS-0.2.4-UEVR-checkpoint-0ed7d4d.zip` and matching symbols.
The subsequent `late-attach` package adds:

- DX11 adoption of an existing DLSS-SR handle when a complete evaluation is first
  observed, covering both exported callback variants and preserving known callback
  pairs. Incomplete metadata is forwarded without creating a private feature.
- Public DX12 handle adoption and cleanup on release, including the C callback
  variant. Missing creation flags, quality, dimensions or required resources cause
  passthrough before the private processing path changes parameters.
- Streamline setter discovery from a future evaluation, followed by an inline
  detour that also covers pointers cached before injection. Historical options
  are not guessed. When this viewport's options are unavailable, the original
  Streamline call can reach native NGX processing instead of suppressing it.
- Game options are forwarded unchanged. Local Streamline reconstruction accepts
  the known v3 options/v1 viewport layout without extension chains; other layouts,
  another viewport's options, and DX11 command contexts use the native fallback.
  Discovery failures are retried at most once a second during evaluation.
- Diagnostics distinguish setter interception, observed options and attempted
  native fallback. A fallback counter alone does not prove active processing.

The native fallback requires the game to reach an intercepted NGX evaluation with
enough metadata. Hidden/inlined entry points, incompatible wrappers, missing
metadata, core-only DX11 evaluation and runtime DLL replacement remain compatibility
limits. This is broader coverage, not universal game support. OptiScaler and
DX11-to-DX12 transport remain outside this change.

For comparison, test the checkpoint first. Save its log/report, exit the game
fully, replace both DLLs and the Lua script with the later package, and repeat
the same scene, settings and injection timing. Keep the same INI for a comparable
run. `BUILD.txt` inside each ZIP identifies its source commit. Each test uses one
complete package; changing versions requires a fresh game process.

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
4. First check whether processing activates after injection without changing
   DLSS settings. If it does not, retain a diagnostic report before trying a
   DLSS off/on toggle. The broader build should adopt existing features when
   their evaluations contain the required metadata; a toggle is a diagnostic
   experiment rather than a prerequisite or guaranteed workaround.
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
  A compatible `nvngx_dlssnr.dll` is searched for beside the **runtime DLL** in
  `plugins/CheekyFoveatedDLSS/` first, then beside the **running game executable**
  (often `Binaries/Win64`, rather than the launcher). Both paths are absolute;
  the working directory is not searched. Each attempted path and Windows loader
  error is logged. After adding a missing DLL, click **Reset NR history / retry**.
  A load error can also mean an incompatible DLL or a missing dependency.
  NVIDIA binaries are not included.
- Start with Native Stereo. Synchronized Sequential and AFR need separate game
  validation; this preview does not establish their temporal-history correctness.
- Reloading the adapter pauses processing and reconnects to the resident runtime.
  **Updating runtime binaries requires restarting the game.** Runtime hooks stay
  installed until process exit; unloading the adapter is not complete unhooking.
- The user reports the preceding UEVR plugin build working in Hogwarts Legacy.
  The UI-parity update still needs their game/headset test; other games and
  stereo modes remain unverified.

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

The build also runs `--late-dx11`, `--late-dx11-c`, `--late-dx12`, `--late-dx12-c`,
`--late-streamline` and `--late-streamline-dx11`. These load local fake NGX and
Streamline DLLs before the actual plugin, initialize/create/evaluate beforehand,
then continue through cached pointers after hook installation. Missing metadata,
reuse, release/recreation, setter interception and per-viewport fallback are checked.
The fake DLLs are isolated in `test-fixtures` and excluded from install ZIPs.
Use `./scripts/package-uevr.ps1 -Label late-attach-<commit>` for a distinct package.

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
Streamline's [public DLSS header](https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_dlss.h)
defines the options layout and a state getter containing a VRAM estimate, rather
than historical options; the fallback therefore relies on future NGX evaluations.

## Eye calibration diagnostics

**Diagnostics and support > Eye calibration** exposes the shared calibration
status, correction counter, sample backlog and CPU/GPU measurements. Counter
reset preserves the current mapping; the enable switch applies to this session.
The backend currently requires native OpenVR and D3D11 2D submissions. D3D12 and
OpenXR do not gain pixel calibration from this update. Full details are in
`EYE-CALIBRATION.md` in the package and the repository root. Support ZIP snapshots
include the same diagnostics.
