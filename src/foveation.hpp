#pragma once

#include <cstdint>

namespace cheeky::foveated_dlss {

// Renderer-independent foveation inputs. Consumers such as DLSS-SR and DLSS
// Ray Reconstruction can share this geometry without depending on UI settings.
struct FoveationParameters {
    float width{0.80F};
    float height{0.45F};
    float x_offset{0.0F};
    float y_offset{-0.44F};
    float roundness{0.0F};
    float transition_width{0.0F};
};

struct FoveationGeometry {
    std::uint32_t input_base_x{};
    std::uint32_t input_base_y{};
    std::uint32_t input_width{};
    std::uint32_t input_height{};
    std::uint32_t output_base_x{};
    std::uint32_t output_base_y{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
};

[[nodiscard]] bool calculate_foveation_geometry(
    const FoveationParameters& parameters,
    std::uint32_t render_width,
    std::uint32_t render_height,
    std::uint32_t output_width,
    std::uint32_t output_height,
    std::uint32_t output_origin_x,
    std::uint32_t output_origin_y,
    FoveationGeometry& geometry
) noexcept;

}  // namespace cheeky::foveated_dlss
