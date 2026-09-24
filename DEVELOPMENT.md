# Building and testing

Requires Visual Studio 2022 or newer with Desktop development with C++ and a Windows 10/11 SDK. Dependencies are vendored under [third_party](third_party/README.md).

## Build

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

## Packaging

```powershell
./scripts/package-uevr.ps1 -Configuration Release
./scripts/package-standalone.ps1 -Configuration Release -Mode Both -Label local
./scripts/build-installer.ps1
```

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
