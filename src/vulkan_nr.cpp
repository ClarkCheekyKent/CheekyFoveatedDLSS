#include "vulkan_nr.hpp"
#include "vulkan_shaders.hpp"
#include "nr_codec_shader.hpp"
#include "nr_guide_shader.hpp"
#include "nr_parameters.hpp"
#include <algorithm>
#include <cstring>

namespace cheeky::foveated_dlss {
bool VulkanNrPipelines::initialize(const VulkanDeviceApi& a,VkFormat format,unsigned& index) {
    if(format==VK_FORMAT_R16G16B16A16_SFLOAT)index=0;
    else if(format==VK_FORMAT_R32G32B32A32_SFLOAT)index=1;
    else if(format==VK_FORMAT_R8G8B8A8_UNORM)index=2;
    else return false;
    using Code=std::span<const std::uint32_t>;
    const Code copies[]={vulkan_shaders::nr_copy16,vulkan_shaders::nr_copy32,vulkan_shaders::nr_copy8};
    const Code encoders[]={vulkan_shaders::nr_encode16,vulkan_shaders::nr_encode32,vulkan_shaders::nr_encode8};
    const Code decoders[]={vulkan_shaders::nr_decode16,vulkan_shaders::nr_decode32,vulkan_shaders::nr_decode8};
    const Code borders[]={vulkan_shaders::nr_border16,vulkan_shaders::nr_border32,vulkan_shaders::nr_border8};
    return guides.create(a,vulkan_shaders::nr_guides,2,"main",2) &&
        copy[index].create(a,copies[index],1,"Main") && encode[index].create(a,encoders[index],3,"EncodeMain",2) &&
        decode[index].create(a,decoders[index],3,"DecodeMain",2) && border[index].create(a,borders[index],3,"BorderMain",2);
}
void VulkanNrPipelines::destroy(const VulkanDeviceApi& a) {
    guides.destroy(a);for(auto* group:{&copy,&encode,&decode,&border})for(auto& p:*group)p.destroy(a);
}
void VulkanNrSlot::destroy(const VulkanDeviceApi& a) {
    feature.reset();for(auto* image:{&processed,&original,&proxy,&neural,&motion,&depth})image->destroy(a);
    for(auto* dispatch:{&copy_dispatch,&encode_dispatch,&decode_dispatch,&guide_dispatch,&border_dispatch})dispatch->destroy(a);
}
namespace {
void nr_dimensions(NgxParameters& p,unsigned w,unsigned h,const Settings& settings,unsigned flags) {
    for(const auto* key:{"Width","OutWidth","DLSSNR.Width","DLSSNR.InputWidth","DLSSNR.OutputWidth","DLSSNR.Output.Width"})p.Set(key,w);
    for(const auto* key:{"Height","OutHeight","DLSSNR.Height","DLSSNR.InputHeight","DLSSNR.OutputHeight","DLSSNR.Output.Height"})p.Set(key,h);
    p.Set("DLSSNR.Enabled",1U);p.Set("DLSSNR.ScalingRatio",1.0F);p.Set("DLSSNR.Scale",1.0F);p.Set("DLSSNR.Upscaling",0U);
    p.Set("PerfQualityValue",0U);p.Set("DLSS.Feature.Create.Flags",flags);p.Set("CreationNodeMask",1U);p.Set("VisibilityNodeMask",1U);
    p.Set("DLSSNRComputeScalingRatioCallback",reinterpret_cast<void*>(&neural_scaling_ratio_callback));
    set_model_tuning(&p,settings);
}
}
bool vulkan_nr(const VulkanDeviceApi& a,VkCommandBuffer cmd,const DlssFrameContract& c,const Settings& settings,
    const CropGeometry* crop,const FoveationCenter* center,VulkanNgxResource& color,VulkanNgxResource& depth,VulkanNgxResource& motion,
    VulkanNrPipelines& pipelines,VulkanNrSlot& slot,VulkanNrHistory& history,const VulkanNgxCallbacks& callbacks,NgxResult& result) {
    slot.produced=false;
    const bool before=settings.nr_processing_order==NrProcessingOrder::before_upscaling;
    const auto size=dlss_nr_processing_resolution(settings.nr_processing_order,c.render_width,c.render_height,c.output_width,c.output_height);
    const auto region=calculate_region(settings,size.width,size.height,crop,c.render_width,c.render_height,center);
    const unsigned w=scaled_extent(region.width,settings.nr_working_scale),h=scaled_extent(region.height,settings.nr_working_scale);
    const unsigned color_x=before?c.color_base_x:c.output_base_x,color_y=before?c.color_base_y:c.output_base_y;
    const auto format=color.resource.image.format;
    unsigned index{};
    if(!pipelines.initialize(a,format,index) || !slot.original.create(a,region.width,region.height,format) ||
        !slot.proxy.create(a,w,h,VK_FORMAT_R16G16B16A16_SFLOAT) || !slot.neural.create(a,w,h,VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !slot.motion.create(a,w,h,VK_FORMAT_R32G32_SFLOAT) || !slot.depth.create(a,w,h,VK_FORMAT_R32_SFLOAT) ||
        !slot.encode_dispatch.create(a,pipelines.encode[index],sizeof(CodecConstants)) ||
        !slot.decode_dispatch.create(a,pipelines.decode[index],sizeof(CodecConstants)) ||
        !slot.border_dispatch.create(a,pipelines.border[index],sizeof(CodecConstants)) ||
        !slot.guide_dispatch.create(a,pipelines.guides,sizeof(NrGuideConstants))) {history.valid=false;return false;}
    if(before && (!slot.processed.create(a,size.width,size.height,format) || !slot.copy_dispatch.create(a,pipelines.copy[index],24)))return false;
    const VulkanFeatureKey key{w,h,w,h,c.create_flags,0U,settings.nr_preset};
    auto signature=nr_settings_signature(settings,region);
    // Guide domains and the temporal color domain also participate in reset.
    for(unsigned v:{c.render_width,c.render_height,c.output_width,c.output_height,c.create_flags,c.mv_base_x,c.mv_base_y,
            c.depth_base_x,c.depth_base_y,color_x,color_y}) {signature^=v;signature*=1099511628211ULL;}
    const auto generation=requested_nr_reset_generation.load(std::memory_order_relaxed);
    const bool changed=!history.feature || !(history.feature->key==key) || history.signature!=signature || history.generation!=generation;
    bool ready=callbacks.evaluate && callbacks.release && (callbacks.create || callbacks.create1);
    if(changed) {
        history.valid=false;history.failed=false;
        auto feature=std::make_shared<VulkanFeature>();feature->key=key;
        slot.feature=feature;history.feature=feature;history.signature=signature;history.generation=generation;
        feature->parameters=&feature->local_parameters;
        if(callbacks.allocate_parameters) {
            feature->parameters=nullptr;feature->destroy_parameters=callbacks.destroy_parameters;
            result=callbacks.allocate_parameters(&feature->parameters);
            ready=ready && ngx_succeeded(result) && feature->parameters;
        }
        if(ready) {
            nr_dimensions(*feature->parameters,w,h,settings,c.create_flags);
            result=vulkan_create_feature(a,cmd,18,*feature->parameters,*feature,callbacks);
        }
        history.failed=!ready || !ngx_succeeded(result) || !feature->handle;
    }
    slot.feature=history.feature;
    ready=ready && !history.failed && history.feature->handle;
    if(!ready && !settings.nr_alignment_border_enabled)return false;
    if(before) {
        vulkan_prepare_image(a,cmd,slot.processed);
        const unsigned constants[]={color_x,color_y,size.width,size.height,size.width,size.height};
        slot.copy_dispatch.record(a,cmd,pipelines.copy[index],std::span(&color.resource.image.view,1),slot.processed.ngx.resource.image.view,
            constants,sizeof(constants),(size.width+15)/16,(size.height+15)/16);
        vulkan_barrier(a,cmd,slot.processed.ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        slot.produced=true;
    }
    auto& target=before?slot.processed.ngx:color;
    CodecConstants codec{{region.width,region.height},{region.width,region.height},{color_x+region.base_x,color_y+region.base_y},{w,h},
        settings.nr_paper_white_scale,settings.nr_hdr_transfer_strength,settings.nr_color_strength,(c.create_flags&1U)?1U:0U,
        {0,0},{region.width,region.height},region.shape_width,region.shape_height,region.roundness,region.transition,
        settings.nr_alignment_border_enabled?1U:0U,region.mask.count,{},{}};
    std::memcpy(codec.mask_bounds,region.mask.bounds,sizeof(codec.mask_bounds));
    // All descriptor bindings remain valid even when an entry point doesn't use
    // them; this also keeps the shared codec layout identical on both APIs.
    for(auto* image:{&slot.original,&slot.proxy,&slot.neural,&slot.motion,&slot.depth})vulkan_prepare_image(a,cmd,*image);
    bool success{};
    if(ready) {
        const float scale_x=dlss_nr_ngx_motion_uv_scale(c.motion_vector_scale_x,c.render_width)*settings.nr_motion_scale_x_multiplier;
        const float scale_y=dlss_nr_ngx_motion_uv_scale(c.motion_vector_scale_y,c.render_height)*settings.nr_motion_scale_y_multiplier;
        const DlssNrHistory geometry{region.base_x,region.base_y,region.width,region.height,c.output_width,c.output_height,w,h,
            scale_x*size.width,scale_y*size.height};
        float ox{},oy{};bool reset=c.reset || !history.valid || !dlss_nr_motion_offset(history.geometry,geometry,ox,oy);
        const float jitter_x=c.jitter_x/c.render_width,jitter_y=c.jitter_y/c.render_height;
        if(reset)ox=oy=0;
        ox+=dlss_nr_jitter_delta(history.jitter_x,jitter_x,before,(c.create_flags&8U)!=0,reset,settings.nr_motion_scale_x_multiplier)*size.width/region.width;
        oy+=dlss_nr_jitter_delta(history.jitter_y,jitter_y,before,(c.create_flags&8U)!=0,reset,settings.nr_motion_scale_y_multiplier)*size.height/region.height;
        const NrGuideConstants guides{{w,h},{region.base_x,region.base_y},{region.width,region.height},{size.width,size.height},
            {c.motion_vectors_low_res?c.render_width:c.output_width,c.motion_vectors_low_res?c.render_height:c.output_height},
            {c.render_width,c.render_height},{float(c.mv_base_x),float(c.mv_base_y)},{float(c.depth_base_x),float(c.depth_base_y)},
            {scale_x*size.width/region.width,scale_y*size.height/region.height},{ox,oy}};
        const VkImageView guide_inputs[]={motion.resource.image.view,depth.resource.image.view};
        const VkImageView guide_outputs[]={slot.motion.ngx.resource.image.view,slot.depth.ngx.resource.image.view};
        slot.guide_dispatch.record(a,cmd,pipelines.guides,guide_inputs,guide_outputs,&guides,sizeof(guides),(w+7)/8,(h+7)/8);
        const VkImageView encode_inputs[]={color.resource.image.view,color.resource.image.view,color.resource.image.view};
        const VkImageView encode_outputs[]={slot.original.ngx.resource.image.view,slot.proxy.ngx.resource.image.view};
        slot.encode_dispatch.record(a,cmd,pipelines.encode[index],encode_inputs,encode_outputs,&codec,sizeof(codec),
            (std::max(region.width,w)+15)/16,(std::max(region.height,h)+15)/16);
        for(auto* image:{&slot.original,&slot.proxy,&slot.motion,&slot.depth})vulkan_barrier(a,cmd,image->ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        auto& p=*history.feature->parameters;nr_dimensions(p,w,h,settings,c.create_flags);
        p.Set("DLSSNR.Color",static_cast<void*>(&slot.proxy.ngx));p.Set("DLSSNR.Output",static_cast<void*>(&slot.neural.ngx));
        p.Set("DLSSNR.MVec",static_cast<void*>(&slot.motion.ngx));p.Set("DLSSNR.Depth",static_cast<void*>(&slot.depth.ngx));
        for(const auto* name:{"DLSSNR.ColorSubrectBaseX","DLSSNR.ColorSubrectBaseY","DLSSNR.OutputSubrectBaseX","DLSSNR.OutputSubrectBaseY",
            "DLSSNR.MVecSubrectBaseX","DLSSNR.MVecSubrectBaseY","DLSSNR.DepthSubrectBaseX","DLSSNR.DepthSubrectBaseY"})p.Set(name,0U);
        for(const auto* name:{"DLSSNR.ColorSubrectWidth","DLSSNR.OutputSubrectWidth","DLSSNR.MVecSubrectWidth","DLSSNR.DepthSubrectWidth"})p.Set(name,w);
        for(const auto* name:{"DLSSNR.ColorSubrectHeight","DLSSNR.OutputSubrectHeight","DLSSNR.MVecSubrectHeight","DLSSNR.DepthSubrectHeight"})p.Set(name,h);
        p.Set("DLSSNR.MVecScaleX",float(w));p.Set("DLSSNR.MVecScaleY",float(h));p.Set("DLSSNR.Reset",reset?1U:0U);
        p.Set("DLSSNR.DepthInverted",settings.nr_depth_convention==1?0U:settings.nr_depth_convention==2?1U:c.depth_inverted?1U:0U);
        VulkanNgxScope private_call;
        result=callbacks.evaluate(cmd,history.feature->handle,&p,nullptr);
        success=ngx_succeeded(result);history.valid=success;
        if(success) {
            vulkan_barrier(a,cmd,slot.neural.ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
            codec.source_base[0]=(before?0U:color_x)+region.base_x;codec.source_base[1]=(before?0U:color_y)+region.base_y;
            const VkImageView inputs[]={slot.original.ngx.resource.image.view,slot.proxy.ngx.resource.image.view,slot.neural.ngx.resource.image.view};
            const VkImageView outputs[]={target.resource.image.view,slot.proxy.ngx.resource.image.view};
            slot.decode_dispatch.record(a,cmd,pipelines.decode[index],inputs,outputs,&codec,sizeof(codec),(region.width+15)/16,(region.height+15)/16);
            history.geometry=geometry;history.jitter_x=jitter_x;history.jitter_y=jitter_y;
        }
    }
    if(!success && settings.nr_alignment_border_enabled) {
        codec.source_base[0]=(before?0U:color_x)+region.base_x;codec.source_base[1]=(before?0U:color_y)+region.base_y;
        const VkImageView inputs[]={slot.original.ngx.resource.image.view,slot.proxy.ngx.resource.image.view,slot.neural.ngx.resource.image.view};
        const VkImageView outputs[]={target.resource.image.view,slot.proxy.ngx.resource.image.view};
        slot.border_dispatch.record(a,cmd,pipelines.border[index],inputs,outputs,&codec,sizeof(codec),(region.width+15)/16,(region.height+15)/16);
    }
    vulkan_barrier(a,cmd,target.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    return success;
}
}
