# Automatic eye calibration

Cheeky follows the destinations of DLSS outputs through the game to OpenVR, so
the sharp region follows the correct physical eye when a video, menu or scene
changes the order of the game's stereo views. Calibration is enabled each time
the ReShade add-on or UEVR plugin attaches. It does not rewrite gaze or foveation
preferences.

The implemented backend is **D3D11 with native OpenVR, two ordinary 2D eye
submissions per frame**. It has been tested in Assetto Corsa Competizione with
ReShade, including the intro-to-menu transition. The shared backend and
diagnostics are included in the UEVR runtime, but D3D12, OpenXR/OpenComposite,
array submissions and rendering/submission on different threads do not gain
pixel calibration from this change. Existing eye-mapping methods remain
available when a valid calibration is unavailable.

## Diagnostics

Open **Diagnostics > Eye calibration** in ReShade or **Diagnostics and support
> Eye calibration** in UEVR. Both show the same information:

- Status and backend, with a session-only enable switch.
- **Corrections applied:** changes to an existing eye assignment. Swapping the
  pair counts once; confirming it on later frames does not increment this.
  Assigning previously unknown views is a confirmation, not a correction.
- **Confirmed mapping updates:** accepted results, including refreshes.
- Valid/completed samples and skipped/in-flight captures.
- CPU work per sampled OpenVR frame, GPU marker/copy time and readback latency.
- Last recognized left/right DLSS view identities. These are historical when
  the status is inactive.

Resetting counters preserves the current mapping. CPU time includes work inside
the calibration lock, including warm-up allocations, but excludes lock waiting
and surrounding hook dispatch. GPU time covers marker and patch-copy commands,
not the entire frame. Averages use the latest 256 GPU/latency samples; CPU is
cumulative since reset. Missing GPU timestamps are shown as unavailable.

ReShade's support ZIP includes the full calibration snapshot in `diagnostics.txt`;
UEVR includes it under `eye_calibration` in `diagnostics.json`. Both also report
whether each eye's crop mapping used pixel calibration. Reports retain additional
allocation, mismatch and maximum-cost counters. No images, ZIPs or logs are
written automatically per frame. The former experiment's F8/F9 hotkeys and
one-shot image capture have been removed.

## Implementation and lifetime rules

`src/eye_calibration.cpp` owns an eight-slot reusable GPU readback ring. After
each successful outer D3D11 NGX evaluation, candidate A receives a 20x20 magenta
block near its top-left corner; B receives cyan near its top-right corner. The
blocks are inset 12 pixels and remain in the image. Before forwarding OpenVR
Submit, Cheeky copies the corresponding regions from the final submitted image,
including packed or flipped submission bounds.

Readbacks use non-flushing event queries and nonblocking maps on the immediate
context's owning thread. Busy slots are skipped, never overwritten or waited
on. Marker textures, staging textures and queries are reused after warm-up;
format, dimensions or device changes may allocate replacements. Calibration
runs every frame, independently of whether foveated DLSS-SR is enabled.

The classifier checks both marker locations using normalized color contrast.
Both eyes must independently identify different candidates, with sufficient
absolute confidence and separation. Source before/after patches verify that
the marker was actually written. Missing/occluded markers, extra evaluations,
duplicate or failed submissions, and unsupported resources do not publish a
pair. This is a marker heuristic, not a guarantee under arbitrary postprocessing.

The settings layer atomically accepts only a newer result for two still-live
DLSS handle generations. Captures older than one second are rejected and the
mapping expires one second after its last accepted capture. Toggling calibration
invalidates outstanding results. View destruction clears a pair containing that
view. Changing a calibrated eye resets the crop's temporal filter through the
existing mapping-change path. Asynchronous readback means a new destination can
take several frames to affect foveation; no same-frame correction is promised.

## Validation

`scripts/build.ps1 -Configuration Release` runs the native suite, including WARP
and hardware D3D11 calibration tests, reversed and changing eye destinations,
packed bounds, occluded/duplicate-eye rejection, supported pixel formats,
resource reuse, counter reset, stale results and handle reuse. Existing gaze
tests cover calibrated crop routing. UEVR host tests cover the snapshot,
commands, support bundle and unload/reload lifecycle. `tests/uevr_menu_tests.py`
runs the real Lua menu in LuaJIT and Lua 5.4, including apply-on-release behavior.

These tests do not substitute for validation in additional games or UEVR VR
sessions.
