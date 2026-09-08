# Third-party dependencies

This directory contains minimal, pinned source snapshots needed to build the add-on. They are build-time dependencies and are not included with the installed `.addon64` file.

| Dependency | Pinned version | Files retained | Upstream |
| --- | --- | --- | --- |
| ReShade | API 20, commit `f596db33ef50c5898997b6dab044aaa9ebe73667` | Public add-on API headers | https://github.com/crosire/reshade |
| Dear ImGui | 1.92.5, commit `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c` | `imgui.h` and `imconfig.h` | https://github.com/ocornut/imgui |
| MinHook | commit `8fda4f5481fed5797dc2651cd91e238e9b3928c6` | Public header and x64 implementation | https://github.com/TsudaKageyu/minhook |
| OpenXR SDK | 1.1.61, tag `release-1.1.61` | Core, platform, and API-layer negotiation headers | https://github.com/KhronosGroup/OpenXR-SDK |
| OpenVR SDK | 2.15.6, commit `0924064316de3effbcd1acf1e309182a2deb1c05` | Public `openvr.h`; no runtime DLL replacement | https://github.com/ValveSoftware/openvr |
| UEVR | API 2.39.0, commit `4ee5c6b6162dee2291fc75f9dfc57667f6d45a2d` | Public C `API.h` only (separately MIT licensed) | https://github.com/praydog/UEVR |

Upstream license files are retained beside each dependency.
The UEVR header also retains its complete MIT notice. The UEVR runtime uses
MinHook and OpenVR headers, but does not include ReShade's API or ImGui headers.

ReShade's overlay header requires an exact Dear ImGui version, so update the ReShade API headers and ImGui headers together. Dependency updates should be deliberate and followed by both Debug and Release builds; the normal build does not download or update third-party code.
