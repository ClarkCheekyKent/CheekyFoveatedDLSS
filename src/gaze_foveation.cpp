#include "gaze_foveation.hpp"

#include "cheeky_gaze_abi.h"
#include "runtime.hpp"
#include "openvr_gaze.hpp"
#include "afw_gaze.hpp"

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

struct AfwGazeState {
    std::array<GazeTemporalPolicyState, 2> temporal{};
    std::array<GazeProjection, 2> projections{};
    std::uint64_t host_generation{}, session{}, swapchain{}, last_display_qpc{}, minimum_publication{};
    std::int64_t display_time{};
    unsigned render_width{}, render_height{}, output_width{}, output_height{}, allocated_width{}, allocated_height{};
    unsigned pattern{}, mode{}, quantum{};
    float width{}, height{}, margin{};
    bool configured{}, openvr{};
};

struct ViewState {
    DlssViewId view_id{};
    GazeMappingPolicyState mapping{};
    GazeTemporalPolicyState temporal{};
    std::int64_t last_snapshot_display_time{};
    std::uint64_t last_snapshot_qpc{};
    bool has_crop{};
    bool calibrated_vertical_flip{};
    bool shared_source{};
    bool next_jump_visible{};
    FoveationOffsets next_jump_offsets{};
    float next_jump_width{}, next_jump_height{};
    FoveationMask afw_mask{};
    unsigned mapping_log_count{};
    bool logged_mapping_ready{};
    std::uint64_t last_mapping_log_qpc{};
    CropGeometry last_crop{};
    CropGeometry afw_last_crop[2]{};
    bool afw_has_crop[2]{};
    AfwGazeState afw{};
};

std::mutex coordinator_mutex;
GazeCopyGraph copy_graph;
struct PendingCopy { std::uint64_t command_list; GazeCopyEdge edge; };
std::deque<PendingCopy> pending_copies;
std::vector<ViewState> view_states;
GazeDiagnostics diagnostics{};
HMODULE snapshot_module{};
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
        HMODULE module{};
        if (GetModuleHandleExW(0U, L"CheekyOpenXRLayer.dll", &module)) {
            const auto function = reinterpret_cast<CheekyOpenXRGetGazeSnapshotFn>(
                GetProcAddress(module, "CheekyOpenXR_GetGazeSnapshot")
            );
            if (function != nullptr) {
                snapshot_module = module;
                snapshot_function = function;
            } else {
                static_cast<void>(FreeLibrary(module));
            }
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
    const auto& source = snapshot.views[index];
    target.xr_resource = source.resource_identity;
    target.xr_x = source.image_rect_x; target.xr_y = source.image_rect_y;
    target.xr_width = source.image_rect_width; target.xr_height = source.image_rect_height;
    target.xr_array = source.array_index;
}

