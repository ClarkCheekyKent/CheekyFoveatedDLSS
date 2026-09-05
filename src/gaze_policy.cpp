#include "gaze_policy.hpp"

#include <algorithm>
#include <cmath>

namespace cheeky::foveated_dlss {

GazeMappingPolicyResult update_gaze_mapping(
    GazeMappingPolicyState& state,
    const std::uint32_t match_count,
    const std::uint32_t matched_view_index,
    const std::uint64_t generation,
    const std::int64_t predicted_display_time
) noexcept {
    const auto previous_index = state.view_index;
    const auto previous_generation = state.generation;
    if (match_count != 1U) {
        const bool invalidated = state.view_index != unmapped_gaze_view;
        state.view_index = unmapped_gaze_view;
        state.consecutive_matches = 0U;
        state.last_display_time = 0;
        return {false, false, invalidated};
    }

    if (state.view_index != matched_view_index || state.generation != generation) {
        state.view_index = matched_view_index;
        state.generation = generation;
        state.consecutive_matches = 0U;
        state.last_display_time = 0;
    }
    if (state.last_display_time != predicted_display_time) {
        ++state.consecutive_matches;
        state.last_display_time = predicted_display_time;
    }
    return {
        state.consecutive_matches >= 2U,
        previous_index != unmapped_gaze_view &&
            (previous_index != state.view_index ||
             previous_generation != state.generation),
        false,
    };
}

std::uint32_t select_packed_stereo_gaze_view(
    const PackedStereoMappingInput& input
) noexcept {
    if (input.view_count != 2U || input.dlss_eye_index >= 2U ||
        input.output_width == 0U || input.output_height == 0U) {
        return unmapped_gaze_view;
    }

    const auto& first = input.views[0];
    const auto& second = input.views[1];
    if (!first.resource_valid || !second.resource_valid ||
        first.array_index != 0U || second.array_index != 0U ||
        first.resource_identity == 0U ||
        second.resource_identity == 0U ||
        first.swapchain_identity == 0U ||
        second.swapchain_identity == 0U ||
        first.rect_x < 0 || second.rect_x < 0 ||
        first.rect_y != 0 || second.rect_y != 0 ||
        first.rect_width != input.output_width ||
        second.rect_width != input.output_width ||
        first.rect_height != input.output_height ||
        second.rect_height != input.output_height) {
        return unmapped_gaze_view;
    }

    // OpenVR-to-OpenXR bridges can submit the two halves of a packed
    // layout through separate swapchains. Match the layout using the existing
    // DLSS eye roles; resource equality is only required within one swapchain.
    const bool same_swapchain = first.swapchain_identity == second.swapchain_identity;
    const bool same_resource = first.resource_identity == second.resource_identity;
    if (same_swapchain != same_resource) return unmapped_gaze_view;

    const auto first_x = static_cast<std::uint32_t>(first.rect_x);
    const auto second_x = static_cast<std::uint32_t>(second.rect_x);
    const bool first_then_second = first_x == 0U &&
        second_x == input.output_width;
    const bool second_then_first = second_x == 0U &&
        first_x == input.output_width;
    if (!first_then_second && !second_then_first) {
        return unmapped_gaze_view;
    }

    // Some engines describe each NGX output in eye-local coordinates, while
    // others retain the eye's base inside the packed stereo target. Validate
    // the latter against the physical DLSS eye before applying the optional
    // semantic eye-order inversion.
    const auto& dlss_eye = input.views[input.dlss_eye_index];
    const bool local_origin = input.output_origin_x == 0U &&
        input.output_origin_y == 0U;
    const bool packed_origin = dlss_eye.rect_x >= 0 && dlss_eye.rect_y >= 0 &&
        input.output_origin_x == static_cast<std::uint32_t>(dlss_eye.rect_x) &&
        input.output_origin_y == static_cast<std::uint32_t>(dlss_eye.rect_y);
    if (!local_origin && !packed_origin) return unmapped_gaze_view;

    return input.dlss_eye_index ^ (input.invert_eye_order ? 1U : 0U);
}

GazeTemporalPolicyResult update_gaze_temporal_policy(
    GazeTemporalPolicyState& state,
    const GazeTemporalPolicyInput& input
) noexcept {
    GazeTemporalPolicyResult result{
        input.fallback_u, input.fallback_v, false, false
    };
    if (input.sample_valid) {
        const auto raw_u = std::clamp(input.raw_u, 0.0F, 1.0F);
        const auto raw_v = std::clamp(input.raw_v, 0.0F, 1.0F);
        if (state.last_sample_time != input.sample_time) {
            if (!state.has_filtered) {
                state.filtered_u = raw_u;
                state.filtered_v = raw_v;
                state.has_filtered = true;
            } else {
                const auto dt = std::max(
                    input.now_seconds - state.last_update_seconds, 0.0
                );
                const auto tau = std::max(input.smoothing_ms, 0.0F) / 1000.0;
                const auto alpha = tau <= 0.0
                    ? 1.0
                    : 1.0 - std::exp(-dt / tau);
                state.filtered_u += static_cast<float>(
                    alpha * (raw_u - state.filtered_u)
                );
                state.filtered_v += static_cast<float>(
                    alpha * (raw_v - state.filtered_v)
                );
            }
            state.last_sample_time = input.sample_time;
            state.last_update_seconds = input.now_seconds;
        }
        result.reacquired = state.returning_to_fixed ||
            !state.was_using_valid_gaze;
        state.returning_to_fixed = false;
        state.was_using_valid_gaze = true;
        state.last_valid_seconds = input.now_seconds;
        result.center_u = state.filtered_u;
        result.center_v = state.filtered_v;
        result.using_gaze = true;
        return result;
    }

    if (!state.has_filtered) return result;
    const auto elapsed = std::max(
        input.now_seconds - state.last_valid_seconds, 0.0
    );
    if (elapsed <= input.hold_seconds) {
        result.center_u = state.filtered_u;
        result.center_v = state.filtered_v;
        result.using_gaze = true;
        return result;
    }

    if (!state.returning_to_fixed) {
        state.returning_to_fixed = true;
        state.return_start_u = state.filtered_u;
        state.return_start_v = state.filtered_v;
    }
    const auto denominator = std::max(input.return_seconds, 0.000001);
    const auto amount = std::clamp(
        (elapsed - input.hold_seconds) / denominator, 0.0, 1.0
    );
    result.center_u = static_cast<float>(
        state.return_start_u + amount *
            (input.fallback_u - state.return_start_u)
    );
    result.center_v = static_cast<float>(
        state.return_start_v + amount *
            (input.fallback_v - state.return_start_v)
    );
    result.using_gaze = amount < 1.0;
    if (amount >= 1.0) state.was_using_valid_gaze = false;
    return result;
}

GazeResetPolicyResult evaluate_gaze_reset(
    const GazeCropPolicyState& previous,
    const GazeCropPolicyState& current,
    const bool using_valid_sample,
    const bool reacquired,
    const bool remapped,
    const float jump_reset_ratio
) noexcept {
    GazeResetPolicyResult result{};
    if (previous.valid) {
        result.delta_x = previous.base_x > current.base_x
            ? previous.base_x - current.base_x
            : current.base_x - previous.base_x;
        result.delta_y = previous.base_y > current.base_y
            ? previous.base_y - current.base_y
            : current.base_y - previous.base_y;
    }
    if (!previous.valid && using_valid_sample) {
        result.reason = GazeResetReason::first_valid;
    } else if (reacquired) {
        result.reason = GazeResetReason::reacquired;
    } else if (remapped) {
        result.reason = GazeResetReason::remapped;
    } else if (previous.valid &&
               (previous.width != current.width ||
                previous.height != current.height)) {
        result.reason = GazeResetReason::crop_size_changed;
    } else if (previous.valid) {
        const auto ratio = std::max(jump_reset_ratio, 0.0F);
        const auto threshold_x = (std::max)(
            64U,
            static_cast<std::uint32_t>(std::lround(current.width * ratio))
        );
        const auto threshold_y = (std::max)(
            64U,
            static_cast<std::uint32_t>(std::lround(current.height * ratio))
        );
        if (result.delta_x > threshold_x || result.delta_y > threshold_y) {
            result.reason = GazeResetReason::large_jump;
        }
    }
    return result;
}

}  // namespace cheeky::foveated_dlss
