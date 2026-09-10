#pragma once

#include "settings.hpp"

#include <cstdint>

namespace cheeky::foveated_dlss {

struct DlssNrResourceBase {
    std::uint32_t x{};
    std::uint32_t y{};
};

struct DlssNrAxis { std::uint32_t base{}, extent{}; };
// Align the extent independently of position, then clamp the moving origin.
[[nodiscard]] DlssNrAxis dlss_nr_aligned_axis(
    std::uint32_t base, std::uint32_t extent, std::uint32_t capacity
) noexcept;

struct DlssNrHistory {
    std::uint32_t x{}, y{}, width{}, height{};
    std::uint32_t output_width{}, output_height{};
    std::uint32_t working_width{}, working_height{};
    float scale_x{}, scale_y{};
};

// Scales convert stored vectors to output pixels before NR working scaling.
// Returns false when history cannot be reprojected safely.
[[nodiscard]] bool dlss_nr_motion_offset(const DlssNrHistory& previous,
    const DlssNrHistory& current, float& x, float& y) noexcept;

// color_is_region means the texture already contains only the NR crop, whose
// resource origin is zero. Otherwise add the eye/output base to the crop offset.
[[nodiscard]] DlssNrResourceBase dlss_nr_resource_base(
    std::uint32_t local_x,
    std::uint32_t local_y,
    std::uint32_t color_base_x,
    std::uint32_t color_base_y,
    bool color_is_region
) noexcept;

[[nodiscard]] FoveationParameters dlss_nr_foveation_parameters(
    const Settings& settings, const FoveationCenter* center) noexcept;

[[nodiscard]] FoveationParameters dlss_nr_foveation_parameters(
    const Settings& settings,
    const FoveationGeometry* shared_sr_crop,
    std::uint32_t render_width,
    std::uint32_t render_height
) noexcept;

}  // namespace cheeky::foveated_dlss
