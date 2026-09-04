# Cheeky Foveated DLSS

Cheeky Foveated DLSS is a 64-bit [ReShade](https://reshade.me/) add-on that reduces the cost of DLSS Super Resolution by applying it to the part of the image that matters most. It supports Direct3D 11 and Direct3D 12 games, including stereo rendering for VR.

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

Eye tracking is optional and remains off by default. Build the project, then
keep `CheekyOpenXRLayer.dll` and
`XR_APILAYER_CHEEKY_foveated_dlss.json` together in `bin\Release`. Register the
manifest as an OpenXR implicit API layer:

1. Open **Registry Editor**.
2. For the current user, navigate to
   `HKEY_CURRENT_USER\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit`. Create any
   missing keys. If the active OpenXR loader requires a machine-wide layer,
   as Assetto Corsa EVO does, use
   `HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit` instead;
   editing this location requires administrator privileges.
3. Create a new **DWORD (32-bit) Value**. Use the full absolute path to
   `XR_APILAYER_CHEEKY_foveated_dlss.json` as the value name and set its data to
   `0` to enable the layer. For example:
   `C:\path\to\CheekyFoveatedDLSS\bin\Release\XR_APILAYER_CHEEKY_foveated_dlss.json`.
4. Start or restart the game and open the ReShade overlay.
5. Open the **Add-ons** tab and select **Cheeky Foveated DLSS**.
6. Enable **Foveated DLSS-SR**, then set **Foveation center** to **OpenXR
   gaze**.
7. To verify eye tracking, enable the red alignment border and open
   **Diagnostics > OpenXR eye tracking**. The layer and gaze must report as
   available, and both eyes must acquire stable DLSS-view mappings before the
   border follows your gaze.

To unregister the layer, delete that DWORD value from the same registry key.
As an emergency per-launch bypass, set `CHEEKY_OPENXR_LAYER_DISABLE=1` before
starting the game. Moving the JSON or DLL after registration requires updating
the registry value to the JSON's new absolute path.

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

The first eye-tracked path targets Pimax Dream Air, Pimax OpenXR, and Assetto
Corsa EVO. It uses the standard `XR_EXT_eye_gaze_interaction` pose, projects the
gaze independently through each asymmetric OpenXR view, and only activates after
the add-on has matched the exact DLSS output resource and rectangle for two
consecutive display frames.

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

If this add-on is useful to you and you would like to buy me a coffee, you can [support me on Ko-fi](https://ko-fi.com/cheekykent).

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
