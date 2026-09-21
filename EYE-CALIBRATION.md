# Automatic eye calibration

Cheeky follows marked DLSS outputs to the physical eyes submitted to OpenVR or
OpenXR. The sharp region can then follow the correct eye when videos, menus or
scenes change the order of the game's stereo views. Calibration samples immediately when enabled, then every 10 VR frames,
and starts enabled when a host attaches, independently of DLSS-SR's
foveation switch. Pending readbacks still drain between samples; the established
eye mapping remains active. A changed mapping can take up to 10 frames plus
readback latency to detect. It does not rewrite saved stereo or gaze preferences.

## Supported paths and installation

| Graphics API | OpenVR | Native OpenXR |
| --- | --- | --- |
| D3D11 | Separate, packed/flipped bounds, array slices | Separate, packed projection rectangles, array slices |
| D3D12 | Separate, packed/flipped bounds, array slices | Separate, packed projection rectangles, array slices |

The same calibration core is shared by ReShade, UEVR, standalone and OptiScaler. Native OpenXR needs the
**matching updated Cheeky OpenXR layer** installed using
`CheekyOpenXRSetup.exe`, in addition to the new add-on or plugin/runtime.
Close the game before installing and restart it afterward. Copying the layer DLL
beside the game does not register an OpenXR API layer.

OpenComposite can expose both APIs. When OpenVR submissions are being observed,
calibration uses OpenVR and suppresses the inner OpenXR capture to avoid mixing
two frame timelines. An OpenXR gaze runtime can therefore appear alongside an
OpenVR calibration backend in diagnostics.

This covers the project's D3D11/D3D12 paths, not Vulkan or OpenGL. Unsupported
formats, multisampled images, non-stereo/ambiguous game projection layers, missing
markers, or unrecognized submission flags do not force an eye assignment.
D3D11 source stamping and readback stay on the original render thread.
For native OpenXR D3D11 pipelines that submit on another thread, calibration
enables the immediate context's `ID3D11Multithread` protection on the render
thread. Pre-release patch copies then run under that D3D lock on the submission
thread. Devices created with `D3D11_CREATE_DEVICE_SINGLETHREADED` still reject
cross-thread capture. Protection remains enabled for the device lifetime; it
adds D3D context locking overhead. Contention on calibration state skips the
submission capture instead of waiting with the D3D lock held.
When XR textures belong to a different D3D11 device, that submission context is
protected separately. Its patch textures and per-eye completion queries are
allocated on the submission device and polled on the submission thread. Source
and submitted scores join only after both devices' readbacks complete; no texture
or query is used through another device's context. The host's existing image
transfer supplies the submitted markers; calibration adds no inter-device copy.
If an XR frame ends on a different thread, query closure is deferred until the
original render thread next stamps an output or ticks calibration. Unfinished
slots remain owned until GPU readback completes.

Native OpenXR D3D11 source captures may span up to four XR intervals / 250 ms,
so alternating eye rendering can supply distinct DLSS views on adjacent frames.
Source renders retain the current corner pair or selected tracking marker between
readback samples. This supports hosts that submit images rendered earlier.
Codes and candidate slots stay bound to view identities for the calibration
epoch, so a delayed submitted frame can still match. Toggling calibration,
changing the backend/session or replacing a source view invalidates those codes.
A sample retains the first source before/after proof for each candidate.
Submissions before both candidates exist are skipped. Once submission copying
starts, source evidence is frozen; both eyes must belong to that one submission
interval and pass the normal release, rectangle, image-index and marker checks.
Mono, third candidates, changed source dimensions/generations and expired
windows cannot force a mapping. OpenVR and D3D12 retain their same-frame policy.

OpenXR calibration records span one `xrEndFrame` to the next. `xrBeginFrame`
does not reset the source markers: hosts may render DLSS before calling it.
The first submitted frame supplies the eye rectangles and arms calibration;
subsequent samples retain the existing ten-frame cadence. Failed submissions
reject their pair and clear the learned rectangles before the next interval.

## Diagnostics

Support snapshots include D3D11 context/thread ownership, deferred query closes,
readback rejection reasons, and separate-device submissions. Calibration support
reports contain metadata only, without captured images.

OpenXR gaze projection uses the application's submitted FOV and eye orientation
relative to the corresponding located views, transformed between reference
spaces when needed. The last submitted projection is applied to fresh tracked
poses for up to 250 ms. Runtime-provided optics remain the fallback before a
valid submitted projection is available. Support snapshots identify the selected
projection and its FOV tangents.

