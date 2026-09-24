# Cropped stereo calibration

`CheekyTests.exe --retained-calibration` tests the optional change-only policy
with actual WARP rendering on DX11/OpenVR, DX11/OpenXR, DX12/OpenVR, DX12/OpenXR,
and DX12-to-DX11/OpenXR. It reads source pixels to verify all markers stop after
acquisition, waits beyond the crop timeout, and checks manual reacquisition,
same-view eye swaps, source dimensions, submission bounds, recreated/new views,
session changes, diagnostic resets, and resuming continuous validation.
Native DX12 manual reacquisition switches to alternating source-eye renders while
submitting both eyes each interval, covering recalibration after AFW starts.

`CheekyTests.exe --crop-calibration` exercises the production DX11, DX12 and
DX12-to-DX11 stamp/read paths on WARP. `--crop-calibration-dx12` isolates the
last two paths. The fixtures select full search and continuous validation
explicitly, and use source images large enough for the 16-marker locator grid.
They check centered and edge-aligned crops, resizing, vertical flips, packed
submission bounds, source origins, array slices, eye swaps, single-marker
tracking, manual recalibration and recovery after sustained marker loss.
These also run in the standard suite; CTest registers
`CheekyCropCalibrationTests` separately.

Full search decodes spatially unique grid locations to infer each eye's visible
source rectangle. Successful acquisition switches to a small corner marker,
padded 20 submitted pixels inside that rectangle. Verification retains the
cached crop rather than moving it with individual marker measurements. The
DX11 test checks that 19 missing-marker observations retain the lock and the
20th releases it, that stale acquisition cannot publish eye identity, and that
a fresh sample resumes single-marker tracking after recovery.

D3D12 fixtures also verify that ambiguous resampled grid evidence publishes no
eye mapping and that explicit recalibration replaces the epoch codes and recovers.

The fixtures wait for the submitted capture's completion, since the next frame
boundary may already have reserved an empty slot. They respect the full-search
throttle and verify all four support images and GPU readback retirement.
The standalone support tests also exercise the shared memory budget and command
recording lifetimes. GPU verification tests explicitly enable continuous
validation; retained-calibration tests select change-only mode separately.

`CheekyTests.exe --calibration-modes` covers automatic escalation, forced
methods, learned routes and change-only acquisition using actual GPU captures.
Support JSON records the inferred rectangles, marker locations, sampled
rectangles and rejection details. Synthetic crop/resize tests do not establish
compatibility with arbitrary shader warps or headset behavior.

# Automatic fixed-alignment history

`CheekyTests.exe --alignment-history` exercises the production crop coordinator
with OpenXR/OpenVR snapshots and camera projections. One-pixel alignment
oscillations must move the crop without resetting DLSS history, allowing the
backend's existing crop-motion correction to preserve accumulation. Mapping
changes or loss, large jumps and crop resizing must still reset history. The
snapshot cases use the BG3 report's 1464-to-2928 resolution and odd crop sizes.
These checks also run in the default suite. Marker-based gaze fixtures publish
verified source dimensions and crop geometry as well as eye identity. Mono
cases check crop offsets and scale, and reject missing geometry, an unverified
second eye, and stale source dimensions.

For an in-game retest, select Fixed with Automatic stereo alignment enabled,
move the headset, and inspect fine detail for shimmer. This change preserves
the existing alignment placement and adds no smoothing or deadband.

# SR resource safety and NR observer retries

`CheekyTests.exe --d3d12-safety` links the production SR backend and uses WARP.
It checks tagged input/output transitions and restoration on mip 0/slice 0,
unknown-state rejection, peripheral base replacement, 257 evaluations without
descriptor overwrite, bounded cache exhaustion, replay, reset while a GPU queue
is blocked, teardown during an active evaluation, resource retention behind
descriptors, and reuse after safe retirement. These tests also run in the default
`CheekyTests.exe` suite. Known-invalid baseline recordings are discarded before
submission; valid recordings execute on WARP.

