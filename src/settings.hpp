#pragma once

#include "foveation.hpp"
#include "eye_calibration_policy.hpp"
#include "../shared/cheeky_fg_shader11.hpp"

#include <cstdint>
#include <array>
#include <vector>

namespace cheeky::foveated_dlss {

// Up to four requested regions (two eyes, fresh and filtered gaze). Bounds
// are normalized to the complete eye image, independently of the allocation.
struct FoveationMask {
    float bounds[4][4]{}; // left, top, right, bottom
    std::uint32_t count{};
};

enum class FoveationCenterMode : std::uint32_t {
    fixed = 0U,
    openxr_gaze = 1U,
    simulated_gaze = 2U,
};

enum class NrProcessingOrder : std::uint32_t {
    after_upscaling = 0U,
    before_upscaling = 1U,
};

inline NrProcessingOrder nr_processing_order(std::uint32_t value) noexcept {
    return value == 1U ? NrProcessingOrder::before_upscaling
                       : NrProcessingOrder::after_upscaling;
}

inline const char* nr_processing_order_name(NrProcessingOrder order) noexcept {
    return order == NrProcessingOrder::before_upscaling
        ? "Before upscaling" : "After upscaling";
}

struct Settings {
    struct FrameGeneration {
        bool enabled{false};
        bool foveated{true};
        bool match_sr_region{true}; // Copy SR size and effective per-eye placement when enabled.
        bool show_border{false}; // Blue FG debug outline, drawn after FG into OpenXR output.
        std::uint32_t resolution_percent{100},peripheral_percent{50};
        std::uint32_t center_method{}; // 0 DLSS, 1 simple motion warp, 2 bidirectional interpolation.
        std::uint32_t peripheral_method{1},depth_bits{32},motion_bits{32}; // Guide precision is fixed at 32-bit; not user-facing.
        float width{0.55F}, height{0.45F}, x{}, y{}, feather{0.04F}; // Same defaults as SR.
        std::uint32_t diagnostic_mode{}; // Fixed at 0 (normal); 1-4 are internal NGX diagnostics.
        bool backbuffer_frame_id{true}; // Native render-frame identity shared by both NGX stereo evaluations.
        std::uint32_t queue_priority{2}; // 0 separate NORMAL; 1 separate HIGH; 2 shared game queue.
        std::uint32_t release_point{3}; // 0 FG submission, 1 preparation, 2 completion, 3 generated XR submission, 4 4ms post-submit, 5 never held.
        bool submit_repeats{false}; // Fixed off: the runtime handles repeated display content.
        std::uint32_t pacing_clock{}; // 0 independent display clock; 1 presented native rate; 2 rate + runtime phase lock.
        std::uint32_t color_capture_mode{1},upload_mode{1}; // Fixed: direct eye capture and batched uploads.
        std::uint32_t latency_diagnostics{0}; // Sparse calibrated GPU/CPU timeline and its per-frame log; opt-in.
        std::uint32_t compact_storage{1}; // Fixed. Native-density center plus reduced peripheral context, reconstructed at XR upload.
        std::uint32_t guide_capture_mode{1}; // Fixed: capture directly into prepared FG guide grids.
        std::uint32_t direct_xr_output{}; // Experimental: acquire final XR images before FG; default uses compact intermediates.
        std::uint32_t direct_peripheral_shading{0}; // Fixed at full-rate warp.
        bool native11_skip_identity_crop{false}; // Native D3D11: reuse a whole-image eye capture instead of copying it again.
    } fg;
    bool enabled{true};
    bool d3d12_lower_hook{true}; // Applied at interception startup; changes require a restart.
    bool d3d11_use_d3d12_transport{false};
    bool d3d12_quad_compose_wait{true}; // D3D12 quad views: xrEndFrame waits for the composition GPU work.
    bool peripheral_dlaa_enabled{true};
    float peripheral_dlaa_scale{0.75F};
    std::uint32_t center_preset{};
    bool fix_motion_blur{false}; // Opt-in DX12 SR center motion-vector copy.
    std::uint32_t rr_center_preset{};
    std::uint32_t rr_peripheral_preset{};
    float center_supersampling{1.0F};
    // AFW needs donors for both eye regions even when the source eye is known.
    // Coverage is opt-in. With both modes off, use calibrated per-eye settings.
    bool afw_manual_coverage{false};
    bool afw_automatic_coverage{false};
    float afw_warp_margin{0.05F};
    unsigned afw_source_eye{UINT32_MAX}; // Verified identity scoped to the original core evaluation.
    bool eye_independent_coverage{}; // Transient: never mirror a resolved AFW envelope by guessed view role.
    float afw_gaze_width{}, afw_gaze_height{}; // Requested size before the fixed fallback envelope expands it.
    float afw_gaze_height_bias{}; // Transient placement bias before fallback geometry is baked.
    struct AfwNrCoverage {
        float width{}, height{}, x{}, y{}, gaze_width{}, gaze_height{}, gaze_height_bias{};
    } afw_nr; // Evaluation-local NR envelope; never persisted.
    bool afw_nr_coverage{}; // NR uses its own gaze allocation without replacing SR coverage diagnostics.
    FoveationMask afw_mask{}, afw_nr_mask{};
    std::uint32_t peripheral_dlaa_preset{5U};
    float width{0.55F};
    float height{0.45F};
    float x_offset{0.0F};
    float height_offset{0.0F};
    bool invert_stereo_x_offset{false};
    bool auto_stereo_alignment{true};
    bool eye_calibration_continuous{false};
    EyeCalibrationMethod eye_calibration_method{EyeCalibrationMethod::automatic};
    // Learning is loaded explicitly and updated separately from editable UI drafts.
    unsigned eye_calibration_learned_method{}, eye_calibration_learned_sessions{};
    std::uint64_t eye_calibration_learned_signature{};
    float aligned_height_offset{0.0F};
    float roundness{0.0F};
    float transition_width{0.04F};
    bool alignment_border_enabled{false};
    FoveationCenterMode center_mode{FoveationCenterMode::fixed};
    std::uint32_t simulation_pattern{};
    bool show_next_jump_target{true};
    // Per-evaluation preview geometry; never persisted.
    bool next_jump_visible{};
    float next_jump_offset_x{}, next_jump_offset_y{};
    float next_jump_width{}, next_jump_height{};
    bool manual_stereo_mapping{false};
    bool manual_stereo_right_first{false};
    bool quad_manual_assignment{false};
    bool quad_show_numbers{false};
    std::array<unsigned,8> quad_saved_roles{}; // Manual layout indexed by displayed view number.
    unsigned quad_saved_source_count{};
    bool quad_view_mode{}; // Evaluation-local; the submitted view already defines its fovea.
    unsigned quad_view_region{}; // 0 unresolved, 1 periphery, 2 center; never persisted.
    bool independent_openxr_gaze{false};
    float gaze_smoothing_ms{20.0F};
    // Shorter gaze loss (a blink) keeps the last gaze without a DLSS history reset.
    float gaze_hold_ms{400.0F};
    std::uint32_t gaze_quantization_pixels{8U};
    float gaze_jump_reset_ratio{0.125F};

