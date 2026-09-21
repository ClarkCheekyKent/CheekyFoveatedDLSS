#include "cheeky_gaze_abi.h"
#include "d3d12_ngx_dispatch.hpp"
#include "afw_compatibility.hpp"
#include "afw_gaze.hpp"
#include "afw_eye_identity.hpp"
#include "crop_motion.hpp"
#include "afw_warp_abi.hpp"
#include "afw_warp_runtime.hpp"
#include "ngx_runtime_discovery.hpp"
#include "d3d12_output_contract.hpp"
#include "diagnostics.hpp"
#include "dlss_nr_contract.hpp"
#include "foveation.hpp"
#include "gaze_foveation.hpp"
#include "gaze_math.hpp"
#include "gaze_policy.hpp"
#include "streamline_viewport.hpp"
#include "streamline_create_extent.hpp"
#include "openvr_gaze.hpp"
#include "openvr_gaze_math.hpp"
#include "graphics_observer.hpp"
#include "ngx_evaluation_extent.hpp"
#include "motion_region.hpp"
#include "mock_ngx_parameters.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <filesystem>

namespace cheeky::foveated_dlss {
// The core GPU harness explicitly drives post-submit/reset notifications.
NativeObserverStatus native_observer_status() noexcept { return {true}; }
bool ensure_native_observer(ID3D12GraphicsCommandList*) noexcept { return true; }
std::uint64_t observe_native_resource(ID3D12Resource* resource) noexcept { return reinterpret_cast<std::uint64_t>(resource); }
bool initialize_native_observer(ID3D12Device*, ID3D12CommandQueue*) noexcept { return true; }

void trace_event(const char*, ...) noexcept {}
// Runtime discovery is excluded from deterministic coordinator tests. Live
// OpenVR acquisition is tested separately, with snapshots exercising shared policy here.
const CheekyGazeSnapshotV1* test_openvr_snapshot{};
bool read_openvr_gaze(const Settings&, IUnknown*, CheekyGazeSnapshotV1& output, std::uint64_t) noexcept {
    if (!test_openvr_snapshot) return false;
    output=*test_openvr_snapshot;
    return true;
}

}  // namespace cheeky::foveated_dlss

