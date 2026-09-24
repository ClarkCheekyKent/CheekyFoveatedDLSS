#include "vulkan_gpu.hpp"
#include <array>
#include <cstring>

namespace cheeky::foveated_dlss {
namespace {
bool allocate(const VulkanDeviceApi& a, VkMemoryRequirements r, VkMemoryPropertyFlags flags, VkDeviceMemory& memory) {
    for (unsigned i=0;i<a.memory.memoryTypeCount;++i) {
        if ((r.memoryTypeBits&(1U<<i)) && (a.memory.memoryTypes[i].propertyFlags&flags)==flags) {
            VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            info.allocationSize=r.size; info.memoryTypeIndex=i;
            return a.AllocateMemory(a.device,&info,nullptr,&memory)==VK_SUCCESS;
        }
    }
    return false;
}
}
bool VulkanImage::create(const VulkanDeviceApi& a,unsigned w,unsigned h,VkFormat format) {
    if (ngx.resource.image.image && ngx.resource.image.width==w && ngx.resource.image.height==h && ngx.resource.image.format==format) return true;
    destroy(a);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType=VK_IMAGE_TYPE_2D; info.format=format; info.extent={w,h,1};
    info.mipLevels=1; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_OPTIMAL;
    info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto& image=ngx.resource.image;
    image.width=w; image.height=h; image.format=format;
    image.range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; ngx.read_write=true;
    if (a.CreateImage(a.device,&info,nullptr,&image.image)!=VK_SUCCESS) return false;
    VkMemoryRequirements requirements{}; a.GetImageMemoryRequirements(a.device,image.image,&requirements);
    if (!allocate(a,requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,memory) ||
        a.BindImageMemory(a.device,image.image,memory,0)!=VK_SUCCESS) { destroy(a); return false; }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image=image.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=format; view.subresourceRange=image.range;
    if (a.CreateImageView(a.device,&view,nullptr,&image.view)!=VK_SUCCESS) { destroy(a); return false; }
    view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (a.CreateImageView(a.device,&view,nullptr,&array_view)!=VK_SUCCESS) { destroy(a); return false; }
    return true;
}
void VulkanImage::destroy(const VulkanDeviceApi& a) noexcept {
    if (array_view) a.DestroyImageView(a.device,array_view,nullptr);
    if (ngx.resource.image.view) a.DestroyImageView(a.device,ngx.resource.image.view,nullptr);
    if (ngx.resource.image.image) a.DestroyImage(a.device,ngx.resource.image.image,nullptr);
    if (memory) a.FreeMemory(a.device,memory,nullptr);
    *this={};
}
bool VulkanBuffer::create(const VulkanDeviceApi& a,VkDeviceSize size,VkBufferUsageFlags usage) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size=size; info.usage=usage;
    if (a.CreateBuffer(a.device,&info,nullptr,&buffer)!=VK_SUCCESS) return false;
    VkMemoryRequirements r{}; a.GetBufferMemoryRequirements(a.device,buffer,&r);
    if (!allocate(a,r,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,memory) ||
        a.BindBufferMemory(a.device,buffer,memory,0)!=VK_SUCCESS ||
        a.MapMemory(a.device,memory,0,size,0,&mapped)!=VK_SUCCESS) { destroy(a); return false; }
    return true;
}
void VulkanBuffer::destroy(const VulkanDeviceApi& a) noexcept {
    if (mapped) a.UnmapMemory(a.device,memory);
    if (buffer) a.DestroyBuffer(a.device,buffer,nullptr);
    if (memory) a.FreeMemory(a.device,memory,nullptr);
    *this={};
}
bool VulkanCompute::create(const VulkanDeviceApi& a,std::span<const std::uint32_t> code,unsigned count,const char* entry,unsigned output_count) {
    if(pipeline)return true;
    inputs=count;outputs=output_count;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for(unsigned i=0;i<count;++i) bindings.push_back({i,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    for(unsigned i=0;i<outputs;++i)bindings.push_back({count+i,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    bindings.push_back({count+outputs,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    VkDescriptorSetLayoutCreateInfo desc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    desc.bindingCount=static_cast<unsigned>(bindings.size()); desc.pBindings=bindings.data();
    if(a.CreateDescriptorSetLayout(a.device,&desc,nullptr,&descriptors)!=VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&descriptors;
    if(a.CreatePipelineLayout(a.device,&pl,nullptr,&layout)!=VK_SUCCESS) { destroy(a); return false; }
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize=code.size_bytes(); si.pCode=code.data();
    VkShaderModule shader{};
    if(a.CreateShaderModule(a.device,&si,nullptr,&shader)!=VK_SUCCESS) { destroy(a); return false; }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.layout=layout; ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module=shader; ci.stage.pName=entry;
    const auto result=a.CreateComputePipelines(a.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline);
    a.DestroyShaderModule(a.device,shader,nullptr);
    if(result!=VK_SUCCESS) { destroy(a); return false; }
    return true;
}
void VulkanCompute::destroy(const VulkanDeviceApi& a) noexcept {
    if(pipeline) a.DestroyPipeline(a.device,pipeline,nullptr);
    if(layout) a.DestroyPipelineLayout(a.device,layout,nullptr);
    if(descriptors) a.DestroyDescriptorSetLayout(a.device,descriptors,nullptr);
    *this={};
}
bool VulkanDispatch::create(const VulkanDeviceApi& a,const VulkanCompute& kernel,unsigned bytes) {
    if(pool) return true;
    const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,kernel.inputs},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,kernel.outputs},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1}};
    VkDescriptorPoolCreateInfo p{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; p.maxSets=1;p.poolSizeCount=kernel.inputs?3U:2U;p.pPoolSizes=sizes+(kernel.inputs?0:1);
    if(a.CreateDescriptorPool(a.device,&p,nullptr,&pool)!=VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo d{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};d.descriptorPool=pool;d.descriptorSetCount=1;d.pSetLayouts=&kernel.descriptors;
    if(a.AllocateDescriptorSets(a.device,&d,&descriptors)!=VK_SUCCESS || !constants.create(a,bytes)) { destroy(a);return false; }
    return true;
}
void VulkanDispatch::destroy(const VulkanDeviceApi& a) noexcept {
    constants.destroy(a);
    if(pool) a.DestroyDescriptorPool(a.device,pool,nullptr);
    *this={};
}
void VulkanDispatch::record(const VulkanDeviceApi& a,VkCommandBuffer cmd,const VulkanCompute& kernel,
    std::span<const VkImageView> inputs,VkImageView output,const void* data,unsigned bytes,unsigned x,unsigned y) {
    record(a,cmd,kernel,inputs,std::span(&output,1),data,bytes,x,y);
}
void VulkanDispatch::record(const VulkanDeviceApi& a,VkCommandBuffer cmd,const VulkanCompute& kernel,
    std::span<const VkImageView> inputs,std::span<const VkImageView> outputs,const void* data,unsigned bytes,unsigned x,unsigned y) {
    std::memcpy(constants.mapped,data,bytes);
    std::array<VkDescriptorImageInfo,8> images{};
    std::array<VkWriteDescriptorSet,9> writes{};
    for(unsigned i=0;i<inputs.size()+outputs.size();++i) {
        images[i].imageView=i<inputs.size()?inputs[i]:outputs[i-inputs.size()]; images[i].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=descriptors;writes[i].dstBinding=i;writes[i].descriptorCount=1;
        writes[i].descriptorType=i<inputs.size()?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[i].pImageInfo=&images[i];
    }
    VkDescriptorBufferInfo buffer{constants.buffer,0,bytes};
    auto& uniform=writes[inputs.size()+outputs.size()]; uniform={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    uniform.dstSet=descriptors;uniform.dstBinding=static_cast<unsigned>(inputs.size()+outputs.size());uniform.descriptorCount=1;
    uniform.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;uniform.pBufferInfo=&buffer;
    a.UpdateDescriptorSets(a.device,static_cast<unsigned>(inputs.size()+outputs.size()+1),writes.data(),0,nullptr);
    a.CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,kernel.pipeline);
    a.CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,kernel.layout,0,1,&descriptors,0,nullptr);
    a.CmdDispatch(cmd,x,y,1);
}
void vulkan_barrier(const VulkanDeviceApi& a,VkCommandBuffer cmd,const VulkanImageInfo& image,
    VkImageLayout before,VkImageLayout after,VkAccessFlags source,VkAccessFlags destination) noexcept {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image=image.image;b.subresourceRange=image.range;
    b.oldLayout=before;b.newLayout=after;b.srcAccessMask=before==VK_IMAGE_LAYOUT_UNDEFINED?0:source;b.dstAccessMask=destination;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    a.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
void vulkan_prepare_image(const VulkanDeviceApi& a,VkCommandBuffer cmd,VulkanImage& image) noexcept {
    // Scratch images are completely overwritten on every use. Discarding the
    // previous contents also handles recorded-but-never-submitted commands:
    // their planned transition must not become the next recording's oldLayout.
    vulkan_barrier(a,cmd,image.ngx.resource.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    image.initialized=true;
}
}
