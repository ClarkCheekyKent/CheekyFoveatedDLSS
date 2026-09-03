#include "foveation.hpp"

#include <algorithm>
#include <cmath>

namespace cheeky::foveated_dlss {

bool calculate_foveation_geometry(
    const FoveationParameters& parameters,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t output_origin_x,
    const std::uint32_t output_origin_y,
    FoveationGeometry& geometry
) noexcept {
    if (render_width == 0U || render_height == 0U || output_width == 0U ||
        output_height == 0U) {
        return false;
    }

    const auto bound_x = std::clamp(parameters.width, 0.0F, 1.0F);
    const auto bound_y = std::clamp(parameters.height, 0.0F, 1.0F);
    const auto input_width = std::clamp(
        static_cast<std::uint32_t>(std::lround(
            static_cast<double>(render_width) * bound_x
        )),
        1U,
        render_width
    );
    const auto input_height = std::clamp(
        static_cast<std::uint32_t>(std::lround(
            static_cast<double>(render_height) * bound_y
        )),
        1U,
        render_height
    );

    const auto available_x = render_width - input_width;
    const auto input_start_x = std::clamp(
        static_cast<std::uint32_t>(std::lround(
            static_cast<double>(available_x) *
            (static_cast<double>(std::clamp(parameters.x_offset, -1.0F, 1.0F)) +
             1.0) * 0.5
        )),
        0U,
        available_x
    );
    const auto available_y = render_height - input_height;
    const auto input_start_y = std::clamp(
        static_cast<std::uint32_t>(std::lround(
            static_cast<double>(available_y) *
            (static_cast<double>(std::clamp(parameters.y_offset, -1.0F, 1.0F)) +
             1.0) * 0.5
        )),
        0U,
        available_y
    );
    const auto input_end_x = input_start_x + input_width;
    const auto input_end_y = input_start_y + input_height;

    const auto relative_output_start_x = static_cast<std::uint32_t>(std::floor(
        static_cast<double>(input_start_x) * output_width / render_width
    ));
    const auto relative_output_start_y = static_cast<std::uint32_t>(std::floor(
        static_cast<double>(input_start_y) * output_height / render_height
    ));
    const auto relative_output_end_x = std::min(
        output_width,
        static_cast<std::uint32_t>(std::ceil(
            static_cast<double>(input_end_x) * output_width / render_width
        ))
    );
    const auto relative_output_end_y = std::min(
        output_height,
        static_cast<std::uint32_t>(std::ceil(
            static_cast<double>(input_end_y) * output_height / render_height
        ))
    );

    geometry = {
        input_start_x,
        input_start_y,
        input_end_x - input_start_x,
        input_end_y - input_start_y,
        output_origin_x + relative_output_start_x,
        output_origin_y + relative_output_start_y,
        relative_output_end_x - relative_output_start_x,
        relative_output_end_y - relative_output_start_y,
    };
    return geometry.output_width != 0U && geometry.output_height != 0U;
}

}  // namespace cheeky::foveated_dlss