`CheekyNrObserverTests.exe --probe` exercises the real observer with a device
facade that fails queue creation or command-list private-data storage. It checks
bounded retries, recovery after the one-second cooldown, and separate probe
results for device wrappers with different factory methods. The default observer
suite includes this regression plus its existing real-hook lifetime tests.

Before the fixes, the regression runs reported:

- SR dispatch used color/output in incompatible states.
- Evaluation 257 reused a descriptor table still owned by an unsubmitted list.
- The ninth distinct SR allocation evicted a resource in an executable list.
- 512 failed NR attempts created 512 queues.

The SR state test required adding state arguments to the backend interface before
the baseline run; they were initially ignored. All behavioral fixes followed the
failing runs. The tests reproduce the unsafe operations, not the tester's exact
Cyberpunk/NVIDIA crash. They enable the D3D12 debug layer when installed and
report when it is unavailable; the explicit state/lifetime assertions and WARP
execution still run without it. Real NVIDIA DLSS and game/mod compatibility need
an in-game retest.

# Standalone and OptiScaler

`CheekyStandaloneHostTests.exe <mode> <host>` uses the actual host DLL and GPU
swap chains with cached NGX/Streamline fixtures. Hosts are `standalone` and
`optiscaler`; modes are `dx11`, `dx11-c`, `dx12`, `dx12-c`, `streamline` and
`streamline-dx11`. Tests exercise private SR and DX12 NR, queue discovery,
presentation and both resize entry points without ReShade or UEVR.

`CheekyRuntimeHostTests.exe` checks the generic API and ownership. Options
`--dx11`, `--optiscaler`, `--transport` and `--openvr-late-022` (also 027/028/029)
cover each host, shared DX11/DX12 resources, full/foveated Before/After NR,
NR-only processing and a compositor cached before Cheeky loaded. The OpenVR
fixture must remain inactive until its runtime is initialized; Cheeky never
initializes VR on the game's behalf.

The transport fixture rejects private NGX initialization without an explicit
search path containing its DLSS library. This covers the nested standalone/ASI
layout that previously failed real NVIDIA feature creation with `0xBAD0000B`.
Initialization must use the NGX core while private SR feature calls use the
snippet, bypassing game core feature hooks. The fixture rejects direct snippet
initialization and core feature evaluation. `--transport-init-failure` also
checks that rejected initialization falls back without retrying every frame,
then recovers after the five-second retry cooldown.
`--transport-forwarded` exercises the core-to-public DLSS call chain as well:
private create/evaluate/release calls must bypass game-frame processing to
avoid recursively locking the backend during the first evaluation.
Transport tests also resize NR and SR independently across every ring slot,
check that allocation logs show reuse of unaffected textures, and exercise
resize while optional processing is disabled followed by re-enabling it.

`CheekyOverlayTests.exe [--dx11] [--hdr10|--scrgb]` renders the real F8 menu to
WARP textures and reads the result back. It checks SDR/HDR luminance, input
capture, resize, queue rejection and same-window swap-chain recreation. A
test-only focus adapter supplies foreground state for headless CI; production
builds use Windows foreground state. `--capture=<absolute.bmp>` saves an SDR
frame for visual inspection.

Bootstrap tests stage isolated layouts, validate DXGI exports and ordinals,
exercise recursive factory creation, concurrent ASI initialization and missing
host pass-through. Chaining fixtures exercise missing/unloadable second DLLs,
export fallback, and a second proxy that calls back into all three Cheeky
factory exports. The loop fixture bounds recursion so the old loader fails
without exhausting the test process stack. An optional fifth argument supplies
an external second DLL for a `proxy` smoke test in an isolated fixture folder.
Use `device` with that argument to also create a real D3D11 device. The regular
`device-chain` fixture exports an intentionally unusable `CompatValue` stub to
verify that private DXGI calls bypass the second DLL during device creation.
Core discovery fixtures verify a genuine-shaped NGX core
alias is intercepted while an OptiScaler-shaped proxy is excluded. The fake
DLLs are test fixtures only and must never be included in release packages.

