#include "dlss_nr_contract.hpp"

namespace cheeky::foveated_dlss {

DlssNrResourceBase dlss_nr_resource_base(
    const std::uint32_t local_x,
    const std::uint32_t local_y,
    const std::uint32_t color_base_x,
    const std::uint32_t color_base_y,
    const bool color_is_region
) noexcept {
    return color_is_region
        ? DlssNrResourceBase{local_x, local_y}
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
    if (settings.nr_use_sr_foveation) {
        FoveationParameters parameters{
            settings.width,
            settings.height,
            settings.x_offset,
            settings.height_offset,
            settings.roundness,
            settings.transition_width,
        };
        if (shared_sr_crop != nullptr && render_width != 0U &&
            render_height != 0U) {
            const auto offsets = foveation_offsets_from_geometry(
                *shared_sr_crop, render_width, render_height
            );
            parameters.x_offset = offsets.x;
            parameters.y_offset = offsets.y;
        }
        return parameters;
    }
    return {
        settings.nr_width,
        settings.nr_height,
        settings.nr_x_offset,
        settings.nr_height_offset,
        settings.nr_roundness,
        settings.nr_transition_width,
    };
}

}  // namespace cheeky::foveated_dlss
