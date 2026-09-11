#pragma once

#include "settings.hpp"

#include <cstdint>

namespace cheeky::foveated_dlss {

struct NrRegion {
    std::uint32_t base_x{};
    std::uint32_t base_y{};
    std::uint32_t width{};
    std::uint32_t height{};
    float shape_width{1.0F};
    float shape_height{1.0F};
    float roundness{};
    float transition{};
};

struct ScaledSubrect {
    std::uint32_t base{};
    std::uint32_t extent{};
};

struct DlssNrGeometry {
    std::uint32_t base_x{};
    std::uint32_t base_y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t working_width{};
    std::uint32_t working_height{};
};

[[nodiscard]] bool calculate_dlss_nr_geometry(
    const Settings& settings,
    std::uint32_t output_width,
    std::uint32_t output_height,
    DlssNrGeometry& geometry,
    const FoveationCenter* center = nullptr
) noexcept;

struct DlssNrResolution { std::uint32_t width{}, height{}; };
[[nodiscard]] inline DlssNrResolution dlss_nr_processing_resolution(NrProcessingOrder order,
    std::uint32_t render_width, std::uint32_t render_height,
    std::uint32_t output_width, std::uint32_t output_height) noexcept {
    return order == NrProcessingOrder::before_upscaling
        ? DlssNrResolution{render_width, render_height} : DlssNrResolution{output_width, output_height};
}
[[nodiscard]] NrRegion calculate_region(const Settings& settings, std::uint32_t width,
    std::uint32_t height, const FoveationGeometry* shared_sr_crop,
    std::uint32_t render_width, std::uint32_t render_height,
    const FoveationCenter* center = nullptr) noexcept;
[[nodiscard]] std::uint32_t scaled_extent(std::uint32_t extent, float scale) noexcept;
[[nodiscard]] ScaledSubrect scale_subrect(std::uint32_t region_base, std::uint32_t region_extent,
    std::uint32_t source_base, std::uint32_t source_extent, std::uint32_t output_extent) noexcept;

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

// NGX scales convert stored vectors to render pixels, independently of the
// resolution of the motion texture. Streamline scales already produce UVs.
[[nodiscard]] inline float dlss_nr_ngx_motion_uv_scale(float scale,
    std::uint32_t render_extent) noexcept {
    return render_extent ? scale / static_cast<float>(render_extent) : 0.0F;
}

struct DlssNrMotionAxis {
    float processing_pixel_scale{};
    float runtime_scale{};
};

// Match the temporal coordinate domain of the color supplied to NR. Before
// uses jittered render color; After uses the stabilized SR output. Jitter is
// already a displacement, so do not apply the user's motion multiplier to it.
[[nodiscard]] inline float dlss_nr_jitter_delta(float previous, float current,
    bool before, bool vectors_jittered, bool reset) noexcept {
    return reset ? 0.0F : (static_cast<int>(before) - static_cast<int>(vectors_jittered)) *
        (previous - current);
}

// NR divides MVecScale by the declared motion subrect extent. Color working
// resolution must not affect the resulting normalized history displacement.
[[nodiscard]] inline DlssNrMotionAxis dlss_nr_motion_axis(float uv_scale,
    std::uint32_t processing_extent, std::uint32_t region_extent,
    std::uint32_t motion_extent) noexcept {
    const float pixels = uv_scale * static_cast<float>(processing_extent);
    return {pixels, region_extent ? pixels * static_cast<float>(motion_extent) /
        static_cast<float>(region_extent) : 0.0F};
}

// Scales convert stored vectors to processing pixels (the crop-origin units).
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
