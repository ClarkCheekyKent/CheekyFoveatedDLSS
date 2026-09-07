# Settings and troubleshooting

Start with the [installation guide](README.md#installation). This page contains the detailed control reference.

## Controls

Start with the defaults, then tune the region while looking at a representative scene:

1. Turn on **Show 5 px red alignment border** so the processed region is visible.
2. Adjust **Fovea width** and **Fovea height**. Smaller values improve performance but make the transition easier to notice.
3. Use **Height offset** to move the region vertically and **Transition width** to soften its boundary.
4. In VR, keep **Automatic stereo alignment** and **Automatic eye calibration** enabled. For OpenXR, the matching OpenXR installer is required. Manual offsets and eye-order overrides are troubleshooting fallbacks: scene changes can invalidate them.
5. Turn the red alignment border off when calibration is complete.

The main controls and their defaults are:

| Control | Default | Purpose |
| --- | --- | --- |
| Enable foveated DLSS-SR | On | Enables the main foveated Super Resolution path. |
| Center preset | Game/default | Preserves the game's DLSS preset or overrides it with E, K, L, or M. |
| Center supersampling | `1.00x` | Scales the center DLSS output by 1-2x per dimension, then area-downsamples to its original size. Applies on slider release. Supports input- and output-resolution motion vectors. |
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
| Foveation center | Fixed | Selects fixed placement, runtime gaze (OpenXR / OpenVR), or simulated gaze. |
| Gaze smoothing | `20 ms` | Sets the time constant for gaze motion. |
| Crop origin quantization | `8 px` | Snaps motion to render-pixel increments. |
| Jump reset threshold | `0.125 crop` | Resets DLSS history above the larger of 64 px or 12.5% of the crop dimension. |

Center supersampling leaves the game's render resolution and the fovea's screen
size unchanged. The range is `1.00x` to `2.00x`. At `1.50x`, DLSS reconstructs 2.25 times as many center output
pixels; at `2.00x`, four times as many. Area downsampling runs in the existing
composite pass, with no sharpening or separate downscale pass. Quality gains are
scene-dependent and cost additional GPU time and memory. The default `1.00x`
preserves the original pixel mapping. Output-resolution motion vectors are
point-resampled in a private compute pass with displacement scaled to the new
output grid; gaze-crop correction is combined with that pass. This avoids blending
unrelated velocities at object boundaries. The game's vector textures remain
unchanged. Unsupported vector resources or 3D Streamline vectors fall back to
the game's DLSS if resampling is required. Extremely large centers are capped
at the GPU's 16,384-pixel texture dimension limit.

Press **Alt+Shift+/** to toggle foveated DLSS-SR. Settings save automatically through the selected integration.

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
DLSS-SR and foveated DLSS-NR share the selected gaze/alignment center. NR keeps
tracking when SR foveation is disabled. **Use DLSS-SR size and shape** also links
width, height, roundness, and transition; otherwise those remain independent.

For OpenXR, install the layer and your adapter from the same release using `CheekyOpenXRSetup.exe`.

The **Diagnostics** and **Performance** panels show whether DLSS interception is active, the received resolutions and crop, call counts, GPU timing, and the last NGX result. If the panel remains on “Waiting for the first DLSS evaluation,” confirm that DLSS is enabled in the game and that ReShade was installed for the correct API.

## Experimental DLSS-NR support

DLSS-NR support is experimental. It is not expected to work correctly in every game, and some of the exposed tuning sliders may have little or no effect depending on the title and the data it supplies.

The required NVIDIA and Streamline runtimes are **not distributed with this project**. You must supply compatible Streamline DLLs yourself, together with a signed `nvngx_dlssnr.dll`. Place `nvngx_dlssnr.dll` beside `CheekyFoveatedDLSS.addon64`; keep the Streamline components in the locations expected by the target game. Runtime versions must be mutually compatible.

Additional notes:

- DLSS-NR is off by default.
- Direct3D 11 games require the **DX12 Transport** processing path for DLSS-NR.
- DLSS-NR uses the shared gaze and stereo alignment settings even with SR foveation disabled. Its fovea width, height, roundness, and transition can be adjusted independently or linked to DLSS-SR, with a green alignment border for checking the region.
- A DLSS-NR failure leaves the composited DLSS-SR result intact.
- RDR 2 currently does not work well with DLSS-NR.

### Eye tracking (experimental)

Eye tracking uses the OpenXR layer installed in the [main installation steps](README.md#installation).
It requires an eye-tracked headset and a runtime that supplies usable gaze input.
Automatic stereo alignment works without eye tracking; Quest 3 users should use
**Fixed** with **Automatic stereo alignment** and adjust **Height offset** as needed.

To enable real tracking, select **Foveation center > Runtime gaze (OpenXR / OpenVR)**. For validation,
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

Separate, packed and array-slice submissions can use marker calibration on D3D11 and D3D12. Quad views are not supported. See [Eye calibration](EYE-CALIBRATION.md) for path-specific limits.
Missing, stale, or ambiguous data also causes fallback. To test motion without an
eye tracker, use [Simulated gaze](DEVELOPMENT.md#simulated-gaze-no-eye-tracker-required); this does not validate real eye-tracker input or latency.


## Reporting a problem

In the ReShade add-on panel, click **Report an issue...**. The add-on prepares a
support ZIP in `%TEMP%\CheekySupport`, opens a GitHub bug report with a short
game/version/graphics summary and prefilled **Diagnostics and settings**,
and selects the ZIP in Explorer. Review the populated report, then drag the ZIP
into **Support ZIP**. Wait for the upload
to finish, describe the problem, and submit the issue. Nothing is
uploaded automatically. A GitHub account is required to submit.

The archive includes live add-on settings, DX11/DX12 and OpenXR diagnostics,
system and loaded DLL version information, and available add-on, ReShade, and
crash logs. Each log is limited to its last 4 MiB; `README.txt` records missing
or truncated logs. The crash log may be from an earlier session. Logs are not
automatically redacted and may contain personal paths or identifiers; the full
ReShade configuration is not included. Reports stay on disk until you delete
them. **Show ZIP** and **Open GitHub issue** let you reopen a prepared report.

The prefilled report includes system and relevant runtime versions, feature status,
active graphics-path diagnostics, readable OpenXR status, and all current add-on
settings in a collapsible section. It flags requested gaze/alignment that was not
observed at capture time. Unused APIs and disabled DLSS-NR are reduced to status
lines; duplicate GPU entries, resource addresses, mapping counters, and routine
system DLL details are omitted from the issue. Zero timings are marked unavailable
rather than presented as measured zero cost. The full original diagnostic dump
and system inventory remain in the ZIP. `issue-report.md`, saved beside and
inside the ZIP, additionally includes log availability and up to 2 KiB of recent
text from each log. **Review report** opens it; **Copy detailed report** lets you
paste this extended version if desired. Exceptionally large reports exceeding
the 7,800-character encoded URL budget display explicit paste instructions
instead of opening a broken link. Logs are never placed in the issue URL.

The description and reproduction prompts prefill the game executable and VR mode
when compatible OpenXR session/mapping activity is detected. Otherwise the user
is asked to confirm desktop or VR. The runtime name is shown separately; the
current diagnostic interface does not expose a headset model, so users supply it.

### Rendering order and working scale

**Rendering order** defaults to **After upscaling**, preserving the existing
post-SR processing. **Before upscaling** runs NR on a private copy of the active
render-resolution color before center SR, peripheral DLAA, and composition.
The option is implemented for native DX12, Streamline, and DX11 with **DX12
Transport**, including when SR foveation is disabled. Game-owned color remains
unchanged. Existing array-slice restrictions still apply.

**Working scale** remains adjustable from **0.10–1.00**, defaults to **1.00**,
and retains its saved value when changing order. It scales the NR region's width
and height in the selected processing resolution. For example, full-frame NR
with a 1600×1200 render size and 3200×2400 display size works at 1280×960 before
SR, or 2560×1920 after SR, at scale 0.80. At 1.00 Before mode uses render-resolution
pixels. Dimensions retain the runtime's eight-pixel alignment and 32-pixel minimum.
Independent and linked NR shapes follow the coordinated SR center; the green
border is drawn after SR at display resolution with its five-pixel width.

Before mode trades potential image quality for fewer NR working pixels. **No
NVIDIA quality or performance improvement has been measured for this change.**
The Release tests and WARP GPU readbacks verify contracts, private copying,
composition, and input/subresource preservation; they cannot establish NVIDIA
runtime or game compatibility. Compare identical scenes and settings on each
route using the separate Before/After NR and total intercepted-pipeline GPU
measurements in diagnostics. Native/Streamline totals include preparation,
NR, SR, peripheral work, and composition; DX11 totals include transport and its
queue waits. These totals do not represent the entire game's GPU frame.

In-game acceptance remains pending: test NR full/foveated, SR foveation on/off,
peripheral DLAA on/off, live order switching, dynamic resolution/resizing, both
eyes, and gaze movement. Record image quality, actual NR working dimensions,
NR GPU time, and total pipeline GPU time for both orders. Support reports include
the selected order, dimensions, skip state, and separate timing samples.
