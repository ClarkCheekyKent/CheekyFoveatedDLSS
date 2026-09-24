#pragma once
#include "vulkan_api.hpp"
#include "ngx_parameter_overlay.hpp"
#include <memory>

namespace cheeky::foveated_dlss {
// Shared by SR, peripheral DLAA and NR. A recorded command owns a reference
// until it is reset/freed; replacing the current feature never frees an
// object that an older recording can still reference.
struct VulkanFeatureKey {
    unsigned width{}, height{}, out_width{}, out_height{}, flags{}, quality{}, preset{};
    bool operator==(const VulkanFeatureKey&) const = default;
};
struct VulkanFeature {
    VulkanFeatureKey key{};
    NgxHandle* handle{};
    VulkanNgxRelease release{};
    NgxParameterOverlay local_parameters;
    NgxParameters* parameters{};
    NgxResult (*destroy_parameters)(NgxParameters*){};
    ~VulkanFeature() {
        VulkanNgxScope private_call;
        if(handle && release)release(handle);
        if(parameters && destroy_parameters)destroy_parameters(parameters);
    }
};
inline void vulkan_feature_parameters(NgxParameters& p,const VulkanFeatureKey& k) {
    p.Set("Width",k.width);p.Set("Height",k.height);p.Set("OutWidth",k.out_width);p.Set("OutHeight",k.out_height);
    p.Set("DLSS.Render.Subrect.Dimensions.Width",k.width);p.Set("DLSS.Render.Subrect.Dimensions.Height",k.height);
    p.Set("DLSS.Feature.Create.Flags",k.flags);p.Set("PerfQualityValue",k.quality);
    if(k.preset)for(const auto* name:{"DLSS.Hint.Render.Preset.DLAA","DLSS.Hint.Render.Preset.Quality","DLSS.Hint.Render.Preset.Balanced",
        "DLSS.Hint.Render.Preset.Performance","DLSS.Hint.Render.Preset.UltraPerformance","DLSS.Hint.Render.Preset.UltraQuality"})p.Set(name,k.preset);
}
inline NgxResult vulkan_create_feature(const VulkanDeviceApi& a,VkCommandBuffer cmd,unsigned type,
    NgxParameters& p,VulkanFeature& feature,const VulkanNgxCallbacks& callbacks) {
    VulkanNgxScope private_call;
    feature.release=callbacks.release;
    return callbacks.create1 ? callbacks.create1(a.device,cmd,type,&p,&feature.handle) :
        callbacks.create ? callbacks.create(cmd,type,&p,&feature.handle) : 0xBAD00001U;
}
}