    bool nr_enabled{false};
    NrProcessingOrder nr_processing_order{NrProcessingOrder::after_upscaling};
    bool nr_foveated{true};
    bool nr_use_sr_foveation{false};
    bool nr_alignment_border_enabled{false};
    // Transient final-resolution border for DX11 transport composition.
    std::uint32_t nr_border_x{}, nr_border_y{}, nr_border_width{}, nr_border_height{};
    float nr_width{0.56F};
    float nr_height{0.56F};
    float nr_roundness{0.0F};
    float nr_transition_width{0.08F};
    float nr_working_scale{1.0F};
    std::uint32_t nr_preset{};
    std::uint32_t nr_style{};
    float nr_intensity{1.0F};
    float nr_local_tone_strength{1.0F};
    float nr_local_structure_strength{1.0F};
    float nr_skin_structure_strength{1.0F};
    bool nr_automatic_mask{false};
    bool nr_ui_correction{false};
    float nr_paper_white_scale{1.0F};
    float nr_hdr_transfer_strength{1.0F};
    float nr_color_strength{1.0F};
    // 0 follows the game's NGX flags, 1 forces normal depth, 2 reversed depth.
    std::uint32_t nr_depth_convention{};
    float nr_motion_scale_x_multiplier{1.0F};
    float nr_motion_scale_y_multiplier{1.0F};
};

inline bool uses_coordinated_center(const Settings& settings) noexcept {
    return settings.auto_stereo_alignment || settings.center_mode != FoveationCenterMode::fixed;
}
inline bool fg_requires_d3d12(const Settings& settings) noexcept {
    return settings.fg.enabled&&!cheeky_fg_shader_only(settings.fg.center_method,settings.fg.peripheral_method,settings.fg.foveated);
}

using CropGeometry = FoveationGeometry;

struct StereoViewStatistics {
    std::uint32_t active{};
    std::uint32_t peak{};
    std::uint32_t seen{};
};

struct StereoViewDetail {
    std::uint64_t view_id{};
    bool second_eye{};
    bool has_eye_assignment{};
    std::uint64_t evaluations{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    CropGeometry crop{};
    bool has_geometry{};
    std::uint64_t generation{};
    // Observed game evaluation calls, including passthrough. These are not
    // successful GPU frames or OpenXR projection-view counts.
    std::uint64_t evaluation_calls{};
    std::uint64_t last_evaluation_age_ms{};
    bool recently_evaluated{}; // A call within the last 2000 ms.
    unsigned graphics_api{}; // 11 = DX11, 12 = DX12, 1 = Vulkan.
    bool calibrated{};
    std::uint32_t evaluation_input_width{}, evaluation_input_height{};
    std::uint32_t evaluation_output_width{}, evaluation_output_height{};
    unsigned quad_region{},quad_view_index{~0U}; // 1 peripheral, 2 focus.
    bool quad_manual{};
};

struct StereoSourceCrop {
    float x{}, y{}, width{1}, height{1}; // Normalized to the original DLSS view.
    unsigned source_width{}, source_height{};
    bool valid{};
    bool operator==(const StereoSourceCrop&) const = default;
};
struct StereoEyeAssignment {
    std::uint32_t eye_index{};
    bool assigned{};
    bool calibrated{};
    std::uint64_t calibration_session{}; // 0 = OpenVR; otherwise OpenXR generation.
    bool vertical_flip{}; // Verified source-to-submission transform, never a game heuristic.
    bool shared_source{}; // One verified source supplies both submitted eyes.
    std::array<StereoSourceCrop, 2> source_crops{}; // Indexed by physical eye.
    // Changes only when the identity/transform changes, not on verification.
    std::uint64_t mapping_revision{};
    bool quad_assigned{},quad_manual{};
    unsigned quad_view_index{~0U};
};

// A registration generation distinguishes a released/recreated NGX handle at
// the same address. Calibrations are atomic, ordered pairs retained until
// invalidation. Incoming readbacks must still be recent.
std::uint64_t stereo_view_generation(std::uint64_t view_id) noexcept;
bool publish_stereo_calibration(std::uint64_t left, std::uint64_t right,
    std::uint64_t left_generation, std::uint64_t right_generation,
    std::uint64_t sequence, std::uint64_t captured_ms, bool* corrected = nullptr,
    std::uint64_t session_generation = 0, bool vertical_flip = false, bool shared_source = false,
    const std::array<StereoSourceCrop, 2>* source_crops = nullptr,
    EyeCalibrationMethod method = EyeCalibrationMethod::full) noexcept;
void invalidate_stereo_crop() noexcept;
bool eye_calibration_continuous_validation(EyeCalibrationMethod method) noexcept;
EyeCalibrationMethod eye_calibration_selected_method() noexcept;
void set_eye_calibration_learning(unsigned method, std::uint64_t signature, unsigned sessions) noexcept;
std::uint64_t eye_calibration_learning_revision() noexcept;
void clear_stereo_calibration() noexcept;

[[nodiscard]] Settings current_settings() noexcept;
[[nodiscard]] Settings configured_settings() noexcept;
void update_settings(const Settings& settings) noexcept;
// A host can stop processing on detach without taking locks under DllMain.
// User preferences remain intact; snapshots consumed by rendering are gated.
void set_processing_allowed(bool allowed) noexcept;

void register_stereo_view(std::uint64_t view_id) noexcept;
void unregister_stereo_view(std::uint64_t view_id) noexcept;
[[nodiscard]] StereoViewStatistics stereo_view_statistics() noexcept;
[[nodiscard]] std::vector<StereoViewDetail> stereo_view_details();
void note_stereo_view_evaluation(std::uint64_t view_id, unsigned graphics_api,
    std::uint32_t input_width, std::uint32_t input_height,
    std::uint32_t output_width, std::uint32_t output_height) noexcept;
[[nodiscard]] bool has_multiple_stereo_views() noexcept;
[[nodiscard]] StereoEyeAssignment stereo_eye_assignment(
    std::uint64_t view_id
) noexcept;
void note_stereo_view_geometry(
    std::uint64_t view_id,
    std::uint32_t render_width,
    std::uint32_t render_height,
    std::uint32_t output_width,
    std::uint32_t output_height,
    const CropGeometry& crop
) noexcept;
[[nodiscard]] Settings settings_for_view(
    const Settings& settings,
    std::uint64_t view_id
) noexcept;

[[nodiscard]] FoveationParameters foveation_parameters(
    const Settings& settings
) noexcept;

[[nodiscard]] bool calculate_crop(
    const Settings& settings,
    std::uint32_t render_width,
    std::uint32_t render_height,
    std::uint32_t output_width,
    std::uint32_t output_height,
    std::uint32_t output_origin_x,
    std::uint32_t output_origin_y,
    CropGeometry& crop
) noexcept;

}  // namespace cheeky::foveated_dlss
