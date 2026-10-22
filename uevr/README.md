# Cheeky Foveated DLSS for UEVR

Runs Cheeky's DLSS processing with controls in UEVR. Remove ReShade from the game before using the plugin.

## Installation

### Build a combined package from source

Install Visual Studio C++ build tools (MSBuild and an x64 C++ toolset) and Inno
Setup 6.3 or newer, then run this from the repository root:

```powershell
.\scripts\build-uevr-package.ps1 -Label release
```

This builds the Release solution, runs the tests, rebuilds the OpenXR layer
installer, and packages the plugin DLL, processing runtime, Lua script, licenses,
and `OpenXR/CheekyOpenXRSetup.exe` into
`bin/Release/CheekyFoveatedDLSS-<version>-UEVR-release.zip`. The packager verifies
every ZIP payload against its staged SHA-256 hash and prints the archive hash.
An existing ZIP with the same version and label is replaced.

Use `-SkipTests` to skip test execution, or
`-IsccPath 'C:\path\to\ISCC.exe'` if Inno Setup is not found automatically.
The installer is compiled during packaging; run the included installer separately
to install the shared OpenXR layer.

### Install the package

1. Close the game. Use UEVR with plugin API **2.39.0 or compatible newer 2.x**.
2. Uninstall ReShade from the game using the ReShade installer, and remove `CheekyFoveatedDLSS.addon64` if present.
3. Open the game's configuration directory from UEVR—normally `%APPDATA%\UnrealVRMod\<game-executable-name>`—and extract the UEVR ZIP there, preserving this structure:

```text
plugins/
  CheekyFoveatedDLSS.dll
  CheekyFoveatedDLSS/
    CheekyFoveatedDLSSRuntime.dll
scripts/
  cheeky_foveated_dlss.lua
```

4. **For OpenXR gaze, or ordinary stereo alignment/calibration, run the matching `OpenXR/CheekyOpenXRSetup.exe` included in the UEVR ZIP before starting the game.** AFW fixed/automatic coverage needs no OpenXR layer. Native OpenVR uses the built-in adapter, including when its compositor initialized before the plugin.
5. Start the game with DLSS enabled, inject UEVR and open **LuaLoader → Cheeky Foveated DLSS**. Check **Enable Cheeky for this game**. New profiles start disabled; the enable choice is saved per game.

For a global installation, extract the same `plugins/` and `scripts/` directories
under `%APPDATA%\UnrealVRMod\UEVR`. Avoid also installing a second copy in the
game's profile. The thin adapter stays dormant until enabled for that game, then
loads the processing runtime. The Lua enable choice is stored in `cheeky.txt`
under that game's UEVR Lua data directory.

Unchecking **Enable Cheeky for this game** detaches processing on the next host
frame. The hooked runtime remains resident until the application exits; restart
the application to remove it from memory. Re-enabling reconnects without a
restart and preserves processing settings. Replace the Lua script and DLLs
together when updating.

The OpenXR installation is shared across games. Install all plugin files and the layer from the same release when updating.

## First-use check

For AFW, follow the dedicated instructions below. Per-eye mode uses the usual eye calibration setup.

Select **Fixed**, enable **Automatic stereo alignment**, and use **Auto** as the mapping method. Use the red alignment border to check both eyes, then open **Stereo and gaze → Stereo Eye Mapping**. Check that calibration becomes **Active** and both regions stay aligned through loading/menu transitions.

In **Stereo Eye Mapping**, **Mapping method** offers Auto, Standard corners,
Timing tolerant corners, Full crop search and Manual. Manual disables automatic
marker calibration and enables **First DLSS view is right eye** (off means left
first). It assumes full-eye images with matching XR coordinates; use the red
border to verify both eyes and edges. Returning to an automatic method restarts
calibration; there is no separate calibration enable switch. Existing verified mapping routes still take precedence over the
manual fallback.

**Height offset** moves fixed, runtime-gaze and simulated-gaze placement, including
AFW and tracking-loss fallback. Negative moves up; positive moves down. Zero
preserves the detected center.

Manual stereo offsets are a troubleshooting fallback, not a replacement for calibration: scene changes can swap eye assignments. If calibration does not become active, check the layer installation and collect a report.

Sliders apply once on release; checkboxes and dropdowns apply immediately. Settings save automatically. Start with **DLSS-NR off**.

Press **Alt+Shift+>** (the period key with Alt and Shift held) to toggle the
entire DLSS-NR feature. Its foveation setting is preserved, and the change saves
automatically. Holding the shortcut toggles only once.

NR **Style** offers Standard, Natural and Cinematic. **Intensity** runs from 0
(no model edit) to 1 (full edit). **Skin structure** appears with Automatic mask
enabled; **Paper white** appears for HDR NR input.