Calibration tries one crop hypothesis at a time, stamping at most a nearby
outer/inner corner pair per source (40px markers with an 8px gap). It never
displays the full hypothesis bank. If small patches cannot locate the markers,
it acquires full submitted images asynchronously and searches their
codes across the submitted view. Each eye gets an independently estimated crop
and horizontal/vertical scale. Successful acquisition locks the selected marker;
subsequent samples stamp only the selected location(s) and read bounded tracking
patches. Each patch has 64 submitted pixels of movement allowance around the
predicted marker, clipped to its own eye viewport and capped at 256 by 256 pixels.
Local recognition fits translation and scale with the same code/contrast thresholds
and recenters the next patch from the new observation, independently for each eye.
Three consecutive complete samples with missing submitted markers, or changed
source/submission geometry, unlock placement and permit acquisition again. A
single failed sample retains the tracking layout but cannot refresh eye identity. Wide acquisitions are limited to one stereo pair in
flight, with at least one second between attempts; they stop while tracking is
locked. Image conversion is bounded to a 2048-pixel maximum dimension, and up to
two CPU workers search owned pixels without accessing graphics contexts.

Among authenticated markers, prefer the one nearest a submitted-image corner that
still leaves room for the complete tracking patch. This reduces visibility but
does not prove the marker is outside the headset's visible area: no hidden-area
mask is used. Acquisition still requires a surviving complete code. Severe
distortion, clipping, or very small markers can prevent recognition. The search
covers axis-aligned crop/resize and vertical flips, not arbitrary reprojection.
The learned crop locates tracking patches; existing camera/XR projection logic
continues to place gaze. After acquisition, a stereo source stamps only its one
selected marker. A shared mono source may need one marker
per eye. Reacquisition first tries the learned crop corner with 12 submitted-pixel
padding mapped back into the source (at least 12 source pixels), plus its nearby
inset fallback. This is computed from the crop boundary, not from the previous
inset marker, to avoid drifting inward after repeated losses. These stamps are
not erased from submitted frames. Keeping one marker at a usable crop corner
reduces their footprint but does not guarantee that the marker is outside the
headset lens view.

Open **Stereo and gaze > Eye calibration** in UEVR for the runtime, status and
session enable switch. Detailed calibration data is included in support ZIPs.
ReShade's **Diagnostics > Eye calibration** panel shows:

- Detected VR backend, graphics API, status and session enable switch.
- **Corrections applied:** changes to an existing eye assignment. Swapping the
  pair counts once; subsequent confirmations do not increment this counter.
- **Confirmed mapping updates:** all accepted results, including refreshes.
- Valid/completed samples, skipped/in-flight captures, CPU/GPU work and latency.
- Last recognized left/right DLSS views, retained as history when inactive.

Resetting counters preserves the mapping. CPU work measures the core capture and
polling calls, including warm-up allocation but excluding lock waiting and
surrounding hook dispatch/post-submit fence bookkeeping. GPU time covers the
instrumented source proof and submission copies, not the whole frame or repeated
D3D11 marker refreshes while awaiting both sources. GPU/latency averages use the latest
256 samples; CPU is cumulative since reset. Missing timestamps show unavailable.
Separate-device D3D11 samples do not publish a combined GPU timing measurement.

ReShade's support ZIP includes calibration in `diagnostics.txt`; UEVR includes
`eye_calibration` in `diagnostics.json`. Both report marker-based crop routing.
Additional allocation, mismatch and maximum-cost counters remain in reports.
Reports also include rejected-capture counts by check: capture/readback errors,
evaluation count, eye submissions, submission results, incomplete patches,
source marker verification, dimensions, and submitted marker recognition.
Checks can overlap on one capture. `last_rejection` contains the newest rejected
sequence, reasons, evaluation/submission counts and eight scores (source A
before/after, source B before/after, then A/B in each submitted eye).
`publication_rejected` counts valid captures that could not be applied, such as
stale/out-of-order results, changed handle lifetimes or disabled capture epochs.
Calibration does not automatically write images, ZIPs or logs.

## Markers and asynchronous readback

After a successful outer DLSS evaluation and final composition, candidates A/B
get distinct 40x40 light/dark patterns near their top-left/top-right corners.
Each pattern has 5x5 cells of 8x8 pixels. Both remain inset 12 pixels.
Native OpenXR D3D11 patterns vary per calibration epoch to reject old-session images.
The other paths use fixed A/B patterns.
Source before/after readbacks cover the full 40x40 stamp. Submitted readbacks
cover 60x60 source pixels, including a 10-pixel border around the expected stamp;
these bounds scale with the submitted image and its vertical-flip alternatives.

