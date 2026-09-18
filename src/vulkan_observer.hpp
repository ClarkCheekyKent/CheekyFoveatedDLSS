#pragma once
#include "vulkan_api.hpp"
#include <memory>

namespace cheeky::foveated_dlss {
bool vulkan_observe_device(VkInstance,VkPhysicalDevice,VkDevice,PFN_vkGetInstanceProcAddr,PFN_vkGetDeviceProcAddr);
void vulkan_runtime_frame(VkDevice, VkQueue = VK_NULL_HANDLE) noexcept;
std::shared_ptr<VulkanDeviceApi> vulkan_command_device(VkCommandBuffer) noexcept;
bool vulkan_image_layout(VkCommandBuffer, const VulkanImageInfo&, VkImageLayout&) noexcept;
void vulkan_forget_command(VkCommandBuffer) noexcept;
void vulkan_release_device(VkDevice) noexcept;
void vulkan_install_ngx_hooks(HMODULE) noexcept;
void vulkan_backend_forget_command(VkCommandBuffer) noexcept;
void vulkan_backend_release_device(VkDevice) noexcept;
}
