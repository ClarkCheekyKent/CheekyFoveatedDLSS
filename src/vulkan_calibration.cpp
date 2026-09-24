#include "vulkan_calibration.hpp"
#include "vulkan_observer.hpp"
#include "vulkan_gpu.hpp"
#include "vulkan_calibration_shaders.hpp"
#include "eye_calibration_source.hpp"
#include "eye_calibration.hpp"
#include <mutex>
#include <vector>

namespace cheeky::foveated_dlss {
namespace {
constexpr unsigned proof_width = calibration_marker_size, proof_height = proof_width * 2;
constexpr unsigned proof_bytes = proof_width * proof_height * 16;
struct Constants {
    std::array<std::array<unsigned,4>,16> points{};
    unsigned count{}, phase{}, candidate{}, reserved{};
};
struct Kernels {
    std::shared_ptr<VulkanDeviceApi> api;
    VulkanCompute f16, f32, u8;
    VulkanCompute* get(VkFormat format) {
        VulkanCompute* k{}; std::span<const std::uint32_t> code;
        if (format == VK_FORMAT_R16G16B16A16_SFLOAT) { k=&f16; code=vulkan_shaders::calibration16; }
        if (format == VK_FORMAT_R32G32B32A32_SFLOAT) { k=&f32; code=vulkan_shaders::calibration32; }
        if (format == VK_FORMAT_R8G8B8A8_UNORM) { k=&u8; code=vulkan_shaders::calibration8; }
        return k && k->create(*api,code,0,"main",2) ? k : nullptr;
    }
    void destroy() { f16.destroy(*api); f32.destroy(*api); u8.destroy(*api); }
};
struct Record final : CalibrationSourceProof {
    std::mutex mutex;
    std::shared_ptr<Kernels> kernels;
    VkCommandBuffer command{};
    VkImageView output_view{};
    VkEvent event{};
    VulkanImage proof;
    VulkanBuffer readback;
    std::array<VulkanDispatch,2> dispatch;
    unsigned candidate{}, code{};
    CalibrationSourceResult result;
    bool initialized{};
    CalibrationSourceResult read() noexcept {
        if (result.ready) return result;
        if (!event) return {true,false,{}};
        const auto& a=*kernels->api;
        const auto status=a.GetEventStatus(a.device,event);
        if (status==VK_EVENT_RESET) return {};
        result.ready=true;
        if (status!=VK_EVENT_SET) return result;
        for (unsigned i=0;i<2;++i)
            result.scores[i]=calibration_pattern_score(static_cast<const unsigned char*>(readback.mapped)+i*proof_bytes/2,
                proof_width*16,proof_width,proof_width,DXGI_FORMAT_R32G32B32A32_FLOAT,candidate,false,code,1);
        result.valid=true;
        return result;
    }
    CalibrationSourceResult poll() noexcept override {
        // A command buffer may be resubmitted without resetting its event.
        // Only its legal reset/free boundary guarantees no replay is writing
        // these bytes. retire() snapshots there; polling uses CPU data only.
        std::lock_guard lock(mutex); return result;
    }
    void unbind() {
        const auto& a=*kernels->api;
        if (output_view) a.DestroyImageView(a.device,output_view,nullptr);
        output_view={}; command={};
    }
    // Called only at an application's legal reset/free/device-destroy boundary.
    // Unsubmitted recordings are rejected; completed proof becomes CPU-only.
    void retire() {
        std::lock_guard lock(mutex);
        if (!read().ready) result={true,false,{}};
        unbind();
    }
    void destroy() {
        std::lock_guard lock(mutex);
        if (!result.ready) result={true,false,{}};
        const auto& a=*kernels->api;
        unbind();
        for (auto& d:dispatch) d.destroy(a);
        readback.destroy(a); proof.destroy(a);
        if (event) a.DestroyEvent(a.device,event,nullptr);
        event={}; initialized=false;
    }
};
std::mutex registry_mutex;
std::vector<std::shared_ptr<Kernels>> devices;
std::vector<std::shared_ptr<Record>> recordings;
struct Context {
    std::shared_ptr<VulkanDeviceApi> api;
    VkCommandBuffer command;
    VulkanImageInfo output;
    VkImageLayout layout;
};
CalibrationSourcePtr record_source(void* opaque,const CalibrationMarkerPoints& points,unsigned candidate) {
    auto& context=*static_cast<Context*>(opaque);
    const auto& a=*context.api;
    if (!points.count || points.count>16) return {};
    for (unsigned i=0;i<points.count;++i) {
        const auto& p=points.points[i]; const unsigned pad=p.locator?16:0;
        if (p.x<pad || p.y<pad || std::uint64_t(p.x)+40+pad>context.output.width ||
            std::uint64_t(p.y)+40+pad>context.output.height) return {};
    }
    auto copy=reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(a.gdpa(a.device,"vkCmdCopyImageToBuffer"));
    if (!copy) return {};
    std::lock_guard registry_lock(registry_mutex);
    std::shared_ptr<Kernels> kernels;
    for (const auto& k:devices) if (k->api->device==a.device) { kernels=k; break; }
    if (!kernels) { kernels=std::make_shared<Kernels>(); kernels->api=context.api; devices.push_back(kernels); }
    auto* kernel=kernels->get(context.output.format);
    if (!kernel) return {};
    std::shared_ptr<Record> r;
    for (const auto& entry:recordings)
        if (entry->kernels==kernels && !entry->command && entry.use_count()==1) { r=entry; break; }
    if (!r) {
        // Bound recorded-but-never-reset command buffers; never overwrite an
        // executable recording just because its first submission has finished.
        if (recordings.size()>=128) return {};
        r=std::make_shared<Record>(); r->kernels=kernels; recordings.push_back(r);
    }
    std::lock_guard record_lock(r->mutex);
    if (!r->initialized) {
        VkEventCreateInfo event{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        if (!r->proof.create(a,proof_width,proof_height,VK_FORMAT_R32G32B32A32_SFLOAT) ||
            (!r->readback.buffer && !r->readback.create(a,proof_bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT)) ||
            (!r->event && a.CreateEvent(a.device,&event,nullptr,&r->event)!=VK_SUCCESS)) return {};
        r->initialized=true;
    }
    for (auto& d:r->dispatch) if (!d.create(a,*kernel,sizeof(Constants))) return {};
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image=context.output.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format=context.output.format;view.subresourceRange=context.output.range;
    if (a.CreateImageView(a.device,&view,nullptr,&r->output_view)!=VK_SUCCESS) return {};
    if (a.ResetEvent(a.device,r->event)!=VK_SUCCESS) {r->unbind();return {};}
    r->result={}; r->candidate=candidate; r->code=points.points[0].code;
    r->command=context.command;
    const auto cmd=context.command;
    Constants constants; constants.count=points.count;constants.candidate=candidate;
    for (unsigned i=0;i<points.count;++i) {
        const auto& p=points.points[i];constants.points[i]={p.x,p.y,p.code,p.locator?1U:0U};
    }
    vulkan_barrier(a,cmd,context.output,context.layout,VK_IMAGE_LAYOUT_GENERAL);
    vulkan_prepare_image(a,cmd,r->proof);
    const VkImageView outputs[]={r->output_view,r->proof.array_view};
    r->dispatch[0].record(a,cmd,*kernel,{},outputs,&constants,sizeof(constants),points.count*9,9);
    // Read the actual stored marker after its writes become visible. Never
    // manufacture a successful source proof from the intended pattern.
    vulkan_barrier(a,cmd,context.output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    vulkan_barrier(a,cmd,r->proof.ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    constants.phase=1;
    r->dispatch[1].record(a,cmd,*kernel,{},outputs,&constants,sizeof(constants),points.count*9,9);
    vulkan_barrier(a,cmd,r->proof.ngx.resource.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    region.imageExtent={proof_width,proof_height,1};
    copy(cmd,r->proof.ngx.resource.image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r->readback.buffer,1,&region);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    a.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    vulkan_barrier(a,cmd,context.output,VK_IMAGE_LAYOUT_GENERAL,context.layout,
        VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    a.CmdSetEvent(cmd,r->event,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    return r;
}
}
void vulkan_calibration_stamp(VkCommandBuffer cmd,const NgxParameters* parameters,const DlssFrameContract& frame) noexcept {
    if (!eye_calibration_enabled() || !parameters) return;
    try {
        auto api=vulkan_command_device(cmd);if (!api) return;
        void* value{};if (!ngx_succeeded(parameters->Get("Output",&value)) || !value) return;
        const auto& resource=*static_cast<const VulkanNgxResource*>(value);
        const auto& image=resource.resource.image;
        if (resource.type!=0 || !resource.read_write || !image.image ||
            image.range.levelCount!=1 || image.range.layerCount!=1 ||
            image.range.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT ||
            std::uint64_t(frame.output_base_x)+frame.output_width>image.width ||
            std::uint64_t(frame.output_base_y)+frame.output_height>image.height) return;
        VkImageLayout layout{};
        if (!vulkan_image_layout(cmd,image,layout) || layout==VK_IMAGE_LAYOUT_UNDEFINED) return;
        Context context{api,cmd,image,layout};
        eye_calibration_external_source(frame.view_id,frame.output_base_x,frame.output_base_y,
            frame.output_width,frame.output_height,13,&context,record_source);
    } catch (...) { /* Calibration must not escape the application's NGX hook. */ }
}
void vulkan_calibration_forget_command(VkCommandBuffer cmd) noexcept {
    std::lock_guard lock(registry_mutex);
    for (const auto& r:recordings) if (r->command==cmd) r->retire();
}
void vulkan_calibration_release_device(VkDevice device) noexcept {
    std::lock_guard lock(registry_mutex);
    for (auto it=recordings.begin();it!=recordings.end();) {
        if ((*it)->kernels->api->device==device) {(*it)->destroy();it=recordings.erase(it);} else ++it;
    }
    for (auto it=devices.begin();it!=devices.end();) {
        if ((*it)->api->device==device) {(*it)->destroy();it=devices.erase(it);} else ++it;
    }
}
}
