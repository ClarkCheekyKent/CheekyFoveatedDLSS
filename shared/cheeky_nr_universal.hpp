#pragma once
// ABI between the OpenXR layer and CheekyFoveatedDLSSRuntime.dll for
// Universal DLSS 5: the layer evaluates DLSS-NR on each submitted eye image
// through the runtime's NGX feature cache, codec and per-view history.
// Both exports are optional; an older runtime simply leaves Universal off.
#include <cstdint>
#include <cstddef>
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

constexpr std::uint32_t CHEEKY_NR_UNIVERSAL_ABI = 1;

// Read once per native stereo pair.
struct CheekyNrUniversalOptionsV1 {
    std::uint32_t abi{2}; // Options v2; eye/status ABI stays v1.
    std::uint32_t size{sizeof(CheekyNrUniversalOptionsV1)};
    std::uint32_t enabled{}; // DLSS-NR on, NR source Universal, processing allowed.
    std::uint32_t motion{}; // 0 optical flow, 1 game DLSS vectors (optical flow when unavailable).
    std::uint32_t depth{}; // 0 automatic, 1 game DLSS depth, 2 none (flat depth).
    std::uint32_t share_optical{}; // Reuse FG's center optical flow when FG computes one.
    std::uint32_t optical_preset{}, optical_percent{}, optical_grid{}; // NR's own flow.
    std::uint32_t center_from_fg{}; // FG's region follows SR placement and gaze; reuse its center.
    // NR region, to size the optical-flow window: fraction of the eye and the
    // static per-eye center (eye UV) used when FG's center is not reused.
    std::uint32_t foveated{};
    float width{1}, height{1};
    float center_u[2]{.5f, .5f}, center_v[2]{.5f, .5f};
    std::uint32_t automatic_alignment{};
    float aligned_height_offset{};
};
constexpr std::uint32_t CHEEKY_NR_UNIVERSAL_OPTIONS_V1_SIZE = offsetof(CheekyNrUniversalOptionsV1, automatic_alignment);

// One eye. The runtime records guides, NR and codec passes on `list`; the
// caller executes it. `color` is the full eye image, UAV-capable, and is
// rewritten in place. Every texture must stay alive until `list` retires.
struct CheekyNrUniversalEyeV1 {
    std::uint32_t abi{CHEEKY_NR_UNIVERSAL_ABI};
    std::uint32_t size{sizeof(CheekyNrUniversalEyeV1)};
    ID3D12GraphicsCommandList* list{};
    std::uint32_t eye{}; // 0 left, 1 right; selects the independent NR history.
    std::uint32_t reset{}; // History discontinuity seen by the layer.
    ID3D12Resource* color{};
    std::uint32_t color_state{}; // D3D12_RESOURCE_STATES before and after the call.
    std::uint32_t width{}, height{}; // Eye image extent inside `color` (origin 0,0).
    std::uint32_t hdr{}; // Color is linear HDR (float format).
    // Stored vectors times uv_scale = current-to-previous displacement in eye UV.
    ID3D12Resource* motion{};
    std::uint32_t motion_state{};
    std::uint32_t motion_x{}, motion_y{}, motion_width{}, motion_height{}; // Texels covering the whole eye.
    float motion_uv_scale_x{1}, motion_uv_scale_y{1};
    std::uint32_t motion_jittered{};
    float jitter_uv_x{}, jitter_uv_y{};
    // Null selects the runtime's flat-depth fallback.
    ID3D12Resource* depth{};
    std::uint32_t depth_state{};
    std::uint32_t depth_x{}, depth_y{}, depth_width{}, depth_height{};
    std::uint32_t depth_inverted{};
    // NR region center in eye UV (FG's SR-matched center); otherwise static SR placement.
    std::uint32_t has_center{};
    float center_u{.5f}, center_v{.5f};
    // Reported in the menu only.
    std::uint32_t game_motion{}, shared_flow{};
};

// Layer status for the menu, reported once per native stereo pair.
struct CheekyNrUniversalStatusV1 {
    std::uint32_t abi{CHEEKY_NR_UNIVERSAL_ABI};
    std::uint32_t size{sizeof(CheekyNrUniversalStatusV1)};
    std::uint32_t evaluated[2]{}; // NR output presented for this eye.
    std::uint32_t game_motion{}, game_depth{}, shared_flow{};
    std::uint32_t fallback{}; // This pair was presented without NR.
    std::uint32_t reset{}; // This pair reset NR history.
    float flow_ms{}, vectors_ms{}; // Last measured GPU time, both eyes; 0 when unmeasured.
    const char* reason{}; // Static string; why NR was skipped, else null.
};

using CheekyNrGetUniversalOptionsV1Fn = bool(__cdecl*)(CheekyNrUniversalOptionsV1*);
using CheekyNrEvaluateUniversalV1Fn = bool(__cdecl*)(const CheekyNrUniversalEyeV1*);
using CheekyNrReportUniversalV1Fn = void(__cdecl*)(const CheekyNrUniversalStatusV1*);

// Optional placement diagnostic, separate from the existing status ABI.
struct CheekyNrUniversalAlignmentV1 {
    std::uint32_t abi{1},size{sizeof(CheekyNrUniversalAlignmentV1)};
    std::uint32_t projection_alignment{}; // Both evaluated eye centers match direct XR alignment.
};
using CheekyNrReportUniversalAlignmentV1Fn = void(__cdecl*)(const CheekyNrUniversalAlignmentV1*);
