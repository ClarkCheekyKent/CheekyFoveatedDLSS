# Automatic eye calibration

Cheeky follows marked DLSS outputs to the physical eyes submitted to OpenVR or
OpenXR. The sharp region can then follow the correct eye when videos, menus or
scenes change the order of the game's stereo views. Calibration runs every frame
and starts enabled when ReShade or UEVR attaches, independently of DLSS-SR's
foveation switch. It does not rewrite saved stereo or gaze preferences.

## Supported paths and installation

| Graphics API | OpenVR | Native OpenXR |
| --- | --- | --- |
| D3D11 | Separate, packed/flipped bounds, array slices | Separate, packed projection rectangles, array slices |
| D3D12 | Separate, packed/flipped bounds, array slices | Separate, packed projection rectangles, array slices |

The same core and diagnostics ship in ReShade and UEVR. Native OpenXR needs the
**matching updated Cheeky OpenXR layer** installed using
`CheekyEyeTrackingSetup.exe`, in addition to the new add-on or plugin/runtime.
Close the game before installing and restart it afterward. Copying the layer DLL
beside the game does not register an OpenXR API layer.

OpenComposite can expose both APIs. When OpenVR submissions are being observed,
calibration uses OpenVR and suppresses the inner OpenXR capture to avoid mixing
two frame timelines. An OpenXR gaze runtime can therefore appear alongside an
OpenVR calibration backend in diagnostics.

This covers the project's D3D11/D3D12 paths, not Vulkan or OpenGL. Unsupported
formats, multisampled images, non-stereo/multiple projection layers, missing
markers, or unrecognized submission flags do not force an eye assignment.
D3D11 marker/copy/readback work must use the immediate context's owning thread.
Frame pipelines must expose both DLSS evaluations and their submissions in the
same observed VR frame; extra or mismatched evaluations are rejected.

## Diagnostics

Open **Diagnostics > Eye calibration** in ReShade or **Diagnostics and support
> Eye calibration** in UEVR. Both show:

- Detected VR backend, graphics API, status and session enable switch.
- **Corrections applied:** changes to an existing eye assignment. Swapping the
  pair counts once; subsequent confirmations do not increment this counter.
- **Confirmed mapping updates:** all accepted results, including refreshes.
- Valid/completed samples, skipped/in-flight captures, CPU/GPU work and latency.
- Last recognized left/right DLSS views, retained as history when inactive.

Resetting counters preserves the mapping. CPU work measures the core capture and
polling calls, including warm-up allocation but excluding lock waiting and
surrounding hook dispatch/post-submit fence bookkeeping. GPU time covers marker
and patch-copy commands, not the whole frame. GPU/latency averages use the latest
256 samples; CPU is cumulative since reset. Missing timestamps show unavailable.

ReShade's support ZIP includes calibration in `diagnostics.txt`; UEVR includes
`eye_calibration` in `diagnostics.json`. Both report marker-based crop routing.
Additional allocation, mismatch and maximum-cost counters remain in reports.
Calibration does not automatically write images, ZIPs or logs.

## Markers and asynchronous readback

After a successful outer DLSS evaluation and final composition, candidate A gets
a 20x20 magenta block near its top-left corner; B gets cyan near its top-right.
Both are inset 12 pixels and remain in the output. Before/after source patches
verify that the marker was written. Two small patches from each submitted eye
are compared using normalized color contrast, absolute confidence and separation
thresholds. Each physical eye must confidently identify a different candidate.
This remains a heuristic under postprocessing that can obscure or alter markers.

`src/eye_calibration.cpp` owns an eight-slot reusable readback ring. Busy slots
are skipped, never waited on or overwritten. The D3D11 implementation uses
non-flushing queries and nonblocking maps. `src/eye_calibration_d3d12.cpp` uses
readback buffers, timestamp queries and independent fence timelines for the
queues that actually execute the recorded commands. Resource states are restored
after marker/copy work. Buffers and GPU objects are reused after warm-up; format,
device or layout changes can require new allocations.

D3D12 game command lists can be resubmitted. A slot is neither mapped nor reused
until the game resets/destroys the recording and every observed execution has
completed. Recordings discarded without execution are rejected. Live recordings
retain their resources through host detach, and the shared native Execute/Reset
observer stays resident to retire them safely. Long-lived, unreset recordings or
failed fence signals can exhaust the ring; captures are skipped rather than
blocking rendering or freeing resources still referenced by GPU commands.

## OpenXR ownership and eye labels

OpenVR copies patches immediately before forwarding `Submit`. OpenXR needs two
steps because applications must not access a swapchain image after release:

1. After successful acquire/wait, copy small patches **before**
   `xrReleaseSwapchainImage`, using the previous projection rectangles as hints.
2. At `xrEndFrame`, match those captures to the actual projection subimages and
   last released image indices. These supply the physical left/right labels.

The first frame learns the rectangles. An eye-label permutation can use that
frame's captured patches immediately; a new rectangle/layout is learned and
sampled on a later frame. Failed releases/end calls, stale image indices and
ambiguous projections reject the entire pair. Swapchain/session destruction
invalidates the associated calibration. OpenXR session generations travel with
the mapping so crop routing cannot use a result from another session.

OpenXR D3D12 copies transition from and restore `RENDER_TARGET`; OpenVR D3D12
copies restore `PIXEL_SHADER_RESOURCE`, following their respective contracts:
[OpenXR release ownership](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrReleaseSwapchainImage.html),
[OpenXR D3D12 state](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_D3D12_enable-swapchain-image-state.html),
[OpenVR D3D12 submission](https://github.com/ValveSoftware/openvr/wiki/DirectX12).

The settings layer accepts only newer results for two still-live DLSS handle
generations. Results older than one second are rejected and mappings expire one
second after their last accepted capture. Disabling calibration invalidates
pending results. Eye changes reset the crop's temporal filter. Asynchronous
readback adds several frames of latency; same-frame correction is not promised.

## Validation

`scripts/build.ps1 -Configuration Release` exercises WARP and hardware GPU
readbacks, changing destinations, supported formats, packed/array submissions,
occluded/duplicate-eye rejection, stable allocations and correction counters.
The D3D12 lifetime tests cover resubmission on a second queue with an independently
blocked fence, and Reset without execution. A simulated loader/runtime drives the
actual OpenXR layer DLL through its instance/session/swapchain/frame entry points
on D3D11 and D3D12. D3D11 also overwrites released textures before EndFrame to
verify that calibration uses pre-release captures. Coordinator tests reject
failed calls, changed rectangles and stale indices, and verify eye relabeling.

Gaze tests cover calibrated crop routing and foreign-session rejection. UEVR
tests cover host ABI, diagnostics, reports, interception and lifecycle. The Lua
menu tests preserve slider apply-on-release behavior in LuaJIT and Lua 5.4.

The earlier D3D11 OpenVR build was confirmed in Assetto Corsa Competizione by the
user. The added native OpenXR and D3D12 paths still need real game/headset testing;
automated tests do not establish compatibility or performance in every game.