namespace {

int failures{};

struct D3D12DispatchHarness {
    int original_calls{};
    int processor_calls{};
    bool nest_core_evaluation{};
    bool active_during_processor{};
    cheeky::foveated_dlss::D3D12NgxRoute observed_route{
        cheeky::foveated_dlss::D3D12NgxRoute::unknown
    };
};

D3D12DispatchHarness* dispatch_harness{};

cheeky::foveated_dlss::NgxResult fake_d3d12_original(
    ID3D12GraphicsCommandList*,
    const cheeky::foveated_dlss::NgxHandle*,
    const cheeky::foveated_dlss::NgxParameters*,
    cheeky::foveated_dlss::NgxProgressCallback
) {
    ++dispatch_harness->original_calls;
    return 0x100U;
}

cheeky::foveated_dlss::NgxResult fake_d3d12_processor(
    const cheeky::foveated_dlss::D3D12NgxEvaluationCall& call,
    cheeky::foveated_dlss::D3D12NgxEvaluateFn original,
    void* const context
) {
    using namespace cheeky::foveated_dlss;
    auto& harness = *static_cast<D3D12DispatchHarness*>(context);
    ++harness.processor_calls;
    harness.active_during_processor = d3d12_ngx_interception_active();
    harness.observed_route = call.route;
    if (harness.nest_core_evaluation) {
        AfwPrivateWorkScope private_work;
        const D3D12NgxEvaluationCall nested{
            D3D12NgxRoute::core_runtime,
            call.command_list,
            call.handle,
            call.parameters,
            call.callback,
        };
        return dispatch_d3d12_ngx_evaluation(
            nested, original, &fake_d3d12_processor, context
        );
    }
    return 0x200U;
}

void expect(const bool condition, const char* const message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void expect_near(
    const float actual,
    const float expected,
    const float tolerance,
    const char* const message
) {
    expect(std::fabs(actual - expected) <= tolerance, message);
}

void test_sr_crop_dimensions_during_gaze() {
    using namespace cheeky::foveated_dlss;
    // Include the reported 99% crop, fractional scaling, unity scaling,
    // packed-output origins, and full-size crops at both image boundaries.
    for (const auto output_width : {3072U, 1782U, 1001U}) {
        for (const float size : {0.99F, 0.68F, 1.0F}) {
            for (const unsigned quantum : {1U, 8U}) {
                FoveationParameters parameters{};
                parameters.width = parameters.height = size;
                FoveationGeometry first{};
                expect(calculate_foveation_geometry_at_center(parameters,
                    {0.0F, 0.0F, quantum}, 1782, 1894, output_width, 3264,
                    3072, 17, first), "SR initial crop is valid");
                for (unsigned step = 0; step <= 4096; ++step) {
                    const float center = step / 4096.0F;
                    FoveationGeometry crop{};
                    expect(calculate_foveation_geometry_at_center(parameters,
                        {center, center, quantum}, 1782, 1894, output_width, 3264,
                        3072, 17, crop), "SR gaze sweep crop is valid");
                    expect(crop.input_width == first.input_width &&
                        crop.input_height == first.input_height &&
                        crop.output_width == first.output_width &&
                        crop.output_height == first.output_height,
                        "SR dimensions stay constant throughout gaze sweep");
                    expect(crop.output_base_x >= 3072 && crop.output_base_y >= 17 &&
                        crop.output_base_x + crop.output_width <= 3072 + output_width &&
                        crop.output_base_y + crop.output_height <= 17 + 3264,
                        "SR moving crop stays inside its output viewport");
                    const auto enlarged = supersampled_crop(crop, 1.5F);
                    const auto first_enlarged = supersampled_crop(first, 1.5F);
                    expect(enlarged.output_width == first_enlarged.output_width &&
                        enlarged.output_height == first_enlarged.output_height,
                        "supersampled SR dimensions stay constant during movement");
                    if (step == 4096 && quantum == 1U) {
                        expect(crop.output_base_x + crop.output_width == 3072 + output_width &&
                            crop.output_base_y + crop.output_height == 17 + 3264,
                            "SR crop reaches right and bottom edges without shrinking");
                    }
                }
                FoveationGeometry fixed{};
                expect(calculate_foveation_geometry(parameters, 1782, 1894,
                    output_width, 3264, 3072, 17, fixed) &&
                    fixed.output_width == first.output_width && fixed.output_height == first.output_height,
                    "fixed and gaze modes use the same SR dimensions");
                if (size == 0.99F && output_width == 3072U) {
                    expect(first.output_width == 3041 && first.output_height == 3232,
                        "reported SR crop retains its expected output size");
                }
            }
        }
    }
}

void test_simulated_gaze() {
    using namespace cheeky::gaze_math;
    const Fov fov{-0.8F, 0.8F, 0.8F, -0.8F};
    const Pose head{{0.0F, 0.70710678F, 0.0F, 0.70710678F}, {}};
    float u{}, v{}, turned_u{}, turned_v{};
    for (int step = 0; step <= 32; ++step) {
        const double t = step * 0.25;
        expect(project_gaze_to_view(simulated_gaze_pose({}, t), {}, fov, u, v),
            "mock projects throughout its loop");
        expect(project_gaze_to_view(simulated_gaze_pose(head, t), head, fov, turned_u, turned_v),
            "mock projects with head rotation");
        expect_near(u, turned_u, 0.0001F, "mock horizontal motion follows head");
        expect_near(v, turned_v, 0.0001F, "mock vertical motion follows head");
        expect(u > 0.0F && u < 1.0F && v > 0.0F && v < 1.0F, "mock stays in view");
    }
    expect_near(u, 0.5F, 0.0001F, "mock loop returns to horizontal center");
    expect_near(v, 0.5F, 0.0001F, "mock loop returns to vertical center");
    expect(project_gaze_to_view(simulated_gaze_pose({}, 1.0), {}, fov, u, v), "mock motion projects");
    expect(std::fabs(u - 0.5F) > 0.05F && std::fabs(v - 0.5F) > 0.05F,
        "mock moves on both axes");
}

void test_simulation_patterns() {
    using namespace cheeky::gaze_math;
    for (unsigned pattern : {2U, 3U}) {
        const double interval = pattern == 2U ? 2.0 : 8.0;
        expect(next_simulated_jump_time(0.0, pattern) == interval, "preview selects first upcoming jump");
        expect(next_simulated_jump_time(interval - 0.001, pattern) == interval, "preview stays at upcoming target until jump");
        expect(next_simulated_jump_time(interval, pattern) == interval * 2.0, "preview advances when jump occurs");
        const auto preview = simulated_gaze_pose({}, next_simulated_jump_time(interval * 4.0, pattern), pattern).orientation;
        expect_near(preview.w, 1.0F, 0.00001F, "preview wraps to center after last corner");
        const auto start = simulated_gaze_pose({}, 0.0, pattern).orientation;
        const auto held = simulated_gaze_pose({}, interval - 0.001, pattern).orientation;
        const auto jumped = simulated_gaze_pose({}, interval, pattern).orientation;
        expect_near(start.y, held.y, 0.00001F, "jump target stays still for requested interval");
        expect(std::fabs(jumped.y - held.y) > 0.1F, "jump happens at interval boundary");
        const auto repeat = simulated_gaze_pose({}, interval * 5.0, pattern).orientation;
        expect_near(start.y, repeat.y, 0.00001F, "five targets repeat");
    }
    const auto sweep = simulated_gaze_pose({}, 5.0, 1U).orientation;
    expect(std::fabs(sweep.y) > 0.1F, "slow sweep reaches side after five seconds");
    expect_near(sweep.x, 0.0F, 0.00001F, "slow sweep stays horizontal");
    const auto center = simulated_gaze_pose({}, 7.0, 5U).orientation;
    expect_near(center.w, 1.0F, 0.00001F, "center pattern holds forward gaze");
    expect(simulated_gaze_valid(3.999, 4U), "tracking stays valid before dropout");
    expect(!simulated_gaze_valid(4.0, 4U), "tracking drops at four seconds");
    expect(!simulated_gaze_valid(4.999, 4U), "dropout lasts one second");
    expect(simulated_gaze_valid(5.0, 4U), "tracking recovers at five seconds");
    expect(simulated_gaze_valid(4.5, 0U), "ordinary figure eight does not drop tracking");
}

void test_projection() {
    using namespace cheeky::gaze_math;
    const Pose identity{};
    constexpr float quarter_pi = 0.78539816339F;
    float u{};
    float v{};
    expect(project_gaze_to_view(
        identity, identity,
        {-quarter_pi, quarter_pi, quarter_pi, -quarter_pi}, u, v
    ), "center gaze projects into a symmetric view");
    expect_near(u, 0.5F, 0.0001F, "symmetric projection has centered U");
    expect_near(v, 0.5F, 0.0001F, "symmetric projection has centered V");

    expect(project_gaze_to_view(
        identity, identity,
        {-0.9F, 0.6F, 0.7F, -0.5F}, u, v
    ), "center gaze projects into an asymmetric view");
    const auto expected_u = -std::tan(-0.9F) /
        (std::tan(0.6F) - std::tan(-0.9F));
    const auto expected_v = std::tan(0.7F) /
        (std::tan(0.7F) - std::tan(-0.5F));
    expect_near(u, expected_u, 0.0001F, "asymmetric FOV changes center U");
    expect_near(v, expected_v, 0.0001F, "asymmetric FOV changes center V");

    Pose backwards{};
    backwards.orientation = {0.0F, 1.0F, 0.0F, 0.0F};
    expect(!project_gaze_to_view(
        backwards, identity,
        {-quarter_pi, quarter_pi, quarter_pi, -quarter_pi}, u, v
    ), "gaze behind the view is rejected");

    const auto sine = std::sin(quarter_pi * 0.5F);
    const auto cosine = std::cos(quarter_pi * 0.5F);
    Pose gaze{};
    Pose view{};
    gaze.orientation = {0.0F, sine, 0.0F, cosine};
    view.orientation = gaze.orientation;
    expect(project_gaze_to_view(
        gaze, view,
        {-quarter_pi, quarter_pi, quarter_pi, -quarter_pi}, u, v
    ), "matching gaze and view poses transform into view space");
    expect_near(u, 0.5F, 0.0001F, "pose transform preserves centered U");
}

void test_geometry() {
    using namespace cheeky::foveated_dlss;
    FoveationParameters parameters{};
    parameters.width = 0.5F;
    parameters.height = 0.5F;
    parameters.x_offset = 0.25F;
    parameters.y_offset = -0.5F;
    FoveationGeometry fixed{};
    FoveationGeometry explicit_center{};
    expect(calculate_foveation_geometry(
        parameters, 1000U, 800U, 2000U, 1600U, 17U, 23U, fixed
    ), "fixed geometry succeeds");
    const float center_u = (fixed.input_base_x + fixed.input_width * 0.5F) /
        1000.0F;
    const float center_v = (fixed.input_base_y + fixed.input_height * 0.5F) /
        800.0F;
    expect(calculate_foveation_geometry_at_center(
        parameters, {center_u, center_v, 1U},
        1000U, 800U, 2000U, 1600U, 17U, 23U, explicit_center
    ), "explicit-center geometry succeeds");
    expect(fixed.input_base_x == explicit_center.input_base_x &&
        fixed.input_base_y == explicit_center.input_base_y &&
        fixed.output_base_x == explicit_center.output_base_x &&
        fixed.output_base_y == explicit_center.output_base_y,
        "fixed-mode geometry remains bit compatible");
    const auto resolved_offsets = foveation_offsets_from_geometry(
        fixed, 1000U, 800U
    );
    expect_near(resolved_offsets.x, parameters.x_offset, 0.0021F,
        "geometry recovers the composite X offset");
    expect_near(resolved_offsets.y, parameters.y_offset, 0.0026F,
        "geometry recovers the composite Y offset");

    FoveationGeometry clamped{};
    expect(calculate_foveation_geometry_at_center(
        parameters, {-2.0F, 4.0F, 8U},
        1000U, 800U, 2000U, 1600U, 0U, 0U, clamped
    ), "out-of-range center is clamped");
    expect(clamped.input_base_x == 0U,
        "left-clamped center starts at the left edge");
    expect(clamped.input_base_y == 400U,
        "bottom-clamped center starts at the bottom edge");

    FoveationGeometry quantized{};
    expect(calculate_foveation_geometry_at_center(
        parameters, {0.613F, 0.427F, 8U},
        1000U, 800U, 2000U, 1600U, 0U, 0U, quantized
    ), "quantized geometry succeeds");
    expect(quantized.input_base_x % 8U == 0U &&
        quantized.input_base_y % 8U == 0U,
        "crop origins are quantized to eight render pixels");
}

void test_mapping_policy() {
    using namespace cheeky::foveated_dlss;
    GazeMappingPolicyState state{};
    auto result = update_gaze_mapping(state, 1U, 0U, 1U, 100);
    expect(!result.stable && state.consecutive_matches == 1U,
        "first exact resource match is provisional");
    result = update_gaze_mapping(state, 1U, 0U, 1U, 100);
    expect(!result.stable && state.consecutive_matches == 1U,
        "repeated evaluation in one display frame does not stabilize mapping");
    result = update_gaze_mapping(state, 1U, 0U, 1U, 101);
    expect(result.stable, "two display-frame matches stabilize mapping");
    result = update_gaze_mapping(state, 0U, unmapped_gaze_view, 1U, 102);
    expect(result.invalidated && state.view_index == unmapped_gaze_view,
        "resource mismatch invalidates mapping immediately");
    static_cast<void>(update_gaze_mapping(state, 1U, 0U, 1U, 103));
    result = update_gaze_mapping(state, 1U, 1U, 2U, 104);
    expect(result.changed && !result.stable,
        "swapchain generation or eye change requires remapping");
}

void test_packed_stereo_mapping_policy() {
    using namespace cheeky::foveated_dlss;
    PackedStereoMappingInput input{};
    input.view_count = 2U;
    input.dlss_eye_index = 0U;
    input.output_width = 3894U;
    input.output_height = 3126U;
    input.views[0] = {
        0, 0, 3894U, 3126U, 0U, 0x100U, 0x200U, true
    };
    input.views[1] = {
        3894, 0, 3894U, 3126U, 0U, 0x100U, 0x200U, true
    };
    expect(select_packed_stereo_gaze_view(input) == 0U,
        "packed stereo maps the first DLSS role to OpenXR eye zero");
    GazeMappingPolicyState state{};
    auto mapping = update_gaze_mapping(
        state, 1U, select_packed_stereo_gaze_view(input), 7U, 100
    );
    expect(!mapping.stable,
        "first packed-stereo display-frame match is provisional");
    mapping = update_gaze_mapping(
        state, 1U, select_packed_stereo_gaze_view(input), 7U, 101
    );
    expect(mapping.stable,
        "two packed-stereo display-frame matches stabilize mapping");
    input.dlss_eye_index = 1U;
    expect(select_packed_stereo_gaze_view(input) == 1U,
        "packed stereo maps the second DLSS role to OpenXR eye one");
    input.output_origin_x = 3894U;
    expect(select_packed_stereo_gaze_view(input) == 1U,
        "packed stereo accepts the second eye's packed output origin");
    input.invert_eye_order = true;
    expect(select_packed_stereo_gaze_view(input) == 0U,
        "packed stereo mapping honors inverted eye order");

    input.invert_eye_order = false;
    input.output_origin_x = 0U;
    input.views[1].resource_identity = 0x101U;
    expect(select_packed_stereo_gaze_view(input) == unmapped_gaze_view,
        "separate OpenXR resources do not use the packed fallback");
    input.views[1].swapchain_identity = 0x201U;
    expect(select_packed_stereo_gaze_view(input) == 1U,
        "ACC split swapchains preserve the second eye's packed layout");
    input.dlss_eye_index = 0U;
    expect(select_packed_stereo_gaze_view(input) == 0U,
        "ACC split swapchains map the first stereo role");
    input.invert_eye_order = true;
    expect(select_packed_stereo_gaze_view(input) == 1U,
        "split swapchain mapping honors eye inversion");
    input.invert_eye_order = false;
    input.views[1].rect_x = 0;
    expect(select_packed_stereo_gaze_view(input) == unmapped_gaze_view,
        "split swapchains still require complementary packed rectangles");
    input.views[1].rect_x = 3894;
    input.views[1].array_index = 1U;
    expect(select_packed_stereo_gaze_view(input) == unmapped_gaze_view,
        "split swapchains reject unsupported array slices");
    input.views[1].array_index = 0U;
    input.views[1].swapchain_identity = 0x200U;
    input.views[1].resource_identity = 0x100U;
    input.views[1].rect_x = 4000;
    expect(select_packed_stereo_gaze_view(input) == unmapped_gaze_view,
        "gapped OpenXR rectangles do not use the packed fallback");
    mapping = update_gaze_mapping(
        state, 0U, select_packed_stereo_gaze_view(input), 7U, 102
    );
    expect(mapping.invalidated,
        "invalid packed layout immediately invalidates a stable mapping");
    input.views[1].rect_x = 3894;
    input.output_width = 3800U;
    expect(select_packed_stereo_gaze_view(input) == unmapped_gaze_view,
        "mismatched DLSS dimensions do not use the packed fallback");
    input.output_width = 3894U;
    input.output_origin_x = 1U;
    expect(select_packed_stereo_gaze_view(input) == unmapped_gaze_view,
        "subrect DLSS output does not use the packed fallback");
}

void test_temporal_policy() {
    using namespace cheeky::foveated_dlss;
    GazeTemporalPolicyState clamp_state{};
    const auto clamped = update_gaze_temporal_policy(
        clamp_state, {0.5, 1, -1.0F, 2.0F, 0.5F, 0.5F, 0.0F,
                      0.100, 0.150, true}
    );
    expect_near(clamped.center_u, 0.0F, 0.0001F,
        "gaze U is clamped to the view");
    expect_near(clamped.center_v, 1.0F, 0.0001F,
        "gaze V is clamped to the view");

    GazeTemporalPolicyState state{};
    auto result = update_gaze_temporal_policy(
        state, {1.0, 1, 0.2F, 0.8F, 0.5F, 0.5F, 20.0F,
                0.100, 0.150, true}
    );
    expect(result.using_gaze && result.reacquired,
        "first valid sample starts gaze tracking");
    expect_near(result.center_u, 0.2F, 0.0001F,
        "first valid sample is not delayed");

    result = update_gaze_temporal_policy(
        state, {1.020, 2, 0.8F, 0.2F, 0.5F, 0.5F, 20.0F,
                0.100, 0.150, true}
    );
    expect(result.center_u > 0.5F && result.center_u < 0.8F,
        "twenty millisecond filter smooths a saccade");
    const auto held_u = result.center_u;
    result = update_gaze_temporal_policy(
        state, {1.100, 2, 0.0F, 0.0F, 0.5F, 0.5F, 20.0F,
                0.100, 0.150, false}
    );
    expect_near(result.center_u, held_u, 0.0001F,
        "tracking loss holds the last gaze for 100 ms");
    result = update_gaze_temporal_policy(
        state, {1.195, 2, 0.0F, 0.0F, 0.5F, 0.5F, 20.0F,
                0.100, 0.150, false}
    );
    expect(result.center_u < held_u && result.center_u > 0.5F,
        "tracking loss interpolates toward fixed center");
    result = update_gaze_temporal_policy(
        state, {1.300, 3, 0.25F, 0.75F, 0.5F, 0.5F, 20.0F,
                0.100, 0.150, true}
    );
    expect(result.reacquired, "new sample after return is marked reacquired");
}

void test_reset_policy() {
    using namespace cheeky::foveated_dlss;
    const GazeCropPolicyState none{};
    const GazeCropPolicyState first{100U, 100U, 400U, 300U, true};
    auto result = evaluate_gaze_reset(
        none, first, true, false, false, 0.125F
    );
    expect(result.reason == GazeResetReason::first_valid,
        "first valid gaze resets history");
    const GazeCropPolicyState small_move{140U, 120U, 400U, 300U, true};
    result = evaluate_gaze_reset(
        first, small_move, true, false, false, 0.125F
    );
    expect(result.reason == GazeResetReason::none,
        "small gaze motion preserves history");
    const GazeCropPolicyState boundary{164U, 100U, 400U, 300U, true};
    result = evaluate_gaze_reset(
        first, boundary, true, false, false, 0.125F
    );
    expect(result.reason == GazeResetReason::large_jump,
        "origin jump at the 64-pixel floor resets history");
    const GazeCropPolicyState large{180U, 100U, 400U, 300U, true};
    result = evaluate_gaze_reset(
        first, large, true, false, false, 0.125F
    );
    expect(result.reason == GazeResetReason::large_jump,
        "origin jump above 64 pixels resets history");
    const GazeCropPolicyState resized{100U, 100U, 420U, 300U, true};
    result = evaluate_gaze_reset(
        first, resized, true, false, false, 0.125F
    );
    expect(result.reason == GazeResetReason::crop_size_changed,
        "crop-size change resets history");
    result = evaluate_gaze_reset(
        first, first, true, false, true, 0.125F
    );
    expect(result.reason == GazeResetReason::remapped,
        "view remapping resets history");
}

void test_abi() {
    static_assert(CHEEKY_GAZE_MAX_VIEWS == 2U);
    static_assert(sizeof(CheekyGazeViewV1) == 88U);
    static_assert(sizeof(CheekyGazeSnapshotV1) == 368U);
    CheekyGazeSnapshotV1 snapshot{};
    snapshot.abi_version = CHEEKY_GAZE_ABI_VERSION;
    snapshot.structure_size = sizeof(snapshot);
    expect(snapshot.abi_version == 4U &&
        snapshot.structure_size >= sizeof(CheekyGazeSnapshotV1),
        "snapshot ABI version and size are self-describing");
}

void test_core_d3d12_evaluation_is_intercepted() {
    using namespace cheeky::foveated_dlss;
    D3D12DispatchHarness harness{};
    dispatch_harness = &harness;
    const D3D12NgxEvaluationCall call{
        D3D12NgxRoute::core_runtime, nullptr, nullptr, nullptr, nullptr
    };
    const auto result = dispatch_d3d12_ngx_evaluation(
        call, &fake_d3d12_original, &fake_d3d12_processor, &harness
    );
    expect(result == 0x200U,
        "core D3D12 evaluation returns the processor result");
    expect(harness.processor_calls == 1,
        "core D3D12 evaluation enters the Cheeky processor once");
    expect(harness.original_calls == 0,
        "core D3D12 evaluation is not forwarded before processing");
    expect(harness.active_during_processor,
        "core D3D12 processing runs inside an interception scope");
    expect(harness.observed_route == D3D12NgxRoute::core_runtime,
        "core D3D12 processing retains its runtime route");
    dispatch_harness = nullptr;
}

void test_nested_d3d12_evaluation_is_forwarded_once() {
    using namespace cheeky::foveated_dlss;
    D3D12DispatchHarness harness{};
    harness.nest_core_evaluation = true;
    dispatch_harness = &harness;
    const D3D12NgxEvaluationCall call{
        D3D12NgxRoute::public_runtime, nullptr, nullptr, nullptr, nullptr
    };
    const auto result = dispatch_d3d12_ngx_evaluation(
        call, &fake_d3d12_original, &fake_d3d12_processor, &harness
    );
    expect(result == 0x100U,
        "nested core evaluation returns the original NGX result");
    expect(harness.processor_calls == 1,
        "public-to-core evaluation enters the Cheeky processor once");
    expect(harness.original_calls == 1,
        "nested core evaluation forwards to NGX exactly once");
    expect(!d3d12_ngx_interception_active(),
        "D3D12 interception scope is released after evaluation");
    dispatch_harness = nullptr;
}

void test_d3d12_route_names() {
    using namespace cheeky::foveated_dlss;
    expect(std::strcmp(
        d3d12_ngx_route_name(D3D12NgxRoute::public_runtime),
        "Public nvngx_dlss.dll"
    ) == 0, "public D3D12 NGX route has a diagnostic label");
    expect(std::strcmp(
        d3d12_ngx_route_name(D3D12NgxRoute::core_runtime),
        "Core _nvngx.dll"
    ) == 0, "core D3D12 NGX route has a diagnostic label");
}

void test_nested_d3d12_lifecycle_scope_is_passthrough() {
    using namespace cheeky::foveated_dlss;
    expect(!d3d12_ngx_interception_active(),
        "D3D12 lifecycle starts outside interception");
    {
        D3D12NgxInterceptionScope outer;
        expect(outer.outermost(),
            "first D3D12 lifecycle hook owns interception");
        expect(d3d12_ngx_interception_active(),
            "D3D12 lifecycle scope marks interception active");
        D3D12NgxInterceptionScope nested;
        expect(!nested.outermost(),
            "nested D3D12 lifecycle hook is passthrough");
    }
    expect(!d3d12_ngx_interception_active(),
        "D3D12 lifecycle scope restores thread state");
}

void test_core_d3d12_route_is_published_to_diagnostics() {
    using namespace cheeky::foveated_dlss;
    diagnostic_note_d3d12_ngx_route(D3D12NgxRoute::core_runtime);
    expect(
        diagnostic_snapshot(DiagnosticApi::d3d12).d3d12_ngx_route ==
            D3D12NgxRoute::core_runtime,
        "core D3D12 route is visible in diagnostics"
    );
}

void test_multimip_game_output_uses_single_mip_private_output() {
    using namespace cheeky::foveated_dlss;
    D3D12_RESOURCE_DESC game_output{};
    game_output.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    game_output.Width = 8824U;
    game_output.Height = 3542U;
    game_output.DepthOrArraySize = 1U;
    game_output.MipLevels = 4U;
    game_output.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    game_output.SampleDesc.Count = 1U;
    game_output.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const auto plan = plan_d3d12_output(game_output, 2206U, 886U);
    expect(plan.compatible,
        "multi-mip game output is compatible with foveated D3D12 processing");
    expect(plan.private_description.Width == 2206U &&
            plan.private_description.Height == 886U,
        "private D3D12 output uses the requested foveated dimensions");
    expect(plan.private_description.MipLevels == 1U,
        "private D3D12 output contains only the DLSS mip");
    expect(plan.private_description.Format == DXGI_FORMAT_R11G11B10_FLOAT,
        "private D3D12 output preserves the game output format");
}

void test_multimip_game_output_is_dlss_nr_compatible() {
    using namespace cheeky::foveated_dlss;
    D3D12_RESOURCE_DESC game_output{};
    game_output.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    game_output.Width = 10380U;
    game_output.Height = 4168U;
    game_output.DepthOrArraySize = 1U;
    game_output.MipLevels = 4U;
    game_output.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    game_output.SampleDesc.Count = 1U;
    game_output.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    expect(is_dlss_nr_output_compatible(game_output),
        "DLSS-NR accepts the multi-mip mip-zero output used by ACE");
}

void test_msfs_array_output_contract() {
    using namespace cheeky::foveated_dlss;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 3024;
    desc.Height = 2836;
    desc.MipLevels = 12;
    desc.DepthOrArraySize = 2;
    desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (const auto slices : {2U, 4U}) {
        desc.DepthOrArraySize = static_cast<UINT16>(slices);
        const auto plan = plan_d3d12_output(desc, 1664, 1276);
        expect(plan.compatible, "MSFS array output accepted for SR");
        expect(plan.private_description.DepthOrArraySize == 1 &&
            plan.private_description.MipLevels == 1 &&
            plan.private_description.Width == 1664 &&
            plan.private_description.Height == 1276,
            "SR scratch is a single-slice single-mip crop");
        expect(!is_dlss_nr_output_compatible(desc),
            "SR array support does not silently enable unsupported NR arrays");
    }
    desc.SampleDesc.Count = 2;
    expect(!plan_d3d12_output(desc, 1664, 1276).compatible, "MSAA stays rejected");
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    expect(!plan_d3d12_output(desc, 1664, 1276).compatible, "missing UAV stays rejected");
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    expect(!plan_d3d12_output(desc, 1664, 1276).compatible, "3D output stays rejected");
}

void test_streamline_supersampling_creation() {
    using namespace cheeky::foveated_dlss;
    struct Parameters {
        unsigned width{688}, height{288}, output_width{1376}, output_height{576};
        unsigned writes{};
        bool missing_height{};
        NgxResult Get(const char* name, unsigned* value) const {
            if (std::strcmp(name, "Height") == 0 && missing_height) return 0xBAD00005U;
            *value = std::strcmp(name, "Width") == 0 ? width : height;
            return 1U;
        }
        void Set(const char* name, unsigned value) {
            ++writes;
            (std::strcmp(name, "Width") == 0 ? width : height) = value;
        }
    } parameters;
    // MSFS Performance mode at 2x: SL proposes 688x288, but the input crop
    // (including low-resolution motion) still contains only 344x144 pixels.
    {
        StreamlineCreateExtentScope scope(&parameters, 344U, 144U);
        expect(parameters.width == 344U && parameters.height == 144U,
            "scaled SL feature uses actual color/depth/low-res MV input dimensions");
        expect(parameters.output_width == 1376U && parameters.output_height == 576U,
            "scaled SL feature retains supersampled output dimensions");
        {
            StreamlineCreateExtentScope nested(&parameters, 0U, 0U);
            expect(parameters.width == 344U, "inactive nested override leaves creation untouched");
        }
    }
    expect(parameters.width == 688U && parameters.height == 288U,
        "SL shared parameters restored after feature creation");
    parameters.writes = 0;
    { StreamlineCreateExtentScope off(&parameters, 0U, 0U); }
    expect(parameters.writes == 0, "1x and unrelated evaluations do not write creation dimensions");
    parameters.missing_height = true;
    { StreamlineCreateExtentScope invalid(&parameters, 344U, 144U); }
    expect(parameters.writes == 0, "unreadable creation dimensions are not partially overridden");
}

void test_streamline_private_sr_viewport() {
    using namespace cheeky::foveated_dlss;
    struct Viewport {
        void* next{};
        std::uint32_t struct_type{1};
        std::uint32_t value{};
    };
    for (const auto id : {0U, 32U}) {
        Viewport host{nullptr, 1, id}, other{nullptr, 2, 77}, cropped{};
        const void* inputs[]{&other, &host};
        std::array<const void*, 2> redirected{};
        expect(prepare_streamline_sr_inputs(inputs, 2, host, cropped, redirected),
            "MSFS viewports route to private SR instances");
        expect(redirected[0] == &other && redirected[1] == &cropped &&
            inputs[1] == &host && host.value == id,
            "redirect only the viewport input, leaving host and other inputs intact");
        expect(cropped.value != host.value &&
            cropped.value != (host.value ^ 0x40000000U),
            "cropped constants do not collide with host or peripheral constants");
        // Model a runtime where a second write to the same frame/viewport fails.
        std::array<std::uint32_t, 3> written{};
        std::size_t used{};
        const auto submit = [&](std::uint32_t viewport) {
            for (std::size_t i{}; i < used; ++i) if (written[i] == viewport) return false;
            written[used++] = viewport;
            return true;
        };
        expect(submit(host.value) && !submit(host.value),
            "same-frame host constants cannot be overwritten");
        expect(submit(cropped.value), "cropped constants can be submitted independently");
        expect(static_cast<const Viewport*>(redirected[1])->value == written[1],
            "evaluation consumes the viewport that received cropped constants");
        Viewport mismatch{nullptr, 1, id + 1U};
        const void* mismatched[]{&mismatch};
        expect(!prepare_streamline_sr_inputs(mismatched, 1, host, cropped, redirected),
            "stale viewport cache is rejected before modifying Streamline state");
        const void* duplicate[]{&host, &host};
        expect(!prepare_streamline_sr_inputs(duplicate, 2, host, cropped, redirected),
            "ambiguous viewport inputs are rejected");
    }
}

void test_dlss_nr_stable_crop_and_history() {
    using namespace cheeky::foveated_dlss;
    // Sweep every pixel, including both edges and a non-aligned capacity.
    for (const auto extent : {400U, 403U, 999U, 1003U}) {
        const auto expected = (std::min)(1003U, (extent + 7U) / 8U * 8U);
        for (unsigned position = 0U; position <= 1003U - extent; ++position) {
            const auto axis = dlss_nr_aligned_axis(position, extent, 1003U);
            expect(axis.extent == expected, "NR extent stays constant throughout gaze sweep");
            expect(axis.base + axis.extent <= 1003U, "NR stays in bounds at image edges");
        }
    }
    DlssNrHistory previous{80U, 160U, 400U, 240U, 2000U, 1200U, 200U, 120U, 2.0F, -4.0F};
    auto current = previous;
    current.x += 8U;
    current.y -= 8U;
    float x{}, y{};
    expect(dlss_nr_motion_offset(previous, current, x, y), "small NR crop move preserves history");
    expect_near(x, 8.0F / 400.0F, 0.0001F, "NR origin motion uses region UVs");
    expect_near(y, -8.0F / 240.0F, 0.0001F, "NR origin motion is independent of signed vector scale");
    // Static scene point: current-local + corrected MV = previous-local,
    // including a half-resolution NR working texture.
    expect_near((100.0F - 8.0F) * 0.5F + x * current.working_width,
        100.0F * 0.5F, 0.0001F, "NR reprojects overlapping static pixels at working scale");
    expect(dlss_nr_motion_offset(current, current, x, y) && x == 0.0F && y == 0.0F,
        "stationary NR region needs no correction");
    current = previous;
    current.width += 8U;
    expect(!dlss_nr_motion_offset(previous, current, x, y), "NR dimension change resets history");
    current = previous;
    current.working_width += 8U;
    expect(!dlss_nr_motion_offset(previous, current, x, y), "NR working dimension change resets history");
    current = previous;
    current.output_width += 8U;
    expect(!dlss_nr_motion_offset(previous, current, x, y), "NR output dimension change resets history");
    current = previous;
    current.x += current.width;
    expect(!dlss_nr_motion_offset(previous, current, x, y), "nonoverlapping NR jump resets history");
    current = previous;
    current.scale_x *= 2.0F;
    expect(!dlss_nr_motion_offset(previous, current, x, y), "changed NR motion scale resets history");
    previous.scale_x = 0.0F;
    current = previous;
    current.x += 8U;
    expect(dlss_nr_motion_offset(previous, current, x, y), "zero NR motion scale still corrects crop motion");
    expect_near(x, 8.0F / 400.0F, 0.0001F, "zero scene motion does not suppress gaze displacement");
}

void test_dlss_nr_maps_right_eye_region_into_packed_output() {
    using namespace cheeky::foveated_dlss;
    const auto base = dlss_nr_resource_base(
        2544U, 928U, 5190U, 0U, false
    );
    expect(base.x == 7734U && base.y == 928U,
        "right-eye DLSS-NR region includes the packed output base");
    const auto isolated_eye = dlss_nr_resource_base(
        2544U, 928U, 0U, 0U, false
    );
    expect(isolated_eye.x == 2544U && isolated_eye.y == 928U,
        "uncropped isolated eye retains the NR crop offset");
}

void test_dlss_nr_transport_crop_fits_resource() {
    using namespace cheeky::foveated_dlss;
    // Transport copies only the NR region into a texture of this exact size.
    // Its original position in the eye image must not be applied a second time.
    constexpr std::uint32_t width = 2120U;
    constexpr std::uint32_t height = 1848U;
    const auto cropped = dlss_nr_resource_base(452U, 494U, 0U, 0U, true);
    expect(cropped.x == 0U && cropped.y == 0U,
        "pre-cropped transport color begins at the resource origin");
    expect(cropped.x + width <= width && cropped.y + height <= height,
        "NR region fits the transport texture without double-applying the crop");
}

void test_dlss_nr_reuses_live_sr_crop_center() {
    using namespace cheeky::foveated_dlss;
    Settings settings{};
    settings.nr_use_sr_foveation = true;
    settings.width = 0.5F;
    settings.height = 0.5F;
    settings.x_offset = -0.8F;
    settings.height_offset = -0.8F;
    const FoveationGeometry live_sr_crop{
        1000U, 600U, 1000U, 800U,
        2000U, 1200U, 2000U, 1600U,
    };
    const auto expected = foveation_offsets_from_geometry(
        live_sr_crop, 3000U, 2000U
    );
    const auto parameters = dlss_nr_foveation_parameters(
        settings, &live_sr_crop, 3000U, 2000U
    );
    expect_near(parameters.x_offset, expected.x, 0.0001F,
        "DLSS-NR follows the live SR horizontal center");
    expect_near(parameters.y_offset, expected.y, 0.0001F,
        "DLSS-NR follows the live SR vertical center");
}

void test_dlss_nr_independent_size_shares_sr_center() {
    using namespace cheeky::foveated_dlss;
    Settings settings{};
    settings.nr_use_sr_foveation = false;
    settings.width = 0.5F;
    settings.height = 0.6F;
    settings.height_offset = -0.25F;
    settings.nr_width = 0.7F;
    settings.nr_height = 0.3F;
    settings.nr_roundness = 0.4F;
    settings.nr_transition_width = 0.12F;
    for (const float eye_offset : {-0.3F, 0.3F}) {
        settings.x_offset = eye_offset;
        const auto nr = dlss_nr_foveation_parameters(settings, nullptr, 0U, 0U);
        expect_near(nr.width, 0.7F, 0.0001F, "NR retains independent width");
        expect_near(nr.height, 0.3F, 0.0001F, "NR retains independent height");
        expect_near(nr.roundness, 0.4F, 0.0001F, "NR retains independent roundness");
        expect_near(nr.transition_width, 0.12F, 0.0001F, "NR retains independent transition");
        expect_near(nr.x_offset * (1.F - nr.width), eye_offset * (1.F - settings.width),
            0.0001F, "different NR width preserves each SR eye center");
        expect_near(nr.y_offset * (1.F - nr.height), -0.1F,
            0.0001F, "different NR height preserves SR vertical center");
    }
    const FoveationGeometry live{900U, 300U, 1000U, 800U};
    const auto nr = dlss_nr_foveation_parameters(settings, &live, 3000U, 2000U);
    expect_near(nr.x_offset * (1.F - nr.width), 2800.F / 3000.F - 1.F,
        0.0001F, "independent NR follows live horizontal center");
    expect_near(nr.y_offset * (1.F - nr.height), -0.3F,
        0.0001F, "independent NR follows live vertical center");
    settings.nr_width = settings.nr_height = 1.F;
    const auto full = dlss_nr_foveation_parameters(settings, &live, 3000U, 2000U);
    expect(full.x_offset == 0.F && full.y_offset == 0.F,
        "full-frame NR uses zero offsets without division by zero");
}

void test_nr_only_center(bool openvr) {
    using namespace cheeky::foveated_dlss;
    reset_gaze_foveation();
    register_stereo_view(1901); register_stereo_view(1902);
    Settings settings{};
    settings.enabled = false;
    settings.nr_enabled = settings.nr_foveated = true;
    settings.width = settings.height = 0.4F;
    settings.nr_width = 0.6F; settings.nr_height = 0.3F;
    settings.nr_use_sr_foveation = false;
    settings.center_mode = FoveationCenterMode::openxr_gaze;
    settings.gaze_smoothing_ms = 0;
    settings.gaze_quantization_pixels = 1;
    CheekyGazeSnapshotV1 snapshot{};
    snapshot.abi_version = CHEEKY_GAZE_ABI_VERSION;
    snapshot.structure_size = sizeof(snapshot);
    snapshot.view_count = 2;
    snapshot.session_generation = 765;
    snapshot.swapchain_generation = 1;
    snapshot.status_flags = CHEEKY_GAZE_STATUS_MAPPING_READY | CHEEKY_GAZE_STATUS_SESSION_FOCUSED |
        CHEEKY_GAZE_STATUS_GAZE_VALID;
    if (openvr) snapshot.status_flags |= CHEEKY_GAZE_STATUS_OPENVR;
    expect(publish_stereo_calibration(1901, 1902, stereo_view_generation(1901), stereo_view_generation(1902),
        5000, GetTickCount64(), nullptr, openvr ? 0 : snapshot.session_generation), "NR-only eye mapping publishes");
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& v = snapshot.views[eye];
        v.view_index = eye;
        v.flags = CHEEKY_GAZE_VIEW_RESOURCE_VALID | CHEEKY_GAZE_VIEW_FORWARD_VALID;
        v.image_rect_width = 2400; v.image_rect_height = 2000;
        v.resource_identity = 100 + eye; v.swapchain_identity = 200 + eye;
        v.forward_u = eye ? 0.45F : 0.55F; v.forward_v = 0.5F;
        v.center_u = eye ? 0.4F : 0.6F; v.center_v = 0.55F;
    }
    if (openvr) test_openvr_snapshot = &snapshot;
    FoveationCenter centers[2];
    const auto step = [&]() {
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        snapshot.publication_qpc = now.QuadPart;
        ++snapshot.predicted_display_time;
        for (unsigned eye = 0; eye < 2; ++eye) {
            bool reset{};
            expect(calculate_coordinated_center(settings, 1901 + eye, nullptr, 1200, 1000, 2400, 2000,
                0, 0, centers[eye], reset, openvr ? nullptr : &snapshot), "NR-only placement resolves without SR");
        }
    };
    step(); step(); step();
    expect(gaze_diagnostics().using_gaze, "NR-only path updates live gaze diagnostics");
    for (unsigned eye = 0; eye < 2; ++eye) {
        expect_near(centers[eye].u, snapshot.views[eye].center_u, 0.002F, "NR-only uses correct eye gaze");
        const auto parameters = dlss_nr_foveation_parameters(settings, &centers[eye]);
        FoveationGeometry nr{};
        expect(calculate_foveation_geometry(parameters, 2400, 2000, 2400, 2000, 0, 0, nr), "NR geometry resolves");
        expect_near((nr.input_base_x + nr.input_width * 0.5F) / 2400.F, centers[eye].u, 0.002F,
            "Independent NR width preserves resolved center");
        expect_near(parameters.width, settings.nr_width, 0.0001F, "Independent NR size retained");
    }
    const float previous = centers[0].u;
    snapshot.views[0].center_u = 0.45F;
    step();
    expect(centers[0].u < previous - 0.1F, "NR-only region follows moving gaze");
    settings.width = settings.height = 1.F;
    step();
    expect_near(centers[0].u, 0.45F, 0.002F, "Full-frame SR size does not pin NR gaze to the middle");
    settings.width = settings.height = 0.4F;
    settings.enabled = true;
    step();
    CropGeometry sr{}; bool reset{};
    expect(calculate_coordinated_crop(settings, 1901, nullptr, 1200, 1000, 2400, 2000,
        0, 0, sr, reset, openvr ? nullptr : &snapshot), "SR placement resolves when enabled");
    expect_near(foveation_center_from_geometry(sr, 1200, 1000).u, centers[0].u, 0.002F,
        "SR and independent NR share center");
    settings.enabled = false;
    step();
    expect_near(centers[0].u, 0.45F, 0.002F, "Disabling SR does not interrupt NR gaze");
    settings.nr_use_sr_foveation = true;
    const auto linked = dlss_nr_foveation_parameters(settings, &centers[0]);
    expect_near(linked.width, settings.width, 0.0001F, "Link option changes size independently of SR enable");
    settings.center_mode = FoveationCenterMode::fixed;
    step();
    expect_near(centers[0].u, snapshot.views[0].forward_u, 0.002F, "NR-only fixed mode uses automatic alignment");
    snapshot.status_flags &= ~CHEEKY_GAZE_STATUS_GAZE_VALID;
    settings.center_mode = FoveationCenterMode::openxr_gaze;
    reset_gaze_foveation(); // No held sample: exercise unavailable-tracker fallback.
    step(); step(); step();
    expect(!gaze_diagnostics().using_gaze, "Unavailable gaze reports fallback in NR-only mode");
    expect_near(centers[0].u, snapshot.views[0].forward_u, 0.002F, "NR-only gaze fallback preserves alignment");
    settings.auto_stereo_alignment = false;
    settings.center_mode = FoveationCenterMode::fixed;
    step();
    expect(std::isfinite(centers[0].u), "NR-only manual placement resolves with SR disabled");
    test_openvr_snapshot = nullptr;
    unregister_stereo_view(1901); unregister_stereo_view(1902);
    clear_stereo_calibration(); reset_gaze_foveation();
}

void test_packed_alignment_coordinator(bool openvr = false) {
    using namespace cheeky::foveated_dlss;
    reset_gaze_foveation();
    register_stereo_view(951U); register_stereo_view(952U);
    Settings settings{};
    settings.width = settings.height = 0.4F;
    settings.gaze_smoothing_ms = 0.F;
    CheekyGazeSnapshotV1 snapshot{};
    snapshot.abi_version = CHEEKY_GAZE_ABI_VERSION;
    snapshot.structure_size = sizeof(snapshot);
    snapshot.view_count = 2U;
    snapshot.swapchain_generation = 1U;
    snapshot.session_generation = 987U;
    snapshot.status_flags = CHEEKY_GAZE_STATUS_MAPPING_READY | CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
    if (openvr) snapshot.status_flags |= CHEEKY_GAZE_STATUS_OPENVR;
    if (openvr) test_openvr_snapshot=&snapshot;
    for (unsigned i = 0; i < 2; ++i) {
        auto& eye = snapshot.views[i];
        eye.view_index = i;
        eye.flags = CHEEKY_GAZE_VIEW_RESOURCE_VALID | CHEEKY_GAZE_VIEW_FORWARD_VALID;
        eye.image_rect_x = i * 3024;
        eye.image_rect_width = 3024U; eye.image_rect_height = 2836U;
        eye.resource_identity = 0x2AC4ED30820ULL + i * 0xC0ULL;
        eye.swapchain_identity = 100U + i;
        eye.forward_u = i == 0 ? 0.62F : 0.38F;
        eye.forward_v = 0.5F;
        eye.center_u = i == 0 ? 0.72F : 0.28F;
        eye.center_v = 0.6F;
    }
    CropGeometry crops[2]{};
    const auto frame = [&]() {
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        snapshot.publication_qpc = now.QuadPart;
        ++snapshot.predicted_display_time;
        for (unsigned i = 0; i < 2; ++i) {
            bool reset{};
            expect(calculate_coordinated_crop(settings, 951U + i, nullptr,
                1512U, 1418U, 3024U, 2836U, 0U, 0U, crops[i], reset, openvr ? nullptr : &snapshot),
                "split packed bridge produces coordinated crop without matching resource or camera");
        }
    };
    frame(); frame(); frame();
    auto diagnostics = gaze_diagnostics();
    for (unsigned i = 0; i < 2; ++i) {
        expect(diagnostics.views[i].resource_mapped && diagnostics.views[i].packed_stereo_mapping,
            "screenshot split-texture layout stabilizes through packed mapping");
        expect(diagnostics.views[i].alignment_source == (openvr ? 3U : 2U),
            "fixed mode aligns through OpenXR without eye tracking support");
        const float actual = (crops[i].input_base_x + crops[i].input_width * 0.5F) / 1512.F;
        expect_near(actual, snapshot.views[i].forward_u, 0.001F, "each eye uses its own forward center");
    }
    settings.aligned_height_offset = -0.2F;
    frame();
    for (const auto& crop : crops) {
        expect_near((crop.input_base_y + crop.input_height * 0.5F) / 1418.F, 0.4F, 0.001F,
            "fixed automatic placement accepts upward user height bias");
    }
    settings.height = 0.6F;
    frame();
    expect_near((crops[0].input_base_y + crops[0].input_height * 0.5F) / 1418.F, 0.4F, 0.001F,
        "height bias keeps its screen position when fovea height changes");
    settings.height = 0.4F;
    settings.center_mode = FoveationCenterMode::openxr_gaze;
    frame();
    expect(gaze_diagnostics().alignment_source == (openvr ? 3U : 2U) && !gaze_diagnostics().using_gaze,
        "gaze mode uses automatic fixed fallback when tracker is unavailable");
    // NR can push a frame beyond the gaze freshness budget. Fixed optical
    // alignment must survive that delay and menu focus transitions, even when
    // the stale publication still claims a valid gaze sample.
    const auto stable_left = crops[0], stable_right = crops[1];
    settings.x_offset = 0.6F;
    settings.height_offset = -0.45F;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    for (const auto mode : {FoveationCenterMode::fixed, FoveationCenterMode::openxr_gaze}) {
        settings.center_mode = mode;
        for (unsigned cycle = 0; cycle < 12; ++cycle) {
            const auto saved_flags = snapshot.status_flags;
            snapshot.status_flags |= CHEEKY_GAZE_STATUS_GAZE_VALID;
            if (cycle % 2) snapshot.status_flags &= ~CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
            LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
            snapshot.publication_qpc = now.QuadPart - frequency.QuadPart;
            for (unsigned i = 0; i < 2; ++i) {
                bool reset{};
                FoveationCenter center{};
                expect(calculate_coordinated_crop(settings, 951U + i, nullptr,
                    1512U, 1418U, 3024U, 2836U, 0U, 0U, crops[i], reset,
                    openvr ? nullptr : &snapshot, &center), "delayed NR frame resolves a center");
                const auto& stable = i ? stable_right : stable_left;
                expect(crops[i].input_base_x == stable.input_base_x &&
                    crops[i].input_base_y == stable.input_base_y && !reset,
                    "delayed or unfocused frame must not jump to manual offsets or reset history");
                expect_near(center.u, snapshot.views[i].forward_u, 0.001F,
                    "NR receives stable optical alignment on delayed frames");
            }
            expect(!gaze_diagnostics().using_gaze, "stale publication cannot activate live gaze");
            snapshot.status_flags = saved_flags;
            frame();
        }
    }
    expect_near((crops[0].input_base_y + crops[0].input_height * 0.5F) / 1418.F, 0.4F, 0.004F,
        "gaze fallback retains fixed height preference");
    snapshot.status_flags |= CHEEKY_GAZE_STATUS_GAZE_VALID;
    frame();
    expect(gaze_diagnostics().using_gaze, "valid gaze remains active with automatic alignment enabled");
    for (unsigned i = 0; i < 2; ++i) {
        const float actual = (crops[i].input_base_x + crops[i].input_width * 0.5F) / 1512.F;
        expect_near(actual, snapshot.views[i].center_u, 0.004F, "gaze center is not offset a second time");
        expect_near((crops[i].input_base_y + crops[i].input_height * 0.5F) / 1418.F,
            snapshot.views[i].center_v, 0.004F, "fixed height bias never shifts valid gaze");
    }
    settings.center_mode = FoveationCenterMode::simulated_gaze;
    snapshot.status_flags |= CHEEKY_GAZE_STATUS_SIMULATED;
    frame();
    expect(gaze_diagnostics().using_gaze, "simulated gaze coexists with automatic alignment");
    settings.center_mode = FoveationCenterMode::fixed;
    settings.auto_stereo_alignment = false;
    frame();
    CropGeometry manual{};
    expect(calculate_crop(settings_for_view(settings, 951U), 1512U, 1418U, 3024U, 2836U, 0U, 0U, manual) &&
        crops[0].input_base_x == manual.input_base_x, "manual override retains configured placement");
    settings.auto_stereo_alignment = true;
    snapshot.views[1].image_rect_x = 0;
    frame();
    expect(gaze_diagnostics().alignment_source == 0U, "invalid packed layout falls back instead of using stale mapping");
    {
        snapshot.views[1].image_rect_x = 3024;
        snapshot.status_flags |= CHEEKY_GAZE_STATUS_AMBIGUOUS_RESOURCE;
        snapshot.status_flags &= ~CHEEKY_GAZE_STATUS_MAPPING_READY;
        settings.invert_stereo_x_offset = true; // Old manual guess must not invert an observed XR eye.
        const auto a = stereo_view_generation(951), b = stereo_view_generation(952);
        const auto calibration_session = openvr ? 0ULL : snapshot.session_generation;
        expect(publish_stereo_calibration(952, 951, b, a, 1000, GetTickCount64(), nullptr, calibration_session), "Marker calibration accepts swapped pair");
        frame(); frame(); frame();
        for (unsigned i = 0; i < 2; ++i) {
            const float actual = (crops[i].input_base_x + crops[i].input_width * 0.5F) / 1512.F;
            expect_near(actual, snapshot.views[1 - i].forward_u, 0.001F, "Crop follows calibrated eye despite previous packed/manual role");
        }
        expect(publish_stereo_calibration(951, 952, a, b, 1001, GetTickCount64(), nullptr, calibration_session), "Marker calibration accepts a later eye transition");
        frame(); frame(); frame();
        for (unsigned i = 0; i < 2; ++i)
            expect_near((crops[i].input_base_x + crops[i].input_width * 0.5F) / 1512.F,
                snapshot.views[i].forward_u, 0.001F, "Crop follows corrected mapping after the next transition");
        // Issue #22 has intermediate DLSS outputs and a larger array swapchain.
        // A verified marker pair must drive gaze despite resource ambiguity.
        settings.center_mode = FoveationCenterMode::openxr_gaze;
        settings.gaze_smoothing_ms = 0;
        snapshot.status_flags &= ~CHEEKY_GAZE_STATUS_SIMULATED;
        snapshot.status_flags |= CHEEKY_GAZE_STATUS_GAZE_VALID | CHEEKY_GAZE_STATUS_MAPPING_READY;
        for (unsigned i = 0; i < 2; ++i) {
            snapshot.views[i].image_rect_x = 0;
            snapshot.views[i].image_rect_width = 4992;
            snapshot.views[i].image_rect_height = 5024;
            snapshot.views[i].resource_identity = 0x12345;
            snapshot.views[i].array_index = i;
        }
        frame(); frame(); frame();
        expect(gaze_diagnostics().using_gaze && gaze_diagnostics().views[0].marker_mapping &&
                   gaze_diagnostics().views[1].marker_mapping,
               "Verified markers must route gaze to scaled array submissions");
        for (unsigned i = 0; i < 2; ++i)
            expect_near((crops[i].input_base_x + crops[i].input_width * 0.5F) / 1512.F,
                        snapshot.views[i].center_u, 0.004F, "Array gaze must use the calibrated eye's center");
        expect(publish_stereo_calibration(951, 952, a, b, 1002, GetTickCount64(), nullptr,
            calibration_session, true), "Verified vertical transform publishes with eye pair");
        snapshot.views[0].center_v = 0.25F; snapshot.views[1].center_v = 0.7F;
        frame(); frame(); frame();
        for (unsigned i = 0; i < 2; ++i)
            expect_near((crops[i].input_base_y + crops[i].input_height * 0.5F) / 1418.F,
                1.F - snapshot.views[i].center_v, 0.004F, "Flipped submissions must invert gaze into DLSS coordinates");
        settings.center_mode = FoveationCenterMode::fixed;
        settings.aligned_height_offset = 0;
        snapshot.views[0].forward_v = 0.3F; snapshot.views[1].forward_v = 0.65F;
        frame();
        for (unsigned i = 0; i < 2; ++i)
            expect_near((crops[i].input_base_y + crops[i].input_height * 0.5F) / 1418.F,
                1.F - snapshot.views[i].forward_v, 0.004F, "Flipped submissions must invert automatic forward alignment");
        settings.center_mode = FoveationCenterMode::fixed;
        snapshot.status_flags &= ~CHEEKY_GAZE_STATUS_MAPPING_READY;
        clear_stereo_calibration();
        frame(); frame();
        expect(gaze_diagnostics().alignment_source == 0U, "Ambiguous images require a live marker calibration");
        expect(publish_stereo_calibration(951, 952, a, b, 1002, GetTickCount64(), nullptr, calibration_session + 1),
            "Foreign-session test calibration publishes");
        frame(); frame();
        expect(gaze_diagnostics().alignment_source == 0U, "A calibration from another XR session cannot route crop coordinates");
        clear_stereo_calibration();
    }
    unregister_stereo_view(951U); unregister_stereo_view(952U);
    test_openvr_snapshot=nullptr;
    reset_gaze_foveation();
}

void test_auto_alignment_history(bool openvr) {
    using namespace cheeky::foveated_dlss;
    reset_gaze_foveation();
    register_stereo_view(1903U); register_stereo_view(1904U);
    Settings settings{};
    settings.center_mode = FoveationCenterMode::fixed;
    settings.auto_stereo_alignment = true;
    settings.width = .55F; settings.height = .45F;
    settings.x_offset = settings.height_offset = 0.F;
    settings.aligned_height_offset = -.11F;
    CheekyGazeSnapshotV1 snapshot{};
    snapshot.abi_version = CHEEKY_GAZE_ABI_VERSION;
    snapshot.structure_size = sizeof(snapshot);
    snapshot.view_count = 2U;
    snapshot.session_generation = snapshot.swapchain_generation = 1U;
    snapshot.status_flags = CHEEKY_GAZE_STATUS_MAPPING_READY | CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
    if (openvr) snapshot.status_flags |= CHEEKY_GAZE_STATUS_OPENVR;
    // Avoid half-pixel ties from the support report's odd crop dimensions.
    constexpr float forward_center = .5002F;
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& view = snapshot.views[eye];
        view.view_index = eye;
        view.flags = CHEEKY_GAZE_VIEW_RESOURCE_VALID | CHEEKY_GAZE_VIEW_FORWARD_VALID;
        view.resource_identity = 1903U + eye;
        view.image_rect_width = view.image_rect_height = 2928U;
        view.forward_u = view.forward_v = forward_center;
    }
    CropGeometry crops[2]{};
    bool resets[2]{};
    const auto frame = [&]() {
        ++snapshot.predicted_display_time;
        for (unsigned eye = 0; eye < 2; ++eye) {
            expect(calculate_coordinated_crop(settings, 1903U + eye, nullptr,
                1464U, 1464U, 2928U, 2928U, 0U, 0U, crops[eye], resets[eye],
                &snapshot, nullptr, 1903U + eye), "Fixed alignment history fixture resolves both eyes");
        }
    };
    frame(); frame(); frame();
    expect(gaze_diagnostics().alignment_source == (openvr ? 3U : 2U),
        "Fixed history regression exercises runtime alignment");
    const auto initial = crops[0];
    for (unsigned cycle = 0; cycle < 16; ++cycle) {
        for (unsigned eye = 0; eye < 2; ++eye) {
            const float delta = cycle % 2 == 0 ? (eye == 0 ? 1.F : -1.F) / 1464.F : 0.F;
            snapshot.views[eye].forward_u = snapshot.views[eye].forward_v = forward_center + delta;
        }
        frame();
        for (unsigned eye = 0; eye < 2; ++eye) {
            const int delta = cycle % 2 == 0 ? (eye == 0 ? 1 : -1) : 0;
            expect(crops[eye].input_base_x == initial.input_base_x + delta &&
                crops[eye].input_base_y == initial.input_base_y + delta,
                "Fixed alignment still follows one-pixel movement in both axes");
            expect(!resets[eye], "One-pixel automatic alignment fluctuations must preserve DLSS history");
        }
    }
    // A new mapping must reset even when its crop is numerically identical.
    ++snapshot.swapchain_generation;
    frame();
    expect(resets[0] && resets[1] && crops[0].input_base_x == initial.input_base_x &&
        crops[0].input_base_y == initial.input_base_y, "Fixed alignment remapping resets unchanged crop history");
    frame(); frame();
    expect(!resets[0] && !resets[1], "Stable fixed mapping does not repeatedly reset");
    for (auto& view : snapshot.views) view.flags &= ~CHEEKY_GAZE_VIEW_RESOURCE_VALID;
    frame();
    expect(resets[0] && resets[1], "Losing a fixed eye mapping invalidates history");
    for (auto& view : snapshot.views) view.flags |= CHEEKY_GAZE_VIEW_RESOURCE_VALID;
    frame(); frame(); frame();
    for (auto& view : snapshot.views) view.forward_u += .15F;
    frame();
    expect(resets[0] && resets[1], "Large automatic alignment jumps still reset history");
    frame();
    expect(!resets[0] && !resets[1], "History settles after a large alignment jump");
    settings.width = .5F;
    frame();
    expect(resets[0] && resets[1], "Fixed aligned crop resizing still resets history");
    frame();
    expect(!resets[0] && !resets[1], "History settles after aligned crop resizing");
    unregister_stereo_view(1903U); unregister_stereo_view(1904U);
    reset_gaze_foveation();
}

