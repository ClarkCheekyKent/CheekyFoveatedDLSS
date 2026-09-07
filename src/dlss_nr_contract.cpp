#include "dlss_nr_contract.hpp"

#include <algorithm>

namespace cheeky::foveated_dlss {

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
    if (shared_sr_crop != nullptr && render_width != 0U && render_height != 0U) {
        center_x = (2.0F * shared_sr_crop->input_base_x +
            shared_sr_crop->input_width) / render_width - 1.0F;
        center_y = (2.0F * shared_sr_crop->input_base_y +
            shared_sr_crop->input_height) / render_height - 1.0F;
    }
    parameters.x_offset = parameters.width < 1.0F
        ? std::clamp(center_x / (1.0F - parameters.width), -1.0F, 1.0F) : 0.0F;
    parameters.y_offset = parameters.height < 1.0F
        ? std::clamp(center_y / (1.0F - parameters.height), -1.0F, 1.0F) : 0.0F;
    return parameters;
}

}  // namespace cheeky::foveated_dlss
