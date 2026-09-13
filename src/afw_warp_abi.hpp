#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cheeky::foveated_dlss {
// Public beta 6 PDAFWPlugin.h. Decode only after verifying the loaded runtime's
// ABI. Resource references are consumed only during its live callback; no
// framebuffer or camera pointers are retained.
struct AfwWarpPrefix {
    void* command_list{};
    void* input_framebuffer{};
    void* output_framebuffer{};
    void* ui_texture{};
    float ui_scale[2]{}, ui_position[3]{}, motion_scale[2]{};
    std::uint32_t mode{}, source_eye{};
};
static_assert(offsetof(AfwWarpPrefix, mode) == 60 && offsetof(AfwWarpPrefix, source_eye) == 64);
struct AfwWarpCameraPrefix { AfwWarpPrefix warp{}; void* cameras{}; };
static_assert(offsetof(AfwWarpCameraPrefix, cameras) == 72);
struct AfwTexturePrefix {
    std::uint32_t type{};
    void* resource{};
    std::int32_t srv{}, uav{};
    std::uint64_t srv_handle{}, uav_handle{}, target_handle{};
    std::uint32_t initial_state{};
};
struct AfwFramebuffer { AfwTexturePrefix color{}, depth{}, motion{}; };
static_assert(sizeof(AfwTexturePrefix) == 56 && offsetof(AfwTexturePrefix, resource) == 8);
struct AfwWarpMetadata { unsigned eye{UINT32_MAX}, mode{UINT32_MAX}; };
inline AfwWarpMetadata afw_warp_metadata(const void* parameters, bool known_abi) noexcept {
    if (!parameters || !known_abi) return {};
    AfwWarpPrefix prefix{};
    std::memcpy(&prefix, parameters, sizeof(prefix));
    if (prefix.source_eye > 1 || prefix.mode > 3) return {};
    return {prefix.source_eye, prefix.mode};
}
}