## Reports and details

**Report an issue...** creates a support ZIP and opens a GitHub issue. Add your description and attach the ZIP; nothing is uploaded automatically. ZIPs are saved in `support/` under the game's UEVR configuration directory.

[Settings reference](../USAGE.md) · [Eye calibration](../EYE-CALIBRATION.md)

## Compatibility

Use Native Stereo first. Other stereo modes need game-specific testing. D3D11 uses direct processing by default. To use DLSS-NR on D3D11, enable **DX11 -> DX12 transport** under **DLSS-NR (experimental)**, then enable NR. Transport also supports SR without NR. This path still needs in-game UEVR/headset validation.

For DLSS-NR (native D3D12 or D3D11 transport), place a compatible `nvngx_dlssnr.dll` beside the nested runtime DLL or the actual game executable. NVIDIA binaries are not included. Use **Reset NR history / retry** after adding it.

Updating either DLL requires a full game restart. Reloading the adapter does not unload the resident runtime.

## AFW

AFW integration works with the public, unmodified AFW UEVR release on **DX12**. It does not require a separate Cheeky build of UEVR. AFW UEVR itself supports DX12 only; selecting it in a DX11 game falls back to AFR.

Use the same installation steps above and replace both Cheeky DLLs and the Lua script together when updating. Fixed AFW coverage uses UEVR's public stereo projections and needs no Cheeky OpenXR layer. Runtime OpenXR gaze requires the matching layer; native OpenVR uses the built-in adapter.

### Setup and controls

1. Select AFW in UEVR and enable DLSS in the game.
2. Open **Stereo and gaze**, leave **Foveation center** on Fixed, and select **Automatic** under **AFW stereo coverage**.
3. Adjust width and height under **DLSS-SR**. The minimum is **0.2 (20%)** for SR and independent NR regions. Older saved values below 0.2 are clamped when loaded.
4. Use the red alignment border to inspect both eyes. Disable the border when finished.

The coverage selector offers:

- **Automatic:** aligns the requested regions using both UEVR eye projections. Height offset adjusts their vertical position. Missing or mismatched projections use centered coverage.
- **Manual:** uses mirrored horizontal offsets and a shared height offset.
- **Per-eye (calibrated):** preserves the requested width, height and offsets and uses the calibrated eye mapping. This is the default when both AFW coverage flags are off.

**Advanced AFW → Extra margin per edge** adds a fixed allowance around the requested regions. It is a fraction of the full image, so 0.05 adds 5% on each edge. Stereo coverage and this margin can enlarge the visible region beyond the requested width and height. Automatic depth adjustment is removed. Existing coverage preferences and manual margin values are preserved.

The same controls apply to foveated NR with SR disabled. NR supports Before/After upscaling, full-frame or foveated processing, working scale, and independent or SR-linked size and shape. Supply a compatible `nvngx_dlssnr.dll` as described above.

### Gaze and transitions

Runtime gaze and simulated gaze use a fixed crop budget derived from the fovea dimensions, warp margin and stereo projection. Fresh gaze takes priority when smoothing trails outside that budget; widely separated eye samples or masks can be clipped to the budget. In gaze modes, **AFW tracking-loss fallback** selects the fixed coverage used when tracking is unavailable. Each eye must provide a valid sample and matching projection. Brief tracking loss holds the last position before returning to fixed coverage; loss of focus stops live gaze immediately.

Gaze movement, smoothing and tracking loss change placement without resizing the allocation. Tracking loss moves the fixed gaze allocation toward the configured fallback position. Changing fovea dimensions or stereo projection can resize it. Small gaze movements retain temporal history. Jump simulations support **Show next jump target** without changing current coverage.

Marker calibration remains available with AFW. Switching UEVR away from AFW restores ordinary stereo controls; explicitly selected AFW coverage returns when AFW is selected again. The resolution-protecting hooks remain installed while the AFW runtime is loaded.

### Diagnostics and compatibility

The overview reports whether UEVR has selected AFW. This does not mean AFW is actively warping during loading screens or other temporary suspensions. **Performance** contains frame-rate comparisons, DLSS GPU timings and processing resolutions. Warp activity, source-eye identification, projection availability and routing details are collected in support ZIPs under **Support**.

Cheeky keeps the game's original full-size evaluation at AFW's outer hook. Private center/periphery SR and NR run inside the nested DLSS call, keeping those dimensions out of AFW's resolution-change detector. Native exports, `_C` exports, Streamline's nested route and NVIDIA OTA SR runtimes are supported. Genuine game-resolution changes still invoke AFW's own suspension behavior.

