#pragma once

#define VK_NO_PROTOTYPES
#include "../third_party/vulkan/include/vulkan/vulkan_core.h"
#include "ngx_abi.hpp"

namespace cheeky::foveated_dlss {
// Public NGX Vulkan resource ABI. Images are never cast to D3D resources.
struct VulkanImageInfo {
    VkImageView view{};
    VkImage image{};
    VkImageSubresourceRange range{};
    VkFormat format{};
    unsigned width{}, height{};
};
struct VulkanNgxResource {
    union { VulkanImageInfo image; struct { VkBuffer buffer; unsigned size; } buffer; } resource{};
    unsigned type{}; // 0 = image view, 1 = buffer
    bool read_write{};
};
static_assert(sizeof(VulkanImageInfo) == 48);
static_assert(sizeof(VulkanNgxResource) == 56);

using VulkanNgxCreate = NgxResult (*)(VkCommandBuffer, unsigned, NgxParameters*, NgxHandle**);
using VulkanNgxEvaluate = NgxResult (*)(VkCommandBuffer, const NgxHandle*, const NgxParameters*, NgxProgressCallback);
using VulkanNgxRelease = NgxResult (*)(NgxHandle*);
using VulkanNgxCreate1 = NgxResult (*)(VkDevice,VkCommandBuffer,unsigned,NgxParameters*,NgxHandle**);
struct VulkanNgxCallbacks {
    VulkanNgxCreate create{};
    VulkanNgxEvaluate evaluate{};
    VulkanNgxRelease release{};
    VulkanNgxCreate1 create1{};
    NgxResult (*allocate_parameters)(NgxParameters**){};
    NgxResult (*destroy_parameters)(NgxParameters*){};
};
inline thread_local unsigned vulkan_ngx_private_depth{};
struct VulkanNgxScope {
    VulkanNgxScope(){++vulkan_ngx_private_depth;}
    ~VulkanNgxScope(){--vulkan_ngx_private_depth;}
};

// All dispatch comes from the intercepted application's device. No secondary
// graphics device, API translation, or cross-API image copies are involved.
struct VulkanDeviceApi {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetDeviceProcAddr gdpa{};
#define CHEEKY_VK_DEVICE_FUNCTIONS(X) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
    X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CmdPipelineBarrier) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch) X(CmdCopyImage) \
    X(CreateEvent) X(DestroyEvent) X(GetEventStatus) X(ResetEvent) X(CmdSetEvent) X(DeviceWaitIdle)
#define CHEEKY_VK_DECLARE(name) PFN_vk##name name{};
    CHEEKY_VK_DEVICE_FUNCTIONS(CHEEKY_VK_DECLARE)
#undef CHEEKY_VK_DECLARE
    VkPhysicalDeviceMemoryProperties memory{};
    bool initialize(VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr) noexcept;
};
}