void test_auto_alignment() {
    using namespace cheeky::foveated_dlss;
    using namespace cheeky::gaze_math;
    Pose head{};
    const Pose left{{0.F, std::sin(0.1F), 0.F, std::cos(0.1F)}, {}};
    const Pose right{{0.F, -std::sin(0.1F), 0.F, std::cos(0.1F)}, {}};
    expect(stereo_forward_pose(left, right, head), "canted stereo has shared forward");
    expect_near(head.orientation.y, 0.F, 0.0001F, "opposite eye cants cancel");
    Pose negative_right = right;
    negative_right.orientation.y *= -1.F;
    negative_right.orientation.w *= -1.F;
    Pose same_head{};
    expect(stereo_forward_pose(left, negative_right, same_head), "quaternion hemisphere is handled");
    expect_near(same_head.orientation.y, 0.F, 0.0001F, "quaternion sign does not change forward");
    float u{}, v{}, right_u{};
    const Fov fov{-0.8F, 0.8F, 0.8F, -0.8F};
    expect(project_gaze_to_view(head, left, fov, u, v) &&
        project_gaze_to_view(head, right, fov, right_u, v), "shared forward projects into both canted eyes");
    expect_near(u + right_u, 1.F, 0.0001F, "canted eyes receive opposite horizontal centers");
    expect(std::abs(u - 0.5F) > 0.05F, "cant correction differs from individual optical axis");

    reset_gaze_foveation();
    Settings settings{};
    settings.center_mode = FoveationCenterMode::fixed;
    settings.auto_stereo_alignment = true;
    settings.width = settings.height = 0.5F;
    update_settings(settings);
    expect(current_settings().auto_stereo_alignment, "automatic alignment survives settings validation");
    register_stereo_view(901U); register_stereo_view(902U);
    (void)settings_for_view(settings, 902U); // Right-first evaluation must not change projection placement.
    CropGeometry crop{};
    bool reset{};
    const auto calculate = [&]() { return calculate_coordinated_crop(settings, 901U, nullptr,
        1000U, 1000U, 2000U, 2000U, 0U, 0U, crop, reset); };
    {
        ScopedGazeProjection scope(901U, {-0.8F, 1.2F, 1.2F, -0.8F, true});
        expect(calculate(), "automatic Streamline crop needs no XR layer");
        expect(crop.input_base_x == 150U && crop.input_base_y == 350U,
            "asymmetric projection aligns both axes");
        expect(gaze_diagnostics().alignment_source == 1U, "projection alignment is reported");
        settings.invert_stereo_x_offset = true;
        expect(calculate() && crop.input_base_x == 150U && !reset,
            "manual eye inversion does not affect automatic alignment");
        {
            ScopedGazeProjection moved(901U, {-0.802F, 1.198F, 1.202F, -0.798F, true});
            expect(calculate() && crop.input_base_x == 151U && crop.input_base_y == 351U && !reset,
                "One-pixel camera-projection alignment movement preserves history");
        }
        expect(calculate() && crop.input_base_x == 150U && crop.input_base_y == 350U && !reset,
            "Returning camera-projection alignment by one pixel preserves history");
        settings.width = 0.3F;
        expect(calculate() && crop.input_base_x == 250U && reset,
            "resizing preserves center and resets changed crop history");
        expect(calculate() && !reset, "stable auto placement keeps history");
        settings.width = 0.9F;
        expect(calculate() && crop.input_base_x == 0U,
            "large crop stays within texture bounds");
    }
    expect(calculate() && gaze_diagnostics().alignment_source == 0U && reset,
        "missing projection returns to manual fallback and resets history");
    const auto fallback = crop;
    {
        ScopedGazeProjection wrong_view(902U, {-0.8F, 1.2F, 1.2F, -0.8F, true});
        expect(calculate() && crop.input_base_x == fallback.input_base_x &&
            gaze_diagnostics().alignment_source == 0U, "another view's projection is rejected");
    }
    expect(!projection_forward_center({0.F, 0.F, 0.F, 0.F, true}, u, v),
        "invalid frustum cannot activate auto alignment");
    unregister_stereo_view(901U); unregister_stereo_view(902U);
    update_settings(Settings{});
    reset_gaze_foveation();
}

