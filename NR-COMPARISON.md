# DLSS-NR implementation audit

Reference: [wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/tree/e237f895623742b761f9e5f00067cb3dc62619f4), commit `e237f895623742b761f9e5f00067cb3dc62619f4`.
The checkout is kept in the ignored `.reference` directory.

## Controls

The reference's [feature-18 forwarder](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/blob/e237f895623742b761f9e5f00067cb3dc62619f4/OptiScaler/dlssnr/forwarder/dlssnr_forwarder.cpp)
sets model tuning before creation and claims evaluation-only writes are ignored.
Cheeky's UI, persistence and evaluation setters were wired,
but the creation path omitted that tuning and its feature cache ignored it.

| Control | Previous behavior | Correction |
| --- | --- | --- |
| Intensity, local tone, local structure, skin structure | Written only at evaluation | Set before creation; included in cache identity |
| Automatic mask, UI correction | Written only at evaluation | Set before creation; included in cache identity; explicit control mask cleared |
| Preset | Cache ignored changes; Default was forced to A | Cache includes preset; Default passes zero |
| ReShade model sliders | Changes submitted throughout a drag | Apply on release, as the UEVR sliders already do |
| Paper white / color conversion | HDR conversion was forced for every frame | Honor the supplied HDR flag; SDR bypasses paper-white and gamma conversion |
| Transfer strength | SDR shader branch ignored it | SDR composition applies transfer strength and preserves source alpha |
| Working scale | Upsampling sampled the wrong neighbor at the top/left edge | Each bilinear tap clamps independently |

Model changes use the existing bounded, fence-tracked feature cache. Returning
to a cached configuration resets its history. Working scale, region geometry,
and processing order still determine the required model dimensions. Color
composition controls do not require model recreation.

The recreation policy follows that reference contract conservatively. The tests
below vary creation and evaluation settings together; they do not establish
whether every runtime version also supports tuning an existing feature live.

## Follow-up: actual NVIDIA model measurements

The Hogwarts Legacy support report showed successful NR evaluations and model
recreation as settings changed. Its installed Cheeky runtime matched the first
repair build. That established that the code was running, but did not explain
which settings actually changed the image. Its saved intensity was 2, tone and
structure approximately 1, and automatic masking was off.

`tools/nr_model_probe.cpp` evaluates the game's **310.8.0.0 NR DLL on the RTX 5070**,
using the installed driver's real NGX parameter allocator. It uses private D3D12
resources with a generated 512x512 image, zero motion and constant depth, creates
a feature per variant, evaluates four frames and reads back its FP16 RGB output.
It does not attach to Hogwarts Legacy. Results: `build/nr-model-probe/results.txt`.

| Change from Standard, intensity/tone/structure/skin 1, mask off | Mean absolute RGB difference | UI decision |
| --- | ---: | --- |
| Intensity 0 | 0.009176590 (exactly matches input) | Keep |
| Intensity 0.5 | 0.004581311 | Keep |
| Intensity 2 | 0 (exactly matches intensity 1) | Limit slider and normalized setting to 0–1 |
| Local tone 0 / 2 | 0.008415522 / 0.006448221 | Keep 0–2 |
| Local structure 0 / 2 | 0.005067929 / 0.005839558 | Keep 0–2 |
| Skin strength 0, mask off | 0 | Hide skin slider until automatic masking is on |
| Automatic mask on | 0.001661206 | Keep; skin values 0, 1 and 2 produce different output with it enabled |
| Style 1 (Natural) | 0.043303189 | Add Style selector |
| Style 2 (Cinematic) | 0.036578081 | Add Style selector |
| Preset hints 1–7 | 0 for each | Remove preset dropdown |
| UI correction on | 0 | Remove unverified checkbox |

All outputs were finite. Standard, Natural and Cinematic are the reference UI's
labels for style values 0, 1 and 2. Cheeky previously hardcoded Style to 0;
the preset hint is a different parameter, not a substitute for Style.
The fork itself labels its preset hints as having unverified visual effect.
Its 0–2 model-intensity range would have the same inactive upper half with this
DLL and direct parameter mapping. Its own extra composition and multipass
processing can produce other changes; the complete fork and RenoDX addon were
not run in this experiment.

