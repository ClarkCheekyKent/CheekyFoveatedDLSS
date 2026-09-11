#include "dlss_nr_contract.hpp"

#include <algorithm>
#include <cmath>

namespace cheeky::foveated_dlss {

DlssNrAxis dlss_nr_aligned_axis(const std::uint32_t base,
    const std::uint32_t extent, const std::uint32_t capacity) noexcept {
    const auto size = static_cast<std::uint32_t>((std::min)(
        static_cast<std::uint64_t>(capacity),
        (static_cast<std::uint64_t>(extent) + 7U) / 8U * 8U));
    return {(std::min)(base - base % 8U, capacity - size), size};
}

bool dlss_nr_motion_offset(const DlssNrHistory& previous,
    const DlssNrHistory& current, float& x, float& y) noexcept {
    x = y = 0.0F;
    if (previous.width != current.width || previous.height != current.height ||
        previous.output_width != current.output_width ||
        previous.output_height != current.output_height ||
        previous.working_width != current.working_width ||
        previous.working_height != current.working_height ||
        previous.scale_x != current.scale_x || previous.scale_y != current.scale_y ||
        current.width == 0U || current.height == 0U) return false;
    const auto dx = static_cast<double>(current.x) - previous.x;
    const auto dy = static_cast<double>(current.y) - previous.y;
    // There is no reusable overlap after a jump larger than the region.
    if (std::abs(dx) >= current.width || std::abs(dy) >= current.height) return false;
    if (dx == 0.0 && dy == 0.0) return true;
    if (!std::isfinite(current.scale_x) || !std::isfinite(current.scale_y) ||
        !std::isfinite(previous.scale_x) || !std::isfinite(previous.scale_y)) return false;
    x = static_cast<float>(dx / current.width);
    y = static_cast<float>(dy / current.height);
    return std::isfinite(x) && std::isfinite(y);
}

DlssNrResourceBase dlss_nr_resource_base(
    const std::uint32_t local_x,
    const std::uint32_t local_y,
    const std::uint32_t color_base_x,
    const std::uint32_t color_base_y,
    const bool color_is_region
) noexcept {
    return color_is_region
        ? DlssNrResourceBase{0U, 0U}
        : DlssNrResourceBase{
            color_base_x + local_x,
            color_base_y + local_y,
        };
}

FoveationParameters dlss_nr_foveation_parameters(
    const Settings& settings,
    const FoveationGeometry* const shared_sr_crop,
    const std::uint32_t render_width,
    const std::uint32_t render_height
) noexcept {
    const auto center = shared_sr_crop
        ? foveation_center_from_geometry(*shared_sr_crop, render_width, render_height) : FoveationCenter{};
    return dlss_nr_foveation_parameters(settings,
        shared_sr_crop && render_width && render_height ? &center : nullptr);
}

FoveationParameters dlss_nr_foveation_parameters(
    const Settings& settings, const FoveationCenter* center) noexcept {
    FoveationParameters parameters{
        settings.nr_use_sr_foveation ? settings.width : settings.nr_width,
        settings.nr_use_sr_foveation ? settings.height : settings.nr_height,
        0.0F,
        0.0F,
        settings.nr_use_sr_foveation ? settings.roundness : settings.nr_roundness,
        settings.nr_use_sr_foveation
            ? settings.transition_width : settings.nr_transition_width,
    };
    // Offsets are fractions of the space left around a crop, not absolute
    // centers. Convert through the SR center so changing NR size cannot move it.
    float center_x = settings.x_offset * (1.0F - settings.width);
    float center_y = settings.height_offset * (1.0F - settings.height);
    if (center != nullptr) {
        center_x = 2.0F * center->u - 1.0F;
        center_y = 2.0F * center->v - 1.0F;
    }
    parameters.x_offset = parameters.width < 1.0F
        ? std::clamp(center_x / (1.0F - parameters.width), -1.0F, 1.0F) : 0.0F;
    parameters.y_offset = parameters.height < 1.0F
        ? std::clamp(center_y / (1.0F - parameters.height), -1.0F, 1.0F) : 0.0F;
    return parameters;
}

[[nodiscard]] NrRegion calculate_region(
    const Settings& settings,
    const std::uint32_t width,
    const std::uint32_t height,
    const FoveationGeometry* const shared_sr_crop,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const FoveationCenter* center
) noexcept {
    if (!settings.nr_foveated) {
        return {0U, 0U, width, height, 1.0F, 1.0F, 0.0F, 0.0F};
    }
    const auto parameters = center ? dlss_nr_foveation_parameters(settings, center)
        : dlss_nr_foveation_parameters(settings, shared_sr_crop, render_width, render_height);
    FoveationGeometry geometry{};
    if (!calculate_foveation_geometry(
            parameters,
            width,
            height,
            width,
            height,
            0U,
            0U,
            geometry
        )) {
        return {0U, 0U, width, height, 1.0F, 1.0F, 0.0F, 0.0F};
    }
    const auto x = dlss_nr_aligned_axis(geometry.output_base_x, geometry.output_width, width);
    const auto y = dlss_nr_aligned_axis(geometry.output_base_y, geometry.output_height, height);
    return {
        x.base, y.base, x.extent, y.extent,
        parameters.width,
        parameters.height,
        parameters.roundness,
        parameters.transition_width,
    };
}

[[nodiscard]] std::uint32_t scaled_extent(
    const std::uint32_t extent,
    const float scale
) noexcept {
    const auto requested = (std::max)(
        32U,
        static_cast<std::uint32_t>(
            static_cast<float>(extent) * std::clamp(scale, 0.1F, 1.0F) + 0.5F
        )
    );
    // Preserve the pixel grid at scale 1 instead of stretching to an aligned size.
    return requested;
}

[[nodiscard]] ScaledSubrect scale_subrect(
    const std::uint32_t region_base,
    const std::uint32_t region_extent,
    const std::uint32_t source_base,
    const std::uint32_t source_extent,
    const std::uint32_t output_extent
) noexcept {
    if (!output_extent || !source_extent || region_base >= output_extent || !region_extent) return {source_base, 0U};
    const auto base = static_cast<std::uint32_t>(std::floor(
        static_cast<double>(region_base) * source_extent / output_extent
    ));
    const auto end = (std::min)(
        source_extent,
        static_cast<std::uint32_t>(std::ceil(
            static_cast<double>(region_base + region_extent) * source_extent /
                output_extent
        ))
    );
    return {source_base + base, (std::max)(1U, end - base)};
}

bool calculate_dlss_nr_geometry(
    const Settings& settings,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    DlssNrGeometry& geometry,
    const FoveationCenter* center
) noexcept {
    if (output_width == 0U || output_height == 0U) return false;
    const auto region = calculate_region(
        settings, output_width, output_height, nullptr, 0U, 0U, center
    );
    if (region.width == 0U || region.height == 0U) return false;
    geometry = {
        region.base_x,
        region.base_y,
        region.width,
        region.height,
        scaled_extent(region.width, settings.nr_working_scale),
        scaled_extent(region.height, settings.nr_working_scale),
    };
    return true;
}

}  // namespace cheeky::foveated_dlss