# NR-only gaze positioning regression

For the feature-creation controls, preset cache, jitter/multiplier and SDR codec
regressions, see [the NR comparison and validation notes](../NR-COMPARISON.md).
They run through `CheekyUEVRTests.exe --late-dx12`, `--late-dx12-c`, and
`--late-streamline` as part of the normal build script.

`CheekyTests.exe` exercises NR-only placement with supplied OpenXR snapshots and
the mocked OpenVR reader: both eye assignments, moving gaze, independent NR size,
SR enable/disable transitions, full-frame SR size, and aligned/manual fallback.
Placement is resolved without evaluating SR, then consumed by NR region geometry.
For live verification, disable SR, enable foveated NR and its green alignment
border, and use Simulated gaze before testing Runtime gaze. Check that the NR
region moves and gaze diagnostics update; eye calibration must already be valid.

# UEVR inactive AFW regression

The standard build also replays cached DX11/DX12 native, `_C`, and Streamline exports
with `PDAFWPlugin.dll` loaded but Native Stereo selected. Run an individual case
with `CheekyUEVRTests.exe --late-dx11 --inactive-afw` (also `--late-dx11-c` and
`--late-streamline-dx11`, `--late-dx12`, `--late-dx12-c`, and `--late-streamline`).
These fixtures must retain private SR creation, reuse, release, and DX12 NR
processing even though AFW is detected. The AFW DLL is a test stub and never
performs frame warp. Dispatch tests also check mode changes, stale/unknown mode,
and public-to-core forwarding without weakening active AFW's private-call guard.
The DX12 fixtures switch through AFW, unknown, and stale host modes, then require
center/peripheral SR and NR to reset their skipped histories once on recovery.

# UEVR cached OpenVR compositor regression

Run `CheekyUEVRTests.exe --openvr-late-027` after building. The same test supports
`022`, `028`, and `029`. A fixture DLL supplies an OpenVR compositor that the
host obtains before loading the actual Cheeky plugin/runtime. The host never
requests the interface again. The test checks that an inactive OpenVR session
is untouched, then activates the host and verifies cached `WaitGetPoses` calls
reach calibration exactly once, including after repeated presents/device reset.
The original implementation fails at the late-attachment assertion.

This is an offline hook/ABI regression, not a headset or gaze accuracy test.
For live verification, restart the game with both updated UEVR DLLs. The log
should report `OpenVR cached host compositor attached interface=...`, and the
calibration backend should become OpenVR with increasing frame counts. Then
verify simulated gaze and runtime gaze separately.

# D3D12 SR array-output regression

After `scripts/build.ps1 -Configuration Release`, run:

```powershell
.\bin\Release\CheekyTests.exe --d3d12-composite
```

This executes the production SR compositor shader on the Windows WARP D3D12
device, without a game or NVIDIA runtime. It verifies the center uses the DLSS
texture, the periphery uses the input texture, output origins are respected,
and every other array slice and mip remains unchanged. Cases cover ordinary
2D textures and two-/four-slice mipmapped textures, including R11G11B10_FLOAT.
When installed, the D3D12 debug layer is enabled and errors fail the test.
The default CheekyTests run also checks the logged MSFS 3024x2836, 12-mip
resource description and the single-slice, single-mip private output contract.

This supports mip zero / slice zero of D3D NGX and Streamline resources. It
does not infer an eye's array slice from its viewport ID or add a nonzero-slice
API. The D3D resource contract currently consumed by the add-on exposes a
native resource and XY extent, not a slice selector. Additional slices in
the allocation remain untouched. This does not enable array outputs for NR.

