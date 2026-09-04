#include "gaze_foveation.hpp"

#include "cheeky_gaze_abi.h"
#include "runtime.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace cheeky::foveated_dlss {
namespace {

constexpr double gaze_stale_seconds = 0.050;
constexpr double gaze_hold_seconds = 0.100;
constexpr double gaze_return_seconds = 0.150;

struct ViewState {
    DlssViewId view_id{};
    GazeMappingPolicyState mapping{};
    GazeTemporalPolicyState temporal{};
    std::int64_t last_snapshot_display_time{};
    std::uint64_t last_snapshot_qpc{};
    bool has_crop{};
    CropGeometry last_crop{};
};

std::mutex coordinator_mutex;
std::vector<ViewState> view_states;
GazeDiagnostics diagnostics{};
CheekyOpenXRGetGazeSnapshotFn snapshot_function{};
std::uint64_t qpc_frequency{};

[[nodiscard]] std::uint64_t qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

[[nodiscard]] double seconds_between(
    const std::uint64_t newer,
    const std::uint64_t older
) noexcept {
    if (newer <= older || qpc_frequency == 0U) return 0.0;
    return static_cast<double>(newer - older) /
        static_cast<double>(qpc_frequency);
}

[[nodiscard]] std::uint64_t canonical_identity(
    IUnknown* const resource
) noexcept {
    if (resource == nullptr) return 0U;
    IUnknown* identity{};
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&identity))) ||
        identity == nullptr) {
        return 0U;
    }
    const auto value = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(identity)
    );
    identity->Release();
    return value;
}

[[nodiscard]] bool load_snapshot(
    CheekyGazeSnapshotV1& snapshot
) noexcept {
    if (snapshot_function == nullptr) {
        const auto module = GetModuleHandleW(L"CheekyOpenXRLayer.dll");
        if (module != nullptr) {
            snapshot_function = reinterpret_cast<CheekyOpenXRGetGazeSnapshotFn>(
                GetProcAddress(module, "CheekyOpenXR_GetGazeSnapshot")
            );
        }
    }
    diagnostics.layer_present = snapshot_function != nullptr;
    if (snapshot_function == nullptr) return false;
    snapshot = {};
    if (snapshot_function(
            CHEEKY_GAZE_ABI_VERSION, &snapshot, sizeof(snapshot)
        ) == 0U) {
        diagnostics.abi_compatible = false;
        return false;
    }
    diagnostics.abi_compatible = snapshot.abi_version ==
            CHEEKY_GAZE_ABI_VERSION &&
        snapshot.structure_size >= sizeof(snapshot);
    return diagnostics.abi_compatible;
}

[[nodiscard]] ViewState& state_for_view(const DlssViewId view_id) {
    for (auto& state : view_states) {
        if (state.view_id == view_id) return state;
    }
    view_states.push_back({});
    view_states.back().view_id = view_id;
    return view_states.back();
}

[[nodiscard]] FoveationCenter fixed_center(
    const Settings& settings,
    const std::uint32_t render_width,
    const std::uint32_t render_height
) noexcept {
    const auto width = std::clamp(
        static_cast<std::uint32_t>(std::lround(
            static_cast<double>(render_width) *
            std::clamp(settings.width, 0.0F, 1.0F)
        )), 1U, render_width
    );
    const auto height = std::clamp(
        static_cast<std::uint32_t>(std::lround(
            static_cast<double>(render_height) *
            std::clamp(settings.height, 0.0F, 1.0F)
        )), 1U, render_height
    );
    const auto start_x = static_cast<double>(render_width - width) *
        (static_cast<double>(std::clamp(settings.x_offset, -1.0F, 1.0F)) +
         1.0) * 0.5;
    const auto start_y = static_cast<double>(render_height - height) *
        (static_cast<double>(std::clamp(
            settings.height_offset, -1.0F, 1.0F
        )) + 1.0) * 0.5;
    return {
        static_cast<float>((start_x + width * 0.5) / render_width),
        static_cast<float>((start_y + height * 0.5) / render_height),
        settings.gaze_quantization_pixels,
    };
}

[[nodiscard]] bool exact_view_match(
    const CheekyGazeViewV1& view,
    const std::uint64_t resource_identity,
    const std::uint32_t output_origin_x,
    const std::uint32_t output_origin_y,
    const std::uint32_t output_width,
    const std::uint32_t output_height
) noexcept {
    return (view.flags & CHEEKY_GAZE_VIEW_RESOURCE_VALID) != 0U &&
        view.array_index == 0U &&
        view.resource_identity == resource_identity &&
        view.image_rect_x == static_cast<std::int32_t>(output_origin_x) &&
        view.image_rect_y == static_cast<std::int32_t>(output_origin_y) &&
        view.image_rect_width == output_width &&
        view.image_rect_height == output_height;
}

void update_diagnostics_view(
    const std::uint32_t index,
    const CheekyGazeSnapshotV1& snapshot
) noexcept {
    if (index >= diagnostics.views.size()) return;
    auto& target = diagnostics.views[index];
    target.center_u = snapshot.views[index].center_u;
    target.center_v = snapshot.views[index].center_v;
}

}  // namespace

