#pragma once
#include <algorithm>
#include <cstdint>
namespace cheeky::foveated_dlss {
struct PeripheralDlaaDimensions { std::uint32_t width{},height{}; };
inline PeripheralDlaaDimensions peripheral_dlaa_dimensions(
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const float scale
) noexcept {
    if (render_width == 0U || render_height == 0U) return {};
    const auto clamped = std::clamp(scale, 0.20F, 1.0F);
    const auto scale_dimension = [clamped](const std::uint32_t value) noexcept {
        const auto scaled = static_cast<std::uint32_t>(
            static_cast<float>(value) * clamped + 0.5F
        );
        return (std::min)(value, (std::max)(32U, scaled));
    };
    return {scale_dimension(render_width), scale_dimension(render_height)};
}

}
