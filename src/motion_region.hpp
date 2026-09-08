#pragma once
#include "foveation.hpp"
#include <cstdint>
#include <limits>

namespace cheeky::foveated_dlss {
enum class DeclaredMotionSpace { unavailable, input, output };
enum class MotionRegionStatus { valid, missing_declaration, missing_resource,
    invalid_dimensions, output_underflow, coordinate_overflow, outside_view, outside_texture };
struct MotionRectangle { std::uint32_t x{}, y{}, width{}, height{}; };
struct MotionRegion {
    DeclaredMotionSpace space{DeclaredMotionSpace::unavailable};
    MotionRectangle rectangle{};
    MotionRegionStatus status{MotionRegionStatus::missing_declaration};
    [[nodiscard]] bool valid() const noexcept { return status == MotionRegionStatus::valid; }
};
[[nodiscard]] inline const char* motion_region_reason(MotionRegionStatus status) noexcept {
    switch (status) {
    case MotionRegionStatus::valid: return "valid";
    case MotionRegionStatus::missing_declaration: return "missing_declaration";
    case MotionRegionStatus::missing_resource: return "missing_resource";
    case MotionRegionStatus::invalid_dimensions: return "invalid_dimensions";
    case MotionRegionStatus::output_underflow: return "output_underflow";
    case MotionRegionStatus::coordinate_overflow: return "coordinate_overflow";
    case MotionRegionStatus::outside_view: return "outside_view";
    case MotionRegionStatus::outside_texture: return "outside_texture";
    }
    return "unknown";
}
// Texture extents only validate. Neither their size nor another fitting region
// can change the game's declaration. All coordinates remain in integer space.
[[nodiscard]] inline MotionRegion resolve_motion_region(bool declared, std::uint32_t flags,
    bool resource_present, std::uint64_t texture_width, std::uint64_t texture_height,
    std::uint32_t mv_x, std::uint32_t mv_y, const FoveationGeometry& crop,
    std::uint32_t input_width, std::uint32_t input_height,
    std::uint32_t output_width, std::uint32_t output_height,
    std::uint32_t output_x, std::uint32_t output_y) noexcept {
    MotionRegion result{};
    if (!declared) return result;
    const bool low = (flags & (1U << 1U)) != 0U;
    result.space = low ? DeclaredMotionSpace::input : DeclaredMotionSpace::output;
    auto& r = result.rectangle;
    r.width = low ? crop.input_width : crop.output_width;
    r.height = low ? crop.input_height : crop.output_height;
    const auto reject = [&](MotionRegionStatus status) { result.status = status; return result; };
    if (!input_width || !input_height || !output_width || !output_height ||
        !r.width || !r.height) return reject(MotionRegionStatus::invalid_dimensions);
    if (!low && (crop.output_base_x < output_x || crop.output_base_y < output_y))
        return reject(MotionRegionStatus::output_underflow);
    const auto x = low ? crop.input_base_x : crop.output_base_x - output_x;
    const auto y = low ? crop.input_base_y : crop.output_base_y - output_y;
    const auto rx = std::uint64_t{mv_x} + x, ry = std::uint64_t{mv_y} + y;
    constexpr auto max = (std::numeric_limits<std::uint32_t>::max)();
    if (rx > max || ry > max || rx + r.width > max || ry + r.height > max)
        return reject(MotionRegionStatus::coordinate_overflow);
    r.x = static_cast<std::uint32_t>(rx); r.y = static_cast<std::uint32_t>(ry);
    if (std::uint64_t{x} + r.width > (low ? input_width : output_width) ||
        std::uint64_t{y} + r.height > (low ? input_height : output_height))
        return reject(MotionRegionStatus::outside_view);
    if (!resource_present) return reject(MotionRegionStatus::missing_resource);
    if (!texture_width || !texture_height || texture_width > max || texture_height > max)
        return reject(MotionRegionStatus::invalid_dimensions);
    if (rx + r.width > texture_width || ry + r.height > texture_height)
        return reject(MotionRegionStatus::outside_texture);
    result.status = MotionRegionStatus::valid;
    return result;
}
} // namespace cheeky::foveated_dlss
