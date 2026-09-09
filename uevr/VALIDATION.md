# UEVR preview validation — 2026-09-08

GPU timing update following the UI parity package (v0.2.4). The user
reports the preceding UEVR plugin working in Hogwarts Legacy. During this update,
no game was launched and no files were installed into a game or UEVR profile.

| Check | Result |
| --- | --- |
| MSBuild x64 Release and Debug, including original ReShade add-on | Pass |
| CMake x64 Release and ten registered CTest tests | Pass |
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
| SR/NR conditional visibility, simulation/NR dropdowns, group-reset draft isolation and stale slider prevention | Pass with mocked Lua host |
| Independent native SR/NR/gaze resets, unknown group rejection, expanded JSON report | Pass |
| Optional DLL search: executable fallback, runtime-folder precedence and missing-file error | Pass with locally authored test DLL |
| Deterministic present cadence, pause/toggle resets and actual host callback sampling | Pass; not a game performance measurement |
| Exhaust timestamp slots with discarded command recordings, then resume evaluations | Reproduces failure before fix; passes with reset cleanup |
| Evaluate through a forwarding COM interface, submit its native command list | Pass; no stranded timing references |
| Native, foveated-center and peripheral timestamps reach the runtime JSON | Pass with mock NGX on WARP and hardware GPU |
| Fake NGX initialized, feature created/evaluated and pointers cached before plugin load: DX11/DX12, regular/C callbacks | Pass |
| Missing create flags, quality, output dimensions or required resources: forward without changing parameters or creating features | Pass |
| Late-adopted private feature reuse, release of game/private handles and game feature recreation | Pass |
| Fake Streamline initialized and setter cached/called before plugin load: native NGX fallback on DX11/DX12 | Pass |
| Cached Streamline setter intercepted without a fresh game lookup; game output dimensions preserved | Pass |
| Unknown options version and a different viewport use native fallback; DX11 never enters DX12 Streamline compositor | Pass |
| ZIP payload names and SHA-256 readback | Pass |

Actual UEVR injection, DLSS-NR evaluation, the updated headset menu/controller
input, both-eye output and performance of **this update** remain untested by the
agent. The user's success report for the preceding plugin does not establish
these results or compatibility with other games.
The late-attachment tests load locally authored fake DLLs with NGX/Streamline
export names. They prove interception/control flow, private-feature management
and parameter handling, not NVIDIA's evaluation behavior or image quality.
The host harness does not load NVIDIA NGX or evaluate DLSS. Its hardware mode
exercises ordinary D3D12 operations only. No performance claim follows from these
results. Use the test sequence in the accompanying preview guide for the remaining checks.
