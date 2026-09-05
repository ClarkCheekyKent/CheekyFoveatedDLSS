#pragma once

#include "frame_contract.hpp"
#include "gaze_policy.hpp"
#include "settings.hpp"

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
};

struct GazeDiagnostics {
    char runtime_name[128]{};
    std::uint32_t status_flags{};
    float sample_age_ms{};
    bool layer_present{};
    bool abi_compatible{};
    bool using_gaze{};
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
    bool& reset_history
) noexcept;

void apply_next_jump_preview(Settings& settings, DlssViewId view_id) noexcept;
[[nodiscard]] GazeDiagnostics gaze_diagnostics() noexcept;
void forget_gaze_view(DlssViewId view_id) noexcept;
void reset_gaze_foveation() noexcept;

}  // namespace cheeky::foveated_dlss