[[nodiscard]] bool run_openxr_gaze_lookup() {
    using namespace cheeky::foveated_dlss;
    Settings settings{};
    settings.center_mode = FoveationCenterMode::openxr_gaze;
    CropGeometry crop{};
    bool reset_history{};
    return calculate_coordinated_crop(
        settings,
        1U,
        nullptr,
        1000U,
        1000U,
        2000U,
        2000U,
        0U,
        0U,
        crop,
        reset_history
    );
}

void test_openxr_layer_is_retained_while_snapshot_export_is_cached() {
    using namespace cheeky::foveated_dlss;
    constexpr wchar_t layer_name[] = L"CheekyOpenXRLayer.dll";

    reset_gaze_foveation();
    expect(GetModuleHandleW(layer_name) == nullptr,
        "OpenXR layer starts unloaded in the test process");
    expect(run_openxr_gaze_lookup(),
        "gaze lookup falls back while the OpenXR layer is absent");
    expect(!gaze_diagnostics().layer_present,
        "absent OpenXR layer is reported as unavailable");

    const auto first_loader_reference = LoadLibraryW(layer_name);
    expect(first_loader_reference != nullptr,
        "test loads the real OpenXR layer DLL");
    if (first_loader_reference == nullptr) return;

    const auto snapshot_export = reinterpret_cast<CheekyOpenXRGetGazeSnapshotFn>(
        GetProcAddress(first_loader_reference, "CheekyOpenXR_GetGazeSnapshot"));
    expect(snapshot_export != nullptr, "layer exports the versioned snapshot function");
    if (snapshot_export) {
        CheekyGazeSnapshotV1 snapshot{};
        snapshot.sequence = 123;
        expect(snapshot_export(2U, &snapshot, 320U) == 0U && snapshot.sequence == 123,
            "old ABI buffer is rejected without being overwritten");
        expect(snapshot_export(3U, &snapshot, 352U) == 0U && snapshot.sequence == 123,
            "previous projection ABI is rejected without overwriting its buffer");
        expect(snapshot_export(4U, &snapshot, sizeof(snapshot)) != 0U && snapshot.abi_version == 4U,
            "new layer and add-on agree on projection snapshot ABI");
    }

    expect(run_openxr_gaze_lookup(),
        "gaze lookup caches the OpenXR layer snapshot export");
    expect(gaze_diagnostics().layer_present,
        "resolved OpenXR layer export is reported as present");
    expect(FreeLibrary(first_loader_reference) != FALSE,
        "simulated OpenXR loader releases its first layer reference");
    expect(GetModuleHandleW(layer_name) != nullptr,
        "cached snapshot export retains the OpenXR layer DLL");
    expect(run_openxr_gaze_lookup(),
        "cached snapshot export remains callable between instances");

    const auto second_loader_reference = LoadLibraryW(layer_name);
    expect(second_loader_reference != nullptr,
        "simulated recreated OpenXR instance reloads the layer");
    if (second_loader_reference != nullptr) {
        expect(run_openxr_gaze_lookup(),
            "cached snapshot export remains callable after instance recreation");
        expect(FreeLibrary(second_loader_reference) != FALSE,
            "simulated OpenXR loader releases its recreated layer reference");
    }
    expect(GetModuleHandleW(layer_name) != nullptr,
        "add-on reference retains the layer after recreated instance teardown");

    reset_gaze_foveation();
    expect(GetModuleHandleW(layer_name) == nullptr,
        "gaze shutdown releases the retained OpenXR layer reference");
}

}  // namespace