Recognition searches offsets of +/-8 source pixels in 2-pixel steps. It samples
four points near each cell center and uses brightness-normalized correlation,
requiring a score of 0.90, separation of 0.15 from the other candidate, at least
24 of 25 correctly ordered cells, and light/dark mean contrast of at least 0.04.
Mirrored templates handle reversed bounds; top/bottom marker locations still
resolve image orientation. Both eyes must identify different candidates.
Uniform, clipped and ambiguous patches remain rejected. Grading that destroys
the light/dark structure can still prevent recognition. Scores in diagnostic
exports now measure pattern correlation, not the previous color contrast.

Wide acquisition uses the same 0.90 correlation, 0.15 separation, 24/25 cell and
0.04 contrast requirements. It searches positions and scales, then refines each
axis separately. Conflicting source identities or orientations are rejected.
The existing source before/after proof, physical-eye pair checks, epoch,
generation and submission-result checks still apply. An acquisition older than
the one-second publication limit may seed tracking for up to ten seconds, but
must pass a fresh small-patch capture before it can publish an eye mapping.
This seeding allowance also applies when a small patch already matched in a
wide-search frame: waiting for the worker must not discard the successful seed.
Failed wide searches may advance to the next corner even after the one-second
tracking limit; otherwise a slow search can keep retrying markers clipped by a
changed resolution or crop. This only updates acquisition, never publishes eye
identity, and still rejects old epochs and out-of-order completions.
The support JSON's `placement_search` includes `wide_searches`,
`wide_search_pending`, `last_wide_results`, and per-eye learned placements.

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

Gaze mapping and calibration share the same projection selection. UEVR's
Virtual Desktop workaround may add an alpha-blended 4x4 dummy swapchain with
two side-by-side 2x4 eye regions. That specific placeholder is excluded,
regardless of layer order. Exactly one remaining stereo projection is required;
multiple game projections and dummy-only frames do not establish a mapping.

OpenXR D3D12 copies transition from and restore `RENDER_TARGET`; OpenVR D3D12
copies restore `PIXEL_SHADER_RESOURCE`, following their respective contracts:
[OpenXR release ownership](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrReleaseSwapchainImage.html),
[OpenXR D3D12 state](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_D3D12_enable-swapchain-image-state.html),
[OpenVR D3D12 submission](https://github.com/ValveSoftware/openvr/wiki/DirectX12).

The settings layer accepts only newer results for two still-live DLSS handle
generations. Incoming results older than one second are rejected, but an accepted
mapping remains authoritative through missing or invalid marker captures. It is
replaced by fresh valid evidence or cleared by view/session destruction, backend
or session changes, or disabling calibration. Disabling calibration invalidates
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
D3D11 WARP and hardware tests reproduce split render/submit threads, submissions
between the A/B renders, alternating single-eye renders, old-epoch image replay,
mono, failed releases, allocation reuse and draining after disable.
The AER fixtures also use two D3D11 devices with a persistent submission thread,
including immediate image reuse after release and old-epoch submitted-image rejection.

Gaze tests cover calibrated crop routing and foreign-session rejection. UEVR
tests cover host ABI, diagnostics, reports, interception and lifecycle. The Lua
menu tests preserve slider apply-on-release behavior in LuaJIT and Lua 5.4.

Automated tests do not establish compatibility or performance in every game. Validate each integration and graphics/runtime combination in actual game/headset sessions.

`CheekyTests --crop-calibration` also covers arbitrary asymmetric crops outside
the hypothesis bank on D3D11, D3D12 and mixed D3D12-to-D3D11 paths, tracking after
acquisition, three-miss loss/reacquisition, corner preference, and conflicting codes. The
Cyberpunk support-capture regression reconstructs sequence 42182's 6288x3568
source markers and 4693x3498 submissions with the crop inferred from the previews:
source B at (0,70), source A at (1595,70). It selects B's (4268,924) marker for
submitted eye 0 and A's (1980,924) marker for eye 1. These are reconstructed
full-resolution codes, not a lossless replay of the downsampled support images.

The Hogwarts 13844 regression reconstructs the 3440x1440 source / 1493x1440
submitted dimensions and location codes. It checks an intact outer corner,
a clipped outer corner with a nearby surviving inset marker, and a decodable
top-center distractor. Acquisition must select the corner, then stamp only one
tracking marker. Padding is checked against the learned crop boundary and must
not drift inward after repeated reacquisition. All GPU crop captures assert
that acquisition never stamps more than two markers per source.