These measurements demonstrate responses on this input and runtime, not that
UI correction or preset hints can never affect other inputs or DLL versions.
Their legacy config keys remain readable for compatibility, but are absent from
both menus. Style is persisted as `NrStyle`, defaults to Standard for old INIs,
and participates in feature caching and history reset. Parameter readback at
feature creation now logs style, intensity, tone, structure, skin and mask state.

`tools/nr_parameter_probe.cpp` separately confirmed the existing typed float ABI
is correct. It also established that integer zero is not a typed null pointer:
`DLSSNR.ControlMask` now uses the pointer setter. The runtime verifies tuning
readback before feature creation, so setter/getter mismatches report a failure.

Paper white is now shown only for HDR NR input; the production SDR shader
bypasses it. Skin strength is shown only when automatic masking is enabled.
Intensity remains available: it demonstrably works between 0 and 1.

## Motion vectors

Cheeky's pipeline resamples depth and motion into private NR working textures.
Its motion texture contains current-to-previous displacement in **region UVs**;
the runtime receives `MVecScaleX/Y` equal to the working texture width/height.
Copying the reference's source-vector scale into this already-normalized path
would scale motion twice. The existing conversion from NGX render-pixel units,
Streamline UV units, independent guide subrects and working resolution remains.

Two corrections were needed:

- Crop movement is added in region UVs without dividing by the user's motion
  multiplier. Zero scene-motion scale no longer resets history on every gaze move.
- When source vectors contain jitter, their scaled jitter is removed before
  adding the jitter required by Before color. After color stays unjittered.
  Negative, fractional and zero multipliers now affect scene motion only.

For multiplier `m` and jitter displacement `J = previousJitter - currentJitter`,
the added full-view UV correction is `(before - vectorsJittered * m) * J`.
Crop-origin displacement is added separately, and the result is divided by the
region's fraction of the full view.

## Comparison boundaries

This is a targeted repair, not a port of the reference's multipass, exposure
metering, skin/environment composition, residual SR or hybrid model pipelines.
Cheeky's Color strength remains an overall color blend; the reference's color
control has separate chroma/luminance behavior. Paper white remains a manual
scale rather than the reference's exposure-driven white point. Those differences
are not disconnected settings.

## Validation

Completed successfully: Release solution build and the full native test script,
`CheekyTests.exe --motion-resample`, and the UEVR menu tests on LuaJIT 2.1 and
Lua 5.4. The package script verified the UEVR archive's payload and checksums.
Build and motion logs are under `build/nr-audit-build.log` and
`build/nr-audit-motion.log`. D3D12 debug validation was unavailable; GPU readback
and resource-lifetime assertions still ran.

`scripts/build.ps1 -Configuration Release` builds both integrations and runs the
native suites. The late-attachment DX12, DX12-C and Streamline fixtures observe
immutable creation parameters of the exact feature being evaluated, so setters
called only at evaluation cannot satisfy the tests. Each model control is changed
and restored independently; tests check recreation, stable-frame reuse, cached
restoration, history reset, and bounded feature counts across views.

The same fixtures read back production NR guide textures with jittered vectors
and negative, zero and fractional multipliers, in both processing orders. An
identity fake model checks SDR encode/decode with several paper-white values,
transfer strengths and working scales, including top/left edge sampling.
The NR contract tests cover crop motion with zero multipliers.
`CheekyTests.exe --motion-resample` additionally exercises the production GPU
guide sampling and invalid-vector preservation.

These tests use a fake NVIDIA runtime and GPU readback; they establish parameter
delivery and numerical behavior, not the visual response of NVIDIA's model.
The follow-up adds the real-model experiment above, Style persistence and live
menu acknowledgement tests, and checks for hidden inactive controls. Full native
build/test output is in `build/nr-controls-followup-build.log`.
In-game validation should compare each model control in a fixed scene, then
check moving-head/moving-gaze behavior at working scales 1 and 0.5, in both eyes
and both NR orders. Compare all three styles. Model tuning
may briefly stall while a new feature is created. Runtime/model versions may
differ in which presets and tuning values produce visible changes.
