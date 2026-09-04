#pragma once

#include "foveation.hpp"

#include <cstdint>
#include <vector>

namespace cheeky::foveated_dlss {

struct Settings {
    bool enabled{true};
    bool d3d11_use_d3d12_transport{false};
    bool peripheral_dlaa_enabled{true};
    float peripheral_dlaa_scale{0.75F};
    std::uint32_t center_preset{};
    std::uint32_t peripheral_dlaa_preset{5U};
    float width{0.55F};
    float height{0.45F};
    float x_offset{0.60F};
    float height_offset{-0.45F};
    bool invert_stereo_x_offset{false};
    float roundness{0.0F};
    float transition_width{0.04F};
    bool alignment_border_enabled{false};

    bool nr_enabled{false};
    bool nr_foveated{true};
    bool nr_use_sr_foveation{false};
    bool nr_alignment_border_enabled{false};
    float nr_width{0.56F};
    float nr_height{0.56F};
    float nr_x_offset{0.31F};
    float nr_height_offset{-0.44F};
    bool nr_invert_stereo_x_offset{false};
    float nr_roundness{0.0F};
    float nr_transition_width{0.08F};
    float nr_working_scale{0.80F};
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

[[nodiscard]] Settings current_settings() noexcept;
void update_settings(const Settings& settings) noexcept;

void register_stereo_view(std::uint64_t view_id) noexcept;
void unregister_stereo_view(std::uint64_t view_id) noexcept;
[[nodiscard]] StereoViewStatistics stereo_view_statistics() noexcept;
[[nodiscard]] std::vector<StereoViewDetail> stereo_view_details();
[[nodiscard]] bool has_multiple_stereo_views() noexcept;
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