void test_gaze_camera_projection() {
    using namespace cheeky::foveated_dlss;
    const GazeProjection left{-1.2F, 0.8F, 1.F, -0.9F, true};
    const GazeProjection right{-0.8F, 1.2F, 1.F, -0.9F, true};
    expect(match_gaze_projection_eyes(left, {left, right}).count == 1U &&
        match_gaze_projection_eyes(left, {left, right}).index == 0U,
        "two distinct XR projections give a unique eye match");
    const auto ambiguous = match_gaze_projection_eyes(left, {left, left});
    GazeMappingPolicyState mapping{};
    expect(!update_gaze_mapping(mapping, ambiguous.count, ambiguous.index, 1, 1).stable &&
        !update_gaze_mapping(mapping, ambiguous.count, ambiguous.index, 1, 2).stable,
        "identical eye projections never establish a stable eye mapping");
    expect(match_gaze_projection_eyes(left, {left, {}}).count == 0,
        "missing other eye projection cannot establish uniqueness");
    // Independent off-center perspective matrix, with a near=0.1 far=100 range.
    std::array<float, 16> matrix{1,0,0,0, 0,2.F/1.9F,0,0,
        0.2F,-0.1F/1.9F,100.F/99.9F,1, 0,0,-10.F/99.9F,0};
    auto camera = gaze_projection_from_matrix(matrix.data());
    expect(gaze_projection_matches(camera, left) && !gaze_projection_matches(camera, right),
        "asymmetric projection uniquely identifies left eye independent of viewport number");
    matrix[8] = -0.2F;
    camera = gaze_projection_from_matrix(matrix.data());
    expect(gaze_projection_matches(camera, right) && !gaze_projection_matches(camera, left),
        "opposite off-center projection identifies right eye");
    matrix[8] *= -1; matrix[9] *= -1; matrix[10] *= -1; matrix[11] = -1;
    expect(gaze_projection_matches(gaze_projection_from_matrix(matrix.data()), right),
        "right-handed projection retains physical eye identity");
    matrix[10] = 0; matrix[14] = 0.1F;
    expect(gaze_projection_matches(gaze_projection_from_matrix(matrix.data()), right),
        "reversed infinite depth does not change eye identification");
    matrix[8] = 0;
    expect(!gaze_projection_matches(gaze_projection_from_matrix(matrix.data()), left) &&
        !gaze_projection_matches(gaze_projection_from_matrix(matrix.data()), right),
        "symmetric desktop projection cannot match asymmetric VR eyes");
    matrix[11] = 0; matrix[15] = 1;
    expect(!gaze_projection_from_matrix(matrix.data()).valid, "orthographic matrix rejected");
    matrix[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!gaze_projection_from_matrix(matrix.data()).valid, "non-finite matrix rejected");
    GazeProjectionCache cache;
    cache.record(42, 7, 100, left); cache.record(1, 7, 101, right);
    expect(gaze_projection_matches(cache.find(42, 7, 102), left),
        "another viewport's constants do not overwrite the eye projection");
    expect(!cache.find(42, 8, 102).valid && !cache.find(42, 7, 201).valid,
        "wrong frame index and stale constants rejected");
    cache.record(42, 8, 202, {});
    expect(!cache.find(42, 8, 203).valid, "rejected constants invalidate previous projection");
    {
        ScopedGazeProjection scope(43, left);
        expect(active_gaze_projection.view == 43, "projection scoped to actual evaluated view");
        { ScopedGazeProjection nested(2, right); }
        expect(active_gaze_projection.view == 43, "nested scope restores outer camera");
    }
    expect(!active_gaze_projection.projection.valid, "camera does not leak into other evaluation paths");
}

void test_gaze_copy_routes() {
    using namespace cheeky::foveated_dlss;
    const GazeCopyRegion source{1, 0, 0, 0, 100, 80};
    const GazeCopyRegion intermediate{2, 0, 10, 20, 100, 80};
    const GazeCopyRegion left{3, 0, 0, 0, 100, 80};
    const GazeCopyRegion right{4, 0, 0, 0, 100, 80};
    GazeCopyGraph graph;
    expect(!graph.reaches(source, left, 100), "same dimensions alone do not map an eye");
    graph.record({source, intermediate}, 100);
    graph.record({intermediate, left}, 101);
    expect(graph.reaches(source, left, 102), "submitted two-hop copy translates regions");
    expect(!graph.reaches(source, right, 102), "copy route identifies only its destination eye");
    auto wrong_slice = left; wrong_slice.subresource = 1;
    expect(!graph.reaches(source, wrong_slice, 102), "copy mapping preserves subresource identity");
    graph.record({intermediate, right}, 103);
    expect(graph.reaches(source, left, 104) && graph.reaches(source, right, 104),
        "shared output exposes both matches so coordinator rejects ambiguity");
    graph.forget(intermediate.resource);
    expect(!graph.reaches(source, left, 104), "destroying intermediate invalidates route");
    graph.record({source, left}, 200);
    expect(!graph.reaches(source, left, 701), "old copy routes expire");
    graph.clear();
    graph.record({intermediate, left}, 800);
    graph.record({source, intermediate}, 801);
    expect(!graph.reaches(source, left, 802), "reversed copy order cannot establish provenance");
    graph.clear();
    auto scaled = left; scaled.width = 200;
    graph.record({source, scaled}, 900);
    expect(!graph.reaches(source, scaled, 900), "scaled copies are not treated as pixel translations");
}

int run_d3d12_composite_tests();
int run_eye_calibration_tests();
int run_stereo_support_tests();
int run_stereo_support12_tests();
int run_openxr_calibration_tests();
int run_support_summary_tests();
int run_nr_processing_tests();

