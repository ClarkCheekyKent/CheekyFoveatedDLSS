#pragma once

#include "settings.hpp"

#include <cstdint>

namespace cheeky::foveated_dlss {

struct DlssNrResourceBase {
    std::uint32_t x{};
    std::uint32_t y{};
};

[[nodiscard]] DlssNrResourceBase dlss_nr_resource_base(
    std::uint32_t local_x,
    std::uint32_t local_y,
    std::uint32_t color_base_x,
    std::uint32_t color_base_y,
    bool color_is_region
) noexcept;

[[nodiscard]] FoveationParameters dlss_nr_foveation_parameters(
    const Settings& settings,
    const FoveationGeometry* shared_sr_crop,
    std::uint32_t render_width,
    std::uint32_t render_height
) noexcept;

}  // namespace cheeky::foveated_dlss
