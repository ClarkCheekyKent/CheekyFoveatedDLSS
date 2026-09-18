#pragma once
#include "overlay.hpp"
#include "../shared/vulkan_overlay_api.hpp"
namespace cheeky::standalone {
VkResult overlay_vulkan_present(const CheekyVulkanPresent&,const OverlayRuntime&) noexcept;
void overlay_vulkan_destroy(VkDevice,VkSwapchainKHR) noexcept;
}