For MSFS 2024 validation, restart the game with the rebuilt add-on and enter
the same VR mode. With NR off, enable fixed SR foveation and its red border.
Check both eyes, vary width/height, and verify the foveated GPU timing becomes
populated. The log reports `D3D12 SR array output accepted` when allocating
scratch resources for an array output. Repeat a flat-to-VR transition.
Compare with peripheral DLAA off first to isolate the separate PR #4
synchronization rejection. WARP tests do not validate NVIDIA evaluation or
the game's presentation path; those still require this in-game check.

The SR Streamline route also uses a private viewport (host ID | 0x20000000),
separate from the peripheral namespace. Some runtimes reject a second constants
submission for the same frame/viewport. Cropped options, tags, constants and
the evaluation input must all select that private viewport; host constants stay
untouched. Unit coverage exercises host IDs 0 and 32 from the MSFS trace,
preservation of unrelated inputs, and rejection of ambiguous/stale viewport
inputs. This models write-once constants; it does not execute Streamline itself.
In a fresh game log, verify `motion constants applied`, `prepare complete`,
and `original begin foveated=yes`. A failure now logs the constants result code.
## Stereo images in support reports

`CheekyTests.exe --mixed-calibration` reproduces a DX12 game whose host submits
DX11 textures to OpenXR. It uses WARP and the real layer bridge, with test-only
CPU transport modeling the host's resize, vertical flip and crop. Source and
submission rendering run on different threads. Tests cover wrapped device
identity, alternating eye rendering, projection-eye swaps, stale marker epochs,
cropped-away markers, bounded/stable allocations, refresh-command lifetime, and
all four support images. Examples are in `%TEMP%/Cheeky-mixed-calibration-tests/<pid>`.
Delayed variants submit images rendered three intervals earlier, including
unsampled renders, and assert continuous DX12 stamping with one-in-ten readbacks.
Marker-only tests check that no readback/timestamp objects are allocated, uploads
survive replay and independent queue fences, and a saturated 16-slot pool skips
without growing or blocking and resumes after retirement. Diagnostics expose
`d3d12.continuous_stamps` and `d3d12.continuous_skipped` separately from captures.
Mono variants start with one registered source, verify its marker in both XR
eyes, then switch mono-to-stereo and back to the other source while both handles
remain registered. Missing-eye evidence and recreated handles must not inherit
a shared mapping. Verified mono reports contain three images, with the unused
source marked `not_applicable_single_source`. `--alignment-history` also checks
real/simulated mono gaze, binocular forward alignment, vertical flips, next-jump
previews, and rejection of a different session's calibration.
The production path only reads each API's own textures; it adds no interop
transport and performs no GPU waits. Mixed API GPU timing is reported unavailable
because the query timelines are separate. Original same-API tests remain in
`--stereo-support` and the default test run; headset validation is still required.

`CheekyTests.exe --stereo-support` checks DX11 and DX12 WARP captures at the
production marker stamp/read points, including failed recognition, array slices,
packed UV bounds and shader-resized/flipped HDR submissions. It verifies preview
size limits with noisy pixels, missing-frame timeouts, and GPU resource retention
when a report times out while command lists remain replayable or in flight.
Small example archives are written to `%TEMP%/Cheeky-stereo-support-tests/<pid>`.

Creating a support ZIP requests one calibration sample while VR and stereo
calibration are active. Continue rendering gameplay for up to five seconds.
The ZIP contains up to four images: `stereo-source-A.bmp`, `stereo-source-B.bmp`
immediately after stamping, and `stereo-submitted-0.bmp`, `stereo-submitted-1.bmp`
at readback. Source A/B are candidates, not confirmed physical eyes. The physical
submitted eye (0 left, 1 right), original texture resolution, array slice, DLSS
view rectangle, marker rectangle, submitted UV bounds and sampled rectangles
are recorded in `stereo-capture.json` and the main report diagnostics.

Previews preserve the full texture/aspect ratio, use nearest-neighbor resizing
and clamp RGB to [0,1]; they are diagnostic images, not color-accurate HDR captures.
Each is a 24-bit BMP below 1,000,000 bytes even without ZIP compression, with at
most 300,000 pixels and a 1024-pixel longest side. There is no continuous image
capture. Missing or failed readbacks leave an explicit status in the JSON and do
not prevent the rest of the ZIP. Readback memory is capped at 128 MiB per image
and 512 MiB across outstanding requests; larger textures report the limit.

