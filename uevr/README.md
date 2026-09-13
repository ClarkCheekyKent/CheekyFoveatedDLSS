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

4. **If UEVR uses OpenXR, run the matching `CheekyOpenXRSetup.exe`. This is required for automatic stereo alignment and eye calibration, even without eye tracking.** Native OpenVR mode uses the built-in adapter instead. It also attaches to UEVR's cached compositor when OpenVR initialized before the plugin.
5. Start the game with DLSS enabled, inject UEVR and open **LuaLoader → Cheeky Foveated DLSS**.

The OpenXR installation is shared across games. Install all plugin files and the layer from the same release when updating.

## First-use check

For the AFW experiment, follow the dedicated instructions below; its coverage modes bypass the usual eye calibration setup.

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

## AFW routing experiment

This integration targets **DX12 with the public, unmodified AFW UEVR build**. It does not add AFW to mainline UEVR or require a custom UEVR executable. SR and NR support fixed and gaze-driven coverage, history recovery, controls and diagnostics. The original centered SR mode kept AFW enabled in a Hogwarts Legacy user test. The completed integration still needs in-game testing; automated tests simulate the hook chain, not NVIDIA inference or the warp shader.

The `afw-ota-2` build also discovers SR runtimes loaded from NVIDIA's `NGX/models/dlss/versions/` cache with generated `.bin` filenames. It requires the expected NGX exports and excludes the separate NR, ray reconstruction, frame generation and Streamline model directories. It selects only an unambiguous runtime, retains that module until game exit, and logs its full path. No NVIDIA files are copied, renamed or replaced.

1. Close the game and install the complete `afw-nr-1` Cheeky UEVR ZIP using the folder layout above, replacing both DLLs and the Lua script. Keep your existing AFW UEVR build. Start with the game's native DLSS Super Resolution path, without an upscaler replacement/proxy. Saved NR settings now take effect; turn NR off for the initial SR-only comparison.
2. Start the game, inject UEVR and enable AFW with its DLSS ghosting fix. First check moving objects with Cheeky SR disabled, then enable Cheeky SR and Peripheral DLAA. Wait several seconds after each transition for AFW's own suspension window to clear.
3. In Cheeky's panel, look for **AFW compatibility detected**, **Lower SR runtime selected: Yes**, increasing **Full-frame core / nested DLSS calls**, SR **Active**, and zero **Rejected core reentry**. **Core calls without nested DLSS** may accumulate during startup while a late-loaded runtime stabilizes, but should stop increasing once SR is active. **Completed warp calls** should advance while AFW is warping, and **Last warp call** shows their age. A loaded module alone does not establish warp activity; a returned CPU call does not prove GPU completion or visual correctness. If the optional observer is unavailable, check AFW's own status.

With **Automatic stereo coverage and Manual stereo coverage both off**, the region is centered and at least **70% width and 70% height**. Larger width/height settings apply. Set **Center supersampling to 1x** to reproduce the original test; the stored 1–2x setting now takes effect and can increase GPU cost considerably.

**Manual stereo coverage on** uses the requested fovea width and height and spans both possible mirrored stereo X offsets in one rectangle. Its placement does not depend on which eye was rendered. The height offset applies to both candidates. **Warp padding per edge** adds a fraction of the full image around this rectangle (default 0.05, or 5% per edge), clipped at image boundaries. For example, width 50%, X offset 0.6 and padding 0.05 yield 90% effective width. The banner reports effective coverage; stored width remains 50%. Roundness is bypassed to retain the rectangle's corners; transition width still feathers its edge. Padding is a coverage heuristic, not a depth-based guarantee against disocclusion. Start generously and inspect both eyes before reducing it.

**Automatic stereo coverage on** uses both optical centers from UEVR's public projection matrices, plus the requested width, height, height bias and warp padding. It covers both possible source-eye positions without assigning an eye to a DLSS handle. It takes precedence over saved manual coverage. This needs no OpenXR calibration layer and works through the same public API in OpenXR and OpenVR. Missing, malformed, inactive or older-than-250-ms projections select the centered fallback. The output must match UEVR's reported eye dimensions with no output subrectangle offset. Device resets and adapter unload/reload invalidate the cache. The banner shows coverage selected at the last DLSS call, which can differ from saved preferences or a currently fresh projection belonging to another output size.

The two official AFW beta 6 packages contain the same warp DLL. When its exact binary fingerprint matches, **Last reported source eye**, **Last reported warp mode**, and source-left/right call counts expose the public warp callback's explicit values. Unknown binaries retain call observation and SR routing without reading parameter layouts. These values describe a past warp call, not an early identity for the next DLSS evaluation; they do not establish a persistent DLSS-handle-to-eye mapping.

Select **Runtime gaze** or **Simulated gaze** under **Stereo and gaze** to drive AFW coverage. OpenXR requires the matching Cheeky OpenXR layer installed before starting the game; native OpenVR uses the built-in adapter and requires a runtime that supplies eye tracking for real gaze. Simulation needs no eye tracker. Each runtime eye's gaze is transformed into that eye's UEVR projection, then both requested regions are covered in one rectangle with warp padding. This works without identifying which eye the game rendered and without marker calibration. Fresh samples from both eyes and matching UEVR output dimensions are required. **Fresh bilateral gaze: Yes**, **Gaze driving foveation: Yes** and **bilateral gaze coverage** establish that the gaze route is active; the late warp-eye readout remains independent.

