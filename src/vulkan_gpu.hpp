#pragma once
#include "vulkan_api.hpp"
#include <span>
#include <vector>

namespace cheeky::foveated_dlss {
struct VulkanImage {
    VulkanNgxResource ngx{};
    VkDeviceMemory memory{};
    VkImageView array_view{};
    bool initialized{};
    bool create(const VulkanDeviceApi&, unsigned, unsigned, VkFormat);
    void destroy(const VulkanDeviceApi&) noexcept;
};
struct VulkanBuffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
    bool create(const VulkanDeviceApi&, VkDeviceSize);
    void destroy(const VulkanDeviceApi&) noexcept;
};
struct VulkanCompute {
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    unsigned inputs{}, outputs{};
    bool create(const VulkanDeviceApi&, std::span<const std::uint32_t>, unsigned, const char*, unsigned outputs=1);
    void destroy(const VulkanDeviceApi&) noexcept;
};
// A descriptor set and constants buffer belong to a single in-flight use. They
// cannot be rewritten until the slot's GPU completion event has been signaled.
struct VulkanDispatch {
    VkDescriptorPool pool{};
    VkDescriptorSet descriptors{};
    VulkanBuffer constants;
    bool create(const VulkanDeviceApi&, const VulkanCompute&, unsigned);
    void destroy(const VulkanDeviceApi&) noexcept;
    void record(const VulkanDeviceApi&, VkCommandBuffer, const VulkanCompute&,
        std::span<const VkImageView>, VkImageView, const void*, unsigned, unsigned, unsigned);
    void record(const VulkanDeviceApi&, VkCommandBuffer, const VulkanCompute&,
        std::span<const VkImageView>, std::span<const VkImageView>, const void*, unsigned, unsigned, unsigned);
};
void vulkan_barrier(const VulkanDeviceApi&, VkCommandBuffer, const VulkanImageInfo&,
    VkImageLayout before, VkImageLayout after,
    VkAccessFlags source=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
    VkAccessFlags destination=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT) noexcept;
void vulkan_prepare_image(const VulkanDeviceApi&, VkCommandBuffer, VulkanImage&) noexcept;
}
