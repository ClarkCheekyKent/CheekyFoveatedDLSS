# Cheeky Foveated DLSS for UEVR

Runs Cheeky's DLSS processing with controls in UEVR. Remove ReShade from the game before using the plugin.

## Installation

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

4. **For OpenXR gaze, or ordinary stereo alignment/calibration, run the matching `OpenXR/CheekyOpenXRSetup.exe` included in the release-candidate ZIP before starting the game.** AFW fixed/automatic coverage needs no OpenXR layer. Native OpenVR uses the built-in adapter, including when its compositor initialized before the plugin.
5. Start the game with DLSS enabled, inject UEVR and open **LuaLoader → Cheeky Foveated DLSS**.

The OpenXR installation is shared across games. Install all plugin files and the layer from the same release when updating.

## First-use check

For AFW, follow the dedicated instructions below; its coverage modes bypass the usual eye calibration setup.

Keep **Fixed**, **Automatic stereo alignment** and **Automatic eye calibration** enabled. Use the red alignment border to check both eyes, then open **Diagnostics and support → Eye calibration**. Check for **Active** and valid samples through loading/menu transitions.

Manual stereo offsets are a troubleshooting fallback, not a replacement for calibration: scene changes can swap eye assignments. If calibration does not become active, check the layer installation and collect a report.

Sliders apply once on release; checkboxes and dropdowns apply immediately. Settings save automatically. Start with **DLSS-NR off**.

Press **Alt+Shift+>** (the period key with Alt and Shift held) to toggle the
entire DLSS-NR feature. Its foveation setting is preserved, and the change saves
automatically. Holding the shortcut toggles only once.

NR **Style** offers Standard, Natural and Cinematic. **Intensity** runs from 0
(no model edit) to 1 (full edit). **Skin structure** appears with Automatic mask
enabled; **Paper white** appears for HDR NR input. The old preset hints and
unverified UI correction checkbox have been removed from the menu.

## Reports and details

**Report an issue...** creates a support ZIP and opens a GitHub issue. Add your description and attach the ZIP; nothing is uploaded automatically. ZIPs are saved in `support/` under the game's UEVR configuration directory.

[Settings reference](../USAGE.md) · [Eye calibration](../EYE-CALIBRATION.md)

## Compatibility

Use Native Stereo first. Other stereo modes need game-specific testing. D3D11 uses the direct processing path; D3D11-to-D3D12 transport and D3D11 DLSS-NR are not available through the plugin.

For D3D12 DLSS-NR, place a compatible `nvngx_dlssnr.dll` beside the nested runtime DLL or the actual game executable. NVIDIA binaries are not included. Use **Reset NR history / retry** after adding it.

Updating either DLL requires a full game restart. Reloading the adapter does not unload the resident runtime.

## AFW release candidate

This integration runs with the **public, unmodified AFW UEVR release on DX12**. No separate Cheeky build of UEVR is required. It does not add AFW to mainline UEVR. Compatibility was confirmed in the user's Hogwarts Legacy test; this candidate adds the remaining controls and recovery behavior for a final headset test.

Install the complete `afw-rc-4` ZIP with the game closed. Replace both Cheeky DLLs and the Lua script. The optional matching OpenXR installer is in `OpenXR/`; run it for OpenXR gaze. NVIDIA binaries are not included. For NR, provide a compatible `nvngx_dlssnr.dll` beside the nested Cheeky runtime or the running game executable.

RC4 lowers the requested SR and NR fovea width/height minimum to 0.1 (10%). These are sizes before stereo coverage and warp padding; the visible region can be larger. For example, 0.1 plus 0.0625 padding on each edge becomes 0.225 (22.5%) before any extra stereo union. Existing saved sizes and the default sizes are retained.

RC3 retains RC2's source-eye projection alignment, confirmed overlapping in the headset, and fixes a repeated center-history reset. A legacy preparation check compared projected settings bit-for-bit: the reported FOV produces slightly different floating-point widths for each eye even when both crops have identical pixel dimensions. That check reset SR on every alternation. History invalidation now belongs to the per-view backend's integer crop geometry, game reset requests and gaze policy. First use, actual resizing, bypassed/failed evaluations and disable/re-enable still invalidate history. The trace now records the actual NGX reset and cumulative reset/correction counts for each private history, including consecutive samples across alternating eyes.

### Coverage and eye identity

