#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cheeky::foveated_dlss {
// Public beta 6 PDAFWPlugin.h, through EyeIndex only. Never dereference its
// resources or cameras. Decode only after verifying the loaded runtime's ABI.
struct AfwWarpPrefix {
    void* command_list{};
    void* input_framebuffer{};
    void* output_framebuffer{};
    void* ui_texture{};
    float ui_scale[2]{}, ui_position[3]{}, motion_scale[2]{};
    std::uint32_t mode{}, source_eye{};
};
static_assert(offsetof(AfwWarpPrefix, mode) == 60 && offsetof(AfwWarpPrefix, source_eye) == 64);
struct AfwWarpMetadata { unsigned eye{UINT32_MAX}, mode{UINT32_MAX}; };
inline AfwWarpMetadata afw_warp_metadata(const void* parameters, bool known_abi) noexcept {
    if (!parameters || !known_abi) return {};
    AfwWarpPrefix prefix{};
    std::memcpy(&prefix, parameters, sizeof(prefix));
    if (prefix.source_eye > 1 || prefix.mode > 3) return {};
    return {prefix.source_eye, prefix.mode};
}
}
