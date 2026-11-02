#pragma once
#include <cstdint>
#include <cstddef>
// Presentation-only bounding boxes for Universal DLSS 5. These never modify
// the color images used by optical flow, NR or frame-generation history.
struct CheekyXrBorderOptionsV1 {
    std::uint32_t abi{2},size{sizeof(CheekyXrBorderOptionsV1)};
    std::uint32_t enabled{},show_sr{},show_nr{},automatic_alignment{};
    float width[2]{1,1},height[2]{1,1}; // SR, NR.
    float center_u[2]{.5f,.5f},center_v[2]{.5f,.5f}; // Physical eyes.
    float aligned_height_offset{};
    std::uint32_t gaze_mask{}; // v2: bit 0 SR box, bit 1 NR box follow the frame's projected gaze.
};
constexpr std::uint32_t CHEEKY_XR_BORDER_OPTIONS_V1_SIZE=offsetof(CheekyXrBorderOptionsV1,gaze_mask);
using CheekyXrGetBorderOptionsV1Fn=bool(__cdecl*)(CheekyXrBorderOptionsV1*);