void test_openvr_geometry() {
    using namespace cheeky::foveated_dlss;
    expect(openvr_submit_slot("IVRCompositor_022")==5 && openvr_submit_slot("IVRCompositor_029")==6,
        "legacy and current compositor layouts use verified slots");
    expect(openvr_submit_slot("IVRCompositor_030")==0 && openvr_submit_slot(nullptr)==0,
        "unknown compositor ABI is rejected");
    float u{},v{};
    expect(openvr_ndc_center(0.5F,0.5F,u,v), "native gaze converts to texture coordinates");
    expect_near(u,0.75F,0.0001F,"NDC right maps right");
    expect_near(v,0.25F,0.0001F,"NDC up maps up");
    expect(!openvr_ndc_center(NAN,0,u,v),"nonfinite gaze is invalid");
    std::int32_t x{}; std::uint32_t width{};
    expect(openvr_bounds(0.5F,1.F,3024,x,width) && x==1512 && width==1512,"packed eye bounds become exact pixel rectangles");
    expect(!openvr_bounds(1.F,0.F,3024,x,width),"flipped submission bounds fail safely");
    expect(!openvr_bounds(0.1F,0.9F,11,x,width),"fractional pixel bounds fail safely");
    const float identity[3][4]{{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    const float up[3]{0,0.5F,-1};
    expect(openvr_project_direction(identity,-1,1,-1,1,up,u,v),"synthetic gaze projects through OpenVR frustum");
    expect_near(v,0.25F,0.0001F,"raw OpenVR top/bottom sign is converted correctly");
    const float forward[3]{0,0,-1};
    expect(openvr_project_direction(identity,-0.8F,1.2F,-0.9F,1.1F,forward,u,v),"asymmetric alignment projects");
    expect_near(u,0.4F,0.0001F,"asymmetric horizontal center");
    expect_near(v,0.45F,0.0001F,"asymmetric vertical center");
    const float c=std::cos(0.2F),s=std::sin(0.2F);
    const float canted[3][4]{{c,0,s,0},{0,1,0,0},{-s,0,c,0}};
    expect(openvr_project_direction(canted,-1,1,-1,1,forward,u,v),"eye cant is included");
    expect_near(u,(1.F+std::tan(0.2F))*0.5F,0.0001F,"inverse eye rotation projects head forward");
}

void test_center_supersampling() {
    using namespace cheeky::foveated_dlss;
    const FoveationGeometry crop{100, 50, 501, 301, 1200, 100, 1002, 602};
    const auto reduced = supersampled_crop(crop, 0.5F);
    expect(reduced.output_width == crop.output_width && reduced.output_height == crop.output_height,
        "legacy sub-1x requests clamp to the original output resolution");
    const auto enlarged = supersampled_crop(crop, 1.5F);
    expect(enlarged.output_width == 1503 && enlarged.output_height == 903,
        "supersampling enlarges only the reconstruction resolution");
    expect(enlarged.input_width == crop.input_width && enlarged.input_height == crop.input_height &&
        enlarged.input_base_x == crop.input_base_x && enlarged.input_base_y == crop.input_base_y &&
        enlarged.output_base_x == crop.output_base_x && enlarged.output_base_y == crop.output_base_y,
        "supersampling retains input crop and packed-eye placement");
    expect(supersampled_crop(crop, 2.0F).output_width == 2004,
        "supersampling is independent of motion-vector resolution");
    expect(supersampled_crop(crop, 1.0F).output_width == crop.output_width,
        "1x retains the original DLSS contract");
    expect(supersampled_crop(crop, std::numeric_limits<float>::quiet_NaN()).output_width == crop.output_width,
        "non-finite scale falls back to 1x");
    auto large = crop;
    large.output_width = 12000; large.output_height = 6000;
    const auto bounded = supersampled_crop(large, 2.0F);
    expect(bounded.output_width == 16384 && bounded.output_height == 8192,
        "texture limit bounds both axes with a shared scale");
    const auto saved = current_settings();
    auto settings = saved;
    settings.center_supersampling = 1.25F;
    update_settings(settings);
    expect(current_settings().center_supersampling == 1.25F, "supersampling setting round trips");
    settings.center_supersampling = 0.5F;
    update_settings(settings);
    expect(current_settings().center_supersampling == 1.0F, "legacy 0.5x settings migrate to 1x");
    settings.center_supersampling = 0.0F;
    update_settings(settings);
    expect(current_settings().center_supersampling == 1.0F, "scale clamps to the 1x lower limit");
    settings.center_supersampling = 20.0F;
    update_settings(settings);
    expect(current_settings().center_supersampling == 2.0F, "supersampling setting is bounded");
    settings.center_supersampling = std::numeric_limits<float>::quiet_NaN();
    update_settings(settings);
    expect(current_settings().center_supersampling == 1.0F, "invalid supersampling setting is disabled");
    update_settings(saved);
}

int run_motion_resample_tests();
int run_openxr_calibration_format_tests();

void test_native_dynamic_resolution_extent() {
    using namespace cheeky::foveated_dlss;
    MockNgxParameters parameters;
    parameters.Set("Width", 4152U); parameters.Set("Height", 3336U);
    // Replay DanceXR's optimal-settings query overwriting a reused bag.
    parameters.Set("OutWidth", 2076U); parameters.Set("OutHeight", 1668U);
    parameters.Set("DLSS.Render.Subrect.Dimensions.Width", 2076U);
    parameters.Set("DLSS.Render.Subrect.Dimensions.Height", 1668U);
    const auto before = parameters.values;
    {
        NgxEvaluationExtentScope scope(&parameters, {4152, 3336});
        expect(get_ui(&parameters, "Width") == 2076 && get_ui(&parameters, "Height") == 1668 &&
            get_ui(&parameters, "OutWidth") == 4152 && get_ui(&parameters, "OutHeight") == 3336,
            "Native evaluation uses current render subrect and created output, not query scratch values");
        Settings settings;
        settings.width = .55F; settings.height = .45F;
        CropGeometry crop;
        const auto width = get_ui(&parameters, "Width"), height = get_ui(&parameters, "Height");
        expect(calculate_foveation_geometry_at_center(foveation_parameters(settings), {.8F, .75F, 1},
            width, height, get_ui(&parameters, "OutWidth"), get_ui(&parameters, "OutHeight"), 0, 0, crop),
            "Dynamic-resolution gaze crop is valid");
        expect(resolve_motion_region(true, 2, true, 2595, 2084, 0, 0, crop, width, height,
            4152, 3336, 0, 0).valid(), "Dynamic-resolution crop fits DanceXR's allocated input motion texture");
    }
    expect(parameters.values == before, "Native normalization restores the application's reused parameter bag");
    {
        NgxEvaluationExtentScope late(&parameters, {});
        expect(get_ui(&parameters, "OutWidth") == 2076, "Late attachment cannot guess a feature's creation size");
    }
    parameters.Set("Width", 2076U); parameters.Set("Height", 1668U);
    parameters.Set("OutWidth", 4152U); parameters.Set("OutHeight", 3336U);
    const auto compatible = parameters.values;
    {
        NgxEvaluationExtentScope scope(&parameters, {4152, 3336});
        expect(parameters.values == compatible, "Compatible native contracts remain unchanged");
    }
    MockNgxParameters absent;
    { NgxEvaluationExtentScope scope(&absent, {4152, 3336}); }
    expect(absent.values.empty(), "Missing keys are not invented and cannot leak into the game");
}

void test_afw_dispatch_and_settings() {
    using namespace cheeky::foveated_dlss;
    expect(is_nvidia_ngx_core_alias_identity(L"C:/driver/NVNGX.DLL", L"NVIDIA Corporation",
        L"nvngx.dll", L"NGX", true, false), "NVIDIA's documented nvngx core alias is recognized");
    expect(is_nvidia_ngx_core_alias_identity(L"C:\\game\\nvngx.dll", L"NVIDIA Corporation",
        L"_nvngx.dll", L"NGX", true, false), "Renamed original NVIDIA core retains its identity");
    expect(!is_nvidia_ngx_core_alias_identity(L"C:/game/nvngx.dll", L"OptiScaler",
        L"OptiScaler.dll", L"OptiScaler", true, false), "OptiScaler renamed nvngx.dll is not a core runtime");
    expect(!is_nvidia_ngx_core_alias_identity(L"C:/game/nvngx.dll", L"NVIDIA Corporation",
        L"nvngx.dll", L"NGX", true, true), "ASI or DXGI proxy exports reject a purported NVIDIA core");
    expect(!is_nvidia_ngx_core_alias_identity(L"C:/game/nvngx.dll", L"NVIDIA Corporation",
        L"nvngx_dlss.dll", L"NGX", true, false), "A renamed feature snippet is not a core runtime");
    expect(!is_nvidia_ngx_core_alias_identity(L"C:/game/nvngx.dll", L"NVIDIA Corporation",
        L"nvngx.dll", L"NGX", false, false) &&
        !is_nvidia_ngx_core_alias_identity(L"C:/game/nvngx.dll", L"", L"", L"", true, false) &&
        !is_nvidia_ngx_core_alias_identity(L"C:/game/dxgi.dll", L"NVIDIA Corporation",
            L"nvngx.dll", L"NGX", true, false), "Missing core exports, unknown metadata and other aliases fail closed");
    expect(is_dlss_sr_runtime_path(L"C:/game/NVNGX_DLSS.DLL") &&
        is_dlss_sr_runtime_path(L"C:/ProgramData/NVIDIA/NGX/models//DLSS/versions/20318464/files/160_E658700.BIN"),
        "SR discovery accepts normal DLLs and generated OTA names with mixed separators and case");
    expect(!is_dlss_sr_runtime_path(L"C:/NGX/models/dlssnr/versions/1/files/160.bin") &&
        !is_dlss_sr_runtime_path(L"C:/NGX/models/dlssd/versions/1/files/160.bin") &&
        !is_dlss_sr_runtime_path(L"C:/NGX/models/sl_dlss_0/versions/1/files/160.bin") &&
        !is_dlss_sr_runtime_path(L"C:/game/160.bin") && !is_dlss_sr_runtime_path(L"C:/game/nvngx_dlssg.dll"),
        "SR discovery rejects NR, RR, Streamline, FG and unrelated generated names");
    Settings saved;
    saved.width = .4F; saved.height = .9F; saved.nr_enabled = true;
    saved.center_supersampling = 2.F; saved.center_mode = FoveationCenterMode::simulated_gaze;
    const auto effective = afw_experiment_settings(saved);
    expect(effective.width == .7F && effective.height == .9F && effective.x_offset == 0.F && effective.height_offset == 0.F &&
        !effective.auto_stereo_alignment && effective.nr_enabled && effective.center_supersampling == 2.F &&
        effective.center_mode == FoveationCenterMode::simulated_gaze, "AFW preserves the requested gaze source with a generous fixed fallback");
    expect(saved.width == .4F && saved.nr_enabled && saved.center_supersampling == 2.F, "AFW overrides leave saved settings intact");
    auto independent = saved;
    independent.afw_manual_coverage = true; independent.nr_use_sr_foveation = false;
    independent.nr_width = .3F; independent.nr_height = .4F;
    const auto nr_envelope = afw_experiment_settings(independent);
    independent.width = .9F;
    expect(afw_experiment_settings(independent).afw_nr.width == nr_envelope.afw_nr.width,
        "Independent AFW NR coverage is unaffected by SR size changes");
    independent.nr_use_sr_foveation = true;
    const auto linked = afw_experiment_settings(independent);
    expect(linked.afw_nr.width == linked.width && linked.afw_nr.height == linked.height,
        "Linked AFW NR includes the same fixed stereo envelope as SR");
    for (const auto width : {.2F, .55F, .7F, 1.F})
    for (const auto height : {.2F, .45F, .7F, 1.F})
    for (const auto x : {-1.F, -.6F, 0.F, .6F, 1.F})
    for (const auto y : {-1.F, -.45F, 0.F, .45F, 1.F})
    for (const auto margin : {0.F, .05F, .25F}) {
        Settings manual;
        manual.afw_manual_coverage = true; manual.afw_warp_margin = margin;
        manual.width = width; manual.height = height;
        manual.x_offset = x; manual.height_offset = y; manual.roundness = 1.F;
        const auto envelope = afw_experiment_settings(manual);
        expect(envelope.roundness == 1.F && envelope.afw_mask.count == 2 && !uses_coordinated_center(envelope),
            "Manual rounded coverage retains two independent eye masks without an eye assignment");
        const float left = (1.F - envelope.width) * .5F;
        const float top = (1.F - envelope.height) * (1.F + envelope.height_offset) * .5F;
        for (const auto eye_sign : {-1.F, 1.F}) {
            const float eye_left = (1.F - width) * (1.F + x * eye_sign) * .5F;
            const float eye_top = (1.F - height) * (1.F + y) * .5F;
            expect(left <= eye_left + 1e-6F && left + envelope.width + 1e-6F >= eye_left + width &&
                top <= eye_top + 1e-6F && top + envelope.height + 1e-6F >= eye_top + height,
                "AFW envelope contains both possible eye rectangles, including image edges");
        }
        auto inverted = manual; inverted.invert_stereo_x_offset = true;
        const auto other = afw_experiment_settings(inverted);
        expect(other.width == envelope.width && other.height_offset == envelope.height_offset,
            "AFW coverage is invariant under an eye-order swap");
    }
    auto invalid = saved;
    invalid.afw_manual_coverage = true;
    invalid.afw_warp_margin = invalid.width = invalid.height = invalid.x_offset =
        invalid.height_offset = invalid.center_supersampling = std::numeric_limits<float>::quiet_NaN();
    const auto sanitized = afw_experiment_settings(invalid);
    expect(std::isfinite(sanitized.width) && std::isfinite(sanitized.height_offset) && sanitized.center_supersampling == 1.F,
        "Invalid AFW coverage inputs fail to finite defaults");
    const auto previous_settings = configured_settings();
    invalid = previous_settings; invalid.afw_manual_coverage = true; invalid.afw_warp_margin = .15F;
    update_settings(invalid);
    expect(configured_settings().afw_manual_coverage && configured_settings().afw_warp_margin == .15F,
        "AFW settings survive the render settings snapshot");
    invalid.afw_warp_margin = 5.F; update_settings(invalid);
    expect(configured_settings().afw_warp_margin == .25F, "AFW warp margin is bounded");
    invalid.width = invalid.height = invalid.nr_width = invalid.nr_height = .1F;
    update_settings(invalid);
    const auto minimum = configured_settings();
    expect(minimum.width == .2F && minimum.height == .2F && minimum.nr_width == .2F && minimum.nr_height == .2F,
        "Older 10 percent fovea settings clamp to the restored 20 percent minimum for SR and NR");
    update_settings(previous_settings);
    // The process-wide latch is intentional, so this runs after normal dispatch tests.
    enable_afw_compatibility();
    D3D12DispatchHarness harness{}; dispatch_harness = &harness;
    D3D12NgxEvaluationCall call{D3D12NgxRoute::core_runtime};
    const auto lower_from_core = +[](ID3D12GraphicsCommandList* list, const NgxHandle* handle,
        const NgxParameters* params, NgxProgressCallback callback) -> NgxResult {
        ++dispatch_harness->original_calls;
        const D3D12NgxEvaluationCall lower{D3D12NgxRoute::public_runtime, list, handle, params, callback};
        return dispatch_d3d12_ngx_evaluation(lower, &fake_d3d12_original, &fake_d3d12_processor, dispatch_harness);
    };
    expect(dispatch_d3d12_ngx_evaluation(call, lower_from_core, &fake_d3d12_processor, &harness) == 0x200U &&
        harness.processor_calls == 1 && harness.observed_route == D3D12NgxRoute::public_runtime,
        "AFW core forwards unchanged and processing belongs to its nested lower route");
    harness.nest_core_evaluation = true;
    expect(dispatch_d3d12_ngx_evaluation(call, lower_from_core, &fake_d3d12_processor, &harness) == 0xBAD00007U &&
        harness.original_calls == 2, "Private core reentry is rejected before reaching core original");
    expect(dispatch_d3d12_ngx_evaluation(call, &fake_d3d12_original, &fake_d3d12_processor, &harness) == 0x100U,
        "AFW core without visible lower route is passthrough");
    call.route = D3D12NgxRoute::public_runtime;
    expect(dispatch_d3d12_ngx_evaluation(call, &fake_d3d12_original, &fake_d3d12_processor, &harness) == 0x100U &&
        harness.processor_calls == 2, "Independent lower call cannot bypass the full-frame AFW boundary");
    const auto core_from_public = +[](ID3D12GraphicsCommandList* list, const NgxHandle* handle,
        const NgxParameters* params, NgxProgressCallback callback) -> NgxResult {
        const D3D12NgxEvaluationCall core{D3D12NgxRoute::core_runtime, list, handle, params, callback};
        return dispatch_d3d12_ngx_evaluation(core, &fake_d3d12_original, &fake_d3d12_processor, dispatch_harness);
    };
    expect(dispatch_d3d12_ngx_evaluation(call, core_from_public, &fake_d3d12_processor, &harness) == 0x100U &&
        harness.processor_calls == 2, "Public-to-core passthrough must not be mistaken for private reentry");
    const auto status = afw_compatibility_status();
    expect(status.core_calls == 4 && status.lower_calls == 2 && status.missing_lower_calls == 2 &&
        status.rejected_core_reentry == 1 && status.standalone_lower_calls == 2 && !d3d12_ngx_interception_active(),
        "AFW routing diagnostics and thread scope survive failure and fallback");
    MockNgxParameters depth_parameters;
    const auto source = reinterpret_cast<ID3D12Resource*>(0x1000);
    const auto list = reinterpret_cast<ID3D12GraphicsCommandList*>(0x2000);
    depth_parameters.Set("Depth", source);
    depth_parameters.Set("Output", reinterpret_cast<ID3D12Resource*>(0x3000));
    afw_bind_depth_eye(1, 0x3000);
    const auto copied_core = +[](ID3D12GraphicsCommandList* cmd, const NgxHandle* h,
        const NgxParameters* p, NgxProgressCallback cb) -> NgxResult {
        ID3D12Resource* src{}; ID3D12Resource* dst{};
        p->Get("Depth", &src); p->Get("Output", &dst);
        afw_observe_depth_copy(cmd, reinterpret_cast<ID3D12Resource*>(0x9999), reinterpret_cast<std::uint64_t>(dst));
        if (afw_current_source_eye() != UINT32_MAX) return 0;
        afw_observe_depth_copy(cmd, src, reinterpret_cast<std::uint64_t>(dst));
        const auto lower = +[](const D3D12NgxEvaluationCall&, D3D12NgxEvaluateFn, void*) -> NgxResult {
            return afw_current_source_eye() == 1 ? 0x100U : 0U;
        };
        return dispatch_d3d12_ngx_evaluation({D3D12NgxRoute::public_runtime, cmd, h, p, cb}, fake_d3d12_original, lower);
    };
    expect(dispatch_d3d12_ngx_evaluation({D3D12NgxRoute::core_runtime, list, nullptr, &depth_parameters}, copied_core, nullptr) == 0x100 &&
        afw_compatibility_status().last_evaluation_eye == 1 && afw_current_source_eye() == UINT32_MAX,
        "Only the current core call's own depth copy identifies the nested evaluation; scope cannot leak into the next frame");
    afw_forget_depth_resource(0x3000);
    allow_afw_stereo_projection(true);
    publish_afw_rendering_mode(0);
    expect(afw_compatibility_enabled() && !afw_coverage_enabled(), "AFW off restores normal geometry while routing stays protected");
    for (unsigned mode : {0U, 1U, 2U}) {
        publish_afw_rendering_mode(mode);
        harness.nest_core_evaluation = false;
        const auto processed = harness.processor_calls;
        expect(dispatch_d3d12_ngx_evaluation(call, fake_d3d12_original, fake_d3d12_processor, &harness) == 0x200U &&
            harness.processor_calls == processed + 1, "Known non-AFW modes retain standalone public processing");
        harness.nest_core_evaluation = true;
        const auto originals = harness.original_calls;
        expect(dispatch_d3d12_ngx_evaluation(call, fake_d3d12_original, fake_d3d12_processor, &harness) == 0x100U &&
            harness.processor_calls == processed + 2 && harness.original_calls == originals + 1 &&
            !d3d12_ngx_interception_active(), "Non-AFW private public-to-core forwarding processes exactly once");
    }
    publish_afw_rendering_mode(3);
    expect(afw_coverage_enabled(), "AFW can be re-enabled without restarting the process");
    auto processed = harness.processor_calls;
    expect(dispatch_d3d12_ngx_evaluation(call, fake_d3d12_original, fake_d3d12_processor, &harness) == 0x100U &&
        harness.processor_calls == processed, "Re-enabling AFW protects independent public calls again");
    const auto core_switches_off = +[](ID3D12GraphicsCommandList* cmd, const NgxHandle* h,
        const NgxParameters* p, NgxProgressCallback cb) -> NgxResult {
        publish_afw_rendering_mode(0);
        return dispatch_d3d12_ngx_evaluation({D3D12NgxRoute::public_runtime, cmd, h, p, cb},
            fake_d3d12_original, fake_d3d12_processor, dispatch_harness);
    };
    expect(dispatch_d3d12_ngx_evaluation({D3D12NgxRoute::core_runtime}, core_switches_off,
        fake_d3d12_processor, &harness) == 0xBAD00007U,
        "An existing AFW core scope stays protected if the host switches modes during the call");
    processed = harness.processor_calls;
    publish_afw_rendering_mode(UINT32_MAX);
    expect(afw_coverage_enabled(), "Unknown host mode cannot disable compatibility coverage");
    expect(dispatch_d3d12_ngx_evaluation(call, fake_d3d12_original, fake_d3d12_processor, &harness) == 0x100U &&
        harness.processor_calls == processed, "Unknown host mode keeps standalone calls in passthrough");
    publish_afw_rendering_mode(0);
    Sleep(270);
    expect(dispatch_d3d12_ngx_evaluation(call, fake_d3d12_original, fake_d3d12_processor, &harness) == 0x100U &&
        harness.processor_calls == processed, "A stale non-AFW mode cannot bypass the protected route");
    allow_afw_stereo_projection(false);
    dispatch_harness = nullptr;
}

void test_afw_projection_and_metadata() {
    using namespace cheeky::foveated_dlss;
    float matrices[2][16]{};
    for (auto& m : matrices) { m[0] = m[5] = m[11] = 1.F; m[14] = 10.F; }
    matrices[0][8] = .4F; matrices[1][8] = -.2F; matrices[0][9] = .2F;
    AfwProjectionCache cache;
    cache.publish(matrices, 256, 256, true, 1000);
    auto projection = cache.snapshot(1100);
    expect(projection.valid && std::abs(projection.centers[0].u - .7F) < 1e-6F &&
        std::abs(projection.centers[1].u - .4F) < 1e-6F && std::abs(projection.centers[0].v - .4F) < 1e-6F,
        "Public UE projection memory order resolves asymmetric optical centers");
    expect(!cache.snapshot(999).valid && !cache.snapshot(1251).valid, "Projection clock reversal and staleness reject automatic placement");
    expect(afw_projection_matches_output(projection, 256, 256) && !afw_projection_matches_output(projection, 512, 256) &&
        afw_projection_matches_output(projection, 256, 256, 1, 0) && !afw_projection_matches_output(projection, 256, 256, UINT32_MAX, 0), "Projection accepts a complete eye inside a padded allocation, with bounded origins");
    Settings settings;
    settings.afw_automatic_coverage = settings.afw_manual_coverage = true;
    settings.width = .4F; settings.height = .3F; settings.afw_warp_margin = .05F;
    auto resolved = afw_experiment_settings(settings, &projection);
    expect(std::abs(resolved.width - .8F) < 1e-6F && std::abs(resolved.height - .5F) < 1e-6F &&
        std::abs(resolved.x_offset - .5F) < 1e-5F && std::abs(resolved.height_offset + .2F) < 1e-5F,
        "Automatic envelope contains both off-center projections and edge padding");
    expect(settings_for_view(resolved, 0xFFFFFFFFULL).x_offset == resolved.x_offset,
        "Unassigned DLSS handle cannot recenter or mirror an AFW envelope");
    std::swap(projection.centers[0], projection.centers[1]);
    const auto swapped = afw_experiment_settings(settings, &projection);
    expect(swapped.x_offset == resolved.x_offset && swapped.width == resolved.width, "Eye-order changes leave automatic coverage unchanged");
    projection.valid = false;
    const auto fallback = afw_experiment_settings(settings, &projection);
    expect(fallback.width == .7F && fallback.height == .7F && fallback.x_offset == 0.F,
        "Unavailable automatic projection uses centered fallback, not saved manual offsets");
    matrices[1][0] = std::numeric_limits<float>::quiet_NaN();
    cache.publish(matrices, 256, 256, true, 1300);
    expect(!cache.snapshot(1301).valid, "A malformed eye invalidates the whole stereo pair");
    matrices[1][0] = 1.F;
    cache.publish(matrices, 256, 256, false, 1400);
    expect(!cache.snapshot(1400).valid, "Inactive HMD invalidates projection placement immediately");
    allow_afw_stereo_projection(true);
    publish_afw_stereo_projection(matrices, 256, 256, true);
    expect(afw_stereo_projection().valid, "Resident runtime accepts copied projection data");
    matrices[0][8] = 99.F;
    expect(afw_stereo_projection().valid, "Resident projection does not retain host matrix pointers");
    allow_afw_stereo_projection(false);
    expect(!afw_stereo_projection().valid, "Loader-safe detach suppresses stale host projections");
    allow_afw_stereo_projection(true);
    expect(!afw_stereo_projection().valid, "Reattachment requires fresh projection data");
    allow_afw_stereo_projection(false);

    AfwWarpPrefix prefix;
    prefix.source_eye = 1; prefix.mode = 3;
    expect(afw_warp_metadata(&prefix, true).eye == 1 && afw_warp_metadata(&prefix, true).mode == 3,
        "Verified AFW ABI reads explicit source eye and warp mode");
    expect(afw_warp_metadata(reinterpret_cast<void*>(1), false).eye == UINT32_MAX,
        "Unknown runtime ABI never reads opaque parameter memory");
    prefix.mode = 4;
    expect(afw_warp_metadata(&prefix, true).eye == UINT32_MAX, "Unknown warp mode invalidates metadata");
    prefix.mode = 1; prefix.source_eye = 2;
    expect(afw_warp_metadata(&prefix, true).eye == UINT32_MAX, "Invalid eye is never guessed");
    afw_note_warp_abi(true);
    afw_note_warp_call(1, 3); afw_note_warp_call(0, 2); afw_note_warp_call(1, 0);
    auto status = afw_compatibility_status();
    expect(status.source_left_calls == 1 && status.source_right_calls == 1 && status.last_warp_mode == 0,
        "Warp source counters count explicit active modes without inventing alternation");
    afw_note_warp_abi(false); afw_note_warp_call(0, 3);
    expect(afw_compatibility_status().last_warp_source_eye == UINT32_MAX,
        "Unsupported ABI clears last eye rather than retaining stale metadata");
}

int run_nr_lifetime_tests();

void test_afw_gaze_integration() {
    using namespace cheeky::foveated_dlss;
    reset_gaze_foveation(); allow_afw_stereo_projection(true);
    float matrices[2][16]{};
    for (auto& m : matrices) { m[0] = m[5] = m[11] = 1.F; m[14] = 10.F; }
    matrices[0][8] = .4F; matrices[1][8] = -.2F;
    auto publish = [&] { publish_afw_stereo_projection(matrices, 2000, 1600, true); };
    publish();
    Settings requested;
    requested.center_mode = FoveationCenterMode::openxr_gaze;
    requested.width = requested.height = .2F; requested.afw_warp_margin = .02F;
    requested.gaze_smoothing_ms = 0.F; requested.roundness = 1.F;
    const auto projection = afw_stereo_projection();
    auto settings = afw_experiment_settings(requested, &projection);
    expect(settings.roundness == 1.F, "AFW retains requested roundness for per-eye masks");
    CheekyGazeSnapshotV1 sample{};
    sample.abi_version = CHEEKY_GAZE_ABI_VERSION; sample.structure_size = sizeof(sample);
    sample.view_count = 2; sample.session_generation = 41; sample.swapchain_generation = 42;
    sample.status_flags = CHEEKY_GAZE_STATUS_LAYER_ACTIVE | CHEEKY_GAZE_STATUS_SESSION_FOCUSED | CHEEKY_GAZE_STATUS_GAZE_VALID;
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& v = sample.views[eye]; v.structure_size = sizeof(v); v.view_index = eye;
        v.flags = CHEEKY_GAZE_VIEW_ORIENTATION_VALID | CHEEKY_GAZE_VIEW_FOV_VALID;
        v.fov_left = v.fov_down = -std::atan(1.F); v.fov_right = v.fov_up = std::atan(1.F);
        v.center_u = .3F; v.center_v = .5F;
    }
    auto fresh = [&] { LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc); sample.publication_qpc = qpc.QuadPart; ++sample.predicted_display_time; publish(); };
    CropGeometry crop{}; bool reset{};
    auto evaluate = [&](std::uint64_t handle = 700) {
        return calculate_coordinated_crop(settings, handle, nullptr, 1000, 800, 2000, 1600, 0, 0, crop, reset, &sample);
    };
    fresh(); expect(evaluate() && reset && gaze_diagnostics().afw_fresh_sample && gaze_diagnostics().using_gaze,
        "AFW gaze starts without two-eye resource or marker mapping");
    expect(!gaze_diagnostics().views[0].resource_mapped && !gaze_diagnostics().views[1].resource_mapped,
        "Bilateral gaze does not claim a DLSS handle-to-eye assignment");
    const auto first = crop;
    fresh(); expect(evaluate() && !reset && crop.input_width == first.input_width && crop.input_base_x == first.input_base_x,
        "Stable gaze reuses geometry and does not reset history each frame");
    for (auto& v : sample.views) v.center_u = .5F;
    fresh(); expect(evaluate() && reset && crop.input_base_x > first.input_base_x && crop.input_width == first.input_width,
        "Gaze jump translates a stable-size envelope and resets its center history");
    for (unsigned eye = 0; eye < 2; ++eye) {
        FoveationCenter projected;
        expect(afw_project_gaze(sample.views[eye], projection.projections[eye], .5F, .5F, projected), "Project both runtime gaze rays");
        expect(crop.input_base_x <= projected.u * 1000 && crop.input_base_x + crop.input_width >= projected.u * 1000,
            "Both possible eye gaze positions lie inside the rendered crop");
    }
    settings.gaze_smoothing_ms = 100.F;
    for (auto& v : sample.views) v.center_u = .9F;
    fresh(); expect(evaluate(), "Large filtered gaze movement evaluates");
    expect(crop.input_base_x + crop.input_width == 1000, "Fresh edge gaze remains covered while smoothing trails behind");
    expect(crop.input_width == first.input_width, "Smoothing lag cannot enlarge the AFW allocation");
    const auto grown = crop.input_width;
    for (auto& v : sample.views) v.center_u = .5F;
    fresh(); expect(evaluate() && crop.input_width == grown, "Coverage does not shrink immediately as gaze returns");
    expect(evaluate(701) && reset, "A separate native DLSS handle starts an independent gaze history");
    fresh(); expect(evaluate(700) && crop.input_width == first.input_width, "Returning to the first handle retains fixed allocation dimensions");
    sample.status_flags &= ~CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
    fresh(); expect(evaluate() && !gaze_diagnostics().using_gaze, "Focus loss stops gaze immediately");
    sample.status_flags |= CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
    fresh(); expect(evaluate() && reset && gaze_diagnostics().using_gaze, "Focus reacquisition resets center history");
    ++sample.session_generation;
    fresh(); expect(evaluate() && reset && crop.input_width == grown, "New runtime session resets smoothing without resizing the configured allocation");
    sample.status_flags |= CHEEKY_GAZE_STATUS_OPENVR;
    fresh(); test_openvr_snapshot = &sample;
    expect(calculate_coordinated_crop(settings, 700, nullptr, 1000, 800, 2000, 1600, 0, 0, crop, reset) && reset &&
        gaze_diagnostics().afw_fresh_sample, "AFW consumes the OpenVR adapter without resource mapping and resets on backend change");
    test_openvr_snapshot = nullptr;
    sample.status_flags &= ~CHEEKY_GAZE_STATUS_OPENVR;
    fresh(); expect(evaluate() && reset, "Returning to OpenXR resets even with identical session numbers");
    Sleep(60); publish();
    LARGE_INTEGER repeated_now{}; QueryPerformanceCounter(&repeated_now); sample.publication_qpc = repeated_now.QuadPart;
    expect(evaluate() && !gaze_diagnostics().afw_fresh_sample, "Republishing an old display time does not refresh gaze validity");
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    fresh(); sample.publication_qpc -= frequency.QuadPart;
    expect(evaluate() && !gaze_diagnostics().afw_fresh_sample, "Old gaze publications are rejected");
    fresh(); sample.publication_qpc += frequency.QuadPart;
    expect(evaluate() && !gaze_diagnostics().afw_fresh_sample, "Future gaze publications are rejected");
    fresh(); sample.views[1].fov_right = std::numeric_limits<float>::quiet_NaN();
    expect(evaluate() && !gaze_diagnostics().afw_fresh_sample, "One invalid eye rejects the stereo gaze sample");
    sample.views[1].fov_right = std::atan(1.F);
    sample.status_flags |= CHEEKY_GAZE_STATUS_SIMULATED;
    fresh(); expect(evaluate() && !gaze_diagnostics().afw_fresh_sample, "Real gaze mode rejects a simulated source");
    requested.center_mode = FoveationCenterMode::simulated_gaze;
    settings = afw_experiment_settings(requested, &projection);
    fresh(); expect(evaluate(), "Simulation source change uses a safe transition");
    fresh(); expect(evaluate() && gaze_diagnostics().afw_fresh_sample, "AFW simulated gaze uses the same production coverage path");
    settings.simulation_pattern = 2;
    for (auto& v : sample.views) {
        v.flags |= CHEEKY_GAZE_VIEW_NEXT_JUMP_VALID; v.next_jump_u = .1F; v.next_jump_v = .2F;
    }
    fresh(); evaluate(); fresh(); expect(evaluate(), "Jump-preview mode acquires a fresh simulation sample");
    auto preview = settings;
    apply_next_jump_preview(preview, 700);
    expect(preview.next_jump_visible && preview.next_jump_width > 0 && preview.next_jump_height > 0 &&
        preview.afw_mask.count == 4, "AFW publishes an independently sized jump preview and both raw/filtered eye masks");
    const auto preview_crop = crop;
    settings.gaze_smoothing_ms = 100.F;
    for (unsigned frame = 0; frame < 160; ++frame) {
        for (unsigned eye = 0; eye < 2; ++eye) {
            sample.views[eye].center_u = (frame % 2 ? .99F : .01F) + (eye ? -.001F : .001F);
            sample.views[eye].center_v = (frame % 3) * .49F + .01F;
        }
        fresh(); expect(evaluate() && crop.input_width == preview_crop.input_width &&
            crop.input_height == preview_crop.input_height && crop.output_width == preview_crop.output_width &&
            crop.output_height == preview_crop.output_height,
            "Simulated jumps, binocular disparity and smoothing never resize SR/NR crop allocations");
    }
    settings.show_next_jump_target = false;
    fresh(); expect(evaluate() && crop.input_width == preview_crop.input_width && crop.input_height == preview_crop.input_height,
        "Turning off the preview cannot resize current DLSS coverage");
    apply_next_jump_preview(preview = settings, 700);
    expect(!preview.next_jump_visible, "Disabled preview is cleared without disabling rounded coverage");
    settings.show_next_jump_target = true;
    sample.views[1].flags &= ~CHEEKY_GAZE_VIEW_NEXT_JUMP_VALID;
    fresh(); evaluate(); apply_next_jump_preview(preview = settings, 700);
    expect(!preview.next_jump_visible, "One missing future eye suppresses an incomplete preview");
    sample.status_flags &= ~CHEEKY_GAZE_STATUS_GAZE_VALID;
    Sleep(110); publish(); expect(evaluate() && !gaze_diagnostics().afw_fresh_sample, "Tracking loss enters hold/return policy");
    Sleep(160); publish(); expect(evaluate() && !gaze_diagnostics().using_gaze && crop.input_width == preview_crop.input_width,
        "Tracking loss moves to fallback placement without resizing the gaze allocation");
    allow_afw_stereo_projection(false);
    expect(evaluate() && !gaze_diagnostics().using_gaze, "Host detach disables gaze despite a retained runtime snapshot");
    reset_gaze_foveation();
}

