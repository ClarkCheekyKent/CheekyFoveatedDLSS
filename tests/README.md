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
