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

For the AFW experiment, follow the dedicated instructions below; its fixed region bypasses the usual alignment and calibration setup.

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

This experiment targets **DX12 with the public, unmodified AFW UEVR build**. It does not add AFW to mainline UEVR or require a custom UEVR executable. The original centered mode kept AFW enabled in a Hogwarts Legacy user test. The coverage and supersampling additions still need in-game testing; automated tests simulate the hook chain, not NVIDIA inference or the warp shader.

The `afw-ota-2` build also discovers SR runtimes loaded from NVIDIA's `NGX/models/dlss/versions/` cache with generated `.bin` filenames. It requires the expected NGX exports and excludes the separate NR, ray reconstruction, frame generation and Streamline model directories. It selects only an unambiguous runtime, retains that module until game exit, and logs its full path. No NVIDIA files are copied, renamed or replaced.

1. Close the game and install the complete `afw-coverage-1` Cheeky UEVR ZIP using the folder layout above, replacing both DLLs and the Lua script. Keep your existing AFW UEVR build. Start with the game's native DLSS Super Resolution path, without an upscaler replacement/proxy.
2. Start the game, inject UEVR and enable AFW with its DLSS ghosting fix. First check moving objects with Cheeky SR disabled, then enable Cheeky SR and Peripheral DLAA. Wait several seconds after each transition for AFW's own suspension window to clear.
3. In Cheeky's panel, look for **AFW compatibility detected**, **Lower SR runtime selected: Yes**, increasing **Full-frame core / nested DLSS calls**, SR **Active**, and zero **Rejected core reentry**. **Core calls without nested DLSS** may accumulate during startup while a late-loaded runtime stabilizes, but should stop increasing once SR is active. **Completed warp calls** should advance while AFW is warping, and **Last warp call** shows their age. A loaded module alone does not establish warp activity; a returned CPU call does not prove GPU completion or visual correctness. If the optional observer is unavailable, check AFW's own status.

**Manual stereo coverage off** retains the centered region of at least **70% width and 70% height**. Larger width/height settings apply. Set **Center supersampling to 1x** to reproduce the original test; the stored 1–2x setting now takes effect and can increase GPU cost considerably.

**Manual stereo coverage on** uses the requested fovea width and height and spans both possible mirrored stereo X offsets in one rectangle. Its placement does not depend on which eye was rendered. The height offset applies to both candidates. **Warp padding per edge** adds a fraction of the full image around this rectangle (default 0.05, or 5% per edge), clipped at image boundaries. For example, width 50%, X offset 0.6 and padding 0.05 yield 90% effective width. The banner reports effective coverage; stored width remains 50%. Roundness is bypassed to retain the rectangle's corners; transition width still feathers its edge. Padding is a coverage heuristic, not a depth-based guarantee against disocclusion. Start generously and inspect both eyes before reducing it.

Gaze, automatic stereo alignment, marker stamping and Cheeky NR remain bypassed. Their saved preferences are retained and unavailable controls are hidden. Detection remains latched until game exit, including while AFW is disabled or suspended. The red border can help locate the region, but matching border positions between eyes is not an acceptance criterion. No eye-calibration layer is needed for these eye-independent modes. DLSS handles are labeled **Unknown (AFW source eye)**: the public warp callback occurs after DLSS, so its timing cannot safely identify the current source eye in advance.

The core AFW hook receives the original full-size inputs once per game call. Only a public DLSS call reached inside that core evaluation may run Cheeky's smaller private passes. If that nested path is unavailable, Cheeky leaves ordinary DLSS running and increments the missing-route counter. A private evaluation failure falls back to the game feature, resetting its stale history when needed.

Check head movement, moving objects, region edges, loading screens, AFW off/on, and Cheeky SR off/on. Then change manual coverage and center supersampling separately, watching AFW's status and the warp counter. Compare GPU time and visual quality with Cheeky disabled. A real resolution change may still trigger AFW's normal temporary suspension. Eye identity, temporal history when AFW changes the rendered eye, region coverage after warping, motion correction and interaction with other NGX hooks remain in-game validation points. Restoring centered coverage and 1x center supersampling provides the original baseline.

If the nested counter stays at zero, AFW repeatedly suspends, or warping looks wrong, disable Cheeky SR and collect a support ZIP with the game name, graphics API and exact AFW UEVR build. The ZIP contains the routing counters. Restore the previous complete Cheeky ZIP and restart the game to revert the experiment.
