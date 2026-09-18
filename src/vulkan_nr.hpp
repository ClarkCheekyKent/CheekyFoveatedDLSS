#pragma once
#include "vulkan_features.hpp"
#include "vulkan_gpu.hpp"
#include "dlss_nr_contract.hpp"
#include "frame_contract.hpp"
#include <array>

namespace cheeky::foveated_dlss {
struct VulkanNrPipelines {
    VulkanCompute guides;
    std::array<VulkanCompute,3> copy,encode,decode,border;
    bool initialize(const VulkanDeviceApi&,VkFormat,unsigned&);
    void destroy(const VulkanDeviceApi&);
};
struct VulkanNrSlot {
    bool produced{};
    VulkanImage processed,original,proxy,neural,motion,depth;
    VulkanDispatch copy_dispatch,encode_dispatch,decode_dispatch,guide_dispatch,border_dispatch;
    std::shared_ptr<VulkanFeature> feature;
    void destroy(const VulkanDeviceApi&);
};
struct VulkanNrHistory {
    std::shared_ptr<VulkanFeature> feature;
    DlssNrHistory geometry{};
    float jitter_x{},jitter_y{};
    std::uint64_t signature{},generation{};
    bool valid{},failed{};
};
// Input and output are GENERAL. Before-SR returns a private replacement and
// leaves the game's render color untouched. After-SR edits only the NR region.
bool vulkan_nr(const VulkanDeviceApi&,VkCommandBuffer,const DlssFrameContract&,const Settings&,
    const CropGeometry*,const FoveationCenter*,VulkanNgxResource&,VulkanNgxResource&,VulkanNgxResource&,
    VulkanNrPipelines&,VulkanNrSlot&,VulkanNrHistory&,const VulkanNgxCallbacks&,NgxResult&);
}
