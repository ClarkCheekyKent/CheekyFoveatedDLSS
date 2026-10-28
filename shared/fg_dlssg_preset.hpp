#pragma once
#include <cstdint>

namespace cheeky::fg {
// 1/2/3 select DLSS-G A/B/C. C is the application default.
constexpr bool valid_dlssg_preset(std::uint32_t preset) noexcept { return preset>=1&&preset<=3; }

// NVIDIA's create-time parameters, also present in the current public SDK:
// https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs_dlssg.h
// Keep the strings here for compatibility with our older vendored headers.
template<class Parameters>
void apply_dlssg_preset(Parameters* parameters,std::uint32_t preset) noexcept {
    if(!parameters||!valid_dlssg_preset(preset))return;
    parameters->Set("DLSSG.UserInterfaceRecompositionEnabled",preset==2?1u:0u);
    parameters->Set("DLSSG.LiteCandidateBlendingEnabled",preset==3?1u:0u);
}
}