# D3D12 gaze copy mapping

The normal test executable checks copy-chain translation, subresource isolation,
ambiguous destinations, destruction, expiry, reversed order, and rejection of
scaled copies. These tests validate the matching policy, not MSFS's rendering path.

For an in-game check, select simulated gaze in normal VR gameplay. Both eye
mapping rows should become stable with the `copy` route, and the alignment borders
should move. The submitted-texture-copy counter confirms observation of eligible
D3D12 copies/resolves; a rising count alone does not prove an eye route exists.
This implementation follows up to four equal-size mip-zero/slice-zero copies,
keeps at most 512 edges for 500 ms, and rejects routes reaching both eyes.
Shader blits, scaled copies and other array slices are not handled by this route.

## Streamline camera projection mapping

ABI 4 includes OpenXR eye fields of view and head-forward centers; update both the add-on and the layer DLL.
The fallback compares the unjittered Streamline perspective projection with both
XR eye frusta. It requires a unique match, matching full-eye output dimensions,
and two distinct snapshot display times. Symmetric or mismatched projections,
missing data, non-perspective matrices and stale/wrong-frame constants cannot
activate this route. Existing resource routes still take precedence.

Constants are cached per viewport and actual frame index (token pointers are reused),
with a 100 ms freshness limit, and exposed only during the corresponding SR prepare.
Matrix and frame-token ABI follow NVIDIA's
[Streamline guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md)
and [public frame token definition](https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_core_types.h).
Tests cover handedness, reversed depth, distinct and ambiguous eyes, invalid matrices,
cache isolation/expiry, scoped context restoration, and rejection of the old snapshot ABI.

In MSFS normal VR gameplay, select simulated gaze and show the alignment borders.
Expected: `Snapshot ABI: Yes`, both eye mappings stable with `projection`, and moving
borders. If mapping remains waiting, capture 15 seconds of gameplay: bounded
`Gaze projection` lines show the Streamline and XR tangent comparisons. These tests
validate policy and ABI; they do not establish MSFS compatibility without a game test.

## Automatic alignment

The default test run covers asymmetric projection placement without the layer,
right-first evaluation, independence from manual inversion, center preservation
when resizing, edge clamping, history reset on crop changes and fallback, missing
or wrong-view camera data, canted-eye forward projection, quaternion hemisphere
handling, and rejection of ABI 3 buffers by the real layer DLL.

In game, leave **Automatic stereo alignment** enabled and enable the red border.
With the updated layer, confirm the latest-view status reports OpenXR head-forward
and inspect both eyes. Repeat with different crop sizes and after leaving and
re-entering VR. On a Streamline route, verify projection alignment without the
layer. Unsupported paths should report manual fallback and retain their saved
offsets. No eye tracker is needed. Hardware validation remains required; these
tests do not execute a game's rendering or headset presentation.

The coordinator regression also replays the reported 3024x2836 split-swapchain
packed layout with unrelated DLSS resources and no camera projection. It verifies
stable mapping and automatic centers in Fixed, automatic fallback in OpenXR gaze,
valid real/simulated gaze without double offsets, manual override, and invalid
layout rejection. This is a synthetic snapshot replay, not headset validation.
# OpenVR adapter

The normal test executable checks known compositor slot layouts, rejects unknown
versions and invalid/fractional/flipped submission bounds, validates NDC-to-UV
conversion, asymmetric frusta and canted-eye projection, and replays the packed
coordinator scenario through both OpenXR and OpenVR snapshot acquisition paths.
Live runtime discovery is replaced with a snapshot fixture in these unit tests.

