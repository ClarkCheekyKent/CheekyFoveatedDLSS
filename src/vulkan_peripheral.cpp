#include "vulkan_peripheral.hpp"
#include "vulkan_shaders.hpp"
#include "peripheral_contract.hpp"
#include "ngx_frame_contract.hpp"

namespace cheeky::foveated_dlss {
bool VulkanPeripheralPipelines::initialize(const VulkanDeviceApi& a) {
    return color.create(a,vulkan_shaders::peripheral_color,1,"Main") &&
        depth.create(a,vulkan_shaders::peripheral_depth,1,"Main") && motion.create(a,vulkan_shaders::peripheral_motion,1,"Main");
}
void VulkanPeripheralPipelines::destroy(const VulkanDeviceApi& a) {color.destroy(a);depth.destroy(a);motion.destroy(a);}
void VulkanPeripheralSlot::destroy(const VulkanDeviceApi& a) {
    feature.reset();color.destroy(a);depth.destroy(a);motion.destroy(a);output.destroy(a);
    color_dispatch.destroy(a);depth_dispatch.destroy(a);motion_dispatch.destroy(a);
}
bool vulkan_peripheral(const VulkanDeviceApi& a,VkCommandBuffer cmd,const NgxParameters* original,
    const DlssFrameContract& c,const Settings& settings,const VulkanNgxCallbacks& callbacks,VulkanPeripheralPipelines& pipelines,
    VulkanPeripheralSlot& slot,VulkanPeripheralHistory& history,VulkanNgxResource& color,VulkanNgxResource& depth,VulkanNgxResource& motion,
    bool reset,NgxResult& result) {
    const auto size=peripheral_dlaa_dimensions(c.render_width,c.render_height,settings.peripheral_dlaa_scale);
    const auto w=size.width,h=size.height;
    // Always supply low-resolution vectors on the DLAA working grid. NGX's
    // scale is measured in render pixels regardless of the source MV extent.
    const VulkanFeatureKey key{w,h,w,h,c.create_flags|2U,5U,settings.peripheral_dlaa_preset};
    if(!pipelines.initialize(a) || !slot.color.create(a,w,h,VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !slot.depth.create(a,w,h,VK_FORMAT_R32_SFLOAT) || !slot.motion.create(a,w,h,VK_FORMAT_R32G32_SFLOAT) ||
        !slot.output.create(a,w,h,VK_FORMAT_R16G16B16A16_SFLOAT) || !slot.color_dispatch.create(a,pipelines.color,24) ||
        !slot.depth_dispatch.create(a,pipelines.depth,24) || !slot.motion_dispatch.create(a,pipelines.motion,24))return false;
    if(!history.feature || !(history.feature->key==key)) {
        auto feature=std::make_shared<VulkanFeature>();feature->key=key;
        // Retain even a failed creation until the caller's recording is reset.
        slot.feature=feature;
        NgxParameterOverlay p(original);vulkan_feature_parameters(p,key);
        result=vulkan_create_feature(a,cmd,1,p,*feature,callbacks);
        if(!ngx_succeeded(result) || !feature->handle){history.valid=false;return false;}
        history.feature=feature;history.valid=false;
    }
    slot.feature=history.feature;
    struct Constants {unsigned base[2],source[2],target[2];};
    const auto prepare=[&](VulkanImage& output,VulkanDispatch& dispatch,const VulkanCompute& kernel,
        const VulkanImageInfo& input,unsigned x,unsigned y,unsigned iw,unsigned ih) {
        vulkan_prepare_image(a,cmd,output);
        const Constants constants{{x,y},{iw,ih},{w,h}};
        dispatch.record(a,cmd,kernel,std::span(&input.view,1),output.ngx.resource.image.view,&constants,sizeof(constants),(w+15)/16,(h+15)/16);
        vulkan_barrier(a,cmd,output.ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    };
    prepare(slot.color,slot.color_dispatch,pipelines.color,color.resource.image,c.color_base_x,c.color_base_y,c.render_width,c.render_height);
    prepare(slot.depth,slot.depth_dispatch,pipelines.depth,depth.resource.image,c.depth_base_x,c.depth_base_y,c.render_width,c.render_height);
    prepare(slot.motion,slot.motion_dispatch,pipelines.motion,motion.resource.image,c.mv_base_x,c.mv_base_y,
        c.motion_vectors_low_res?c.render_width:c.output_width,c.motion_vectors_low_res?c.render_height:c.output_height);
    vulkan_prepare_image(a,cmd,slot.output);
    NgxParameterOverlay p(original);vulkan_feature_parameters(p,key);
    p.Set("Color",static_cast<void*>(&slot.color.ngx));p.Set("Depth",static_cast<void*>(&slot.depth.ngx));
    p.Set("MotionVectors",static_cast<void*>(&slot.motion.ngx));p.Set("Output",static_cast<void*>(&slot.output.ngx));
    for(const auto* name:{"DLSS.Input.Color.Subrect.Base.X","DLSS.Input.Color.Subrect.Base.Y","DLSS.Input.Depth.Subrect.Base.X",
        "DLSS.Input.Depth.Subrect.Base.Y","DLSS.Input.MV.Subrect.Base.X","DLSS.Input.MV.Subrect.Base.Y","DLSS.Output.Subrect.Base.X","DLSS.Output.Subrect.Base.Y"})p.Set(name,0U);
    const float sx=static_cast<float>(w)/c.render_width,sy=static_cast<float>(h)/c.render_height;
    p.Set("MV.Scale.X",c.motion_vector_scale_x*sx);p.Set("MV.Scale.Y",c.motion_vector_scale_y*sy);
    p.Set("Jitter.Offset.X",c.jitter_x*sx);p.Set("Jitter.Offset.Y",c.jitter_y*sy);
    p.Set("Reset",reset || !history.valid ? 1U:0U);
    VulkanNgxScope private_call;
    result=callbacks.evaluate(cmd,history.feature->handle,&p,nullptr);
    history.valid=ngx_succeeded(result);
    if(history.valid)vulkan_barrier(a,cmd,slot.output.ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    return history.valid;
}
}