The outer AFW hook receives the game's original full-size inputs once. Cheeky's private SR and NR work runs only in a lower DLSS evaluation reached inside that call. This keeps center/periphery dimensions out of AFW's resolution-change detector. Native exports, `_C` exports, Streamline's nested route, and NVIDIA OTA `.bin` SR runtimes are covered. Genuine game-resolution changes still trigger AFW's own temporary suspension.

The **Stereo and gaze** section contains the shared AFW controls, including when SR is disabled and only NR is running:

- **Automatic stereo coverage** uses both optical centers from UEVR's public projections, the requested region sizes and height bias. It takes precedence over manual coverage. Fresh matching full-eye dimensions are required; a complete eye may occupy a subrectangle in a larger output allocation.
- **Manual stereo coverage** contains both mirrored X-offset regions, with a shared vertical offset.
- With both options off, the fixed fallback is at least 70% wide/high. With verified source identity it follows the optical center; otherwise it stays image-centered. Larger requested sizes apply.
- **Warp padding per edge** supplies a manual safety allowance, measured as a fraction of the full image. Automatic and manual modes add it around the requested regions. **Depth-adaptive warp padding** adds measured geometric displacement, including to the centered fallback, when valid depth feedback is available.
- **Roundness** applies independently to the covered eye regions. In gaze mode it includes both fresh and filtered positions. The private DLSS allocation remains their bounding rectangle; masking changes the composite, not the rectangular DLSS inference cost. Rectangle and rounded modes both use the actual projected region masks, so retained allocation headroom does not change the visible center/periphery boundary.

On the verified AFW beta 6 warp DLL, each warp callback explicitly identifies its source eye and depth buffer. A later core evaluation can identify its source eye **before its nested DLSS call** by copying its original depth into that exact buffer. This works without frame parity or permanently assigning an eye to an NGX handle. Partial copies, conflicting bindings, expired observations, resource destruction and session changes invalidate the match. The diagnostics distinguish **Source eye at last DLSS call** from the later warp callback's own eye/mode.

Knowing the source eye does not eliminate the other eye's warp donors. Coverage therefore accounts for both requested eye regions after converting their viewing directions into the actual source projection. It reserves a common allocation size for either eye while moving the crop origin with that eye's projection. Gaze-jump detection compares successive observations of the same eye; private DLSS still receives the game's temporal motion convention with crop-origin compensation. Both eyes use the larger valid depth estimate, avoiding alternating padding sizes. Marker-based two-eye calibration is bypassed while AFW is selected because AFW's synthesized views are not ordinary stereo submissions. Automatic coverage provides alignment through the public projection API instead.

### Depth-adaptive padding

This option is enabled by default. The verified warp callback supplies the current source/destination camera matrices and depth resource. Cheeky copies depth asynchronously, restores the resource's declared state, and leaves the command list's shader bindings intact. It reads the copy only after the recording has retired and every executing queue has completed; replayable, unsubmitted, failed and pending work cannot supply an estimate.

A grid of at most 64 by 64 depth samples estimates the maximum geometric reprojection displacement beyond the static optical projection map. A sample-cell guard and upward quantization accompany the estimate. Padding grows immediately and shrinks after one second of sustained lower demand. The user's manual padding remains additional. Capture is limited to ten copies per second per eye, with four bounded readback slots. There are no render-thread GPU waits.

