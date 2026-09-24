#pragma once

#include "foveation.hpp"
#include "eye_calibration_policy.hpp"

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
    bool enabled{true};
    bool d3d12_lower_hook{true}; // Applied at interception startup; changes require a restart.
    bool d3d11_use_d3d12_transport{false};
    bool peripheral_dlaa_enabled{true};
    float peripheral_dlaa_scale{0.75F};
    std::uint32_t center_preset{};
    std::uint32_t rr_center_preset{};
    std::uint32_t rr_peripheral_preset{};
    float center_supersampling{1.0F};
    // AFW needs donors for both eye regions even when the source eye is known.
    // Manual mode covers both offsets; the centered fallback starts at 70%.
    bool afw_manual_coverage{false};
    bool afw_automatic_coverage{false};
    float afw_warp_margin{0.05F};
    unsigned afw_source_eye{UINT32_MAX}; // Verified identity scoped to the original core evaluation.
    bool eye_independent_coverage{}; // Transient: never mirror a resolved AFW envelope by guessed view role.
    float afw_gaze_width{}, afw_gaze_height{}; // Requested size before the fixed fallback envelope expands it.
    struct AfwNrCoverage {
        float width{}, height{}, x{}, y{}, gaze_width{}, gaze_height{};
    } afw_nr; // Evaluation-local NR envelope; never persisted.
    bool afw_nr_coverage{}; // NR uses its own gaze allocation without replacing SR coverage diagnostics.
    FoveationMask afw_mask{}, afw_nr_mask{};
    std::uint32_t peripheral_dlaa_preset{5U};
    float width{0.55F};
    float height{0.45F};
    float x_offset{0.0F};
    float height_offset{-0.45F};
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
    float gaze_smoothing_ms{20.0F};
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
};

// A registration generation distinguishes a released/recreated NGX handle at
// the same address. Calibrations are atomic, ordered pairs retained until
// invalidation. Incoming readbacks must still be recent.
std::uint64_t stereo_view_generation(std::uint64_t view_id) noexcept;
bool publish_stereo_calibration(std::uint64_t left, std::uint64_t right,
    std::uint64_t left_generation, std::uint64_t right_generation,
    std::uint64_t sequence, std::uint64_t captured_ms, bool* corrected = nullptr,
    std::uint64_t session_generation = 0, bool vertical_flip = false, bool shared_source = false,
    const std::array<StereoSourceCrop, 2>* source_crops = nullptr) noexcept;
void invalidate_stereo_crop() noexcept;
bool eye_calibration_continuous_validation() noexcept;
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