Source-eye identification uses the verified AFW beta 6 warp metadata and full depth-buffer copies already made by AFW. It does not sample depth or guess eyes from frame order. Coverage maps both eyes' requested regions into the current source projection. Matching pixel dimensions preserve temporal history across alternate-eye crop origins; size changes, game resets and skipped/failed private passes invalidate the affected history.

Missing or ambiguous nested DLSS routes pass through to ordinary DLSS. Unknown warp binaries retain routing and conservative coverage but cannot provide verified source-eye metadata. Other NGX proxies can affect compatibility.

Hogwarts Legacy compatibility and overlapping fixed coverage have been tested in a headset. Automated tests cover native/Streamline/OTA routing, SR/NR histories, real copy hooks and GPU composites, gaze/fallback transitions and the Lua menu. Those tests substitute NVIDIA inference; additional games and real eye tracking still need headset testing.

For final testing, compare SR off/on with peripheral DLSS, check both eyes during head and object motion, then exercise NR Before/After and gaze modes. Include loading screens, game-resolution changes and switching AFW off/on. If something fails, use **Report an issue...** to collect a support ZIP. Nothing is uploaded automatically. Restore a previous complete Cheeky package and restart the game to roll back.

## FG GPU queue comparison

The Lua **FG GPU queue** dropdown selects **Separate HIGH queue**,
**Separate NORMAL queue**, or **Shared game queue** (the default). The saved key
remains `FgQueuePriority`: 1, 0, and 2 respectively. Switching applies live after
the current FG job retires and recreates NGX histories. FG processing, calibrated
latency, source-age, pacing-wait, and FPS windows restart for the new queue.
Capture/upload overhead measurements are not reset. Support logs identify
both the requested and actual queue, including unavailable-priority fallback.

For an A/B test, keep the scene, release policy, quality settings, and repeats
unchanged; use repeats OFF. Allow 10 seconds to settle after each switch and
record about 20 seconds per mode. Compare native and unique FPS as well as GPU
queue entry, capture dependency, and FG GPU work. Shared queue markers reach the
GPU after preceding work on the game queue, so dependency time may move into
queue-entry time; compare their sum rather than expecting both fields to fall.
These are scheduling experiments, not an established fix for gameplay slowdown.

## Prepared FG guide capture

**FG guide capture** selects **Prepared FG layouts** (default) or **Raw snapshots
(baseline)**. Prepared capture normalizes depth and motion immediately after the
native DLSS evaluation, retaining only the center and required peripheral guide
grids. Inference borrows those exact allocations under the stereo frame lease;
it does not allocate or normalize another guide set. Center guides keep 32-bit
precision; peripheral guides retain the selected precision and algorithm.

Install the matching plugin/runtime/Lua files and OpenXR installer, then restart
the game. This changes the guide-pair ABI. For clean memory comparisons, restart
after changing modes and keep the same scene and quality settings. Captured guide
rows include producer-prepared allocations; worker-owned depth/motion should
show zero in prepared mode. An unsupported source layout falls back to raw
capture. Logs report prepared/raw/fallback eye counts, allocated and leased eye
slots, actual bytes and equivalent raw snapshot bytes. Cold, unleased snapshots
are reclaimed after their producer fence completes; no game-thread wait is added.

Savings depend on the number of live eye slots and guide dimensions. For the
reported 198.8 MiB raw snapshots plus 53.2 MiB worker guide preparation, four
prepared eye slots would require about 106.4 MiB, a provisional 145.6 MiB saving.
Larger in-flight pools reduce that saving; allocation counters establish the
actual result. GPU pixel comparisons cover all periphery methods, 16/32-bit
guides, source crops, jitter, invalid vectors and reused engine textures. Real
NGX generation and sustained queue/release-policy checks also pass.

## GPU memory diagnostics

Lua's FG diagnostics and Performance section report the render adapter/node's
DXGI process memory usage, assigned budget, local headroom, sampled peak, and
over-budget sample count. Local memory is VRAM on a discrete GPU. Nonlocal
usage is shown separately; it does not by itself prove paging. The adapter's
dedicated capacity is hardware capacity, not free memory or a process budget.
Failed/unsupported queries show unavailable, including unsupported renderers.

Sampling is limited to once per second, with no GPU fence wait. The runtime log
records samples with FG state, selected queue request, and FPS; support ZIPs
also include the latest values and peak in diagnostics.json. Peaks are sampled,
not an exhaustive allocation high-water mark. Capture is available with FG OFF.

