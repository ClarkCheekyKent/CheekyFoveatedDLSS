#pragma once
#include "vulkan_api.hpp"
#include <vector>
#include <string>
namespace cheeky::foveated_dlss {
bool vulkan_nr_runtime(const VulkanDeviceApi&,VulkanNgxCallbacks&,NgxResult&) noexcept;
void vulkan_nr_release_device(VkDevice) noexcept;
// Queried before the application creates its instance/device. The layer adds
// supported, missing extensions; it never strips the game's own requirements.
bool vulkan_nr_extensions(std::vector<std::string>&,VkInstance=VK_NULL_HANDLE,VkPhysicalDevice=VK_NULL_HANDLE) noexcept;
}
