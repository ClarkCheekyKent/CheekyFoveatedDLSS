#include "foveation.hpp"

#include <algorithm>
#include <cmath>

namespace cheeky::foveated_dlss {

namespace {

[[nodiscard]] std::uint32_t quantized_center_start(
    const float center,
    const std::uint32_t capacity,
    const std::uint32_t size,
    const std::uint32_t quantum
) noexcept {
    const auto available = capacity - size;
    const auto desired = static_cast<std::int64_t>(std::llround(
        static_cast<double>(std::clamp(center, 0.0F, 1.0F)) * capacity -
        static_cast<double>(size) * 0.5
    ));
    auto start = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        desired, 0, available
    ));
    const auto safe_quantum = (std::max)(quantum, 1U);
    if (safe_quantum > 1U && available != 0U) {
        start = (std::min)(
            available,
            ((start + safe_quantum / 2U) / safe_quantum) * safe_quantum
        );
    }
    return start;
}

[[nodiscard]] bool calculate_geometry_from_start(
    const std::uint32_t input_start_x,
    const std::uint32_t input_start_y,
    const std::uint32_t input_width,
    const std::uint32_t input_height,
    const std::uint32_t render_width,
    const std::uint32_t render_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height,
    const std::uint32_t output_origin_x,
    const std::uint32_t output_origin_y,
    FoveationGeometry& geometry
) noexcept {
    const auto input_end_x = input_start_x + input_width;
    const auto input_end_y = input_start_y + input_height;
    const auto relative_output_start_x = static_cast<std::uint32_t>(std::floor(
        static_cast<double>(input_start_x) * output_width / render_width
    ));
    const auto relative_output_start_y = static_cast<std::uint32_t>(std::floor(
        static_cast<double>(input_start_y) * output_height / render_height
    ));
    const auto relative_output_end_x = (std::min)(
        output_width,
        static_cast<std::uint32_t>(std::ceil(
            static_cast<double>(input_end_x) * output_width / render_width
        ))
    );
    const auto relative_output_end_y = (std::min)(
        output_height,
        static_cast<std::uint32_t>(std::ceil(
            static_cast<double>(input_end_y) * output_height / render_height
        ))
    );
    geometry = {
        input_start_x, input_start_y, input_width, input_height,
        output_origin_x + relative_output_start_x,
        output_origin_y + relative_output_start_y,
        relative_output_end_x - relative_output_start_x,
        relative_output_end_y - relative_output_start_y,
    };
    return geometry.output_width != 0U && geometry.output_height != 0U;
}

}  // namespace

FoveationOffsets foveation_offsets_from_geometry(
    const FoveationGeometry& geometry,
    const std::uint32_t render_width,
    const std::uint32_t render_height
) noexcept {
    const auto available_x = render_width > geometry.input_width
        ? render_width - geometry.input_width
        : 0U;
    const auto available_y = render_height > geometry.input_height
        ? render_height - geometry.input_height
        : 0U;
    return {
        available_x == 0U
            ? 0.0F
            : 2.0F * geometry.input_base_x / available_x - 1.0F,
        available_y == 0U
            ? 0.0F
            : 2.0F * geometry.input_base_y / available_y - 1.0F,
    };
}

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
    return calculate_geometry_from_start(
        input_start_x, input_start_y, input_width, input_height,
        render_width, render_height, output_width, output_height,
        output_origin_x, output_origin_y, geometry
    );
}

bool calculate_foveation_geometry_at_center(
    const FoveationParameters& parameters,
    const FoveationCenter& center,
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
    const auto width = std::clamp(parameters.width, 0.0F, 1.0F);
    const auto height = std::clamp(parameters.height, 0.0F, 1.0F);
    const auto input_width = std::clamp(
        static_cast<std::uint32_t>(std::lround(
            static_cast<double>(render_width) * width
        )), 1U, render_width
    );
    const auto input_height = std::clamp(
        static_cast<std::uint32_t>(std::lround(
            static_cast<double>(render_height) * height
        )), 1U, render_height
    );
    const auto start_x = quantized_center_start(
        center.u, render_width, input_width, center.quantization_pixels
    );
    const auto start_y = quantized_center_start(
        center.v, render_height, input_height, center.quantization_pixels
    );
    return calculate_geometry_from_start(
        start_x, start_y, input_width, input_height,
        render_width, render_height, output_width, output_height,
        output_origin_x, output_origin_y, geometry
    );
}

}  // namespace cheeky::foveated_dlss
