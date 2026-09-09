# Building and testing

Requires Visual Studio 2022 or newer with Desktop development with C++ and a Windows 10/11 SDK. Dependencies are vendored under [third_party](third_party/README.md).

## Build

From PowerShell at the repository root:

```powershell
./scripts/build.ps1 -Configuration Release
```

Builds both integrations, the OpenXR layer and tests, then runs the native test suites. Outputs go to `bin/Release`. Use `-Configuration Debug` for a debug build. The solution also supports Visual Studio and CMake 3.24 or newer.

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
./scripts/build-installer.ps1
```

The OpenXR installer requires Inno Setup 6.3 or newer. Pass `-IsccPath` if its compiler is outside the standard locations. To package existing Release binaries, use `-SkipBuild`; for CMake output also supply `-ArtifactsDirectory`.

The installer output is `bin/installer/CheekyOpenXRSetup.exe`. Publish it alongside the ReShade add-on and UEVR package. The installer contains only the shared layer, manifest and licenses. Keep its AppId and install path stable so upgrades replace the existing installation. Verify install, upgrade and uninstall before publishing.

Set the release version in `shared/version.h`; the UI, CMake and installer share it. NVIDIA runtimes and test fixtures are not included in packages.

## Source layout

- `src/`: shared settings, interception, rendering, calibration and resource observation; `addon.cpp` supplies the ReShade UI and host integration.
- `uevr/plugin.cpp`: UEVR adapter; `uevr/runtime.cpp`: resident runtime, settings and reporting; `uevr/scripts/`: Lua controls.
- `openxr_layer/`: shared OpenXR layer, installed separately for either integration.
- `tests/`: native GPU/host fixtures and Lua menu tests.

The UEVR runtime remains resident because hooks and GPU work can outlive adapter detach. See [eye calibration](EYE-CALIBRATION.md) for calibration ownership and lifetime rules.
