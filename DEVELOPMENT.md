# Building and testing

Requires Visual Studio 2022 or newer with Desktop development with C++ and a Windows 10/11 SDK. Dependencies are vendored under [third_party](third_party/README.md).

## Optional gaze compatibility

The Stereo/gaze controls in standalone, UEVR and ReShade expose two independent,
per-application options, both disabled by default:

- `ManualStereoMapping`: selected by Stereo Eye Mapping > Mapping method > Manual;
  suspends marker calibration in the runtime while selected and permits the full-eye layout fallback when automatic
  routes cannot map distinct eye resources. `ManualStereoRightFirst` selects
  right-first instead of left-first DLSS evaluation order. This assumes matching
  source/XR coordinates, with no crop, flip or projection change; dimensions alone
  do not verify the assumption. Check the red alignment border in both eyes,
  including the edges. Changes apply immediately. Foreign-session calibration
  cannot supply an eye role, and session/backend changes reacquire mapping before
  using gaze. Diagnostics call this manual stereo layout.
- `IndependentOpenXRGaze`: permits gaze-only action attachment after 90 successful
  frames when the host has not created, attached or synchronized action sets.
  **Restart the application after changing this option.** The first setting sent
  to the OpenXR layer is latched for its lifetime. The delay is only a grace period:
  attachment is irreversible and a host initializing input later cannot attach.
  Existing RealVR/IL-2 compatibility paths remain independent of this option.

Post-attachment action-space creation is enabled generally for runtime
compatibility. Run `CheekyTests --manual-gaze`, `--openxr-independent-on`,
`--openxr-independent-off`, and `--openxr-input` for focused coverage. Each
independent-input test runs in its own process to exercise the restart latch.

## Build

### Experimental MSFS 2024 quad-view startup

For `FlightSimulator2024.exe`, an OpenXR 1.1 request rejected with
`XR_ERROR_API_VERSION_UNSUPPORTED` is retried inside the layer using a 1.0
downstream instance. The application's original version and the layer's quad
provider remain active. Native 1.1 runtimes, existing 1.0 applications, other
executables and requests for future API versions retain their normal behavior.
The bridge supplies core `xrLocateSpaces` through `xrLocateSpace`, including
optional velocities, and enables available local-floor/palm-pose extensions.
This is a targeted capture compatibility path, not general OpenXR 1.1 runtime
support. Restart MSFS after replacing the layer DLL.

The startup log records `QUAD-CAPTURE version_bridge` and the retry result.
Successful version negotiation does not force the game to select quad views:
check the logged offered configurations, selected application configuration and
submitted view counts separately. `CheekyOpenXRBindingTests --quad-version`
checks the ordinary-host path; run a copy of that test executable named
`FlightSimulator2024.exe --quad-version` to exercise the MSFS retry. The
`--quad-capture` test validates four-view poses and GPU composition on WARP.

For MSFS and DCS capture instances, quad views are listed first as the preferred
configuration; stereo remains available. Other hosts retain the downstream
preference order. Negotiation logs include count-only queries, short-buffer and
downstream failures, view descriptions for both configurations, function
resolution and the application's session request. The layer does not force a
stereo session to become a quad session.

Build `tools/openxr-capability-probe.vcxproj` and run the resulting
`bin/Release/quad-capability-probe/FlightSimulator2024.exe` with the absolute
path of MSFS's `openxr_loader.dll` as its argument. This is a read-only probe,
named to exercise the MSFS compatibility path; it does not start the game or
create a graphics session. It checks global and layer-specific extension
visibility through the real loader, then queries both view configurations with
the headset connected. `--extensions-only` skips instance/system queries. Its
printed log path identifies the corresponding layer trace. Pre-instance
extension enumeration is performed by the loader using the registered layer
manifest, so this separate check complements the layer's application-call logs.

### Experimental DCS quad-view composition