In the same scene, disable FG, allow memory to settle, and press **Mark VRAM
baseline**. Enable FG and compare the signed local/shared changes and budget
headroom. The baseline button takes a fresh sample. Counts cover the entire
game process, including Cheeky and in-process runtime allocations; they exclude
other processes. Changes can include game streaming and driver allocations, so
the comparison is not an exact count of memory allocated by FG. Baselines and
peaks reset when the observed graphics device changes. Saved queue choices
remain unchanged; new/reset profiles default to the shared game queue.

Expand **FG resource memory breakdown** for capture/output pools, captured depth
and motion guides, prepared color/depth/motion inputs, application-owned NGX
output textures/flags, periphery/history, composition scratch, and native,
generated and auxiliary XR images. Counts deduplicate resource identities;
borrowed engine textures are excluded. Reusable bytes are already part of the
pool/guide totals. They indicate allocations the current pool can reuse, not
necessarily every resource whose GPU work has finished.

Sizes use the driver's resource allocation requirements, including alignment.
XR images are estimates because the runtime owns their backing heaps. These
counts are not residency, and omit NVIDIA internal allocations, command and
descriptor storage, readback/system-memory buffers, and driver overhead. Do not
subtract the sum from DXGI usage or the FG ON/OFF difference to infer NVIDIA's
share. Categories are sampled on their owning threads, approximately once per
second while frames advance; capture/worker/presenter/guide sample ticks are in
the JSON and logs. Older OpenXR layers show a partial guide-only breakdown;
install the matching OpenXR update for all presenter categories. The resource
export is additive and preserves existing status/performance ABIs.

Capture/output transport pools now release pool-only textures unused for at
least three seconds, checking at most once per second as frames advance.
Recent working textures remain cached. Pending GPU contexts, history, queued
pairs and uploads retain their image references, so reclamation adds no GPU
fence wait and does not free resources still held by those clients. A reusable
snapshot is not a promise that every such byte will be reclaimed: some textures
are reused in the next frames. Compare settled resource counts and DXGI usage.
Completed native lease slots also drop color/guide/UI references once the slot
is their only owner; this prevents unused slots from pinning older allocations.

Custom periphery modes now write directly into the generated compact atlas,
skipping the fully center-covered interior while retaining the feather and
bilinear-filter border. Native input contexts stay complete for motion sampling;
the NVIDIA periphery mode still evaluates its full reduced-resolution image.
Matching center/context regions use texture copies instead of an atlas-wide
resampling dispatch. Final feather blending happens during XR upload and avoids
sampling the unused context interior. The **Periphery + output assembly** timer
sums both eyes' generation, packing and optional depth-history work; compact
feather blending belongs to **OpenXR uploads / presentation**.

**FG output destination** offers an opt-in **Direct OpenXR image** experiment.
It acquires the existing shared scene images only after the preceding native
frame was displayed, then lets NGX write its center subrectangle and custom
periphery fill/blend the final image in place. No extra generated swapchain or
generated atlas/upload is needed for successful direct jobs. A rejected NGX
destination or scaled center uses center scratch with direct final assembly;
unsupported periphery modes/runtime images use the existing compact path.
The default remains **Compact intermediate**. Settings persist and apply live.

Direct output waits for generation on the presentation thread. **Direct periphery
shading** offers three live A/B modes: full-resolution warping, coarse blocks
(default), and a coarse grid with bilinear filtering. Coarse modes evaluate the
expensive motion/depth work at **FG periphery scale**, then expand into the final
XR image without an intermediate texture. Blocks replicate one warped color;
bilinear filtering shares a tile of coarse samples and smooths between them.
Final pixel stores and center feather blending remain at output resolution.
At 100% periphery scale all modes use full-resolution warping. Lua/support JSON
report the last successful direct mode, shading grid, output extent and coarse/
full-rate frame counts. Compact fallback keeps its existing low-resolution path.
SDR uses writable linear UNORM XR images with gamma conversion in final assembly;
check dark gradients as linear 8-bit storage has less shadow precision than sRGB.
Lua and support diagnostics show direct/fallback frames, direct/scratch NGX eyes,
unsupported/native-not-ready cases, and the presentation wait. Existing stage
timers still include both eyes; direct final blending is in output assembly.
The NGX direct-write paths now set UpdateOnlyInsideExtents, preserving pixels
outside the requested center/context rectangle when destinations are shared.

Direct output also accepts compatible typeless XR backing images, using a typed
UAV for final assembly. Rejection details report the exact stage, requested and
actual formats/extents, resource flags and API error in Lua, support JSON and
the layer log. Unsupported backing images are cached by size/format so fallback
does not add an acquisition handshake every native frame; toggle the output
mode off/on to retry. A fallback wake never enables repeated native submissions
when the repeat setting is off.