void test_afw_source_projection_coverage() {
    using namespace cheeky::foveated_dlss;
    reset_gaze_foveation(); allow_afw_stereo_projection(true);
    float matrices[2][16]{};
    for (auto& m : matrices) { m[0] = m[5] = m[11] = 1.F; m[14] = .1F; }
    matrices[0][8] = .242513F; matrices[1][8] = -.242513F;
    publish_afw_stereo_projection(matrices, 2000, 1600, true);
    const auto projection = afw_stereo_projection();
    Settings requested;
    requested.center_mode = FoveationCenterMode::openxr_gaze;
    requested.width = requested.height = .2F; requested.afw_warp_margin = .13F;
    requested.gaze_smoothing_ms = 0; requested.roundness = 0;
    CheekyGazeSnapshotV1 sample{};
    sample.abi_version = CHEEKY_GAZE_ABI_VERSION; sample.structure_size = sizeof(sample);
    sample.view_count = 2; sample.session_generation = 71; sample.swapchain_generation = 72;
    sample.status_flags = CHEEKY_GAZE_STATUS_LAYER_ACTIVE | CHEEKY_GAZE_STATUS_SESSION_FOCUSED | CHEEKY_GAZE_STATUS_GAZE_VALID;
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& v = sample.views[eye]; v.structure_size = sizeof(v); v.view_index = eye;
        v.flags = CHEEKY_GAZE_VIEW_ORIENTATION_VALID | CHEEKY_GAZE_VIEW_FOV_VALID;
        v.fov_left = v.fov_down = -std::atan(1.F); v.fov_right = v.fov_up = std::atan(1.F);
        v.center_u = v.center_v = .5F;
    }
    CropGeometry crops[2]{}; FoveationMask masks[2]{};
    for (unsigned frame = 0; frame < 12; ++frame) {
        const auto eye = frame % 2; requested.afw_source_eye = eye;
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now); sample.publication_qpc = now.QuadPart; ++sample.predicted_display_time;
        publish_afw_stereo_projection(matrices, 2000, 1600, true);
        auto settings = afw_experiment_settings(requested, &projection); bool reset{};
        expect(calculate_coordinated_crop(settings, 771, nullptr, 1000, 800, 2000, 1600, 0, 0, crops[eye], reset, &sample),
            "Verified source-eye gaze resolves without an NGX-handle eye assignment");
        if (frame >= 2) expect(!reset, "Alternating optical offsets must not reset DLSS temporal history");
        apply_next_jump_preview(settings, 771); masks[eye] = settings.afw_mask;
        expect(crops[eye].input_width < 500, "Same gaze ray must not become a 70% raw-UV stereo union");
    }
    for (unsigned source : {UINT32_MAX, 0U, UINT32_MAX, 1U}) {
        requested.afw_source_eye = source;
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now); sample.publication_qpc = now.QuadPart; ++sample.predicted_display_time;
        publish_afw_stereo_projection(matrices, 2000, 1600, true);
        const auto settings = afw_experiment_settings(requested, &projection);
        CropGeometry current{}; bool reset{};
        expect(calculate_coordinated_crop(settings, 771, nullptr, 1000, 800, 2000, 1600, 0, 0, current, reset, &sample) &&
            current.input_width == crops[0].input_width && current.input_height == crops[0].input_height,
            "Temporary source-eye metadata loss never resizes a warmed AFW gaze allocation");
    }
    expect(crops[0].input_width == crops[1].input_width && crops[0].input_height == crops[1].input_height,
        "Alternating source eyes retain the same private reconstruction allocation");
    expect(std::abs(static_cast<float>(crops[0].input_base_x) - crops[1].input_base_x - 242.513F) <= 8.F,
        "The crop translates by the actual eye projection offset");
    for (unsigned i = 0; i < 4; ++i) for (unsigned corner = 0; corner < 4; corner += 2) {
        // Independently compare viewing rays, rather than comparing pixel UVs.
        const float left_ray = projection.projections[0].left + masks[0].bounds[i][corner] * 2.F;
        const float right_ray = projection.projections[1].left + masks[1].bounds[i][corner] * 2.F;
        expect_near(left_ray, right_ray, 1e-5F, "Left/right sharp-region boundaries represent the same viewing directions");
    }
    for (unsigned i = 0; i <= 20; ++i) {
        const float ray = -.40F + i * .04F;
        const float u0 = (ray - projection.projections[0].left) / 2.F;
        const float u1 = (ray - projection.projections[1].left) / 2.F;
        const float previous_pixel = u0 * 1000.F - crops[0].input_base_x;
        const float current_pixel = u1 * 1000.F - crops[1].input_base_x;
        expect(previous_pixel >= 0 && previous_pixel < crops[0].input_width && current_pixel >= 0 && current_pixel < crops[1].input_width,
            "Side regions must have center-history donors in both alternating source images");
        CropMotionOffset correction{};
        expect(crop_motion_offset(crops[0], crops[1], true, 1000, 800, correction), "AFW crop-motion compensation is available");
        expect_near(current_pixel + ((u0 - u1) + correction.x) * 1000.F, previous_pixel, .001F,
            "Optical motion plus crop-origin correction lands on the same center-history pixel");
    }
    // Fixed automatic and independent NR use the same source-space mapping.
    requested.center_mode = FoveationCenterMode::fixed; requested.afw_automatic_coverage = true;
    requested.nr_use_sr_foveation = false; requested.nr_width = .3F; requested.nr_height = .25F;
    Settings fixed[2];
    for (unsigned eye = 0; eye < 2; ++eye) { requested.afw_source_eye = eye; fixed[eye] = afw_experiment_settings(requested, &projection); }
    expect_near(fixed[0].afw_nr_mask.bounds[0][0] - fixed[1].afw_nr_mask.bounds[0][0], .242513F, 1e-5F,
        "Independent NR aligns its own shape to the source eye");
    expect_near(fixed[0].width, fixed[1].width, 1e-5F, "Fixed optical coverage keeps stable dimensions");
    for (const float scale : {.7F, 1.F, 1.4F}) for (const float width : {.2F, .4F, .55F, .7F}) {
        matrices[1][0] = scale;
        publish_afw_stereo_projection(matrices, 2000, 1600, true);
        const auto asymmetric = afw_stereo_projection(); requested.width = width;
        CropGeometry pair[2]{};
        for (unsigned eye = 0; eye < 2; ++eye) {
            requested.afw_source_eye = eye;
            const auto shaped = afw_experiment_settings(requested, &asymmetric);
            expect(calculate_crop(shaped, 2259, 2118, 2000, 1600, 0, 0, pair[eye]), "Asymmetric source-eye crop resolves");
        }
        expect(pair[0].input_width == pair[1].input_width && pair[0].output_width == pair[1].output_width,
            "Unequal eye FOV spans cannot resize private DLSS every other frame");
    }
    allow_afw_stereo_projection(false); reset_gaze_foveation();
}

