#include "cheeky_gaze_abi.h"
#include "d3d12_ngx_dispatch.hpp"
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

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

namespace cheeky::foveated_dlss {
// The core GPU harness explicitly drives post-submit/reset notifications.
NativeObserverStatus native_observer_status() noexcept { return {true}; }
bool initialize_native_observer(ID3D12Device*, ID3D12CommandQueue*) noexcept { return true; }

void trace_event(const char*, ...) noexcept {}
// Runtime discovery is excluded from deterministic coordinator tests. Live
// OpenVR acquisition is tested separately, with snapshots exercising shared policy here.
const CheekyGazeSnapshotV1* test_openvr_snapshot{};
bool read_openvr_gaze(const Settings&, IUnknown*, CheekyGazeSnapshotV1& output) noexcept {
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
    expect_near(x, 4.0F, 0.0001F, "NR origin motion uses output-pixel MV scale");
    expect_near(y, 2.0F, 0.0001F, "NR origin motion respects negative vertical scale");
    // Static scene point: current-local + corrected MV = previous-local,
    // including a half-resolution NR working texture.
    expect_near((100.0F - 8.0F) * 0.5F + x * current.scale_x * 0.5F,
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
    expect(!dlss_nr_motion_offset(previous, current, x, y), "invalid NR motion scale rejects crop correction");
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
int run_openxr_calibration_tests();
int run_support_summary_tests();

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

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--calibration-formats") == 0)
        return run_openxr_calibration_format_tests();
    if (argc == 2 && std::strcmp(argv[1], "--motion-resample") == 0)
        return run_motion_resample_tests();
    test_center_supersampling();
    test_packed_alignment_coordinator();
    test_packed_alignment_coordinator(true);
    test_openvr_geometry();
    test_auto_alignment();
    if (argc == 2 && std::strcmp(argv[1], "--d3d12-composite") == 0) {
        return run_d3d12_composite_tests();
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
    failures += run_openxr_calibration_tests();
    failures += run_openxr_calibration_format_tests();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All Cheeky tests passed\n";
    return 0;
}