The gaze rectangle contains both fresh and smoothed positions so smoothing cannot leave current gaze outside it. Its pixel dimensions grow when necessary and remain allocated until settings, session or projection geometry changes, avoiding DLSS feature recreation from small gaze movements. A large jump or long tracking loss can therefore increase GPU cost. Reduce smoothing or select Fixed then gaze to restart the allocation; changing fovea size also restarts it. The effective size in the banner includes both eyes, padding and any retained allocation. Roundness and next-jump preview are bypassed in this mode; the red border shows the actual combined region.

Samples older than 50 ms, repeated stale display times, malformed eye data and the wrong real/simulated source are rejected. Short tracking loss holds the last position for 100 ms, then returns over 150 ms to the selected fixed coverage. Focus loss drops live gaze immediately. Missing/stale UEVR projections use fixed coverage immediately. Reacquisition, session changes, size changes and large crop jumps reset center history; small movements retain it with crop-aware motion handling. The fallback rectangle may retain a larger allocation until the next settings/session reset.

Native eye-specific automatic alignment and marker stamping remain bypassed. Their saved preferences are retained and unavailable controls are hidden. Detection remains latched until game exit, including while AFW is disabled or suspended. The red border can help locate the region, but matching border positions between eyes is not an acceptance criterion. Fixed coverage needs no eye-calibration layer. DLSS handles remain labeled **Unknown (AFW source eye)**.

**DLSS-NR with AFW:** place a compatible `nvngx_dlssnr.dll` beside the nested Cheeky runtime or the running game executable, then enable **DLSS-NR**. The ZIP does not include NVIDIA binaries. Both rendering orders run inside the nested DLSS route, after AFW prepares the full-frame motion data. **After upscaling** processes the completed SR image. **Before upscaling** processes a private copy of the original-size input and feeds it to SR; the game texture and AFW's outer parameters stay intact. NR also works with Cheeky foveated SR disabled, while the game's ordinary DLSS still runs. A missing/ambiguous nested route leaves both Cheeky SR and NR inactive.

NR supports full-frame processing, working scale, style/intensity and the existing advanced controls. **Foveated NR** uses the same bilateral coverage policy and warp padding. **Use SR size and shape** reuses the actual SR crop when available, including its gaze allocation; with SR disabled it resolves the requested coverage itself. Independent NR width/height have their own gaze allocation and do not resize SR features. Independent AFW NR uses a rectangle; its roundness control is hidden. NR reserves a small alignment margin to keep its pixel dimensions stable as gaze moves. **NR status and GPU timing** reports the actual NR region and working resolution; the AFW coverage banner describes SR. The Before NR border uses the region actually processed, even if inference outlasts the gaze sample's freshness window.

Each native game handle owns its own NR feature/history in addition to its center and peripheral SR histories. Host resets, NR failures, changes of processing order, skipped NR frames and changed geometry reset the affected histories. Entering/leaving successful Before NR also resets SR history once because its color input changes. NR failure falls back to ordinary color and does not disable AFW. Unknown source-eye identity is handled with bilateral coverage; NR is not assigned an eye by frame parity.

The core AFW hook receives the original full-size inputs once per game call. Only a public DLSS call reached inside that core evaluation may run Cheeky's smaller private passes. If that nested path is unavailable, Cheeky leaves ordinary DLSS running and increments the missing-route counter. A private evaluation failure falls back to the game feature, resetting its stale history when needed. Each native game handle owns separate center and peripheral histories. Shared game handles stay shared; separate handles stay isolated. Game reset signals reach both private passes and the original parameter values are restored. Skipped center/peripheral histories reset when resumed. The late warp eye is never used to guess a history assignment or to force a reset every frame.

For final gameplay validation, check head movement, moving objects, region edges, loading screens, AFW off/on, and Cheeky SR and Peripheral DLAA off/on. Test fixed coverage first, then simulated gaze (sweep, jumps and tracking loss), then real eye tracking. Change coverage and center supersampling separately, watching AFW's status, effective size and the warp counter. Compare GPU time and visual quality with Cheeky disabled. A real resolution change may still trigger AFW's normal temporary suspension. Temporal image quality when AFW changes the rendered eye, region coverage after warping, motion correction and interaction with other NGX hooks remain in-game validation points. Restoring Fixed, centered coverage and 1x center supersampling provides the original baseline.

Then test NR After and Before separately, full-frame and foveated, linked and independent sizes, working scale, gaze, NR off/on and NR with Cheeky SR disabled. Check NR **Active** and its evaluation counter alongside warp activity. Compare image quality and GPU time; neural detail, temporal ghosting and coverage after the warp require actual headset testing.

If the nested counter stays at zero, AFW repeatedly suspends, or warping looks wrong, disable Cheeky SR and NR and collect a support ZIP with the game name, graphics API and exact AFW UEVR build. The ZIP contains the routing counters. Restore the previous complete Cheeky ZIP and restart the game to revert the experiment.
