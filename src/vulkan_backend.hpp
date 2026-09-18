#pragma once
#include "vulkan_api.hpp"
#include "frame_contract.hpp"
#include "settings.hpp"
namespace cheeky::foveated_dlss {
struct VulkanBackendStatus {
    std::uint64_t calls{}, active{}, passthrough{}, failed{}, allocations{};
    unsigned input_width{},input_height{},output_width{},output_height{};
    unsigned last_result{};
    std::uint64_t peripheral_active{},nr_active{};
    unsigned peripheral_result{},nr_result{};
    const char* reason{"Waiting for Vulkan DLSS"};
};
VulkanBackendStatus vulkan_backend_status() noexcept;
bool evaluate_vulkan_backend(VkCommandBuffer,const NgxParameters*,const DlssFrameContract&,
    const Settings&,const VulkanNgxCallbacks&,NgxResult&,const NgxHandle* game_handle=nullptr,
    NgxProgressCallback=nullptr,const VulkanNgxCallbacks* nr_callbacks=nullptr) noexcept;
void vulkan_release_view(DlssViewId) noexcept;
}