bool calculate_afw_crop(const Settings& settings, DlssViewId view_id, IUnknown* output,
    unsigned rw, unsigned rh, unsigned ow, unsigned oh, unsigned ox, unsigned oy,
    CropGeometry& crop, bool& reset, const CheekyGazeSnapshotV1* supplied, FoveationCenter* resolved) noexcept {
    std::lock_guard lock(coordinator_mutex);
    auto& state = state_for_view(view_id);
    auto& afw = state.afw;
    diagnostics.afw_bilateral = true; diagnostics.afw_fresh_sample = false;
    diagnostics.using_gaze = false; diagnostics.alignment_source = 0;
    for (auto& eye : diagnostics.views) eye = {};
    state.next_jump_visible = false;
    state.afw_mask = settings.afw_mask;
    const auto finish = [&](bool valid, bool sample = false, bool reacquired = false, bool epoch = false) {
        if (!valid) return false;
        if (epoch) state.afw_has_crop[0] = state.afw_has_crop[1] = false;
        const auto source = settings.afw_source_eye;
        const auto previous = source < 2 ? state.afw_last_crop[source] : state.last_crop;
        const bool previous_valid = source < 2 ? state.afw_has_crop[source] : state.has_crop;
        const auto decision = evaluate_gaze_reset(
            {previous.input_base_x, previous.input_base_y, previous.input_width, previous.input_height, previous_valid},
            {crop.input_base_x, crop.input_base_y, crop.input_width, crop.input_height, true},
            sample, reacquired, epoch, settings.gaze_jump_reset_ratio);
        reset = decision.reason != GazeResetReason::none;
        diagnostics.last_reset_reason = decision.reason;
        state.last_crop = crop; state.has_crop = true;
        // Compare gaze movement with the previous observation of this eye.
        // Alternating optical offsets are not gaze jumps. DLSS itself retains
        // the game's shared temporal convention and crop-motion compensation.
        if (source < 2) { state.afw_last_crop[source] = crop; state.afw_has_crop[source] = true; }
        auto selected = settings;
        selected.width = static_cast<float>(crop.input_width) / rw;
        selected.height = static_cast<float>(crop.input_height) / rh;
        const auto offsets = foveation_offsets_from_geometry(crop, rw, rh);
        selected.x_offset = offsets.x; selected.height_offset = offsets.y;
        if (!settings.afw_nr_coverage)
            note_afw_coverage(selected, settings.afw_automatic_coverage && afw_projection_matches_output(afw_stereo_projection(), ow, oh, ox, oy),
                diagnostics.using_gaze);
        if (resolved) *resolved = foveation_center_from_geometry(crop, rw, rh);
        return true;
    };
    const auto fallback = [&] { return finish(calculate_crop(settings, rw, rh, ow, oh, ox, oy, crop)); };
    const auto projection = afw_stereo_projection();
    const bool source_known = afw_has_source_projection(settings, &projection);
    if (!rw || !rh || !ow || !oh) return false;
    if (settings.center_mode == FoveationCenterMode::fixed || !afw_projection_matches_output(projection, ow, oh, ox, oy)) {
        afw = {};
        return fallback();
    }
    if (!qpc_frequency) { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); qpc_frequency = f.QuadPart; }
    const auto now = qpc_now();
    CheekyGazeSnapshotV1 snapshot{};
    bool loaded = supplied ? (snapshot = *supplied, snapshot.abi_version == CHEEKY_GAZE_ABI_VERSION && snapshot.structure_size >= sizeof(snapshot))
        : load_snapshot(snapshot);
    if (!supplied && (!loaded || !snapshot.session_generation)) loaded = read_openvr_gaze(settings, output, snapshot);
    bool projection_changed{};
    for (unsigned eye = 0; eye < 2; ++eye) projection_changed |= !gaze_projection_matches(afw.projections[eye], projection.projections[eye]);
    const bool mode_changed = afw.configured && (afw.mode != static_cast<unsigned>(settings.center_mode) || afw.pattern != settings.simulation_pattern);
    const bool epoch = !afw.configured || mode_changed || projection_changed || afw.host_generation != projection.generation ||
        afw.render_width != rw || afw.render_height != rh || afw.output_width != ow || afw.output_height != oh ||
        afw.width != settings.afw_gaze_width || afw.height != settings.afw_gaze_height ||
        afw.margin != settings.afw_warp_margin ||
        afw.quantum != settings.gaze_quantization_pixels ||
        (loaded && (afw.session != snapshot.session_generation || afw.swapchain != snapshot.swapchain_generation ||
            afw.openvr != ((snapshot.status_flags & CHEEKY_GAZE_STATUS_OPENVR) != 0)));
    if (epoch) {
        afw = {};
        afw.configured = true; afw.projections = projection.projections; afw.host_generation = projection.generation;
        afw.render_width = rw; afw.render_height = rh; afw.output_width = ow; afw.output_height = oh;
        afw.width = settings.afw_gaze_width; afw.height = settings.afw_gaze_height; afw.margin = settings.afw_warp_margin;
        afw.quantum = settings.gaze_quantization_pixels; afw.mode = static_cast<unsigned>(settings.center_mode); afw.pattern = settings.simulation_pattern;
        afw.session = snapshot.session_generation; afw.swapchain = snapshot.swapchain_generation;
        afw.openvr = (snapshot.status_flags & CHEEKY_GAZE_STATUS_OPENVR) != 0;
        afw.minimum_publication = mode_changed ? now : 0;
    }
    if (loaded && afw.display_time != snapshot.predicted_display_time) {
        afw.display_time = snapshot.predicted_display_time; afw.last_display_qpc = now;
    }
    constexpr auto required = CHEEKY_GAZE_STATUS_LAYER_ACTIVE | CHEEKY_GAZE_STATUS_SESSION_FOCUSED | CHEEKY_GAZE_STATUS_GAZE_VALID;
    bool valid = loaded && snapshot.view_count == 2 && snapshot.session_generation && snapshot.predicted_display_time &&
        (snapshot.status_flags & required) == required && !(snapshot.status_flags & CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG) &&
        ((snapshot.status_flags & CHEEKY_GAZE_STATUS_SIMULATED) != 0) == (settings.center_mode == FoveationCenterMode::simulated_gaze) &&
        snapshot.publication_qpc && now >= snapshot.publication_qpc && snapshot.publication_qpc >= afw.minimum_publication &&
        seconds_between(now, snapshot.publication_qpc) <= gaze_stale_seconds && seconds_between(now, afw.last_display_qpc) <= gaze_stale_seconds;
    std::array<FoveationCenter, 2> raw{};
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto& source = snapshot.views[eye];
        valid &= source.structure_size >= sizeof(source) && source.view_index == eye && (source.flags & CHEEKY_GAZE_VIEW_ORIENTATION_VALID) &&
            afw_project_gaze(source, projection.projections[eye], source.center_u, source.center_v, raw[eye]);
    }
    diagnostics.status_flags = loaded ? snapshot.status_flags : 0;
    diagnostics.sample_age_ms = loaded && snapshot.publication_qpc && now >= snapshot.publication_qpc
        ? static_cast<float>(seconds_between(now, snapshot.publication_qpc) * 1000.) : -1.F;
    diagnostics.afw_fresh_sample = valid;
    if (loaded) strncpy_s(diagnostics.runtime_name, snapshot.runtime_name, _TRUNCATE);
    if (!valid && !afw.temporal[0].has_filtered && !afw.temporal[1].has_filtered && !afw.allocated_width) return fallback();
    CropGeometry fixed{};
    if (!calculate_crop(settings, rw, rh, ow, oh, ox, oy, fixed)) return false;
    const auto fixed_center = foveation_center_from_geometry(fixed, rw, rh);
    AfwGazeBounds bounds;
    FoveationMask mask{};
    bool reacquired{}, tracking{};
    const bool focus_lost = loaded && !(snapshot.status_flags & CHEEKY_GAZE_STATUS_SESSION_FOCUSED);
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (focus_lost) afw.temporal[eye] = {};
        const auto eye_fallback = source_known ? afw_project_between_eyes(fixed_center,
            projection.projections[settings.afw_source_eye], projection.projections[eye]) : fixed_center;
        const auto filtered = update_gaze_temporal_policy(afw.temporal[eye],
            {seconds_between(now, 0), snapshot.predicted_display_time, raw[eye].u, raw[eye].v,
                eye_fallback.u, eye_fallback.v, settings.gaze_smoothing_ms, gaze_hold_seconds, gaze_return_seconds, valid});
        reacquired |= filtered.reacquired; tracking |= filtered.using_gaze;
        bounds.include({filtered.center_u, filtered.center_v, 1}, settings.afw_gaze_width, settings.afw_gaze_height);
        afw_mask_include(mask, {filtered.center_u, filtered.center_v, 1}, settings.afw_gaze_width, settings.afw_gaze_height,
            settings.afw_warp_margin, &projection, eye, settings.afw_source_eye);
        // Filtering never cuts the actual fresh gaze out of the sharp region.
        if (valid) {
            bounds.include(raw[eye], settings.afw_gaze_width, settings.afw_gaze_height);
            afw_mask_include(mask, raw[eye], settings.afw_gaze_width, settings.afw_gaze_height, settings.afw_warp_margin,
                &projection, eye, settings.afw_source_eye);
        }
        diagnostics.views[eye].center_u = filtered.center_u; diagnostics.views[eye].center_v = filtered.center_v;
    }
    bounds.pad(settings.afw_warp_margin);
    if (!tracking) bounds.include(fixed_center, static_cast<float>(fixed.input_width) / rw, static_cast<float>(fixed.input_height) / rh);
    state.afw_mask = tracking ? mask : settings.afw_mask;
    if (source_known && state.afw_mask.count) {
        const auto b = afw_mask_extent(state.afw_mask, &projection, settings.afw_source_eye);
        bounds = {b.left, b.top, b.right, b.bottom};
    }
    if (!afw.allocated_width) {
        // Source-eye metadata can briefly disappear. That changes placement,
        // not the allocation chosen when this gaze session began.
        const auto budget = afw_gaze_budget(settings, projection);
        afw.allocated_width = afw_gaze_size(budget.width, rw, settings.gaze_quantization_pixels);
        afw.allocated_height = afw_gaze_size(budget.height, rh, settings.gaze_quantization_pixels);
    }
    // Prefer fresh gaze when smoothing lags outside the fixed budget. Never
    // enlarge the allocation to span a saccade or a tracking-loss transition.
    if (valid) {
        FoveationMask fresh_mask{};
        for (unsigned eye = 0; eye < 2; ++eye)
            afw_mask_include(fresh_mask, raw[eye], settings.afw_gaze_width, settings.afw_gaze_height,
                settings.afw_warp_margin, &projection, eye, settings.afw_source_eye);
        const auto fresh_bounds = afw_mask_extent(fresh_mask);
        const auto fit = [](float center, float lo, float hi, float size) {
            if (hi - lo >= size) return (lo + hi) * .5F;
            return std::clamp(center, hi - size * .5F, lo + size * .5F);
        };
        const float u = fit((bounds.left + bounds.right) * .5F, fresh_bounds.left, fresh_bounds.right, float(afw.allocated_width) / rw);
        const float v = fit((bounds.top + bounds.bottom) * .5F, fresh_bounds.top, fresh_bounds.bottom, float(afw.allocated_height) / rh);
        bounds = {u, v, u, v};
    }
    const auto x = afw_gaze_start((bounds.left + bounds.right) * .5F, rw, afw.allocated_width, settings.gaze_quantization_pixels);
    const auto y = afw_gaze_start((bounds.top + bounds.bottom) * .5F, rh, afw.allocated_height, settings.gaze_quantization_pixels);
    auto parameters = foveation_parameters(settings);
    parameters.width = static_cast<float>(afw.allocated_width) / rw;
    parameters.height = static_cast<float>(afw.allocated_height) / rh;
    if (valid && settings.show_next_jump_target && settings.center_mode == FoveationCenterMode::simulated_gaze &&
            (settings.simulation_pattern == 2U || settings.simulation_pattern == 3U)) {
        AfwGazeBounds next;
        FoveationMask next_mask{};
        bool next_valid = true;
        for (unsigned eye = 0; eye < 2; ++eye) {
            FoveationCenter target;
            const auto& source = snapshot.views[eye];
            const bool projected = (source.flags & CHEEKY_GAZE_VIEW_NEXT_JUMP_VALID) &&
                afw_project_gaze(source, projection.projections[eye], source.next_jump_u, source.next_jump_v, target);
            next_valid &= projected;
            if (projected) {
                next.include(target, settings.afw_gaze_width, settings.afw_gaze_height);
                afw_mask_include(next_mask, target, settings.afw_gaze_width, settings.afw_gaze_height, settings.afw_warp_margin,
                    &projection, eye, settings.afw_source_eye);
            }
        }
        if (next_valid) {
            next.pad(settings.afw_warp_margin);
            const auto nw = afw.allocated_width, nh = afw.allocated_height;
            if (source_known) {
                const auto b = afw_mask_extent(next_mask, &projection, settings.afw_source_eye);
                next = {b.left, b.top, b.right, b.bottom};
            }
            const auto nx = afw_gaze_start((next.left + next.right) * .5F, rw, nw, settings.gaze_quantization_pixels);
            const auto ny = afw_gaze_start((next.top + next.bottom) * .5F, rh, nh, settings.gaze_quantization_pixels);
            CropGeometry preview{nx, ny, nw, nh};
            state.next_jump_visible = true;
            state.next_jump_offsets = foveation_offsets_from_geometry(preview, rw, rh);
            state.next_jump_width = static_cast<float>(nw) / rw;
            state.next_jump_height = static_cast<float>(nh) / rh;
        }
    }
    diagnostics.using_gaze = tracking;
    return finish(calculate_foveation_geometry_at_center(parameters,
        {(x + afw.allocated_width * .5F) / rw, (y + afw.allocated_height * .5F) / rh, 1},
        rw, rh, ow, oh, ox, oy, crop), valid, reacquired, epoch);
}

}  // namespace

