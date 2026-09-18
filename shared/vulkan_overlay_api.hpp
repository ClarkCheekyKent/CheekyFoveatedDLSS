#pragma once
#include <Windows.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "../third_party/vulkan/include/vulkan/vulkan_core.h"

// Resident host boundary. All pointed-to data is borrowed for this call only.
struct CheekyVulkanPresent {
    unsigned size{sizeof(CheekyVulkanPresent)};
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};
    VkQueue queue{};unsigned queue_family{};
    PFN_vkGetInstanceProcAddr gipa{};PFN_vkGetDeviceProcAddr gdpa{};
    HWND window{};VkSwapchainKHR swapchain{};VkFormat format{};VkColorSpaceKHR color_space{};
    VkExtent2D extent{};unsigned image_count{};const VkImage* images{};
    unsigned present_index{};
    const VkPresentInfoKHR* present{};PFN_vkQueuePresentKHR next{};
};
using CheekyVulkanPresentFn=VkResult(*)(const CheekyVulkanPresent*);
using CheekyVulkanDestroyFn=void(*)(VkDevice,VkSwapchainKHR);
