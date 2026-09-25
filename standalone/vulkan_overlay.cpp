#include "vulkan_overlay.hpp"
#include "overlay_ui.hpp"
#include "overlay_input.hpp"
#include "../src/vulkan_api.hpp"
#include "../src/vulkan_shaders.hpp"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_win32.h>
#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace cheeky::standalone {
namespace {
using namespace cheeky::foveated_dlss;
struct Frame {VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};};
struct Target {VkImageView view{};VkFramebuffer framebuffer{};VkSemaphore finished{};};
struct Renderer : OverlayUiState {
    VulkanDeviceApi api;
    VkSwapchainKHR swapchain{};VkQueue queue{};unsigned family{},serial{};
    VkRenderPass pass{};VkExtent2D extent{};VkFormat format{};VkColorSpaceKHR color_space{};
    std::vector<VkImage> images;
    std::vector<Frame> frames;std::vector<Target> targets;
    HWND window{};InputState* input{};ImGuiContext* context{};
    bool win32_ready{},ready{},poisoned{};
    ULONGLONG last_present{};
};
std::mutex mutex;
std::unique_ptr<Renderer> renderer;
struct Context {
    ImGuiContext* previous{ImGui::GetCurrentContext()};
    explicit Context(ImGuiContext* value){ImGui::SetCurrentContext(value);}
    ~Context(){ImGui::SetCurrentContext(previous);}
};
template<class T>T proc(const Renderer& r,const char* name){return reinterpret_cast<T>(r.api.gdpa(r.api.device,name));}
void check(VkResult result){if(result<0)throw result;}
int color_mode(VkFormat f,VkColorSpaceKHR s) {
    if(s==VK_COLOR_SPACE_HDR10_ST2084_EXT)return 2;
    if(s==VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)return 1;
    if(s!=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)return -1;
    return f==VK_FORMAT_R8G8B8A8_SRGB || f==VK_FORMAT_B8G8R8A8_SRGB?3:0;
}
void destroy(Renderer& r) {
    if(r.input){r.input->enabled=false;r.input->open=false;restore_cursor(*r.input,true);}
    // Destruction/recreation is a lifetime boundary. No waits occur on steady
    // frames; a busy overlay frame is simply skipped.
    if(r.queue)proc<PFN_vkQueueWaitIdle>(r,"vkQueueWaitIdle")(r.queue);
    if(r.context) {
        Context scope(r.context);
        if(ImGui::GetIO().BackendRendererUserData)ImGui_ImplVulkan_Shutdown();
        if(r.win32_ready)ImGui_ImplWin32_Shutdown();
    }
    if(r.context){auto* previous=ImGui::GetCurrentContext();ImGui::DestroyContext(r.context);if(previous!=r.context)ImGui::SetCurrentContext(previous);r.context=nullptr;}
    for(auto& t:r.targets) {
        if(t.framebuffer)proc<PFN_vkDestroyFramebuffer>(r,"vkDestroyFramebuffer")(r.api.device,t.framebuffer,nullptr);
        if(t.view)r.api.DestroyImageView(r.api.device,t.view,nullptr);
        if(t.finished)proc<PFN_vkDestroySemaphore>(r,"vkDestroySemaphore")(r.api.device,t.finished,nullptr);
    }
    for(auto& f:r.frames) {
        if(f.fence)proc<PFN_vkDestroyFence>(r,"vkDestroyFence")(r.api.device,f.fence,nullptr);
        if(f.pool)proc<PFN_vkDestroyCommandPool>(r,"vkDestroyCommandPool")(r.api.device,f.pool,nullptr);
    }
    if(r.pass)proc<PFN_vkDestroyRenderPass>(r,"vkDestroyRenderPass")(r.api.device,r.pass,nullptr);
}
void initialize_gpu(Renderer& r) {
    if(!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1,[](const char* name,void* user) {
        auto& a=*static_cast<VulkanDeviceApi*>(user);
        // Physical/instance commands must use the instance dispatcher.
        const auto device=a.gdpa(a.device,name);return device?device:a.gipa(a.instance,name);
    },&r.api))throw VK_ERROR_INITIALIZATION_FAILED;
    VkAttachmentDescription attachment{};attachment.format=r.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;
    const VkSubpassDependency dependencies[]={
        {VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0},
        {0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0,0}};
    VkRenderPassCreateInfo pi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};pi.attachmentCount=1;pi.pAttachments=&attachment;pi.subpassCount=1;pi.pSubpasses=&subpass;
    pi.dependencyCount=2;pi.pDependencies=dependencies;
    check(proc<PFN_vkCreateRenderPass>(r,"vkCreateRenderPass")(r.api.device,&pi,nullptr,&r.pass));
    r.targets.resize(r.images.size());r.frames.resize(r.images.size());
    for(unsigned i=0;i<r.images.size();++i) {
        auto& t=r.targets[i];auto& f=r.frames[i];
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=r.images[i];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=r.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        check(r.api.CreateImageView(r.api.device,&vi,nullptr,&t.view));
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=r.pass;fi.attachmentCount=1;fi.pAttachments=&t.view;fi.width=r.extent.width;fi.height=r.extent.height;fi.layers=1;
        check(proc<PFN_vkCreateFramebuffer>(r,"vkCreateFramebuffer")(r.api.device,&fi,nullptr,&t.framebuffer));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};check(proc<PFN_vkCreateSemaphore>(r,"vkCreateSemaphore")(r.api.device,&si,nullptr,&t.finished));
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};ci.queueFamilyIndex=r.family;
        check(proc<PFN_vkCreateCommandPool>(r,"vkCreateCommandPool")(r.api.device,&ci,nullptr,&f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=f.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
        check(proc<PFN_vkAllocateCommandBuffers>(r,"vkAllocateCommandBuffers")(r.api.device,&ai,&f.command));
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;
        check(proc<PFN_vkCreateFence>(r,"vkCreateFence")(r.api.device,&fence,nullptr,&f.fence));
    }
    ImGui_ImplVulkan_InitInfo init{};init.ApiVersion=VK_API_VERSION_1_1;init.Instance=r.api.instance;init.PhysicalDevice=r.api.physical;init.Device=r.api.device;
    init.QueueFamily=r.family;init.Queue=r.queue;init.DescriptorPoolSize=64;init.MinImageCount=2;init.ImageCount=static_cast<unsigned>(r.images.size());
    init.PipelineInfoMain.RenderPass=r.pass;init.PipelineInfoMain.MSAASamples=VK_SAMPLE_COUNT_1_BIT;init.CheckVkResultFn=check;
    using Code=std::span<const std::uint32_t>;
    const Code shaders[]={vulkan_shaders::overlay0,vulkan_shaders::overlay1,vulkan_shaders::overlay2,vulkan_shaders::overlay3};
    const auto shader=shaders[color_mode(r.format,r.color_space)];
    init.CustomShaderFragCreateInfo={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};init.CustomShaderFragCreateInfo.codeSize=shader.size_bytes();init.CustomShaderFragCreateInfo.pCode=shader.data();
    if(!ImGui_ImplVulkan_Init(&init))throw VK_ERROR_INITIALIZATION_FAILED;
    r.ready=true;
}
}
VkResult overlay_vulkan_present(const CheekyVulkanPresent& p,const OverlayRuntime& runtime) noexcept {
    const auto forward=[&] {return p.next(p.queue,p.present);};
    std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock())return forward();
    try {
        if(p.size!=sizeof(p) || !p.window || !foreground(p.window) || !IsWindowVisible(p.window) ||
            p.image_count<2 || p.extent.width<160 || p.extent.height<100 || color_mode(p.format,p.color_space)<0)return forward();
        const auto now=GetTickCount64();
        if(renderer && (renderer->swapchain!=p.swapchain || renderer->api.device!=p.device || renderer->queue!=p.queue)) {
            if(now-renderer->last_present<1000)return forward();
            destroy(*renderer);renderer.reset();
        }
        if(!renderer) {
            renderer=std::make_unique<Renderer>();auto& r=*renderer;
            if(!r.api.initialize(p.instance,p.physical,p.device,p.gipa,p.gdpa)) {renderer.reset();return forward();}
            r.swapchain=p.swapchain;r.queue=p.queue;r.family=p.queue_family;r.extent=p.extent;r.format=p.format;r.color_space=p.color_space;r.window=p.window;
            r.images.assign(p.images,p.images+p.image_count);r.context=ImGui::CreateContext();
            Context scope(r.context);auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
            io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NoMouseCursorChange;ImGui::StyleColorsDark();
            r.win32_ready=ImGui_ImplWin32_Init(r.window);r.input=attach_input(r.window);
            if(!r.win32_ready || !r.input) {destroy(r);renderer.reset();return forward();}
        }
        auto& r=*renderer;r.last_present=now;
        if(r.poisoned)return forward();
        r.input->enabled=true;poll_overlay_hotkey(*r.input);Context scope(r.context);
        process_overlay_input(*r.input);
        if(!r.input->open){ImGui::GetIO().ClearInputKeys();ImGui::GetIO().ClearInputMouse();return forward();}
        if(!r.ready)initialize_gpu(r);
        auto& f=r.frames[r.serial%r.frames.size()];
        if(proc<PFN_vkGetFenceStatus>(r,"vkGetFenceStatus")(p.device,f.fence)!=VK_SUCCESS)return forward();
        const auto image=p.present->pImageIndices[p.present_index];if(image>=r.targets.size())return forward();
        auto& target=r.targets[image];
        release_cursor(*r.input);ImGui::GetIO().MouseDrawCursor=true;
        ImGui_ImplVulkan_NewFrame();ImGui_ImplWin32_NewFrame();set_overlay_framebuffer_scale(r.extent.width,r.extent.height);ImGui::NewFrame();
        bool open=r.input->open.load();draw_overlay_ui(r,runtime,*r.input,"Vulkan","Native Vulkan menu",open);
        if(!open){r.input->open=false;restore_cursor(*r.input,true);}
        ImGui::Render();
        check(proc<PFN_vkResetCommandPool>(r,"vkResetCommandPool")(p.device,f.pool,0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(proc<PFN_vkBeginCommandBuffer>(r,"vkBeginCommandBuffer")(f.command,&begin));
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};pass.renderPass=r.pass;pass.framebuffer=target.framebuffer;pass.renderArea.extent=r.extent;
        proc<PFN_vkCmdBeginRenderPass>(r,"vkCmdBeginRenderPass")(f.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),f.command);
        proc<PFN_vkCmdEndRenderPass>(r,"vkCmdEndRenderPass")(f.command);
        check(proc<PFN_vkEndCommandBuffer>(r,"vkEndCommandBuffer")(f.command));
        // One completion semaphore per swapchain image: reacquiring that image
        // proves the preceding presentation consumed its semaphore.
        std::vector<VkPipelineStageFlags> stages(p.present->waitSemaphoreCount,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=p.present->waitSemaphoreCount;submit.pWaitSemaphores=p.present->pWaitSemaphores;submit.pWaitDstStageMask=stages.data();
        submit.commandBufferCount=1;submit.pCommandBuffers=&f.command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&target.finished;
        auto present=*p.present;present.waitSemaphoreCount=1;present.pWaitSemaphores=&target.finished;
        check(proc<PFN_vkResetFences>(r,"vkResetFences")(p.device,1,&f.fence));
        const auto submitted=proc<PFN_vkQueueSubmit>(r,"vkQueueSubmit")(p.queue,1,&submit,f.fence);
        if(submitted!=VK_SUCCESS){r.poisoned=true;return submitted;}
        ++r.serial;
        return p.next(p.queue,&present);
    }catch(...) {
        if(renderer)renderer->poisoned=true;
        OutputDebugStringA("Cheeky Vulkan overlay failed; preserving game presentation.\n");
        return forward();
    }
}
void overlay_vulkan_destroy(VkDevice device,VkSwapchainKHR swapchain) noexcept {
    try {std::lock_guard lock(mutex);if(renderer && renderer->api.device==device && (!swapchain || renderer->swapchain==swapchain)){destroy(*renderer);renderer.reset();}}catch(...){}
}
}
