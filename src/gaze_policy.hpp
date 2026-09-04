#pragma once

#include <array>
#include <cstdint>

namespace cheeky::foveated_dlss {

constexpr std::uint32_t unmapped_gaze_view = UINT32_MAX;

struct GazeMappingPolicyState {
    std::uint32_t view_index{unmapped_gaze_view};
    std::uint32_t consecutive_matches{};
    std::int64_t last_display_time{};
    std::uint64_t generation{};
};

struct GazeMappingPolicyResult {
    bool stable{};
    bool changed{};
    bool invalidated{};
};

[[nodiscard]] GazeMappingPolicyResult update_gaze_mapping(
    GazeMappingPolicyState& state,
    std::uint32_t match_count,
    std::uint32_t matched_view_index,
    std::uint64_t generation,
    std::int64_t predicted_display_time
) noexcept;

struct PackedStereoView {
    std::int32_t rect_x{};
    std::int32_t rect_y{};
    std::uint32_t rect_width{};
    std::uint32_t rect_height{};
    std::uint32_t array_index{};
    std::uint64_t resource_identity{};
    std::uint64_t swapchain_identity{};
    bool resource_valid{};
};

struct PackedStereoMappingInput {
    std::array<PackedStereoView, 2U> views{};
    std::uint32_t view_count{};
    std::uint32_t dlss_eye_index{unmapped_gaze_view};
    std::uint32_t output_origin_x{};
    std::uint32_t output_origin_y{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    bool invert_eye_order{};
};

[[nodiscard]] std::uint32_t select_packed_stereo_gaze_view(
    const PackedStereoMappingInput& input
) noexcept;

struct GazeTemporalPolicyState {
    std::int64_t last_sample_time{};
    double last_update_seconds{};
    double last_valid_seconds{};
    float filtered_u{0.5F};
    float filtered_v{0.5F};
    float return_start_u{0.5F};
    float return_start_v{0.5F};
    bool has_filtered{};
    bool returning_to_fixed{};
    bool was_using_valid_gaze{};
};

struct GazeTemporalPolicyInput {
    double now_seconds{};
    std::int64_t sample_time{};
    float raw_u{0.5F};
    float raw_v{0.5F};
    float fallback_u{0.5F};
    float fallback_v{0.5F};
    float smoothing_ms{20.0F};
    double hold_seconds{0.100};
    double return_seconds{0.150};
    bool sample_valid{};
};

struct GazeTemporalPolicyResult {
    float center_u{0.5F};
    float center_v{0.5F};
    bool using_gaze{};
    bool reacquired{};
};

[[nodiscard]] GazeTemporalPolicyResult update_gaze_temporal_policy(
    GazeTemporalPolicyState& state,
    const GazeTemporalPolicyInput& input
) noexcept;

enum class GazeResetReason : std::uint32_t {
    none,
    first_valid,
    reacquired,
    remapped,
    crop_size_changed,
    large_jump,
};

struct GazeCropPolicyState {
    std::uint32_t base_x{};
    std::uint32_t base_y{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool valid{};
};

struct GazeResetPolicyResult {
    GazeResetReason reason{GazeResetReason::none};
    std::uint32_t delta_x{};
    std::uint32_t delta_y{};
};

[[nodiscard]] GazeResetPolicyResult evaluate_gaze_reset(
    const GazeCropPolicyState& previous,
    const GazeCropPolicyState& current,
    bool using_valid_sample,
    bool reacquired,
    bool remapped,
    float jump_reset_ratio
) noexcept;

}  // namespace cheeky::foveated_dlss