thread_local const ScopedCoordinatedCrop* coordinated_crop_override{};
ScopedCoordinatedCrop::ScopedCoordinatedCrop(DlssViewId view, const CropGeometry& value,
    bool reset_history, const FoveationCenter* resolved_center) noexcept : view_id(view), crop(value), reset(reset_history),
    center(resolved_center ? *resolved_center : FoveationCenter{}), has_center(resolved_center != nullptr),
    previous(coordinated_crop_override) { coordinated_crop_override = this; }
ScopedCoordinatedCrop::~ScopedCoordinatedCrop() { coordinated_crop_override = previous; }

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
    bool& reset_history,
    const CheekyGazeSnapshotV1* supplied_snapshot,
    FoveationCenter* resolved_center,
    std::uint64_t native_resource_identity
) noexcept {
    if (coordinated_crop_override && coordinated_crop_override->view_id == view_id && view_id != 0U) {
        crop = coordinated_crop_override->crop;
        reset_history = coordinated_crop_override->reset;
        if (resolved_center) *resolved_center = coordinated_crop_override->has_center
            ? coordinated_crop_override->center
            : foveation_center_from_geometry(crop, render_width, render_height);
        return true;
    }
    reset_history = false;
    const auto fixed_settings = settings_for_view(settings, view_id);
    if (resolved_center) *resolved_center = fixed_center(fixed_settings, render_width, render_height);
    const auto eye_assignment = stereo_eye_assignment(view_id);
    if (const auto module = GetModuleHandleW(L"CheekyOpenXRLayer.dll")) {
        using SetSimulationFn = void(__cdecl*)(std::uint32_t);
        const auto set_simulation = reinterpret_cast<SetSimulationFn>(
            GetProcAddress(module, "CheekyOpenXR_SetSimulatedGaze"));
        const auto set_pattern = reinterpret_cast<SetSimulationFn>(
            GetProcAddress(module, "CheekyOpenXR_SetSimulationPattern"));
        if (set_pattern != nullptr) set_pattern(settings.simulation_pattern);
        if (set_simulation != nullptr) {
            set_simulation(settings.center_mode == FoveationCenterMode::simulated_gaze ? 1U : 0U);
        }
    }
    if (settings.eye_independent_coverage) {
        return calculate_afw_crop(settings, view_id, output_resource, render_width, render_height,
            output_width, output_height, output_origin_x, output_origin_y, crop, reset_history, supplied_snapshot, resolved_center);
    }
    if (!uses_coordinated_center(settings)) {
        std::lock_guard lock(coordinator_mutex);
        diagnostics.alignment_source = 0U;
        diagnostics.using_gaze = false;
        if (eye_assignment.assigned && eye_assignment.eye_index < diagnostics.views.size()) {
            auto& view = diagnostics.views[eye_assignment.eye_index];
            const auto center = fixed_center(fixed_settings, render_width, render_height);
            view.alignment_source = 0U;
            view.aligned_u = center.u; view.aligned_v = center.v;
        }
        return calculate_foveation_geometry(
            foveation_parameters(fixed_settings), render_width, render_height,
            output_width, output_height, output_origin_x, output_origin_y, crop
        );
    }

    std::lock_guard lock(coordinator_mutex);
    state_for_view(view_id).next_jump_visible = false;
    const bool automatic = settings.auto_stereo_alignment;
    const auto camera = active_gaze_projection.view == view_id ?
        active_gaze_projection.projection : GazeProjection{};
    // A projection belongs to the current DLSS view, so this route does not
    // depend on guessed left/right evaluation order. Require stereo and a
    // full local view; packed subrect projections need explicit XR mapping.
    bool calibrated_vertical_flip{};
    const auto aligned_center = [&](const CheekyGazeViewV1* xr_view) {
        auto center = fixed_center(fixed_settings, render_width, render_height);
        float u{}, v{};
        unsigned source{};
        if (automatic && xr_view && (xr_view->flags & CHEEKY_GAZE_VIEW_FORWARD_VALID) != 0U &&
            std::isfinite(xr_view->forward_u) && std::isfinite(xr_view->forward_v)) {
            u = xr_view->forward_u;
            v = calibrated_vertical_flip ? 1.F - xr_view->forward_v : xr_view->forward_v;
            source = (diagnostics.status_flags & CHEEKY_GAZE_STATUS_OPENVR) != 0U ? 3U : 2U;
        } else if (automatic && has_multiple_stereo_views() && output_origin_x == 0U && output_origin_y == 0U &&
            projection_forward_center(camera, u, v)) {
            source = 1U;
        }
        diagnostics.alignment_source = source;
        if (source != 0U) center = {u, v, 1U};
        // A user bias for fixed placement (including gaze-loss fallback),
        // never added to a valid gaze sample. Independent of fovea size.
        if (automatic) {
            center.v = std::clamp(center.v + 0.5F * settings.aligned_height_offset, 0.F, 1.F);
            center.quantization_pixels = 1U;
        }
        const auto index = xr_view ? xr_view->view_index : eye_assignment.eye_index;
        if ((xr_view || eye_assignment.assigned) && index < diagnostics.views.size()) {
            auto& view = diagnostics.views[index];
            view.alignment_source = source;
            view.aligned_u = center.u; view.aligned_v = center.v;
            if (xr_view && eye_assignment.shared_source) {
                auto& other = diagnostics.views[1 - index];
                other.alignment_source = source;
                other.aligned_u = center.u; other.aligned_v = center.v;
            }
        }
        return center;
    };
    const auto auto_crop = [&](const CheekyGazeViewV1* xr_view, bool remapped = false) {
        const auto center = aligned_center(xr_view);
        if (resolved_center) *resolved_center = center;
        const bool valid = diagnostics.alignment_source == 0U && settings.aligned_height_offset == 0.F
            ? calculate_foveation_geometry(foveation_parameters(fixed_settings), render_width, render_height, output_width,
                output_height, output_origin_x, output_origin_y, crop)
            : calculate_foveation_geometry_at_center(foveation_parameters(fixed_settings),
                center, render_width, render_height, output_width, output_height,
                output_origin_x, output_origin_y, crop);
        diagnostics.using_gaze = false;
        if (valid) {
            auto& state = state_for_view(view_id);
            // Automatic alignment can fluctuate by a pixel. Preserve history
            // through the backend's crop-motion correction, just as gaze does;
            // only mapping changes, resizing and large jumps require a reset.
            const auto decision = evaluate_gaze_reset(
                {state.last_crop.input_base_x, state.last_crop.input_base_y,
                    state.last_crop.input_width, state.last_crop.input_height, state.has_crop},
                {crop.input_base_x, crop.input_base_y, crop.input_width, crop.input_height, true},
                false, false, remapped, settings.gaze_jump_reset_ratio);
            reset_history = decision.reason != GazeResetReason::none;
            if (reset_history) {
                diagnostics.last_reset_reason = decision.reason;
                trace_event("VR alignment history reset view=%llu reason=%u",
                    static_cast<unsigned long long>(view_id), static_cast<unsigned>(decision.reason));
            }
            state.last_crop = crop;
            state.has_crop = true;
        }
        return valid;
    };
    if (qpc_frequency == 0U) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
    }
    CheekyGazeSnapshotV1 snapshot{};
    // Callers can supply a frame snapshot; otherwise read the live layer.
    bool loaded = supplied_snapshot
        ? (snapshot = *supplied_snapshot, snapshot.abi_version == CHEEKY_GAZE_ABI_VERSION &&
            snapshot.structure_size >= sizeof(snapshot))
        : load_snapshot(snapshot);
    if (!supplied_snapshot && (!loaded || snapshot.session_generation == 0U)) {
        if (read_openvr_gaze(settings, output_resource, snapshot,native_resource_identity)) {
            loaded = true;
            diagnostics.layer_present = true; // Runtime adapter present; UI labels this generically.
            diagnostics.abi_compatible = true;
        }
    }
    if (!loaded) {
        if (automatic) return auto_crop(nullptr);
        diagnostics.using_gaze = false;
        return calculate_foveation_geometry(
            foveation_parameters(fixed_settings), render_width, render_height,
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

    const auto resource_identity = native_resource_identity?native_resource_identity:canonical_identity(output_resource);
    std::uint32_t matched_index{UINT32_MAX};
    std::uint32_t match_count{};
    bool packed_stereo_match{};
    bool copy_match{};
    bool projection_match{};
    const bool openvr_snapshot = (snapshot.status_flags & CHEEKY_GAZE_STATUS_OPENVR) != 0U;
    const bool marker_match = eye_assignment.calibrated && snapshot.view_count == 2U &&
        (openvr_snapshot ? eye_assignment.calibration_session == 0 :
            eye_assignment.calibration_session != 0 && eye_assignment.calibration_session == snapshot.session_generation);
    calibrated_vertical_flip = marker_match && eye_assignment.vertical_flip;
    const bool shared_source = marker_match && eye_assignment.shared_source;
    // Both submitted images were verified to contain the same source. That
    // source gets one binocular center, not an arbitrary left/right role.
    CheekyGazeViewV1 shared_view = snapshot.views[0];
    if (shared_source) {
        const auto& other = snapshot.views[1];
        shared_view.flags &= other.flags;
        shared_view.center_u = (shared_view.center_u + other.center_u) * .5F;
        shared_view.center_v = (shared_view.center_v + other.center_v) * .5F;
        shared_view.forward_u = (shared_view.forward_u + other.forward_u) * .5F;
        shared_view.forward_v = (shared_view.forward_v + other.forward_v) * .5F;
        shared_view.next_jump_u = (shared_view.next_jump_u + other.next_jump_u) * .5F;
        shared_view.next_jump_v = (shared_view.next_jump_v + other.next_jump_v) * .5F;
    }
    if (marker_match) { matched_index = eye_assignment.eye_index; match_count = 1U; }
    std::array<GazeProjection, 2> xr_projections{};
    for (unsigned i = 0; i < (std::min)(snapshot.view_count, CHEEKY_GAZE_MAX_VIEWS); ++i) {
        const auto& v = snapshot.views[i];
        xr_projections[i] = {std::tan(v.fov_left), std::tan(v.fov_right),
            std::tan(v.fov_up), std::tan(v.fov_down), (v.flags & CHEEKY_GAZE_VIEW_FOV_VALID) != 0U};
    }
    for (std::uint32_t index{};
         index < (std::min)(snapshot.view_count, CHEEKY_GAZE_MAX_VIEWS);
         ++index) {
        if (marker_match) break;
        if (exact_view_match(
                snapshot.views[index], resource_identity,
                output_origin_x, output_origin_y,
                output_width, output_height
            )) {
            matched_index = index;
            ++match_count;
        }
    }
    if (match_count == 0U) {
        const GazeCopyRegion source{resource_identity, 0U, output_origin_x,
            output_origin_y, output_width, output_height};
        for (std::uint32_t index{}; index < (std::min)(snapshot.view_count, CHEEKY_GAZE_MAX_VIEWS); ++index) {
            const auto& target = snapshot.views[index];
            if ((target.flags & CHEEKY_GAZE_VIEW_RESOURCE_VALID) == 0U ||
                target.array_index != 0U || target.image_rect_x < 0 || target.image_rect_y < 0) continue;
            if (copy_graph.reaches(source, {target.resource_identity, 0U,
                    static_cast<std::uint32_t>(target.image_rect_x),
                    static_cast<std::uint32_t>(target.image_rect_y),
                    target.image_rect_width, target.image_rect_height}, GetTickCount64())) {
                matched_index = index;
                ++match_count;
                copy_match = true;
            }
        }
    }
    if (match_count == 0U && camera.valid && snapshot.view_count == 2 &&
        xr_projections[0].valid && xr_projections[1].valid &&
        output_origin_x == 0 && output_origin_y == 0 &&
        (snapshot.status_flags & CHEEKY_GAZE_STATUS_MAPPING_READY) != 0U &&
        (snapshot.status_flags & CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG) == 0U) {
        const auto projection_result = match_gaze_projection_eyes(camera, xr_projections);
        match_count = projection_result.count;
        matched_index = projection_result.index;
        if (match_count == 1U) {
            const auto& v = snapshot.views[matched_index];
            if (v.image_rect_width == output_width && v.image_rect_height == output_height)
                projection_match = true;
            else match_count = 0U;
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
    const bool calibration_changed = state.calibrated_vertical_flip != calibrated_vertical_flip ||
        state.shared_source != shared_source;
    if (calibration_changed) {
        state.calibrated_vertical_flip = calibrated_vertical_flip;
        state.shared_source = shared_source;
        state.temporal = {};
    }
    const bool mapping_ready = (snapshot.status_flags & CHEEKY_GAZE_STATUS_MAPPING_READY) != 0U;
    if (mapping_ready != state.logged_mapping_ready) {
        state.logged_mapping_ready = mapping_ready;
        state.mapping_log_count = 0U;
    }
    // Capture actual inputs on a bounded schedule, including intermediate outputs.
    if (match_count != 1U && state.mapping_log_count < 8U &&
        (state.mapping_log_count == 0U || seconds_between(now, state.last_mapping_log_qpc) >= 2.0)) {
        ++state.mapping_log_count;
        state.last_mapping_log_qpc = now;
        trace_event("Gaze projection view=%llu valid=%u tangents=(%.6f,%.6f,%.6f,%.6f) XR0 valid=%u tangents=(%.6f,%.6f,%.6f,%.6f) XR1 valid=%u tangents=(%.6f,%.6f,%.6f,%.6f)",
            static_cast<unsigned long long>(view_id), camera.valid ? 1U : 0U,
            camera.left, camera.right, camera.up, camera.down,
            xr_projections[0].valid ? 1U : 0U, xr_projections[0].left, xr_projections[0].right, xr_projections[0].up, xr_projections[0].down,
            xr_projections[1].valid ? 1U : 0U, xr_projections[1].left, xr_projections[1].right, xr_projections[1].up, xr_projections[1].down);
        trace_event("Gaze mapping rejected DLSS view=%llu resource=0x%llX rect=(%u,%u %ux%u) stereo_assigned=%u eye=%u flags=0x%X views=%u",
            static_cast<unsigned long long>(view_id), static_cast<unsigned long long>(resource_identity),
            output_origin_x, output_origin_y, output_width, output_height,
            eye_assignment.assigned ? 1U : 0U, eye_assignment.eye_index, snapshot.status_flags, snapshot.view_count);
        for (unsigned i = 0; i < (std::min)(snapshot.view_count, CHEEKY_GAZE_MAX_VIEWS); ++i) {
            const auto& v = snapshot.views[i];
            trace_event("Gaze mapping XR eye=%u resource=0x%llX swapchain=0x%llX rect=(%d,%d %ux%u) array=%u flags=0x%X",
                i, static_cast<unsigned long long>(v.resource_identity), static_cast<unsigned long long>(v.swapchain_identity),
                v.image_rect_x, v.image_rect_y, v.image_rect_width, v.image_rect_height, v.array_index, v.flags);
        }
        // One bounded graph dump per view after VR has settled. This captures
        // intermediate resources as well as the two endpoints; a total-copy
        // counter alone cannot explain why a route was rejected.
        if (mapping_ready && state.mapping_log_count == 4U) {
            const auto copy_now = GetTickCount64();
            const auto& edges = copy_graph.recent_edges(copy_now);
            trace_event("Gaze copy graph view=%llu retained=%llu capacity=512 pending=%llu pendingCapacity=2048 submitted=%llu maxAgeMs=500 maxHops=4",
                static_cast<unsigned long long>(view_id),
                static_cast<unsigned long long>(edges.size()),
                static_cast<unsigned long long>(pending_copies.size()),
                static_cast<unsigned long long>(diagnostics.submitted_copies));
            for (const auto& edge : edges) {
                const auto& s = edge.source; const auto& d = edge.destination;
                trace_event("Gaze copy edge seq=%llu ageMs=%llu src=0x%llX sub=%u rect=(%u,%u %ux%u) dst=0x%llX sub=%u rect=(%u,%u %ux%u)",
                    static_cast<unsigned long long>(edge.sequence),
                    static_cast<unsigned long long>(copy_now - edge.time_ms),
                    static_cast<unsigned long long>(s.resource), s.subresource, s.x, s.y, s.width, s.height,
                    static_cast<unsigned long long>(d.resource), d.subresource, d.x, d.y, d.width, d.height);
            }
        }
    }
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
            "VR gaze mapping invalidated view=%llu",
            static_cast<unsigned long long>(view_id)
        );
    } else if (mapping_result.changed) {
        if (marker_match) state.temporal = {};
        trace_event(
            "VR gaze mapping changed view=%llu eye=%u generation=%llu",
            static_cast<unsigned long long>(view_id),
            state.mapping.view_index,
            static_cast<unsigned long long>(state.mapping.generation)
        );
    } else if (mapping_result.stable && !mapping_was_stable) {
        trace_event(
            "VR gaze mapping established view=%llu eye=%u route=%s",
            static_cast<unsigned long long>(view_id),
            state.mapping.view_index,
            marker_match ? "pixel-marker" : projection_match ? "camera-projection" : copy_match ? "submitted-copy" : packed_stereo_match ? "packed-stereo" : "exact-resource"
        );
    }

    for (std::uint32_t index{}; index < CHEEKY_GAZE_MAX_VIEWS; ++index) {
        update_diagnostics_view(index, snapshot);
        if (diagnostics.views[index].dlss_view_id == view_id) {
            diagnostics.views[index].resource_mapped = false;
            diagnostics.views[index].stable_matches = 0U;
            diagnostics.views[index].packed_stereo_mapping = false;
            diagnostics.views[index].copy_mapping = false;
            diagnostics.views[index].projection_mapping = false;
            diagnostics.views[index].marker_mapping = false;
        }
    }
    if (eye_assignment.assigned && eye_assignment.eye_index < CHEEKY_GAZE_MAX_VIEWS) {
        auto& candidate = diagnostics.views[eye_assignment.eye_index];
        candidate.has_candidate = true;
        candidate.candidate_view = view_id; candidate.candidate_resource = resource_identity;
        candidate.candidate_x = output_origin_x; candidate.candidate_y = output_origin_y;
        candidate.candidate_width = output_width; candidate.candidate_height = output_height;
    }
    for (unsigned index = 0; index < CHEEKY_GAZE_MAX_VIEWS; ++index) {
        if (index != state.mapping.view_index && !shared_source) continue;
        auto& view_diagnostics = diagnostics.views[index];
        view_diagnostics.dlss_view_id = state.view_id;
        view_diagnostics.stable_matches = state.mapping.consecutive_matches;
        view_diagnostics.resource_mapped = mapping_result.stable;
        view_diagnostics.packed_stereo_mapping = packed_stereo_match;
        view_diagnostics.copy_mapping = copy_match;
        view_diagnostics.projection_mapping = projection_match;
        view_diagnostics.marker_mapping = marker_match;
        const auto& projected = snapshot.views[index];
        view_diagnostics.submitted_projection = (projected.flags & CHEEKY_GAZE_VIEW_SUBMITTED_PROJECTION) != 0;
        view_diagnostics.fov_tangents = {std::tan(projected.fov_left), std::tan(projected.fov_right),
            std::tan(projected.fov_up), std::tan(projected.fov_down)};
    }
    const bool mapping_stable = mapping_result.stable &&
        state.mapping.view_index < CHEEKY_GAZE_MAX_VIEWS;
    // Alignment is independent of gaze availability. The packed bridge route
    // uses the existing eye roles (and manual inversion override).
    // Forward alignment describes the stereo optics, not a live gaze sample.
    // Slow NR frames and menu focus changes must not alternate between this
    // center and the manual offsets. Keep validating the mapping and view
    // configuration; only gaze requires a fresh, focused publication.
    const bool alignment_usable = mapping_stable && snapshot.view_count == 2U &&
            ((snapshot.status_flags & CHEEKY_GAZE_STATUS_MAPPING_READY) != 0U || marker_match) &&
            (snapshot.status_flags & CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG) == 0U &&
            ((snapshot.status_flags & CHEEKY_GAZE_STATUS_AMBIGUOUS_RESOURCE) == 0U || marker_match);
    const bool usable = alignment_usable &&
            (snapshot.status_flags & CHEEKY_GAZE_STATUS_SESSION_FOCUSED) != 0U &&
            snapshot.predicted_display_time != 0 && snapshot.publication_qpc != 0U &&
            now >= snapshot.publication_qpc &&
            seconds_between(now, snapshot.publication_qpc) <= gaze_stale_seconds &&
            sample_age_seconds <= gaze_stale_seconds;
    const auto* alignment_view = alignment_usable ?
        (shared_source ? &shared_view : &snapshot.views[state.mapping.view_index]) : nullptr;
    if (settings.center_mode == FoveationCenterMode::fixed)
        return auto_crop(alignment_view,
            mapping_result.changed || mapping_result.invalidated || calibration_changed);
    const bool source_matches =
        ((snapshot.status_flags & CHEEKY_GAZE_STATUS_SIMULATED) != 0U) ==
        (settings.center_mode == FoveationCenterMode::simulated_gaze);
    const bool snapshot_valid = usable && source_matches &&
        (snapshot.status_flags & CHEEKY_GAZE_STATUS_GAZE_VALID) != 0U &&
        (snapshot.status_flags & CHEEKY_GAZE_STATUS_MAPPING_READY) != 0U &&
        (snapshot.status_flags & CHEEKY_GAZE_STATUS_SESSION_FOCUSED) != 0U &&
        sample_age_seconds <= gaze_stale_seconds;
    const bool use_sample = mapping_stable && snapshot_valid && (!shared_source ||
        (std::isfinite(shared_view.center_u) && std::isfinite(shared_view.center_v)));
    if (use_sample && settings.show_next_jump_target &&
        settings.center_mode == FoveationCenterMode::simulated_gaze &&
        (settings.simulation_pattern == 2U || settings.simulation_pattern == 3U)) {
        const auto& target = shared_source ? shared_view : snapshot.views[state.mapping.view_index];
        CropGeometry next_crop{};
        if ((target.flags & CHEEKY_GAZE_VIEW_NEXT_JUMP_VALID) != 0U &&
            calculate_foveation_geometry_at_center(foveation_parameters(fixed_settings),
                {target.next_jump_u, calibrated_vertical_flip ? 1.F - target.next_jump_v : target.next_jump_v,
                    settings.gaze_quantization_pixels},
                render_width, render_height, output_width, output_height,
                output_origin_x, output_origin_y, next_crop)) {
            state.next_jump_visible = true;
            state.next_jump_offsets = foveation_offsets_from_geometry(next_crop, render_width, render_height);
        }
    }
    const auto fallback = aligned_center(alignment_view);
    float raw_u = fallback.u;
    float raw_v = fallback.v;
    if (use_sample) {
        const auto& source = shared_source ? shared_view : snapshot.views[state.mapping.view_index];
        raw_u = source.center_u;
        raw_v = calibrated_vertical_flip ? 1.F - source.center_v : source.center_v;
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
        return auto_crop(alignment_view,
            mapping_result.changed || mapping_result.invalidated || calibration_changed);
    }

    if (resolved_center) *resolved_center = {temporal_result.center_u, temporal_result.center_v,
        settings.gaze_quantization_pixels};
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
            "VR gaze history reset view=%llu reason=%u",
            static_cast<unsigned long long>(view_id),
            static_cast<unsigned int>(reset_result.reason)
        );
    }
    if (eye_assignment.assigned && eye_assignment.eye_index < CHEEKY_GAZE_MAX_VIEWS) {
        auto& candidate = diagnostics.views[eye_assignment.eye_index];
        candidate.has_candidate = true;
        candidate.candidate_view = view_id; candidate.candidate_resource = resource_identity;
        candidate.candidate_x = output_origin_x; candidate.candidate_y = output_origin_y;
        candidate.candidate_width = output_width; candidate.candidate_height = output_height;
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

bool calculate_coordinated_center(
    const Settings& settings, DlssViewId view_id, IUnknown* output_resource,
    std::uint32_t render_width, std::uint32_t render_height,
    std::uint32_t output_width, std::uint32_t output_height,
    std::uint32_t output_origin_x, std::uint32_t output_origin_y,
    FoveationCenter& center, bool& reset_history,
    const CheekyGazeSnapshotV1* supplied_snapshot) noexcept {
    CropGeometry placement{};
    if (!calculate_coordinated_crop(settings, view_id, output_resource,
            render_width, render_height, output_width, output_height,
            output_origin_x, output_origin_y, placement, reset_history, supplied_snapshot, &center)) return false;
    return true;
}

void apply_next_jump_preview(Settings& settings, const DlssViewId view_id) noexcept {
    settings.next_jump_visible = false;
    const bool preview = settings.show_next_jump_target && settings.center_mode == FoveationCenterMode::simulated_gaze;
    if (!preview && !settings.eye_independent_coverage) return;
    std::lock_guard lock(coordinator_mutex);
    for (const auto& state : view_states) if (state.view_id == view_id) {
        if (settings.eye_independent_coverage) settings.afw_mask = state.afw_mask;
        settings.next_jump_visible = preview && state.next_jump_visible;
        settings.next_jump_offset_x = state.next_jump_offsets.x;
        settings.next_jump_offset_y = state.next_jump_offsets.y;
        settings.next_jump_width = settings.eye_independent_coverage ? state.next_jump_width : settings.width;
        settings.next_jump_height = settings.eye_independent_coverage ? state.next_jump_height : settings.height;
        break;
    }
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
                return state.view_id == view_id || state.view_id == afw_nr_gaze_view(view_id);
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
    copy_graph.clear();
    pending_copies.clear();
    diagnostics = {};
    snapshot_function = nullptr;
    if (snapshot_module != nullptr) {
        static_cast<void>(FreeLibrary(snapshot_module));
        snapshot_module = nullptr;
    }
}

void record_gaze_copy(std::uint64_t command_list, GazeCopyEdge edge) noexcept {
    std::lock_guard lock(coordinator_mutex);
    pending_copies.push_back({command_list, edge});
    if (pending_copies.size() > 2048U) pending_copies.pop_front();
}
void submit_gaze_copies(std::uint64_t command_list) noexcept {
    std::lock_guard lock(coordinator_mutex);
    const auto now = GetTickCount64();
    // Closed lists can be submitted again without being recorded again.
    // Retain their edges until reset/destruction, subject to the bounded cache.
    for (const auto& pending : pending_copies) {
        if (pending.command_list == command_list) {
            copy_graph.record(pending.edge, now);
            ++diagnostics.submitted_copies;
        }
    }
}
void reset_gaze_copies(std::uint64_t command_list) noexcept {
    std::lock_guard lock(coordinator_mutex);
    std::erase_if(pending_copies, [=](const auto& p) { return p.command_list == command_list; });
}
void forget_gaze_resource(std::uint64_t resource) noexcept {
    std::lock_guard lock(coordinator_mutex);
    copy_graph.forget(resource);
    std::erase_if(pending_copies, [=](const auto& p) {
        return p.edge.source.resource == resource || p.edge.destination.resource == resource;
    });
}

}  // namespace cheeky::foveated_dlss
