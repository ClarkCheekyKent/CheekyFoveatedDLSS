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

4. **For OpenXR gaze, or ordinary stereo alignment/calibration, run the matching `OpenXR/CheekyOpenXRSetup.exe` included in the UEVR ZIP before starting the game.** AFW fixed/automatic coverage needs no OpenXR layer. Native OpenVR uses the built-in adapter, including when its compositor initialized before the plugin.
5. Start the game with DLSS enabled, inject UEVR and open **LuaLoader → Cheeky Foveated DLSS**.

The OpenXR installation is shared across games. Install all plugin files and the layer from the same release when updating.

## First-use check

For AFW, follow the dedicated instructions below; its coverage modes bypass the usual eye calibration setup.

Keep **Fixed**, **Automatic stereo alignment** and **Automatic eye calibration** enabled. Use the red alignment border to check both eyes, then open **Stereo and gaze → Eye calibration**. Check that calibration becomes **Active** and both regions stay aligned through loading/menu transitions.

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

Use Native Stereo first. Other stereo modes need game-specific testing. D3D11 uses the direct processing path; D3D11-to-D3D12 transport and D3D11 DLSS-NR are not available through the plugin.

For D3D12 DLSS-NR, place a compatible `nvngx_dlssnr.dll` beside the nested runtime DLL or the actual game executable. NVIDIA binaries are not included. Use **Reset NR history / retry** after adding it.

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
- **Centered (70% minimum):** uses at least 70% width and height. Larger requested sizes apply.

**Advanced AFW → Extra margin per edge** adds a fixed allowance around the requested regions. It is a fraction of the full image, so 0.05 adds 5% on each edge. Stereo coverage and this margin can enlarge the visible region beyond the requested width and height. Automatic depth adjustment is removed. Existing coverage preferences and manual margin values are preserved.

The same controls apply to foveated NR with SR disabled. NR supports Before/After upscaling, full-frame or foveated processing, working scale, and independent or SR-linked size and shape. Supply a compatible `nvngx_dlssnr.dll` as described above.

### Gaze and transitions

Runtime gaze and simulated gaze cover both eyes' fresh and smoothed gaze positions. In gaze modes, **AFW tracking-loss fallback** selects the fixed coverage used when tracking is unavailable. Each eye must provide a valid sample and matching projection. Brief tracking loss holds the last position before returning to fixed coverage; loss of focus stops live gaze immediately.

The allocation grows when needed and shrinks after one second of sustained smaller coverage. Small gaze movements retain temporal history. Jump simulations support **Show next jump target** without changing current coverage.

Switching UEVR away from AFW restores ordinary stereo controls and marker calibration during the same session. AFW coverage returns when AFW is selected again. The resolution-protecting hooks remain installed while the AFW runtime is loaded.

### Diagnostics and compatibility

The overview reports whether UEVR has selected AFW. This does not mean AFW is actively warping during loading screens or other temporary suspensions. **Performance** contains frame-rate comparisons, DLSS GPU timings and processing resolutions. Warp activity, source-eye identification, projection availability and routing details are collected in support ZIPs under **Support**.

Cheeky keeps the game's original full-size evaluation at AFW's outer hook. Private center/periphery SR and NR run inside the nested DLSS call, keeping those dimensions out of AFW's resolution-change detector. Native exports, `_C` exports, Streamline's nested route and NVIDIA OTA SR runtimes are supported. Genuine game-resolution changes still invoke AFW's own suspension behavior.

Source-eye identification uses the verified AFW beta 6 warp metadata and full depth-buffer copies already made by AFW. It does not sample depth or guess eyes from frame order. Coverage maps both eyes' requested regions into the current source projection. Matching pixel dimensions preserve temporal history across alternate-eye crop origins; size changes, game resets and skipped/failed private passes invalidate the affected history.

Missing or ambiguous nested DLSS routes pass through to ordinary DLSS. Unknown warp binaries retain routing and conservative coverage but cannot provide verified source-eye metadata. Other NGX proxies can affect compatibility.

Hogwarts Legacy compatibility and overlapping fixed coverage have been tested in a headset. Automated tests cover native/Streamline/OTA routing, SR/NR histories, real copy hooks and GPU composites, gaze/fallback transitions and the Lua menu. Those tests substitute NVIDIA inference; additional games and real eye tracking still need headset testing.

For final testing, compare SR off/on with peripheral DLAA, check both eyes during head and object motion, then exercise NR Before/After and gaze modes. Include loading screens, game-resolution changes and switching AFW off/on. If something fails, use **Report an issue...** to collect a support ZIP. Nothing is uploaded automatically. Restore a previous complete Cheeky package and restart the game to roll back.