void test_afw_gaze_pixel_coverage() {
    using namespace cheeky::foveated_dlss;
    for (unsigned extent : {999U, 1000U, 1651U}) {
        const auto size = afw_gaze_size(.34F, extent, 8);
        for (unsigned step = 0; step <= 1000; ++step) {
            const auto start = afw_gaze_start(step / 1000.F, extent, size, 8);
            expect(start + size <= extent, "Fixed AFW allocation stays inside the texture at both edges");
            expect(size == afw_gaze_size(.34F, extent, 8), "Placement never changes AFW allocation dimensions");
        }
    }
    AfwDepthEyes identity;
    identity.record(1, 20, 100, 4); identity.record(0, 10, 101, 4);
    expect(identity.lookup(10, 102, 4) == 0 && identity.lookup(20, 102, 4) == 1,
        "Explicit depth resources identify eyes independently of observation order");
    expect(identity.lookup(10, 352, 4) == UINT32_MAX && identity.lookup(10, 102, 5) == UINT32_MAX,
        "Stale and previous-session depth bindings are rejected");
    identity.forget(10);
    expect(identity.lookup(10, 102, 4) == UINT32_MAX, "Destroyed depth resources cannot identify reused addresses");
    identity.record(0, 20, 102, 4);
    expect(identity.lookup(20, 103, 4) == UINT32_MAX, "A depth buffer shared by both eyes is ambiguous");
    identity.record(1, 20, 104, 4);
    expect(identity.lookup(20, 105, 4) == UINT32_MAX, "Shared-resource ambiguity cannot clear on the next eye's observation");
    for (unsigned extent : {33U, 128U, 999U, 2259U, 4096U}) {
        Settings nr;
        nr.eye_independent_coverage = nr.nr_foveated = true; nr.nr_use_sr_foveation = false;
        nr.nr_width = .217F; nr.nr_height = .3F;
        unsigned previous_width{};
        for (unsigned step = 0; step <= 100; ++step) {
            const FoveationCenter center{step / 100.F, .5F, 1};
            const auto parameters = dlss_nr_foveation_parameters(nr, &center);
            CropGeometry wanted{};
            expect(calculate_foveation_geometry(parameters, extent, extent, extent, extent, 0, 0, wanted), "NR target geometry resolves");
            const auto region = calculate_region(nr, extent, extent, nullptr, extent, extent, &center);
            expect(region.base_x <= wanted.input_base_x && region.base_x + region.width >= wanted.input_base_x + wanted.input_width &&
                region.base_x + region.width <= extent, "Aligned AFW NR retains its complete gaze envelope");
            expect(!previous_width || previous_width == region.width, "Gaze translation keeps NR allocation dimensions stable");
            previous_width = region.width;
        }
        for (unsigned quantum : {1U, 8U, 64U}) {
            const auto size = afw_gaze_size(.217F, extent, quantum);
            for (unsigned step = 0; step <= 100; ++step) {
                const auto start = afw_gaze_start(step / 100.F, extent, size, quantum);
                FoveationParameters p; p.width = p.height = float(size) / extent;
                CropGeometry crop{};
                expect(calculate_foveation_geometry_at_center(p, {(start + size * .5F) / extent, .5F, 1},
                    extent, extent, extent * 2, extent * 2, 0, 0, crop), "Fixed AFW crop resolves at odd and even render dimensions");
                expect(crop.input_width == size && crop.output_width == size * 2 && crop.input_base_x + size <= extent,
                    "Quantized translation preserves input and output allocation extents at image boundaries");
            }
        }
    }
}

int run_d3d12_history_tests();
int run_d3d12_safety_tests();

int run_vulkan_tests(bool real=false, bool integration=false);
int run_d3d11_binding_tests();
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--stereo-support") == 0)
        return run_stereo_support_tests() + run_stereo_support12_tests();
    if (argc == 2 && std::strcmp(argv[1], "--alignment-history") == 0) {
        test_auto_alignment_history(false); test_auto_alignment_history(true); test_auto_alignment();
        if (!failures) std::cout << "Automatic alignment history tests passed\n";
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--d3d11-bindings") == 0) return run_d3d11_binding_tests();
    if (argc == 2 && std::strcmp(argv[1], "--vulkan-layer-model") == 0) return run_vulkan_tests(true,true);
    if (argc == 2 && std::strcmp(argv[1], "--vulkan-model") == 0) return run_vulkan_tests(true);
    if (argc == 2 && std::strcmp(argv[1], "--vulkan") == 0) return run_vulkan_tests();
    if (argc == 2 && std::strcmp(argv[1], "--afw-gaze") == 0) {
        test_afw_gaze_integration(); test_afw_source_projection_coverage(); test_afw_gaze_pixel_coverage();
        if (!failures) std::cout << "AFW fixed allocation gaze tests passed\n";
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--d3d12-history") == 0) return run_d3d12_history_tests();
    if (argc == 2 && std::strcmp(argv[1], "--d3d12-safety") == 0) return run_d3d12_safety_tests();
    if (argc == 3 && std::strcmp(argv[1], "--afw-runtime-file") == 0) {
        const bool supported = cheeky::foveated_dlss::known_afw_warp_file(std::filesystem::path(argv[2]).c_str());
        std::cout << (supported ? "Verified AFW warp ABI file\n" : "Unknown AFW warp ABI file\n");
        return supported ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--nr-lifetime") == 0) return run_nr_lifetime_tests();
    if (argc == 2 && std::strcmp(argv[1], "--nr-processing") == 0) {
        return run_nr_processing_tests();
    }
    test_native_dynamic_resolution_extent();
    if (argc == 2 && std::strcmp(argv[1], "--calibration-formats") == 0) {
        failures += run_openxr_calibration_format_tests();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--motion-resample") == 0) {
        failures += run_motion_resample_tests();
        return failures ? 1 : 0;
    }
    test_center_supersampling();
    test_nr_only_center(false);
    test_nr_only_center(true);
    test_packed_alignment_coordinator();
    test_packed_alignment_coordinator(true);
    test_openvr_geometry();
    test_auto_alignment();
    test_auto_alignment_history(false);
    test_auto_alignment_history(true);
    if (argc == 2 && std::strcmp(argv[1], "--d3d12-composite") == 0) {
        failures += run_d3d12_composite_tests();
        return failures ? 1 : 0;
    }
    test_simulated_gaze();
    test_gaze_copy_routes();
    test_gaze_camera_projection();
    test_simulation_patterns();
    test_projection();
    test_geometry();
    test_sr_crop_dimensions_during_gaze();
    test_mapping_policy();
    test_packed_stereo_mapping_policy();
    test_temporal_policy();
    test_reset_policy();
    test_abi();
    test_core_d3d12_evaluation_is_intercepted();
    test_nested_d3d12_evaluation_is_forwarded_once();
    test_d3d12_route_names();
    test_nested_d3d12_lifecycle_scope_is_passthrough();
    test_core_d3d12_route_is_published_to_diagnostics();
    test_multimip_game_output_uses_single_mip_private_output();
    test_msfs_array_output_contract();
    test_streamline_private_sr_viewport();
    test_streamline_supersampling_creation();
    test_multimip_game_output_is_dlss_nr_compatible();
    test_dlss_nr_maps_right_eye_region_into_packed_output();
    test_dlss_nr_stable_crop_and_history();
    test_dlss_nr_transport_crop_fits_resource();
    test_dlss_nr_reuses_live_sr_crop_center();
    test_dlss_nr_independent_size_shares_sr_center();
    test_openxr_layer_is_retained_while_snapshot_export_is_cached();
    failures += run_support_summary_tests();
    failures += run_eye_calibration_tests();
    failures += run_stereo_support_tests();
    failures += run_stereo_support12_tests();
    failures += run_openxr_calibration_tests();
    failures += run_openxr_calibration_format_tests();
    failures += run_nr_processing_tests();
    test_afw_dispatch_and_settings();
    test_afw_projection_and_metadata();
    test_afw_gaze_integration();
    test_afw_source_projection_coverage();
    test_afw_gaze_pixel_coverage();
    failures += run_d3d12_history_tests();
    failures += run_d3d12_safety_tests();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All Cheeky tests passed\n";
    return 0;
}
