#pragma once
#include "vulkan_gpu.hpp"
#include "vulkan_features.hpp"
#include "frame_contract.hpp"
#include "settings.hpp"

namespace cheeky::foveated_dlss {
struct VulkanPeripheralPipelines {
    VulkanCompute color,depth,motion;
    bool initialize(const VulkanDeviceApi&);
    void destroy(const VulkanDeviceApi&);
};
struct VulkanPeripheralSlot {
    VulkanImage color,depth,motion,output;
    VulkanDispatch color_dispatch,depth_dispatch,motion_dispatch;
    std::shared_ptr<VulkanFeature> feature;
    void destroy(const VulkanDeviceApi&);
};
struct VulkanPeripheralHistory {
    std::shared_ptr<VulkanFeature> feature;
    bool valid{};
};
bool vulkan_peripheral(const VulkanDeviceApi&,VkCommandBuffer,const NgxParameters*,
    const DlssFrameContract&,const Settings&,const VulkanNgxCallbacks&,VulkanPeripheralPipelines&,
    VulkanPeripheralSlot&,VulkanPeripheralHistory&,VulkanNgxResource&,VulkanNgxResource&,VulkanNgxResource&,bool,NgxResult&);
}