`tools/steamvr-mock/adapter_probe.cpp` builds the actual adapter into a separate
background application. On SteamVR 2.16.7 it successfully armed interfaces
022/027/028/029, observed the legacy 022 WaitGetPoses call, and invalidated its
snapshot on runtime shutdown. Result 103 from WaitGetPoses is expected for that
background probe without scene focus. This does not validate scene submissions.

In ACC, test Runtime gaze with driver mock off, center, sweep, dropout, then off.
Check both eye mappings, crop placement, loss/recovery, and automatic alignment.
Also test the internal Simulated gaze mode with the driver off. Re-enter VR and
change render resolution to check mapping rebuilds. Missing mappings must keep
fixed fallback. Keep the previous add-on available for rollback.

## Center supersampling

The default tests check reconstruction dimensions, unchanged input/eye placement,
settings round trips, invalid scales, texture limits, and both motion-vector
resolution paths. `--d3d12-composite` executes both production compositor
shaders on WARP (DX11 shader model 5.0 with 2D views, DX12 with array views).
Checkerboard readback is compared against a double-precision area reference at
1x, 1.25x, 1.5x, 2x, and unequal fractional dimensions. A bilinear reference
checks the internal shader's 0.5x and 0.75x handling, including clamped texture
edges; these scales are not exposed by the 1-2x supersampling slider. HDR constant-color cases
check brightness preservation; existing origin, slice and mip isolation checks
remain active. This exercises shader code, not the DX11 runtime or NVIDIA DLSS.

In games with input- and output-resolution motion vectors, compare 1x/1.25x/1.5x/2x with
fixed and moving gaze, both eyes, and peripheral DLAA on/off. Confirm unchanged
placement, history reset on scale changes, and quality/timing while moving.
Repeat DX11 Direct, DX11 Transport and DX12/Streamline where available. Live NVIDIA evaluation and
visual quality require hardware validation.

Run `bin/Release/CheekyTests.exe --motion-resample` for WARP tests of the actual
DX11 and DX12 motion passes. They exercise 0.5x through 2x grids, fractional
axis ratios, nonzero packed crop origins, gaze offsets, discontinuous velocities,
NaN/Inf and large invalid markers, and slice-zero array SRVs. Output-space displacement
scales with the output grid, while invalid markers stay unchanged. DX11 source readback
checks that both array slices remain untouched. The DX12 test exercises source
state restoration and fence-tracked pass lifetime. Native NGX crop correction tests use
the pixel space selected by the motion-vector flag. Streamline's normalized
scale compensates for the vector resampling ratio.

ACC DX11 reported heavy head-motion blur away from 1x with unscaled output vectors.
The correction scales displacement with the resized output grid and restores
output-space gaze offsets. GPU readback verifies this numerical behavior; an
ACC moving-head A/B comparison against 1x is still required.
## NR rendering order

Run `bin/Release/CheekyTests.exe --nr-processing` to run the NR contracts independently.

The standard suite tests processing-resolution selection, Working scale, crop
centers, independent resource origins, render/output-resolution motion fields,
per-eye history changes, and restoration of NGX parameters and Streamline tags
on success and failure. NVIDIA evaluation is stubbed in the test executable.
Both CMake and MSBuild register the new sources.

`CheekyUEVRTests.exe --late-dx12`, `--late-dx12-c`, and `--late-streamline`
also observe Color and Reset inside fake SR evaluation through the real hooks.
An isolated copy of the fake NGX DLL supplies feature 18, with complete current
Streamline viewport tags/constants. Deterministic vertical region jumps check
After NR with SR foveation disabled, then peripheral-before-center SR evaluation:
the input scope preserves the host reset and does not consume the center reset.
Coverage includes successful Before substitution, per-eye/viewport transitions,
repeated After/disabled frames, failed NR or tag submission, incomplete/ambiguous
viewport metadata, another viewport cached last, and parameter/tag restoration.
Native private-SR failure also checks fallback with the prepared input intact.
These fixtures do not launch a game or run NVIDIA's implementation.

