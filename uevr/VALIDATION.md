# UEVR preview validation — 2026-09-08

Local implementation based on v0.2.4 / `97f8e4e`. No game was launched and no
files were installed into a game or UEVR profile.

| Check | Result |
| --- | --- |
| MSBuild x64 Release and Debug, including original ReShade add-on | Pass |
| CMake x64 Release and four registered CTest tests | Pass |
| Existing core unit tests | Pass |
| Existing D3D11/D3D12 compositor GPU readback tests | Pass; D3D debug layer unavailable |
| Existing motion-resampling GPU tests | Pass |
| Load actual plugin DLL through a fake UEVR C API host | Pass |
| Reject null/old API, invalid private ABI and duplicate processing owner | Pass |
| D3D12 copy, submission, second queue, repeat submission, discarded recording, reset and lifetime observations | Pass on WARP and default hardware GPU |
| DX11 host initialization, reset/recovery and unsupported transport rejection | Pass on WARP |
| Atomic settings transactions, invalid values, coherent concurrent reads, INI roundtrip and asynchronous save/report | Pass |
| Real FreeLibrary/reload of adapter, resident processing gate and stale attachment rejection | Pass |
| Actual Lua menu under LuaJIT 2.1 and Lua 5.4, using mocked UEVR/ImGui bindings | Pass |
| Float and integer sliders: no update while dragging/holding, exactly one update on release | Pass |
| Keyboard edits, dirty false values, rejected edits, delayed acknowledgements, reconnect | Pass |
| ZIP payload names and SHA-256 readback | Pass |

Actual UEVR injection, Hogwarts Legacy DLSS evaluation, hook coexistence, headset
menu visibility/controller input, both-eye output and performance remain **untested**.
The host harness does not load NVIDIA NGX or evaluate DLSS. Its hardware mode
exercises ordinary D3D12 operations only. No performance claim follows from these
results. Use the test sequence in the accompanying preview guide for the remaining checks.