bool calculate_coordinated_crop(
    const Settings& settings,
    const DlssViewId view_id,
    IUnknown* const output_resource,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t output_origin_x,
    const std::uint32_t output_origin_y,
    CropGeometry& crop,
    bool& reset_history
) noexcept {
    reset_history = false;
    const auto fixed_settings = settings_for_view(settings, view_id);
    const auto eye_assignment = stereo_eye_assignment(view_id);
    if (settings.center_mode != FoveationCenterMode::openxr_gaze) {
        return calculate_crop(
            fixed_settings, render_width, render_height,
            output_width, output_height, output_origin_x, output_origin_y, crop
        );
    }

    std::lock_guard lock(coordinator_mutex);
    if (qpc_frequency == 0U) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
    }
    CheekyGazeSnapshotV1 snapshot{};
    if (!load_snapshot(snapshot)) {
        diagnostics.using_gaze = false;
        return calculate_crop(
            fixed_settings, render_width, render_height,
            output_width, output_height, output_origin_x, output_origin_y, crop
        );
    }
    diagnostics.status_flags = snapshot.status_flags;
    static_cast<void>(strncpy_s(
        diagnostics.runtime_name, snapshot.runtime_name, _TRUNCATE
    ));
    const auto now = qpc_now();
    diagnostics.mapping_ambiguous =
        (snapshot.status_flags & CHEEKY_GAZE_STATUS_AMBIGUOUS_RESOURCE) != 0U;

    const auto resource_identity = canonical_identity(output_resource);
    std::uint32_t matched_index{UINT32_MAX};
    std::uint32_t match_count{};
    bool packed_stereo_match{};
    for (std::uint32_t index{};
         index < (std::min)(snapshot.view_count, CHEEKY_GAZE_MAX_VIEWS);
         ++index) {
        if (exact_view_match(
                snapshot.views[index], resource_identity,
                output_origin_x, output_origin_y,
                output_width, output_height
            )) {
            matched_index = index;
            ++match_count;
        }
    }
    if (match_count == 0U && eye_assignment.assigned &&
        (snapshot.status_flags & CHEEKY_GAZE_STATUS_MAPPING_READY) != 0U &&
        (snapshot.status_flags &
         CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG) == 0U) {
        PackedStereoMappingInput packed_input{};
        packed_input.view_count = snapshot.view_count;
        packed_input.dlss_eye_index = eye_assignment.eye_index;
        packed_input.output_origin_x = output_origin_x;
        packed_input.output_origin_y = output_origin_y;
        packed_input.output_width = output_width;
        packed_input.output_height = output_height;
        packed_input.invert_eye_order = settings.invert_stereo_x_offset;
        for (std::uint32_t index{}; index < 2U; ++index) {
            const auto& source = snapshot.views[index];
            packed_input.views[index] = {
                source.image_rect_x,
                source.image_rect_y,
                source.image_rect_width,
                source.image_rect_height,
                source.array_index,
                source.resource_identity,
                source.swapchain_identity,
                (source.flags & CHEEKY_GAZE_VIEW_RESOURCE_VALID) != 0U,
            };
        }
        const auto packed_index = select_packed_stereo_gaze_view(packed_input);
        if (packed_index != unmapped_gaze_view) {
            matched_index = packed_index;
            match_count = 1U;
            packed_stereo_match = true;
        }
    }
    diagnostics.mapping_ambiguous = diagnostics.mapping_ambiguous ||
        match_count > 1U;
    auto& state = state_for_view(view_id);
    if (state.last_snapshot_display_time != snapshot.predicted_display_time) {
        state.last_snapshot_display_time = snapshot.predicted_display_time;
        state.last_snapshot_qpc = now;
    }
    const auto sample_age_seconds = seconds_between(
        now, state.last_snapshot_qpc
    );
    diagnostics.sample_age_ms = static_cast<float>(
        sample_age_seconds * 1000.0
    );
    const bool mapping_was_stable = state.mapping.view_index <
            CHEEKY_GAZE_MAX_VIEWS &&
        state.mapping.consecutive_matches >= 2U;
    const auto mapping_result = update_gaze_mapping(
        state.mapping,
        match_count,
        matched_index,
        snapshot.swapchain_generation,
        snapshot.predicted_display_time
    );
    if (mapping_result.invalidated) {
        trace_event(
            "OpenXR gaze mapping invalidated view=%llu",
            static_cast<unsigned long long>(view_id)
        );
    } else if (mapping_result.changed) {
        trace_event(
            "OpenXR gaze mapping changed view=%llu eye=%u generation=%llu",
            static_cast<unsigned long long>(view_id),
            state.mapping.view_index,
            static_cast<unsigned long long>(state.mapping.generation)
        );
    } else if (mapping_result.stable && !mapping_was_stable) {
        trace_event(
            "OpenXR gaze mapping established view=%llu eye=%u route=%s",
            static_cast<unsigned long long>(view_id),
            state.mapping.view_index,
            packed_stereo_match ? "packed-stereo" : "exact-resource"
        );
    }

    for (std::uint32_t index{}; index < CHEEKY_GAZE_MAX_VIEWS; ++index) {
        update_diagnostics_view(index, snapshot);
        if (diagnostics.views[index].dlss_view_id == view_id) {
            diagnostics.views[index].resource_mapped = false;
            diagnostics.views[index].stable_matches = 0U;
            diagnostics.views[index].packed_stereo_mapping = false;
        }
    }
    if (state.mapping.view_index < CHEEKY_GAZE_MAX_VIEWS) {
        auto& view_diagnostics = diagnostics.views[state.mapping.view_index];
        view_diagnostics.dlss_view_id = state.view_id;
        view_diagnostics.stable_matches = state.mapping.consecutive_matches;
        view_diagnostics.resource_mapped = mapping_result.stable;
        view_diagnostics.packed_stereo_mapping = packed_stereo_match;
    }
    const bool mapping_stable = mapping_result.stable &&
        state.mapping.view_index < CHEEKY_GAZE_MAX_VIEWS;
    const bool snapshot_valid =
        (snapshot.status_flags & CHEEKY_GAZE_STATUS_GAZE_VALID) != 0U &&
        (snapshot.status_flags & CHEEKY_GAZE_STATUS_MAPPING_READY) != 0U &&
        (snapshot.status_flags & CHEEKY_GAZE_STATUS_SESSION_FOCUSED) != 0U &&
        sample_age_seconds <= gaze_stale_seconds;
    const bool use_sample = mapping_stable && snapshot_valid;
    const auto fallback = fixed_center(
        fixed_settings, render_width, render_height
    );
    float raw_u = fallback.u;
    float raw_v = fallback.v;
    if (use_sample) {
        const auto& source = snapshot.views[state.mapping.view_index];
        raw_u = source.center_u;
        raw_v = source.center_v;
    }
    const auto temporal_result = update_gaze_temporal_policy(
        state.temporal,
        {
            seconds_between(now, 0U),
            snapshot.predicted_display_time,
            raw_u,
            raw_v,
            fallback.u,
            fallback.v,
            settings.gaze_smoothing_ms,
            gaze_hold_seconds,
            gaze_return_seconds,
            use_sample,
        }
    );
    diagnostics.using_gaze = temporal_result.using_gaze;
    if (!state.temporal.has_filtered) {
        return calculate_crop(
            fixed_settings, render_width, render_height,
            output_width, output_height, output_origin_x, output_origin_y, crop
        );
    }

    if (!calculate_foveation_geometry_at_center(
            foveation_parameters(fixed_settings),
            {
                temporal_result.center_u,
                temporal_result.center_v,
                settings.gaze_quantization_pixels
            },
            render_width, render_height, output_width, output_height,
            output_origin_x, output_origin_y, crop
        )) {
        return false;
    }

    const auto reset_result = evaluate_gaze_reset(
        {
            state.last_crop.input_base_x,
            state.last_crop.input_base_y,
            state.last_crop.input_width,
            state.last_crop.input_height,
            state.has_crop,
        },
        {
            crop.input_base_x,
            crop.input_base_y,
            crop.input_width,
            crop.input_height,
            true,
        },
        use_sample,
        temporal_result.reacquired,
        mapping_result.changed,
        settings.gaze_jump_reset_ratio
    );
    reset_history = reset_result.reason != GazeResetReason::none;
    if (reset_history) {
        diagnostics.last_reset_reason = reset_result.reason;
        trace_event(
            "OpenXR gaze history reset view=%llu reason=%u",
            static_cast<unsigned long long>(view_id),
            static_cast<unsigned int>(reset_result.reason)
        );
    }
    if (state.mapping.view_index < CHEEKY_GAZE_MAX_VIEWS) {
        auto& view_diagnostics = diagnostics.views[state.mapping.view_index];
        view_diagnostics.crop_delta_x = static_cast<std::int32_t>(
            reset_result.delta_x
        );
        view_diagnostics.crop_delta_y = static_cast<std::int32_t>(
            reset_result.delta_y
        );
    }
    state.last_crop = crop;
    state.has_crop = true;
    return true;
}

GazeDiagnostics gaze_diagnostics() noexcept {
    std::lock_guard lock(coordinator_mutex);
    return diagnostics;
}

void forget_gaze_view(const DlssViewId view_id) noexcept {
    std::lock_guard lock(coordinator_mutex);
    view_states.erase(
        std::remove_if(
            view_states.begin(), view_states.end(), [&](const auto& state) {
                return state.view_id == view_id;
            }
        ),
        view_states.end()
    );
    for (auto& view : diagnostics.views) {
        if (view.dlss_view_id == view_id) view = {};
    }
}

void reset_gaze_foveation() noexcept {
    std::lock_guard lock(coordinator_mutex);
    view_states.clear();
    diagnostics = {};
    snapshot_function = nullptr;
}

}  // namespace cheeky::foveated_dlss
