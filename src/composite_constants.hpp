#pragma once
#include <cstdint>
namespace cheeky::foveated_dlss {
struct CompositeConstants {
    std::uint32_t output_size[2];
    std::uint32_t output_origin[2];
    std::uint32_t input_base[2];
    std::uint32_t input_size[2];
    std::uint32_t rect_base[2];
    std::uint32_t rect_size[2];
    float shape_width;
    float shape_height;
    float shape_offset_x;
    float shape_offset_y;
    float shape_roundness;
    float feather;
    std::uint32_t dlss_origin[2];
    std::uint32_t show_alignment_border;
    float next_jump_offset_x;
    float next_jump_offset_y;
    std::uint32_t show_next_jump;
    float next_jump_width, next_jump_height;
    std::uint32_t mask_count, padding;
    float mask_bounds[4][4];
};

static_assert(sizeof(CompositeConstants) == 44U * sizeof(std::uint32_t));
}
