#pragma once

#include "foveation.hpp"

#include <cstdint>
#include <vector>

namespace cheeky::foveated_dlss {

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
    bool d3d11_use_d3d12_transport{false};
    bool peripheral_dlaa_enabled{true};
    float peripheral_dlaa_scale{0.75F};
    std::uint32_t center_preset{};
    float center_supersampling{1.0F};
    std::uint32_t peripheral_dlaa_preset{5U};
    float width{0.55F};
    float height{0.45F};
    float x_offset{0.60F};
    float height_offset{-0.45F};
    bool invert_stereo_x_offset{false};
    bool auto_stereo_alignment{true};
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

struct StereoEyeAssignment {
    std::uint32_t eye_index{};
    bool assigned{};
    bool calibrated{};
    std::uint64_t calibration_session{}; // 0 = OpenVR; otherwise OpenXR generation.
    bool vertical_flip{}; // Verified source-to-submission transform, never a game heuristic.
};

// A registration generation distinguishes a released/recreated NGX handle at
// the same address. Calibrations are atomic, ordered pairs retained until
// invalidation. Incoming readbacks must still be recent.
std::uint64_t stereo_view_generation(std::uint64_t view_id) noexcept;
bool publish_stereo_calibration(std::uint64_t left, std::uint64_t right,
    std::uint64_t left_generation, std::uint64_t right_generation,
    std::uint64_t sequence, std::uint64_t captured_ms, bool* corrected = nullptr,
    std::uint64_t session_generation = 0, bool vertical_flip = false) noexcept;
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
