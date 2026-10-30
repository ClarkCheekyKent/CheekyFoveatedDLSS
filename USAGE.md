# Settings and troubleshooting

Start with the [installation guide](README.md#installation). This page contains the detailed control reference.

For **UEVR AFW**, use the [AFW setup and validation guide](uevr/README.md#afw-routing-experiment). That DX12 path supports SR, RR and NR. With AFW coverage controls off, it uses the configured per-eye region and ordinary eye calibration. Optional manual/automatic AFW coverage includes both possible source eyes. Effective coverage can exceed the stored fovea dimensions; the AFW banner reports SR coverage, while NR status reports its processing region and working resolution.

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
| Peripheral DLSS | On | Enables the auxiliary DLSS pass for the area outside the fovea. |
| Peripheral preset | E (Fastest) | Selects E, K, L, or M for the peripheral DLSS pass. |
| Periphery scale | `0.75` | Sets peripheral processing to 20%-100% of the game DLSS input width and height; 75% is the default. |
| Fovea width / height | `0.55` / `0.45` | Sets the normalized size of the DLSS-processed region. |
| Automatic stereo alignment | On | Uses OpenXR or usable Streamline projection data to align each eye without manual X adjustment. |
| Stereo X offset | `0.00` | Manual horizontal placement; shown when two views are detected, Fixed is selected, and automatic alignment is off. |
| Invert stereo eye order | Off | Advanced override under Stereo mapping override for reversed packed eye order and manual stereo offsets. |
| Height offset | `0.00` | Moves fixed and eye-tracked placement up (negative) or down (positive), including tracking-loss fallback. Zero preserves the detected center. |
| Roundness | `0.00` | Blends the region shape from rectangular (`0`) to elliptical (`1`). This does not affect performance. |
| Transition width | `0.040` | Feathers the edge of the region. |
| Show 5 px red alignment border | Off | Displays the processed region while calibrating the fovea. |
| DX11 game processing path | DX11 Direct | **DX12 Transport** enables DX12-only features for DX11 games. |
| Foveation center | Fixed | Selects fixed placement, runtime gaze (OpenXR / OpenVR / LibOVR), or simulated gaze. |
| Gaze smoothing | `20 ms` | Sets the time constant for gaze motion. |
| Hold gaze on signal loss | `400 ms` | Keeps the last gaze through blinks and short dropouts before returning to fixed placement. |
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

Press **Alt+Shift+/** to toggle foveated DLSS-SR. Press **Alt+Shift+>** (hold Alt and Shift, then press the period key) to toggle the entire DLSS-NR feature, preserving its foveation setting. Both shortcuts toggle once per press and save automatically through the selected integration.

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
is saved separately from legacy manual placement. The same **Height offset**
also shifts valid runtime and simulated gaze, including AFW coverage. **Stereo X offset** is
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
- **Style** selects Standard, Natural or Cinematic. This changes a different model parameter from the old preset hints, which have been removed from the menus along with the unverified UI correction toggle.
- **Intensity** controls the model edit from 0 (none) to 1 (full). Values above 1 from older settings files normalize to 1. **Local tone** and **Local structure** remain 0–2; compare their endpoints in a stationary scene to judge the effect.
- **Model passes** (1–3, Direct3D 12 and DX12 Transport) runs the model again on its own output inside the same region. Each pass keeps its own temporal history, and the picture is decoded once, so the colour transform and fovea feather are not applied repeatedly. **Pass 2 settings** and **Pass 3 settings** each have a **Same as pass 1** checkbox, on by default. Untick it to give that pass its own style, intensity, local structure and local tone; game-input settings under **Additional settings** always follow pass 1. Each extra pass costs about one more NR evaluation. If a pass cannot start, NR runs the passes that are ready; if one fails during a frame, that frame keeps the game image.
- **Skin structure** appears when **Automatic mask** is enabled. **Paper white** appears only when NR receives HDR input. These controls do not affect the picture outside those conditions.
- Direct3D 11 games require the **DX12 Transport** processing path for DLSS-NR.
- DLSS-NR uses the shared gaze and stereo alignment settings even with SR foveation disabled. Its fovea width, height, roundness, and transition can be adjusted independently or linked to DLSS-SR, with a green alignment border for checking the region.
- A DLSS-NR failure leaves the composited DLSS-SR result intact.
- RDR 2 currently does not work well with DLSS-NR.

### Eye tracking (experimental)

Eye tracking uses the OpenXR layer installed in the [main installation steps](README.md#installation).
It requires an eye-tracked headset and a runtime that supplies usable gaze input.
Automatic stereo alignment works without eye tracking; Quest 3 users should use
**Fixed** with **Automatic stereo alignment** and adjust **Height offset** as needed.

To enable real tracking, select **Foveation center > Runtime gaze (OpenXR / OpenVR / LibOVR)**. For validation,
disable the game's built-in eye-tracked foveation, enable the red alignment border,
and open **Diagnostics > OpenXR eye tracking**. Check **System support**, **Gaze
action active**, **Tracking valid**, and **Using gaze**, along with stable, distinct
DLSS-view mappings for both eyes. **Eye gaze extension: Yes** alone does not mean
the headset supplies eye tracking.

Valid gaze sets both eye centers directly; it needs no manual stereo X offset.
**Height offset** shifts valid gaze and fixed fallback placement. Leave it at
zero unless a game needs a vertical placement correction. If tracking is unavailable, a red message appears
directly below the selector and the add-on falls back to fixed placement, using
automatic alignment where available and saved manual placement otherwise.
Temporary signal loss, such as a blink, holds the last valid gaze for
**Hold gaze on signal loss** (default 400 ms, under **Advanced eye tracking**)
without resetting DLSS history, then returns toward the fixed fallback over 150 ms.

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

In UEVR DX12, enable DLSS-NR and open **Neural rendering > Rendering order**.
The selector applies and saves through the normal acknowledged settings flow.
`NrProcessingOrder=0` means After; `1` means Before. Older configuration files
without the key default to After, and other values reject the settings transaction.
If the runtime omits the setting, the menu shows it as unavailable. The configuration
schema and UI protocol remain at version 1. UEVR's DX11 path still does not support
NR or DX12 Transport.

**Rendering order** defaults to **After upscaling**, preserving the existing
post-SR processing. **Before upscaling** runs NR on a private copy of the active
render-resolution color before center SR, peripheral DLSS, and composition.
The option is implemented for native DX12, Streamline, and DX11 with **DX12
Transport**, including when SR foveation is disabled. Game-owned color remains
unchanged. Existing array-slice restrictions still apply.

**Working scale** remains adjustable from **0.10–1.00**, defaults to **1.00**,
and retains its saved value when changing order. It scales the NR region's width
and height in the selected processing resolution. For example, full-frame NR
with a 1600×1200 render size and 3200×2400 display size works at 1280×960 before
SR, or 2560×1920 after SR, at scale 0.80. At 1.00 Before mode uses render-resolution
pixels without rounding working dimensions to multiples of eight. Scaled dimensions
are rounded to the nearest pixel and retain a 32-pixel minimum.
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
peripheral DLSS on/off, live order switching, dynamic resolution/resizing, both
eyes, and gaze movement. Record image quality, actual NR working dimensions,
NR GPU time, and total pipeline GPU time for both orders. Support reports include
the selected order, dimensions, skip state, and separate timing samples.

### RealVR OpenXR gaze without game input actions

The OpenXR layer enables `XR_EXT_eye_gaze_interaction` when available and creates
its own gaze action. For an identified R.E.A.L. VR runtime, it can also attach that
action and synchronize it independently when the game never initializes OpenXR
input. Acquisition starts at a focused stereo `xrLocateViews` call in a running
session; synchronization runs once per display time. Focus loss or a failed sync
invalidates gaze, and focus recovery resumes polling without attaching again.

Standalone attachment is attempted once per session and only if the host has not
created action sets or attempted attachment. If the host has created actions,
Cheeky waits and appends gaze to the host's attachment. Once the host calls
`xrSyncActions`, Cheeky uses that merged synchronization path for the remainder of
the session. It does not issue extra gaze-only syncs that could deactivate host
controller actions. A host that starts creating and attaching input only after
Cheeky's standalone attachment cannot attach additional sets to that session;
OpenXR's already-attached error is preserved. Restart with a host-managed input
path in that case.

Support reports include `gaze.input` with the detected RealVR state, host and
fallback call counts, binding/attachment state, and the runtime results for binding,
attachment, synchronization, action-state queries, action-space creation and gaze
location. A `null` result means the operation has not been called. Old layer builds
without these diagnostics report `input: null`. Extension support alone does not
mean the headset/runtime is providing gaze. Independent polling cannot override
runtime focus or eye-tracking permissions.

This change requires updating the installed **OpenXR layer**, as well as the game
runtime. Close VR applications and run the matching OpenXR setup included in the
RealVR gaze test package; replacing only the game's DLLs will not update the layer.

### LibOVR games and Pimax eye tracking

Games and mods that use the Oculus PC SDK (LibOVR) are observed in-process, with
no OpenXR involvement. This includes R.E.A.L. VR in its LibOVR mode
(`PreferredAPI2=1`) on a Pimax headset, where Pimax Play's Oculus compatibility
runtime serves LibOVR. The OpenXR layer is not used for these games, and the mod
does not need to switch to OpenXR.

Cheeky finds the LibOVR runtime the game already loaded by its exports and
observes swap-chain commits and `ovr_EndFrame`/`ovr_SubmitFrame`: the submitted
eye images, viewports, FOVs and render poses drive automatic stereo alignment and
eye calibration, as with OpenXR. On Pimax, gaze comes from Pimax's PVR client that
the compatibility runtime has already loaded: Cheeky reads eye tracking from the
game's own PVR session. It never loads or initializes a VR runtime and never
creates a second session. The combined gaze of both eyes is projected into each
submitted eye view, as Pimax's OpenXR eye gaze interaction reports it.

Enable and calibrate eye tracking in Pimax Play first. Then check **Diagnostics >
Eye tracking details > LibOVR runtime** in the F8 menu: **Game PVR session found**
and **Eye tracker supplying gaze** should both read Yes, even with **Fixed**
placement selected. Eye calibration reports the **LibOVR** backend.

Limits: eye calibration supports D3D11 LibOVR swap chains, including D3D12
rendering transferred into those D3D11 textures. Native D3D12 LibOVR swap-chain
calibration is unsupported; gaze and alignment still depend on usable mapping.
Vulkan-to-LibOVR support has not been validated and is deferred. The
Oculus runtime itself (for example Quest through Link) exposes no eye tracking to
LibOVR; such games get alignment and calibration only. A PVR client that is not
already loaded by the game's runtime is never used.

### Direct3D 12 Ray Reconstruction

Cheeky detects native NGX Ray Reconstruction separately from Super Resolution,
including NVIDIA OTA runtimes and calls nested below R.E.A.L. VR. The center and
periphery use separate RR features and histories. RR guide textures follow the
center crop and peripheral scale, keeping the periphery denoised. Turning off
Peripheral DLSS uses input-resolution RR there instead of raw ray-traced color.

When RR is active, preset menus offer Game default, D, E and F. RR preferences
are stored separately as `RrCenterPreset` and `RrPeripheralPreset`; SR preferences
are retained when switching back. Preset availability depends on the installed
NVIDIA runtime. SR-only periphery is not offered because SR cannot replace RR's
denoising of noisy ray-traced input. NR remains available before or after RR.

This path remains experimental; game/headset validation is limited. Cropped RR currently
requires input-resolution motion and valid albedo/normal/roughness guides.
Output alpha and legacy research inputs without independent crop coordinates
use the game's native full-frame RR with optional NR. Moving the RR center
preserves history using crop-relative motion vectors and a cropped projection;
game resets, gaze jumps, geometry changes and discontinuities still invalidate it.

The center uses crop-sized inputs and fractional composite alignment to preserve
the full-frame sampling grid as it moves. Change-only eye calibration retains the
first valid published mapping until a view, geometry, session or explicit
calibration-setting change; continuous mode continues verifying markers.

### DLSS hook path (D3D12)

**Use lower DLSS hook (DX12)** is enabled by default. Cheeky processes DLSS at the feature runtime, leaving upstream core and Streamline calls intact. Turn it off to use the higher call (the previous outermost-interception path). The control is in the DLSS-SR panel for standalone/UEVR and beside the processing-path controls in ReShade.

The saved setting is `D3D12LowerHook=true` (`false` selects higher). Restart the game after changing it; the active path remains unchanged until restart so existing feature handles and histories keep their owner. The runtime snapshot reports `d3d12_lower_hook_active` and `d3d12_hook_restart_required`. If the lower runtime cannot be identified or hooked, its path leaves game DLSS unchanged; it does not automatically switch to higher processing.


## Shader frame generation

**Force NVIDIA Reflex off** is under **FG → Advanced settings** and defaults
off (`FgForceReflexOff=0`). Checking it disables NVIDIA's DX11/DX12 Reflex
sleep and Reflex-owned frame limiter, even while FG itself is off. Other
game frame limiters and latency markers remain active. The UEVR panel counts
blocked sleep calls so you can verify the override is taking effect.
Unchecking restores the latest intercepted game options. If injection happened
after the game configured Reflex, only its prior low-latency enable state can
be recovered; reapply the game's Reflex setting to restore Boost/frame-limit
options that were set before injection.

Choose **FG multiplier** in the standalone/OptiScaler FG tab or UEVR FG panel:
**2x** (default), **3x**, or **4x** (`FgMultiplier=2`, `3`, or `4`). These produce
one, two, or three distinct interpolated frames between real frames. The game
targets headset refresh divided by the selected multiplier once FG is running.
Shader methods support all three multipliers. DLSS regions require the GPU and
NGX runtime to report support for the selected count; otherwise native frames
continue and the FG status explains the unsupported multiplier. The D3D12
direct-OpenXR experiment uses intermediate outputs at 3x/4x. Replace the runtime,
OpenXR layer, and UEVR script together when updating.

**Automatic FG multiplier** (`FgAutoMultiplier=true`, off by default) chooses
2x-4x from CPU/GPU work timings and the measured cost of generated frames. It
excludes the layer's deliberate Wait/Begin pacing, increases after at least
150 ms and three fresh overloaded samples, and reduces one step after 1.5 s
of headroom. Query delivery and smoothing add response time. The readout shows
the selected and currently presented multiplier and the supported maximum.
DLSS limits are respected; shader FG can use all three counts. Auto changes
apply to complete stereo intervals without restarting the presenter or clearing
color/depth history. It will not increase the count when extra interpolation
would worsen GPU load. Missing timings retain the current count. The manual
selection is restored when Auto is turned off.

The FG center and periphery select a vector source and reconstruction method
independently. Sources are **DLSS-G**, **DLSS Vectors**, **Optiflow**, and
**Optiflow + Depth**; the periphery also offers **Previous frame**.
Shader reconstruction offers **Simple Warping**, **Bidirectional Interpolation**,
**Confidence-Guided Interpolation**, and its **Fast** variant. DLSS-G remains
the default center source.

Optical estimation always uses 8-bit color inputs for both regions. Bright input
values are compressed with a shared RGB scale to preserve hue; generated color
retains its original range. Confidence-guided optical methods automatically cache
vector/cost data once per native frame pair and reuse it across generated phases.
The foveated center estimates motion over its cropped region plus a halo.
There is no extra full-eye estimation override.

Optical flow validation always uses image agreement in both regions: the warped
frames are compared with a camera-aligned blend, replacing cost/forward-backward
confidence in confidence-guided methods. All optical reconstruction methods use
it, including depth variants and cached vectors. Head rotation is accounted for
when valid camera poses are available; otherwise the baseline is unwarped.

Optical presets, input resolution and vector grid remain independently selectable
for both regions. For a controlled quality comparison, try Medium, 50%, 4x4 in
both regions, then change one option at a time. Image validation can favor a
camera-aligned blend where estimated motion disagrees with the colors; compare
moving edges and occlusions in the game as well as generation time. Retired RGB,
cache, full-eye and validation settings (`FgOpticalCenterValidation`,
`FgOpticalPeripheralValidation`) in older profiles are ignored and omitted when saved.
Update the runtime, menu script and OpenXR layer together.

**DLSS-G preset** selects **Preset A**, **Preset B**, or **Preset C**
(`FgDlssgPreset=1`, `2`, or `3`). Preset C is the default, including for old
profiles and Reset FG defaults. The choice applies to every region using
DLSS-G; shader methods are unaffected. Changing it safely rebuilds the FG
presenter. Preset C requests Lite candidate blending and requires DLSS-G
310.9.16 or newer. Preset B requests UI recomposition, which needs separate
HUD-less scene and UI inputs; without those inputs it behaves like A.

For D3D11 OpenXR stereo and D3D11 quad views, choosing shader methods for both
regions selects native D3D11 capture, guide history and final-image rendering.
This path creates no D3D12 device or bridge and does not query DLSS-G capability.
Choosing DLSS for either active region retains the D3D12 processing path. The
separate DX11-to-DX12 SR transport option still applies if explicitly enabled.

DLSS-vector methods need captured game inputs: simple warping uses motion vectors and
previous/current color; bidirectional interpolation also uses depth history and
camera reprojection. The existing NGX-SR guide source remains necessary. Stereo
uses calibrated per-eye source crops; quad views require all four assigned views.
Missing or ambiguous guides keep native presentation.

Native D3D11 writes directly into acquired final OpenXR images, using a compute
shader where writable UAV views are supported and a pixel shader for render-target
images, including BGRA and sRGB. It serializes native/generated presentation and
keeps full-eye guide history. DX12 queue, compact-storage and upload experiments
do not apply to this path. Replace both the runtime and OpenXR layer together.
