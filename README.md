# Cheeky Foveated DLSS

Cheeky Foveated DLSS is a 64-bit [ReShade](https://reshade.me/) add-on that reduces the cost of DLSS Super Resolution by applying it to the part of the image that matters most. It supports Direct3D 11 and Direct3D 12 games, including stereo rendering for VR.

DLSS 5 Neural Rendering (DLSS-NR) is also supported experimentally. See [Experimental DLSS-NR support](#experimental-dlss-nr-support) before enabling it.

## Future Plans
I plan to try to expand this technique to as many integrations as possible (UEVR plugin, no reshade depedency, etc.) so that it works with as many games as possible and plays well with other mods.
Create an issue if you encounter problem with game compatibility, bugs, or have an idea for an improvement so we can continue to improve this project!

## Requirements

- Windows 10 or Windows 11, 64-bit
- An NVIDIA RTX GPU and current NVIDIA driver
- A Direct3D 11 or Direct3D 12 game with DLSS Super Resolution
- The 64-bit version of ReShade **with full add-on support**
- `CheekyFoveatedDLSS.addon64` from this project's release package
- For VR: `CheekyEyeTrackingSetup.exe` from the matching release to install the OpenXR layer for automatic stereo alignment and gaze; automatic alignment needs no eye tracker
- For real eye tracking: the OpenXR layer, an eye-tracked headset, and a runtime exposing usable `XR_EXT_eye_gaze_interaction` input

This is intended for games where ReShade add-ons and DLL replacement are allowed. Avoid using it with competitive or anti-cheat-protected games unless the game's rules explicitly permit modding.

## Installation

1. Install the 64-bit **ReShade with full add-on support** build into the game. Select the game's correct rendering API when prompted.
2. Copy `CheekyFoveatedDLSS.addon64` into the game directory containing the ReShade DLL and game executable.
3. For VR, close OpenXR games and run **CheekyEyeTrackingSetup.exe** from the matching release. Accept the Windows administrator prompt. This installs and registers the OpenXR layer used for automatic stereo alignment, including on headsets without eye tracking. It only needs to be installed once for all games.
4. Start the game and enable DLSS in the game's graphics settings.
5. Open the ReShade overlay and select **Cheeky Foveated DLSS** in the **Add-ons** tab. Confirm that **Foveated DLSS-SR** is enabled (it is on by default).
6. Leave **Foveation center** set to **Fixed** and **Automatic stereo alignment** enabled. Use **Height offset** to move the region up or down. If you have an eye-tracked headset and a compatible runtime, select **OpenXR gaze** to follow your eyes instead.
7. In VR, enable the red alignment border and check **Diagnostics > OpenXR eye tracking** for stable per-eye mappings and **OpenXR** alignment. Eye-tracking indicators can remain **No** during fixed placement. Turn off the border when finished.

The layer installer places a shared copy in
`C:\Program Files\CheekyFoveatedDLSS\OpenXR` and registers it automatically;
no game-folder selection is needed. Install ReShade and the add-on separately
for each game. Changes in the overlay apply live on the next DLSS evaluation.

Quest 3 has no eye-tracking hardware: use Fixed with automatic alignment and
Height offset. Real gaze requires an eye-tracked headset; otherwise the panel
shows a red message and falls back to fixed placement. Automatic alignment
requires a supported OpenXR rendering path or usable Streamline projection
data from the game; when neither is available, manual offsets are used.

To update, close OpenXR games, run the newer installer, and update the add-on in
each game folder from the matching release. To uninstall the layer, remove
**Cheeky OpenXR Eye Tracking** through Windows **Settings > Apps**. This removes
the shared layer and its registration for all games; the add-on remains in each
game. OpenXR alignment and gaze then become unavailable; usable Streamline
projection data can still provide automatic alignment, otherwise manual fixed
placement is used. As an emergency per-launch bypass, set
`CHEEKY_OPENXR_LAYER_DISABLE=1` before starting the game.

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
4. In VR, leave **Automatic stereo alignment** enabled to center each eye using available projection data. If the status reports manual fallback, install or update the OpenXR layer first. If the rendering path is unsupported, select **Fixed** and disable **Automatic stereo alignment** to expose **Stereo X offset** for manual adjustment. For reversed packed eye order, use **Stereo mapping override > Invert stereo eye order**.
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
| Automatic stereo alignment | On | Uses OpenXR or usable Streamline projection data to align each eye without manual X adjustment. |
| Stereo X offset | `0.60` | Manual horizontal placement; shown when two views are detected, Fixed is selected, and automatic alignment is off. |
| Invert stereo eye order | Off | Advanced override under Stereo mapping override for reversed packed eye order and manual stereo offsets. |
| Height offset | `0.00` with automatic alignment; `-0.45` in manual placement | Moves fixed placement up (negative) or down (positive). With automatic alignment, zero preserves the detected center. In gaze modes this is Fallback height offset and does not shift valid gaze. |
| Roundness | `0.00` | Blends the region shape from rectangular (`0`) to elliptical (`1`). This does not affect performance. |
| Transition width | `0.040` | Feathers the edge of the region. |
| Show 5 px red alignment border | Off | Displays the processed region while calibrating the fovea. |
| DX11 game processing path | DX11 Direct | **DX12 Transport** enables DX12-only features for DX11 games. |
| Foveation center | Fixed | Selects fixed placement, OpenXR gaze, or simulated gaze. |
| Gaze smoothing | `20 ms` | Sets the time constant for gaze motion. |
| Crop origin quantization | `8 px` | Snaps motion to render-pixel increments. |
| Jump reset threshold | `0.125 crop` | Resets DLSS history above the larger of 64 px or 12.5% of the crop dimension. |

Press **Alt+Shift+/** to toggle foveated DLSS-SR without opening the overlay. Settings are saved through ReShade and restored the next time the game starts.

**Automatic stereo alignment** is enabled by default and needs no eye tracker.
It aligns fixed placement and the fallback center when gaze is unavailable;
valid gaze already provides per-eye projected centers and is used directly. With the updated OpenXR layer, it
projects a shared forward direction into each eye, including eye-view cant and
asymmetric fields of view. The layer must reliably match each DLSS output to an
OpenXR eye. Without that mapping, a valid current-view Streamline projection can
provide the eye's optical-axis center instead; that fallback cannot infer headset
cant. The panel reports the source used for the latest evaluated view.

Automatic alignment controls both horizontal and vertical placement, keeping the
center stable when the fovea size changes, except where the crop reaches an image
edge. With automatic alignment enabled, **Height offset** adjusts the fixed center up
or down; zero preserves the detected center. This preference starts at zero and
is saved separately from legacy manual placement. In gaze modes it is labeled
**Fallback height offset** and never shifts valid gaze. **Stereo X offset** is
shown only for manual fixed placement. An advanced **Stereo mapping override**
retains eye-order inversion for reversed packed layouts.
Existing fixed/gaze selections are preserved. The previous experimental Auto
alignment mode is migrated to Fixed with automatic alignment enabled. OpenVR-only games without usable
Streamline projections still require manual alignment. DLSS-NR inherits the SR center, including automatic alignment and gaze on
routes that supply the live SR crop. **Use DLSS-SR size and shape** also links
width, height, roundness, and transition; otherwise those remain independent.

The automatic OpenXR route uses snapshot ABI 4: update both
`CheekyFoveatedDLSS.addon64` and `CheekyOpenXRLayer.dll` together. An older layer
is rejected safely and cannot supply OpenXR alignment or gaze to this build.

The **Diagnostics** and **Performance** panels show whether DLSS interception is active, the received resolutions and crop, call counts, GPU timing, and the last NGX result. If the panel remains on “Waiting for the first DLSS evaluation,” confirm that DLSS is enabled in the game and that ReShade was installed for the correct API.

## Experimental DLSS-NR support

DLSS-NR support is experimental. It is not expected to work correctly in every game, and some of the exposed tuning sliders may have little or no effect depending on the title and the data it supplies.

The required NVIDIA and Streamline runtimes are **not distributed with this project**. You must supply compatible Streamline DLLs yourself, together with a signed `nvngx_dlssnr.dll`. Place `nvngx_dlssnr.dll` beside `CheekyFoveatedDLSS.addon64`; keep the Streamline components in the locations expected by the target game. Runtime versions must be mutually compatible.

Additional notes:

- DLSS-NR is off by default.
- Direct3D 11 games require the **DX12 Transport** processing path for DLSS-NR.
- DLSS-NR follows the DLSS-SR center and stereo alignment. Its fovea width, height, roundness, and transition can be adjusted independently or linked to DLSS-SR, with a green alignment border for checking the region.
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

### Eye tracking (experimental)

I do not own an eye tracked headset, however due to the open source nature of the
project @Williem3 was able to add in the initial implementaiton. I cannot fully validate 
the eye tracking experience but rely on community reports if there are issues.

Eye tracking uses the OpenXR layer installed in the [main installation steps](#installation).
It requires an eye-tracked headset and a runtime that supplies usable gaze input.
Automatic stereo alignment works without eye tracking; Quest 3 users should use
**Fixed** with **Automatic stereo alignment** and adjust **Height offset** as needed.

To enable real tracking, select **Foveation center > OpenXR gaze**. For validation,
disable the game's built-in eye-tracked foveation, enable the red alignment border,
and open **Diagnostics > OpenXR eye tracking**. Check **System support**, **Gaze
action active**, **Tracking valid**, and **Using gaze**, along with stable, distinct
DLSS-view mappings for both eyes. **Eye gaze extension: Yes** alone does not mean
the headset supplies eye tracking.

Valid gaze sets both eye centers directly; it needs no manual stereo X offset.
**Fallback height offset** only adjusts fixed placement when gaze is unavailable
and does not shift valid gaze. If tracking is unavailable, a red message appears
directly below the selector and the add-on falls back to fixed placement, using
automatic alignment where available and saved manual placement otherwise.
Temporary signal loss holds the last valid gaze for 100 ms, then returns toward
the fixed fallback over 150 ms.

Separate eye textures and supported packed stereo layouts can be mapped. Quad
views, non-zero texture-array slices, and OpenVR-only gaze paths are not supported.
Missing, stale, or ambiguous data also causes fallback. To test motion without an
eye tracker, use [Simulated gaze](#simulated-gaze-no-eye-tracker-required); this does not validate real eye-tracker input or latency.

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
layer installer as described above. In the ReShade add-on panel, select **Foveation
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

### Building the OpenXR layer installer

With [Inno Setup 6.3 or newer](https://jrsoftware.org/isdl.php) installed in a
standard location or available on PATH, run from the repository root:

```powershell
.\scripts\build-installer.ps1
```

If you already have the portable compiler in `build\installer-tools\inno`,
use this command instead; no additional Inno Setup installation is needed:

```powershell
.\scripts\build-installer.ps1 -IsccPath .\build\installer-tools\inno\ISCC.exe
```

The script does not automatically search that portable directory. The compiler
is a local build tool and is not included in the repository; on a fresh checkout,
install Inno Setup or pass `-IsccPath` pointing to your own compiler. Set
`shared/version.h` to the release version you are packaging; the UI, CMake, and installer use this shared value. An optional `-Version` argument must match it.

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

## Reporting a problem

In the ReShade add-on panel, click **Report an issue...**. The add-on prepares a
support ZIP in `%TEMP%\CheekySupport`, opens a GitHub bug report with a short
game/version/graphics summary, and selects the
ZIP in Explorer. Review the archive, drag it into the **Support ZIP** field, wait
for the upload to finish, describe the problem, and submit the issue. Nothing is
uploaded automatically. A GitHub account is required to submit.

The archive includes live add-on settings, DX11/DX12 and OpenXR diagnostics,
system and loaded DLL version information, and available add-on, ReShade, and
crash logs. Each log is limited to its last 4 MiB; `README.txt` records missing
or truncated logs. The crash log may be from an earlier session. Logs are not
automatically redacted and may contain personal paths or identifiers; the full
ReShade configuration is not included. Reports stay on disk until you delete
them. **Show ZIP** and **Open GitHub issue** let you reopen a prepared report.

The issue destination is defined in `src/support_report.cpp`. Publish
`.github/ISSUE_TEMPLATE/bug_report.yml` to that repository's default branch
before distributing the add-on. Local builds alone do not publish the template.