Feedback expires after 500 ms and is invalidated by host/projection changes. The readback path supports single-sample, single-mip, single-layer R32/D32 float, R16/D16 UNORM, D24S8 and D32S8 depth families, including their typeless resource formats, up to 64 MiB per depth-plane copy with a declared shader-readable state. Stencil is left untouched; planar readbacks follow [Microsoft's D3D12 depth/stencil layout](https://microsoft.github.io/DirectX-Specs/d3d/PlanarDepthStencilDDISpec.html). Unsupported layouts, camera data, unknown warp DLLs or unavailable readbacks use manual padding. **Fresh depth estimate**, capture/completion/pending counters, depth format/state and an explicit capture status explain whether it is active and why captures were rejected.

This is recent sampled feedback, not a per-pixel prediction of a future warp. Thin foreground objects between samples, abrupt scene changes, object motion and disocclusion can need more manual padding. It can also expand coverage to the full image and reduce or remove the performance benefit. Turn the option off to reproduce the previous fixed-padding baseline.

### Gaze and previews

**Runtime gaze** and **Simulated gaze** work through the matching OpenXR layer or native OpenVR adapter. Real gaze needs a compatible tracker/runtime; simulation does not. Each eye's gaze ray is converted into that same eye's UEVR projection. Both fresh and smoothed positions remain covered so filter lag cannot leave current gaze outside the sharp regions.

Both eyes must supply valid samples. Samples older than 50 ms, stale display times, malformed FOVs and the wrong real/simulated source are rejected. Brief loss holds the previous position for 100 ms, then returns over 150 ms to fixed coverage. Focus loss drops live gaze immediately; missing projections select fixed coverage. Reacquisition, large crop jumps, session changes and allocation changes reset affected histories.

Gaze allocation grows immediately and **shrinks automatically** after one second of sustained smaller coverage, retaining alignment headroom. Small gaze motion does not recreate DLSS features every frame. Settings and projection changes start a new geometry epoch. The effective coverage readout includes retained allocation as well as requested regions and padding.

Jump simulation supports **Show next jump target**. Its green rectangle shows the upcoming target envelope using a separate preview size; displaying or hiding it never changes the current allocation. The preview is suppressed unless both future eye targets are valid. The red alignment border follows the current composite shape, including rounded eye-region unions.

### NR and live transitions

NR supports **Before upscaling** and **After upscaling**, full-frame and foveated processing, working scale, style/intensity, independent width/height/roundness, and linking to SR size/shape. Before NR processes a private original-size input copy and restores the game's parameters; After NR processes the completed SR image. NR also works with Cheeky's foveated SR disabled. Linked foveated NR uses the actual coordinated SR crop and mask; independent NR has its own gaze allocation. Its Before border uses the region actually processed rather than sampling gaze again after inference.

Each game NGX handle retains its own private SR/periphery/NR histories. A shared game handle stays shared: splitting it solely by eye would also require changing the game's temporal motion convention. Skipped private passes, NR failures, order changes and transitions between processed and original Before inputs reset the affected histories. Native fallback resets stale game history once. Original Reset values and resource parameters are restored.

Changing UEVR's rendering method away from AFW restores ordinary Cheeky stereo controls and marker calibration during the same game session. Selecting AFW again restores AFW coverage. The resolution-protecting hooks stay installed so warmup and suspension remain safe. Missing, malformed or stale host mode information keeps conservative compatibility behavior. Updating either DLL still requires a game restart.

### Supported boundaries and final test

An unambiguous nested DLSS route is required. Missing, ambiguous or reversed hook topologies remain ordinary-DLSS passthrough. Other NGX proxies may change that topology. Unknown warp DLLs retain SR/NR routing, bilateral coverage, public projection/gaze support and opaque warp activity reporting; resource/camera interpretation requires the verified ABI. These are explicit compatibility boundaries, not inferred eye assignments or unchecked memory layouts.

The automated suite exercises real D3D12 copy hooks, rectangular and rounded GPU composites, float/UNORM/depth-stencil GPU readbacks, lifetime/replay protection, NR ordering and fallback, native/OTA/Streamline routes, projection/adapter resets, live mode publication, gaze allocation policy, and both supported Lua runtimes. The SR history regression uses the reported headset FOV through production preparation and private evaluation, checks actual reset parameters across eye changes and real invalidation events, and reads back the corrected motion resource passed to inference for input- and output-resolution vectors. It substitutes NVIDIA inference and does not establish final headset image quality or performance.

For the final headset pass:

1. Compare Cheeky off with SR on, starting with Fixed and center supersampling 1x. Check AFW's own state, increasing core/lower/warp counters, SR Active, and zero rejected core reentry. Disable depth-adaptive padding for the previous 70% centered baseline, then enable it and check the depth counters and effective size.
2. Test automatic and manual coverage, padding, roundness, supersampling and periphery off/on. Inspect head motion, moving foreground objects and both eyes' region edges. Change the real game resolution and pass through loading screens.
3. Test simulated sweep, jumps/green preview, tracking loss and allocation recovery; then test real gaze. Check fresh bilateral gaze and gaze-driven coverage.
4. Test NR Before and After, linked and independent foveation/roundness, full-frame, working scale, NR off/on and NR with SR disabled. Check NR Active and timing alongside continued warp activity.
5. Switch AFW off/on in UEVR and check ordinary stereo controls, calibration and AFW coverage recover. Adapter reload/reset must not reuse stale projection or depth data.

If a route fails or warping looks wrong, disable Cheeky SR/NR and collect a support ZIP with the exact AFW build and settings. Nothing is uploaded automatically. Restore a previous complete Cheeky ZIP and restart to roll back.