`CheekyTests.exe --d3d12-composite` also exercises the production private-color
copy on WARP. A nonzero-origin render region in a mipmapped source is copied,
then the private color is changed and passed through the production compositor.
Readback checks the original source and every unrelated mip/slice, plus both
successful private-input propagation and failed-NR fallback. This does not
execute the full game/Streamline/transport hooks or NVIDIA feature 18.

NVIDIA acceptance is still pending for native DX12, Streamline, and DX11
Transport. On each route compare the same scene/settings in both orders across
NR full/foveated, linked/independent shapes, SR foveation on/off, peripheral DLAA
on/off, live switches, resize/dynamic resolution, stereo, and moving gaze.
Record the displayed processing/working dimensions, NR and total pipeline GPU
milliseconds, and image-quality observations. Test missing NR runtime and
unsupported resources too: SR must use original color without post-SR NR.

## NR GPU lifetime validation

For an opt-in experiment with the real NVIDIA model, build
`tools/nr_model_probe.cpp` in an x64 Visual Studio Developer PowerShell:

```powershell
New-Item -ItemType Directory -Force build/nr-model-probe | Out-Null
cl /nologo /std:c++20 /EHsc /MT /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tools/nr_model_probe.cpp /Fo:build/nr-model-probe/ /Fe:build/nr-model-probe/nvngx.dll-probe.exe /link d3d12.lib
# Supply the actual driver _nvngx.dll and game nvngx_dlssnr.dll paths:
./build/nr-model-probe/nvngx.dll-probe.exe 'C:/path/to/_nvngx.dll' 'C:/path/to/nvngx_dlssnr.dll'
```

The executable name is required by the model's NGX caller-module lookup.
This uses private D3D12 resources and generated pixels, without attaching to a
game. It needs compatible NVIDIA hardware, driver and NR DLL. It prints numeric
differences rather than imposing version-specific visual assertions. See
[the audit](../NR-COMPARISON.md) for measured results and their limits.
`tools/nr_parameter_probe.cpp` is a smaller driver-only ABI probe; compile with
the same flags and `d3d12.lib`, and pass only the `_nvngx.dll` path.

`CheekyTests.exe --nr-lifetime` and `CheekyNrObserverTests.exe` run the same
production NR lifetime assertions on WARP. Windows Graphics Tools is optional:
the fixture prints whether D3D12 debug validation is enabled or unavailable.
When enabled, failure to acquire the info queue and reported errors or corruption
still fail the test. When unavailable, only debug-message inspection is skipped.
All lifetime assertions remain active, including 1000 two-view completed-fence
collection cycles, replay, multiple queues, Reset, aliases, destruction, and
failed signaling. The observer executable uses the real native hooks and also
runs the compositor tests.

The test-only environment variable `CHEEKY_NR_TEST_NO_DEBUG_LAYER=1` forces the
lifetime fixture through the unavailable-layer path without uninstalling
Graphics Tools. The compositor fixture also honors this override so the native
observer executable keeps one debug mode across its lifetime and copy tests.
CTest registers normal and forced-fallback runs of both
executables, each in a fresh process so debug-layer state cannot carry over.
After building with CMake, run just the fallback cases (replace `build/cmake`
with your CMake build directory):

```powershell
ctest --test-dir build/cmake -C Release -R 'CheekyNr.*NoDebugLayer' --output-on-failure -V
```

Run all four lifetime configurations with `-R 'CheekyNr'`, or omit `-R` for the
full suite. Each lifetime run must print the same assertion-success summary.

## Vulkan source eye calibration

`CheekyTests.exe --vulkan` also exercises Vulkan marker writes and source proof
readback on the GPU, then transfers the captured pixels to D3D11 OpenXR
submission textures. It covers RGBA8/16F/32F outputs, swapped separate eyes,
shared mono output, cropped locator-grid acquisition, retained unstamped
frames, missing submitted markers, and reset without submission. The test
uses CPU transfer to emulate the host bridge; actual R.E.A.L. VR interop and
headset alignment still require an in-game check.
