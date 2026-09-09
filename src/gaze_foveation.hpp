#pragma once

#include "frame_contract.hpp"
#include "gaze_policy.hpp"
#include "settings.hpp"
#include "gaze_copy_graph.hpp"
#include "gaze_projection.hpp"
#include "cheeky_gaze_abi.h"

#include <Unknwn.h>

#include <array>
#include <cstdint>

namespace cheeky::foveated_dlss {

struct GazeViewDiagnostics {
    float center_u{};
    float center_v{};
    std::uint64_t dlss_view_id{};
    std::uint32_t stable_matches{};
    std::int32_t crop_delta_x{};
    std::int32_t crop_delta_y{};
    std::uint64_t xr_resource{};
    std::int32_t xr_x{}, xr_y{};
    std::uint32_t xr_width{}, xr_height{}, xr_array{};
    std::uint64_t candidate_view{}, candidate_resource{};
    std::uint32_t candidate_x{}, candidate_y{}, candidate_width{}, candidate_height{};
    bool has_candidate{};
    bool resource_mapped{};
    bool packed_stereo_mapping{};
    bool copy_mapping{};
    bool projection_mapping{};
    bool marker_mapping{};
    unsigned alignment_source{};
    float aligned_u{}, aligned_v{};
};

struct GazeDiagnostics {
    std::uint64_t submitted_copies{};
    char runtime_name[128]{};
    std::uint32_t status_flags{};
    float sample_age_ms{};
    bool layer_present{};
    bool abi_compatible{};
    bool using_gaze{};
    // Latest evaluated view: 0 manual fallback, 1 Streamline, 2 OpenXR, 3 OpenVR.
    unsigned alignment_source{};
    bool mapping_ambiguous{};
    GazeResetReason last_reset_reason{GazeResetReason::none};
    std::array<GazeViewDiagnostics, 2U> views{};
};

[[nodiscard]] bool calculate_coordinated_crop(
    const Settings& settings,
    DlssViewId view_id,
    IUnknown* output_resource,
    std::uint32_t render_width,
    std::uint32_t render_height,
    std::uint32_t output_width,
    std::uint32_t output_height,
    std::uint32_t output_origin_x,
    std::uint32_t output_origin_y,
    CropGeometry& crop,
    bool& reset_history,
    const CheekyGazeSnapshotV1* supplied_snapshot = nullptr
) noexcept;

void apply_next_jump_preview(Settings& settings, DlssViewId view_id) noexcept;
[[nodiscard]] GazeDiagnostics gaze_diagnostics() noexcept;
void forget_gaze_view(DlssViewId view_id) noexcept;
void reset_gaze_foveation() noexcept;
void record_gaze_copy(std::uint64_t command_list, GazeCopyEdge edge) noexcept;
void submit_gaze_copies(std::uint64_t command_list) noexcept;
void reset_gaze_copies(std::uint64_t command_list) noexcept;
void forget_gaze_resource(std::uint64_t resource) noexcept;

}  // namespace cheeky::foveated_dlss
