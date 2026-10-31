#pragma once
#include <cstdint>
// Presentation-only bounding boxes for Universal DLSS 5. These never modify
// the color images used by optical flow, NR or frame-generation history.
struct CheekyXrBorderOptionsV1 {
    std::uint32_t abi{1},size{sizeof(CheekyXrBorderOptionsV1)};
    std::uint32_t enabled{},show_sr{},show_nr{},automatic_alignment{};
    float width[2]{1,1},height[2]{1,1}; // SR, NR.
    float center_u[2]{.5f,.5f},center_v[2]{.5f,.5f}; // Physical eyes.
    float aligned_height_offset{};
};
using CheekyXrGetBorderOptionsV1Fn=bool(__cdecl*)(CheekyXrBorderOptionsV1*);
