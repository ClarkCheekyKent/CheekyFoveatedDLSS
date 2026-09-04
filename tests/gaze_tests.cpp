#include "cheeky_gaze_abi.h"
#include "d3d12_ngx_dispatch.hpp"
#include "d3d12_output_contract.hpp"
#include "diagnostics.hpp"
#include "foveation.hpp"
#include "gaze_math.hpp"
#include "gaze_policy.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

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
    static_assert(sizeof(CheekyGazeViewV1) == 56U);
    static_assert(sizeof(CheekyGazeSnapshotV1) == 304U);
    CheekyGazeSnapshotV1 snapshot{};
    snapshot.abi_version = CHEEKY_GAZE_ABI_VERSION;
    snapshot.structure_size = sizeof(snapshot);
    expect(snapshot.abi_version == 1U &&
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

}  // namespace

int main() {
    test_projection();
    test_geometry();
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
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All Cheeky tests passed\n";
    return 0;
}