`DCS.exe` requests `XR_VARJO_quad_views` on OpenXR 1.0 and uses D3D11. The layer
now prefers quad views for that executable, accepts its D3D11 quad session and
begins stereo downstream. It samples four application views and blends each
focus view over its peripheral view into two private runtime swapchains. Array
slices, nonzero image rectangles, typed SRGB views over typeless textures, and
other application layers are retained. Color swapchains created before
`xrBeginSession` are also made sampleable for the targeted quad hosts.

D3D11 composition uses a private deferred context, executes with application
state restoration and flushes before releasing the output images. Pending image
waits can be retried without acquiring the same image twice. DCS focus FOVs are
corrected from the matching `xrLocateViews` display time; without a matching
sample the fixed inset is reconstructed from the outer FOV. This follows the
DCS compatibility issue documented in the
[Quad-Views-Foveated implementation](https://github.com/mbucchia/Quad-Views-Foveated/blob/main/openxr-api-layer/layer.cpp).
It does not add eye tracking or the planned per-view SR/NR/FG policy.

`CheekyOpenXRBindingTests --quad-d3d11` exercises the production interceptors
with WARP and a stereo-only mock runtime, including output pixel readback, context
state, pre-begin image creation, SRGB, FOV correction and wait recovery. Also run
`--quad-capture` for D3D12 regression coverage. A copy named `DCS.exe` running
`--quad-version` checks executable-scoped quad-first preference without enabling
the MSFS API-version bridge. Build and install packages separately before a live
DCS retest; successful mock composition does not prove DCS selected quad views.
Look for `prefer_quad=1`, `app_configuration=1000037000`,
`compositor graphics_api=11`, `submit input_views=4` and `submit output_views=2`.

UEVR deferred-loading coverage: `CheekyUEVRTests --loader-lifecycle` and
`--loader-missing` exercise dormancy, event isolation, deferred transitions,
disable/re-enable, concurrent requests, missing exports and package repair.
`lua tests/uevr_loader_tests.lua` exercises the actual Lua panel with a mocked
UEVR API, including malformed profiles, persistence errors, startup ordering
and disabled-state command suppression. CMake registers this test when Lua is
available, or when `CHEEKY_LUA_EXECUTABLE` names a Lua 5.4+ interpreter.

From PowerShell at the repository root:

```powershell
./scripts/build.ps1 -Configuration Release
```

Builds ReShade, UEVR, standalone, OptiScaler, the OpenXR layer and tests, then
runs the native test suites. Outputs go to `bin/Release`; the standalone proxy
is in `standalone-loader/dxgi.dll` so ordinary test executables do not load it
implicitly. Use `-Configuration Debug` for a debug build. The solution also
supports Visual Studio and CMake 3.24 or newer. The standalone proxy requires
the x64 MASM tools included with the Visual C++ workload.

Use `./scripts/build.ps1 -Configuration Release -SkipTests` to build without
running the tests (test executables are still compiled).

Direct3D shaders are checked in as bytecode in `src/d3d_shaders.hpp`, so normal
builds do not require `fxc.exe` or compile shaders when a game runs. After
editing their HLSL sources, regenerate the header with the Windows SDK compiler:

```powershell
python scripts/build-d3d-shaders.py --fxc 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'
```

Add `--check` to verify that the checked-in bytecode matches its sources.

## Tests

The native suites exercise GPU processing, eye calibration, the OpenXR layer, UEVR host lifecycle and late NGX/Streamline attachment using local fixtures. They do not launch games or evaluate NVIDIA DLSS.

Additional checks:

```powershell
./bin/Release/CheekyUEVRTests.exe --hardware
./bin/Release/CheekyTests.exe --d3d12-composite
./bin/Release/CheekyTests.exe --motion-resample
python tests/uevr_menu_tests.py
```

Lua tests require `lupa==2.8`, installed in the Python environment or `build/test-python`. They run the menu with mocked bindings and check apply-on-release behavior. [Compositor test details](tests/README.md)

GPU and host tests complement game/headset testing; they do not establish compatibility or performance in every game.

`CheekyTests.exe --openxr-input` also launches isolated copies of the test host
named `IL2Series.exe`, a mixed-case equivalent, and a nonmatching name against
the actual OpenXR layer DLL. The IL-2 cases cover no-host-input gaze bootstrap,
once-only attachment, per-display-time sync, focus/session recovery, setup and
runtime failures, truthful RealVR diagnostics, and host input ownership. Existing
ordinary-host and RealVR cases remain in the same suite. These fixtures do not
launch IL-2 or validate a headset/runtime combination. In-game validation is
still needed with IL-2, VDXR and Galaxy XR, including focus loss/recovery and
VR/session restart. The startup log records IL-2 compatibility admission; input
diagnostics record the actual fallback attach/sync calls and results.

`CheekyTests --libovr-policy` checks mixed OpenVR/LibOVR gaze selection,
invalid PVR samples and calibration ownership across OpenVR, OpenXR and LibOVR.
`CheekyTests --libovr-transfer` checks retained calibration with both D3D11
and D3D12 sources feeding LibOVR D3D11 submissions. These checks do not add
native LibOVR D3D12 swap-chain calibration. LibOVR Vulkan work is deferred:
no native LibOVR Vulkan implementation or validation is included. The standard
suite still exercises the existing Vulkan paths.

`CheekyTests --calibration-search` reproduces a partial-code match that falsely
resembled a flipped grid marker during resize reacquisition. Refined grid hits
must match the locator's white/black rings; genuine conflicting identities and
orientations still reject acquisition. The seven retained-calibration paths,
including OpenXR and LibOVR D3D12-to-D3D11 transfers, pass this regression.
Acquisition failures print the API/backend, geometry and calibration diagnostics;
do not assume the 10-second deadline means GPU load.

UEVR default and `--dx11` tests distinguish the stored full-search retention
preference from Auto corner calibration's effective continuous validation.
Core alias discovery fixtures explicitly select the higher D3D12 hook because
their synthetic core has no lower SR runtime, and wait for the complete D3D12
callback set before invoking cached exports.

`CheekyRuntimeHostTests --libovr` (also `--libovr-legacy`, `--libovr-late`,
`--libovr-borrowed` and `--libovr-copied-table`) loads synthetic
`LibOVRRT64_1.dll` and `LibPVRClient64.dll` fixtures before the runtime, as a
LibOVR game on Pimax would. They cover export discovery, both layer header
layouts, a missed `ovr_Initialize`, swap-chain reference ownership, PVR session
capture through the interface table or its implementations, gaze sampling and
LibOVR-driven calibration frames. They do not validate Pimax hardware, Pimax
Play's compatibility runtime or R.E.A.L. VR. `CheekyTests --libovr-aer-calibration`
commits one eye per LibOVR frame, as R.E.A.L. VR's legacy AER does, and checks
that calibration pairs the commits across frames.

The `--libovr` runtime-host case also exercises real NGX and LibOVR hooks with
three rotating swap-chain images: alternate-eye acquisition, switching to
ordinary stereo, failed commits/submissions, and recreation during an open pair.
It runs for standalone and OptiScaler. These are WARP/fixture checks; BG3 on a
Pimax headset still needs validation with both AER modes and menu transitions.

DX11 late-attachment tests cover stale output dimensions after feature creation,
with and without peripheral DLAA, plus parameter restoration after native
success and failure. Late-adopted features keep their input and output dimensions
until their creation contract is observed. The transport fixtures read back the
entire output after a stale query, including with peripheral DLAA enabled.
Native LibOVR D3D12 swap chains and LibOVR Vulkan remain outside these changes.

## Packaging

```powershell
./scripts/build-installer.ps1
./scripts/package-uevr.ps1 -Configuration Release
./scripts/package-standalone.ps1 -Configuration Release -Mode Both -Label local
```

Packages include `OpenXR/CheekyOpenXRSetup.exe` by default; build the matching
installer before packaging. Standalone and OptiScaler packages also include
the Vulkan layer and manifest by default. Release ZIPs contain runtime files,
the installer and licenses, with no READMEs, diagnostic guides, build metadata
or checksum files. Packaging still verifies the archived payload hashes.

The OpenXR installer requires Inno Setup 6.3 or newer. Pass `-IsccPath` if its compiler is outside the standard locations. To package existing Release binaries, use `-SkipBuild`; for CMake output also supply `-ArtifactsDirectory`.

The installer output is `bin/installer/CheekyOpenXRSetup.exe`. Publish it alongside the ReShade add-on and UEVR package. The installer contains only the shared layer, manifest and licenses. Keep its AppId and install path stable so upgrades replace the existing installation. Verify install, upgrade and uninstall before publishing.

Set the release version in `shared/version.h`; the UI, CMake and installer share it. NVIDIA runtimes and test fixtures are not included in packages.

## Source layout

- `src/`: shared settings, interception, rendering, calibration and resource observation; `addon.cpp` supplies the ReShade UI and host integration.
- `uevr/plugin.cpp`: UEVR adapter; `uevr/runtime.cpp`: resident runtime, settings and reporting; `uevr/scripts/`: Lua controls.
- `shared/runtime_host_api.hpp`: versioned host-neutral resident runtime ABI; legacy UEVR exports remain compatible.
- `standalone/`: graphics observation and F8 ImGui controls shared by standalone and OptiScaler.
- `bootstrap/`: DXGI export forwarding and OptiScaler `InitializeASI` loaders, with isolated loader fixtures.
- `openxr_layer/`: shared OpenXR layer, installed separately for either integration.
- `tests/`: native GPU/host fixtures and Lua menu tests.

The shared runtime remains resident because hooks and GPU work can outlive
adapter detach. The standalone host and loaders also remain resident. See
[eye calibration](EYE-CALIBRATION.md) for calibration ownership and lifetime rules.

The standalone host matrix reuses cached NGX/Streamline fixtures for both hosts,
including native and `_C` exports on D3D11/D3D12, NR histories, actual queue
submission, and resize. Generic runtime tests also cover D3D11-to-D3D12 SR/NR
transport, ownership conflicts, settings atomicity and support ZIPs. The overlay
suite reads GPU pixels for SDR, scRGB and HDR10 on both APIs. Its test-only focus
adapter substitutes desktop foreground state in headless CI; real game input
and multi-overlay interaction still require manual testing.

DX11 OTA regression coverage includes cached SR runtimes loaded after the game's
DLL, separate callback ownership across multiple runtimes, native and `_C`
evaluation exports, and nested forwarding without duplicate processing. Run
`CheekyCoreDiscoveryTests dx11-ota <runtime.dll> <fake-ngx.dll> unused` for discovery
and `CheekyUEVRTests --late-dx11 --ota-runtime` (also `--late-dx11-c`) for GPU-backed
private-feature reuse and release/recreation. These use synthetic runtimes;
BG3 DX11 with Luke Ross and NVIDIA App override was also verified in-game: cached SR transport active, with 3,422 NR evaluations and zero NR failures.

`CheekyRuntimeHostTests --transport-ota` checks late cached-runtime selection
for private DX12 SR/NR while the named DLL rejects DX12 evaluations.
`--transport-ota-only` covers a cached snippet without a loaded named SR DLL,
using explicit game feature paths. The General-tab status distinguishes cache
discovery from observed evaluation through the cached runtime.

Cached-runtime regression coverage also includes the `dx12-ota` and `vulkan-ota`
modes of `CheekyCoreDiscoveryTests` with the same runtime/fixture arguments. These test
late-loaded caches alongside the named DLL, module-owned callbacks, and observed
source status. DX12 GPU/Streamline coverage uses `--late-dx12`, `--late-dx12-c`,
`--late-streamline`, and `--late-streamline-c` with `--ota-runtime`. AFW/RealVR
`ota-ambiguous` now verifies independent routing with both SR modules present;
it no longer expects concurrent modules to disable the lower hook. Vulkan cache
routing uses synthetic exports; real NVIDIA override coverage still needs a
Vulkan game check.

## Per-view activity diagnostics

The Diagnostics tab's **DLSS view activity** group distinguishes registered
handles from observed evaluation calls. Receiving calls means a hook observed an
evaluation within 2000 ms; idle handles retain their lifetime count. Each handle
includes its registration generation, API, last-call age, input/output dimensions,
last recorded crop and calibrated/unverified/unassigned eye mapping. Focus versus
periphery remains unknown until explicitly mapped. These counters do not count
successful GPU frames or OpenXR projection views.

Support snapshots retain `gaze.views` as the legacy registered-handle count and
add `registered_views`, `receiving_views`, `evaluated_views`, and
`never_evaluated_views`. `view_details[].evaluation_calls` counts observed calls;
the older `evaluations` field counts geometry observations. Released handles are
removed; reusing an address starts a new generation. ReShade's text support report
also includes per-handle activity. `CheekyTests --view-activity` verifies unused,
passthrough, idle and reused handles; overlay tests verify the visible breakdown.

## Quad view assignment

In a running D3D11 quad session, **Stereo / Gaze** shows four-view assignment
controls instead of two-eye calibration. Automatic assignment tags successful
game DLSS outputs with distinct small patterns and checks the four raw OpenXR
views before composition. Two matching submissions establish the association;
handle order and equal dimensions are insufficient. Acquisition stops after
120 captures if unresolved and can be retried. This path currently targets DCS
D3D11; D3D12 quad composition does not yet provide this assignment interface.

**Quad assignment mode: Manual** exposes a role dropdown for each numbered
source: Periphery Left, Periphery Right, Center Left, or Center Right. Each role
can be assigned once; all four must be assigned before the mapping is used.
**Show view numbers** is an independent checkbox in either mode. Small numbers
repeat in a 7 by 5 grid to remain visible outside the inset.
Only evaluated game handles receive numbers, so unused feature registrations
do not consume labels. Manual roles save by view number in the game's settings,
along with the evaluated source count. On the next session they restore once
that count matches; handle addresses are never persisted. This is a manual
declaration of stable game ordering, not automatic verification. Check labels
after changing the game's rendering setup. Reset quad assignments also clears
the saved layout. The mode and label preference remain separate settings.

Diagnostics and support reports include `quad_views` with source numbers,
handle generations, raw submission roles and automatic versus manual mapping.
`CheekyOpenXRBindingTests --quad-roles` is the focused GPU check. Actual DCS
postprocessing and headset label visibility still require manual validation.

Assigned quad views bypass the extra gaze/crop/roundness/feather/alignment
settings. Center views use the center preset and supersampling over their full
image, with no additional peripheral pass. Peripheral views use the peripheral
preset and a single full-view DLAA pass at `PeripheralDlaaScale` times the game's
input width and height; the result expands to the original output rectangle.
For example, input 880x824 at 0.75 runs DLAA at 660x618 before expanding to
1512x1420. This reduces Cheeky's reconstruction workload; it does not change the
game's raster resolution or the OpenXR swapchain dimensions. Disable
**Override peripheral views** to retain the game's peripheral DLSS. The master
**Enable quad-view DLSS-SR** switch controls both overrides. Unresolved handles
retain the game's rendering until all four roles are assigned.

NR applies over the whole assigned center views only (D3D11 requires DX12
Transport); its extra fovea controls are hidden.

Experimental DCS D3D11 quad FG captures each assigned view's depth and motion
after its successful game evaluation, before DCS reuses those resources. The
four raw color views are captured before their OpenXR image releases. Center
and peripheral color stay in separate planes: memory packing copies pixels
without resampling, feathering or compositing. DLSS-G consumes the raw center
planes; peripheral methods are previous-frame reuse, motion warping or
bidirectional interpolation. Final presentation combines the processed planes.

Guide atlases normalize all four views into the full-eye projection using the
corrected submitted FOVs. One fresh evaluation per assigned view is required
within the same quad submission interval. Native presentation continues if
guides, sharing, storage or submission layouts are unavailable. The current raw
plane path targets DCS's separate per-eye swapchains; unsupported shared-eye
layouts keep native rendering. The layer owns D3D12 processing and leases;
enabling quad FG does not force the game's SR through DX12 Transport.

The FG tab exposes center scale and cheaper peripheral methods while hiding
stereo fovea placement. `quad_views.fg` records captured guide counts, assembled
pairs and last stages (capture 8 and assembly 4 indicate success). Source,
layer, UI and shader compilation are checked locally; DCS generation cadence,
headset quality and persistence after restart require manual validation.

The DCS 42032 capture from October 21 records an NVIDIA D3D11 command-dispatch
access violation (execution at `0x11`) during FG bridge presentation. The later
stack overflow is in DCS's crash reporter. No generated resources were recorded,
so this capture does not establish a DLSS-G inference failure. Quad FG now uses
D3D11-created shared textures and deferred bridge copies, with copy/signal/flush
protected as one immediate-context transaction. These changes address the
observed bridge path; a manual DCS run must confirm whether they resolve the
driver fault. Missing four-view guides use coordinated native composition,
without sending precomposed eye images through FG. `QUAD-FG submit` logs include
mapping status, guide counts, packing readiness and capture/assembly stages.

The follow-up 14724 capture confirms automatic roles were verified and all four
guide snapshots succeeded, but atlas allocation stopped at assembly stage 2.
A local adapter probe reproduced `E_INVALIDARG` creating a shared RG32_FLOAT
motion texture; shared RGBA32_FLOAT creation, views and D3D12 opening succeeded.
Quad guide motion storage therefore uses RGBA32_FLOAT, retaining float32 XY.
Before first activation, native runtime frames continue until a complete guide
pair proves allocation and sharing. Color capture and asynchronous presentation
start only afterward. Quad presenter retirement waits for accepted application
frames to end before removing their session owner, since xrWaitFrame can overlap
the preceding render-thread xrEndFrame. Cockpit loading, active generation and
turning FG off still need manual DCS validation; the prior bridge-only change
did not resolve the crash.

Capture 35424 confirms actual quad generation (43 generated submissions at the
support snapshot), with matched color/guide tokens and successful atlas assembly.
It also records recurring native passthroughs. Bridge upload previously held its
mutex during a GPU wait while guide assembly used try_lock; contention therefore
discarded valid intervals and cleared the presenter epoch/history. Upload now
pins its eye locally, waits outside the mutex and reacquires only for the D3D11
copy. Guide assembly waits for that bounded CPU section. Raw color packing can
still publish a native frame when no inference guide lease is available, without
forcing coordinated passthrough and resetting the whole epoch.

The later dump still faults in NVIDIA D3D11 command dispatch. Quad NGX evaluation,
bridge copies and runtime release/end submissions now share a CPU dispatch gate.
Runtime calls do not hold the immediate-context lock across external work, since
the runtime may use helper threads; ordinary D3D11 API protection remains enabled.
This excludes NGX's driver submission from overlapping those presentation calls;
it remains a crash mitigation requiring manual DCS validation. The additive
Acquire/ReleaseD3D11V1 exports require updating both runtime and OpenXR layer.
Sampled `FG HISTORY` messages identify worker restart, token gap, frame gap and
upstream source reset separately. Resets remain required for real discontinuities.

Games that composite four DLSS views themselves before a stereo OpenXR submission
(MSFS 2024's eye-tracked foveation opens a stereo session) no longer disable FG.
The two periphery evaluations, which cover the full eye FOV at lower resolution,
become the stereo guide pair; the center evaluations are ignored. Eyes come from
the four-view roles (default order or Manual assignment) and the guide output size
is the submitted eye rect. Color is the game's own composite. TODO: composite the
center guides into the inset, located from gaze plus depth registration, for
full-resolution guides where the user looks.

D3D12 games using real quad views through the layer also run FG: the layer composes
each eye, captures the composed images for the shared buffered presenter and
presents them as an ordinary stereo frame. Guides use the same periphery pair
(hook-free pairing at xrEndFrame). The composed eye rects are published as the
submitted views. D3D12 quad has no automatic role proof yet, so it uses the
default evaluation order unless roles are assigned manually.
