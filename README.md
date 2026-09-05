# Cheeky Foveated DLSS

Cheeky Foveated DLSS is a 64-bit [ReShade](https://reshade.me/) add-on that reduces the cost of DLSS Super Resolution by applying it to the part of the image that matters most. It supports Direct3D 11 and Direct3D 12 games, including stereo rendering for VR.

**Enjoying smoother games or VR?** [Help fund an eye-tracked headset for development on Ko-fi](https://ko-fi.com/cheekykent) · [Why it helps](#support)

DLSS 5 Neural Rendering (DLSS-NR) is also supported experimentally. See [Experimental DLSS-NR support](#experimental-dlss-nr-support) before enabling it.

## Requirements

- Windows 10 or Windows 11, 64-bit
- An NVIDIA RTX GPU and current NVIDIA driver
- A Direct3D 11 or Direct3D 12 game with DLSS Super Resolution
- The 64-bit version of ReShade **with full add-on support**
- `CheekyFoveatedDLSS.addon64` from this project's release package
- For eye tracking: an OpenXR runtime exposing `XR_EXT_eye_gaze_interaction`

This is intended for games where ReShade add-ons and DLL replacement are allowed. Avoid using it with competitive or anti-cheat-protected games unless the game's rules explicitly permit modding.

## Installation

1. Install the 64-bit **ReShade with full add-on support** build into the game. Select the game's correct rendering API when prompted.
2. Copy `CheekyFoveatedDLSS.addon64` into the game directory containing the ReShade DLL and game executable.
3. Start the game and enable DLSS in the game's graphics settings.
4. Open the ReShade overlay and select **Cheeky Foveated DLSS** in the **Add-ons** tab.
5. Confirm that **Foveated DLSS-SR** is enabled (it is on by default). Changes apply live on the next DLSS evaluation.

### Optional eye tracking (Experimental)

Eye tracking is optional and remains off by default. If you only want fixed
foveation, the add-on installation above is all you need.

1. Download and run **CheekyEyeTrackingSetup.exe** from the release package.
   Close OpenXR games first and accept the Windows administrator prompt.
2. The installer places one shared copy of the OpenXR layer in
   `C:\Program Files\CheekyFoveatedDLSS\OpenXR` (on the Windows system drive)
   and registers it automatically. No game-folder selection is needed.
3. Start or restart the game and open **Add-ons > Cheeky Foveated DLSS** in
   the ReShade overlay.
4. Enable **Foveated DLSS-SR**, then set **Foveation center** to **OpenXR
   gaze**.
5. To verify eye tracking, enable the red alignment border and open
   **Diagnostics > OpenXR eye tracking**. The layer and gaze must report as
   available, and both eyes must acquire stable DLSS-view mappings before the
   border follows your gaze.

The installer only installs the shared OpenXR layer; it does not include or
modify `CheekyFoveatedDLSS.addon64`, install ReShade, or change game files.
For each additional game, install ReShade with full add-on support, copy the
add-on into its game folder, and select **OpenXR gaze**. The game and runtime
must support the OpenXR eye-tracking path described below.

To update, close OpenXR games and run the newer installer. To uninstall, remove
**Cheeky OpenXR Eye Tracking** through Windows **Settings > Apps**. This removes
the shared layer and its registration for all games; the add-on remains in
each game and falls back to fixed foveation. As an emergency per-launch bypass,
set `CHEEKY_OPENXR_LAYER_DISABLE=1` before starting the game.

If the game ships with an older DLSS model, use [DLSS Swapper](https://github.com/beeradmoore/dlss-swapper) to install a newer DLSS 4.5 model. Use only the official DLSS Swapper releases, and be aware that a game update may restore its original DLL.

## Recommended settings

For the largest performance gain, use the game's **DLSS Performance** mode with a DLSS 4.5 Gen 2 model (Preset M).

Performance mode renders at 50% resolution. Its Gen 2 upscaler is heavier than the Gen 1 model used by DLAA and DLSS Quality (Preset K), but it reconstructs a good image from the lower input resolution. Foveation then avoids paying the full cost of that heavier model across the entire frame by upscaling only the center region. In tested cases this can save roughly **2-3 ms per frame** on the DLSS pass, in addition to the lower render time from Performance mode.

That extra headroom can be spent on a higher game or VR resolution scale. The result can be better clarity than DLAA or DLSS Quality while still running at a higher frame rate.

Actual gains depend on the game, GPU, output resolution, and fovea size. The add-on's performance panel shows the measured full-frame and foveated DLSS timings for the current game.

## Using the add-on

Start with the defaults, then tune the region while looking at a representative scene:

1. Turn on **Show 5 px red alignment border** so the processed region is visible.
2. Adjust **Fovea width** and **Fovea height**. Smaller values improve performance but make the transition easier to notice.
3. Use **Height offset** to move the region vertically and **Transition width** to soften its boundary.
4. In VR, adjust **Stereo X offset** until the region is centered correctly in both eyes. Enable **Invert stereo eye order** if the offsets move in the wrong directions.
5. Turn the red alignment border off when calibration is complete.

The main controls and their defaults are:

| Control | Default | Purpose |
| --- | --- | --- |
| Enable foveated DLSS-SR | On | Enables the main foveated Super Resolution path. |
| Center preset | Game/default | Preserves the game's DLSS preset or overrides it with E, K, L, or M. |
| Peripheral DLAA | On | Enables the auxiliary DLAA pass for the area outside the fovea. |
| Peripheral preset | E (Fastest) | Selects E, K, L, or M for the peripheral DLAA pass. |
| Periphery scale | `0.75` | Downscales the periphery further from the original render resolution. |
| Fovea width / height | `0.55` / `0.45` | Sets the normalized size of the DLSS-processed region. |
| Stereo X offset | `0.60` | Moves the two eye regions in equal and opposite horizontal directions. It appears after two views are detected. |
| Invert stereo eye order | Off | Swaps the stereo offset directions for games that report the right eye first. |
| Height offset | `-0.45` | Moves the region from the top (`-1`) through center (`0`) to bottom (`+1`). |
| Roundness | `0.00` | Blends the region shape from rectangular (`0`) to elliptical (`1`). This does not affect performance. |
| Transition width | `0.040` | Feathers the edge of the region. |
| Show 5 px red alignment border | Off | Displays the processed region while calibrating the fovea. |
| DX11 game processing path | DX11 Direct | **DX12 Transport** enables DX12-only features for DX11 games. |
| Foveation center | Fixed | Keeps the compatible fixed placement or opts into eye-tracked OpenXR placement. |
| Gaze smoothing | `20 ms` | Sets the time constant for gaze motion. |
| Crop origin quantization | `8 px` | Snaps motion to render-pixel increments. |
| Jump reset threshold | `0.125 crop` | Resets DLSS history above the larger of 64 px or 12.5% of the crop dimension. |

Press **Alt+Shift+/** to toggle foveated DLSS-SR without opening the overlay. Settings are saved through ReShade and restored the next time the game starts.

The **Diagnostics** and **Performance** panels show whether DLSS interception is active, the received resolutions and crop, call counts, GPU timing, and the last NGX result. If the panel remains on “Waiting for the first DLSS evaluation,” confirm that DLSS is enabled in the game and that ReShade was installed for the correct API.

## Experimental DLSS-NR support

DLSS-NR support is experimental. It is not expected to work correctly in every game, and some of the exposed tuning sliders may have little or no effect depending on the title and the data it supplies.

The required NVIDIA and Streamline runtimes are **not distributed with this project**. You must supply compatible Streamline DLLs yourself, together with a signed `nvngx_dlssnr.dll`. Place `nvngx_dlssnr.dll` beside `CheekyFoveatedDLSS.addon64`; keep the Streamline components in the locations expected by the target game. Runtime versions must be mutually compatible.

Additional notes:

- DLSS-NR is off by default.
- Direct3D 11 games require the **DX12 Transport** processing path for DLSS-NR.
- DLSS-NR has independent foveation controls and a green alignment border, or it can reuse the DLSS-SR foveation values.
- A DLSS-NR failure leaves the composited DLSS-SR result intact.
- RDR 2 currently does not work well with DLSS-NR.

## Tested games

These games have been tested; other DLSS titles may also work. Support depends on hooking of the addon into the games DLSS calls. These are just the DLSS games I own. Hopefully community can add more as time goes on.

### Flat screen

- Forza Horizon 6
- RDR 2 - DLSS-SR works, but DLSS-NR does not currently work well (Make sure to change reshade binding to insert instead of home and disable notifacations in Social Club or else it can cause crashes)

### VR

- Assetto Corsa Competizione
- Hogwarts Legacy with [UEVR](https://uevr.io/) (And also flat)

### Eye tracking

I do not own an eye tracked headset, however due to the open source nature of the
project @Williem3 was able to add in the initial implementaiton. I cannot fully validate 
the eye tracking experience but rely on community reports if there are issues.

The implementation intentionally falls back to the configured fixed center if
the layer is absent, gaze becomes stale, the ABI does not match, the game uses a
non-stereo view configuration, or resource correlation is ambiguous. Separate
eye textures and side-by-side subrectangles are supported. Quad views, non-zero
texture-array slices, OpenVR-only games, and transitive intermediate-resource
tracking are deferred.

For hardware validation, disable the game's built-in eye-tracked foveation,
enable the red alignment border, and inspect **Diagnostics > OpenXR eye
tracking**. Both eyes must show stable and different DLSS-view mappings before
the border follows gaze. A blink holds the last sample for 100 ms and then
returns to the fixed center over 150 ms.

## Support

### Help me test eye tracking on real hardware

If Cheeky Foveated DLSS has given you smoother VR, extra FPS, or room to turn up the resolution, please consider supporting its development.

[![Donate on Ko-fi](assets/donate-ko-fi.svg)](https://ko-fi.com/cheekykent)

Any amount helps toward the goal of funding an eye tracked headset. Cheeky Foveated DLSS is free and open source, and donating is entirely optional.

## License

Cheeky Foveated DLSS is free software licensed under the [GNU General Public License version 3](LICENSE) (`GPL-3.0-only`). You may use, modify, and redistribute it under the terms of that license. Vendored third-party components remain covered by their respective upstream licenses in `third_party`.

## Development

Development requires Visual Studio 2022 or newer with the **Desktop development
with C++** workload and a Windows 10/11 SDK. Minimal pinned snapshots of the
ReShade API, Dear ImGui, MinHook, and OpenXR headers are vendored under
[`third_party`](third_party/README.md), so normal builds do not download
dependencies.

Build the x64 Release add-on from PowerShell:

```powershell
.\scripts\build.ps1
```

The script builds the ReShade add-on, `CheekyOpenXRLayer.dll`, implicit-layer
manifest, and dependency-free tests, then runs the tests. Artifacts are written
to `bin\Release`; use `-Configuration Debug` for a debug build. You can also open
`CheekyFoveatedDLSS.sln` in Visual Studio or use CMake 3.24 or newer. Core
implementation lives in `src`; `src/gaze_foveation.hpp` is the add-on-side
OpenXR seam and `src/foveation.hpp` contains renderer-independent crop geometry.

### Simulated gaze (no eye tracker required)

For Quest 3 PC VR testing, install the add-on and the matching OpenXR
eye-tracking installer as described above. In the ReShade add-on panel, select **Foveation
center > Simulated gaze** and enable **Show 5 px red alignment border**.
The simulated direction follows a repeating pattern relative
to your head. Both eyes use the real OpenXR views and the existing projection,
resource mapping, smoothing, crop quantization, and DLSS history reset logic.
No eye tracking extension or eye tracking hardware is required for this mode.
The game still needs a supported OpenXR stereo and DLSS rendering path.

Under **Diagnostics > OpenXR eye tracking**, check **Simulated gaze**, gaze
validity, the per-eye mappings, and **Using gaze**. Hardware support and eye
gaze extension indicators may correctly remain off. If the layer is missing or
mapping fails, the region stays at its fixed fallback. Use the matching newly
built layer DLL; older layer builds do not implement simulation.
Choose **Fixed** to stop or **OpenXR gaze** to return to actual tracking.
The selected mode is saved with the other settings. This tests synthetic motion,
not real eye tracker acquisition or latency.

### Building the eye-tracking installer

With [Inno Setup 6.3 or newer](https://jrsoftware.org/isdl.php) installed in a
standard location or available on PATH, run from the repository root:

```powershell
.\scripts\build-installer.ps1 -Version 0.2.0
```

If you already have the portable compiler in `build\installer-tools\inno`,
use this command instead; no additional Inno Setup installation is needed:

```powershell
.\scripts\build-installer.ps1 -Version 0.2.0 -IsccPath .\build\installer-tools\inno\ISCC.exe
```

The script does not automatically search that portable directory. The compiler
is a local build tool and is not included in the repository; on a fresh checkout,
install Inno Setup or pass `-IsccPath` pointing to your own compiler. Set
`-Version` to the release version you are packaging.

This builds and tests Release artifacts and produces
`bin\installer\CheekyEyeTrackingSetup.exe`. Pass `-IsccPath` if `ISCC.exe` is
not installed in a standard location. To package an existing Release build,
use `-SkipBuild`; for CMake output also pass `-ArtifactsDirectory` with the
directory containing the layer DLL and manifest. The Release layer links the
Visual C++ runtime statically, so the installer needs no redistributable download.

Publish the installer and `CheekyFoveatedDLSS.addon64` as separate release
downloads. The installer payload is explicitly limited to the OpenXR layer,
manifest, and licenses. Keep its AppId stable across releases so updates reuse
the installation and uninstall entry. Before releasing, test install, upgrade,
and uninstall on a Windows test machine; verify that the manifest's DWORD is
`0` in the 64-bit HKLM OpenXR implicit-layer key, and that uninstall removes
only that value and the installed files. Verify gaze in a supported game after
restarting it. Release signing, when available, should be applied to the DLL
before packaging and to the final installer EXE before publishing.
