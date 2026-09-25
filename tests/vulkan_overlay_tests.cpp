#include "vulkan_overlay.hpp"
#include "vulkan_api.hpp"
#include "../third_party/vulkan/include/vulkan/vulkan_win32.h"
#include <vector>
#include <MinHook.h>
#include <cstdio>
#include <stdexcept>
#include <string>
using namespace cheeky::standalone;
using namespace cheeky::foveated_dlss;
extern HWND test_foreground;
namespace {
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
VulkanDeviceApi api;
template<class T>T proc(const char* name){return reinterpret_cast<T>(api.gdpa(api.device,name));}
template<class T>T iproc(const char* name){return reinterpret_cast<T>(api.gipa(api.instance,name));}
unsigned snapshots{};
PFN_vkQueuePresentKHR original_present{};
bool snapshot(char* p,unsigned n){++snapshots;return strcpy_s(p,n,R"({"settings":{"Enabled":true,"Width":0.55,"Height":0.45},"message":"Vulkan overlay test"})")==0;}
bool command(std::uint64_t,const char*){return true;}
VkBuffer readback{};VkDeviceMemory memory{};VkCommandPool pool{};VkCommandBuffer cmd{};
VkSwapchainKHR chain{};std::vector<VkImage> images;VkExtent2D extent{};
std::vector<unsigned char> pixels;
VkResult VKAPI_CALL read_present(VkQueue queue,const VkPresentInfoKHR* info) {
    require(proc<PFN_vkResetCommandPool>("vkResetCommandPool")(api.device,pool,0)==VK_SUCCESS,"reset readback");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    require(proc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cmd,&begin)==VK_SUCCESS,"begin readback");
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=images[info->pImageIndices[0]];b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    api.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};
    proc<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmd,b.image,b.newLayout,readback,1,&copy);
    b.oldLayout=b.newLayout;b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=0;
    api.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&b);
    require(proc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cmd)==VK_SUCCESS,"end readback");
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=info->waitSemaphoreCount;submit.pWaitSemaphores=info->pWaitSemaphores;submit.pWaitDstStageMask=stages.data();submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
    require(proc<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS,"submit readback");
    require(proc<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue)==VK_SUCCESS,"wait readback");
    void* mapped{};require(api.MapMemory(api.device,memory,0,pixels.size(),0,&mapped)==VK_SUCCESS,"map readback");memcpy(pixels.data(),mapped,pixels.size());api.UnmapMemory(api.device,memory);
    auto present=*info;present.waitSemaphoreCount=0;present.pWaitSemaphores=nullptr;
    return original_present(queue,&present);
}
VkResult capture_layer_present(const CheekyVulkanPresent* input) {
    original_present=input->next;
    auto p=*input;p.next=read_present;
    const OverlayRuntime runtime{1,command,snapshot,"Vulkan layer regression"};
    return overlay_vulkan_present(p,runtime);
}
}
int run_vulkan_overlay_tests(bool layer) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);setvbuf(stdout,nullptr,_IONBF,0);
    try {
        if(layer) {
            wchar_t path[32768]{};
            require(GetEnvironmentVariableW(L"CHEEKY_VULKAN_TEST_BOOTSTRAP",path,32768)>0,"drop-in bootstrap path");
            require(LoadLibraryW(path)!=nullptr,"load drop-in bootstrap");
        }
        WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"CheekyVulkanOverlayTests";RegisterClassW(&wc);
        HWND window=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"Vulkan overlay regression",WS_OVERLAPPEDWINDOW,-2000,-2000,820,760,nullptr,nullptr,wc.hInstance,nullptr);
        require(window!=nullptr,"create window");ShowWindow(window,SW_SHOWNOACTIVATE);ShowWindow(window,SW_SHOWNOACTIVATE);test_foreground=window;
        const auto loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);require(loader!=nullptr,"Vulkan loader");
        auto gi=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
        const char* ie[]={"VK_KHR_surface","VK_KHR_win32_surface"};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ii.pApplicationInfo=&app;ii.enabledExtensionCount=2;ii.ppEnabledExtensionNames=ie;
        VkInstance instance{};require(reinterpret_cast<PFN_vkCreateInstance>(gi(nullptr,"vkCreateInstance"))(&ii,nullptr,&instance)==VK_SUCCESS,"create instance");
        api.gipa=gi;api.instance=instance;
        VkWin32SurfaceCreateInfoKHR wi{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};wi.hinstance=wc.hInstance;wi.hwnd=window;VkSurfaceKHR surface{};
        require(iproc<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR")(instance,&wi,nullptr,&surface)==VK_SUCCESS,"surface");
        unsigned count{};iproc<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance,&count,nullptr);std::vector<VkPhysicalDevice> physicals(count);iproc<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(instance,&count,physicals.data());require(count!=0,"physical device");
        const auto physical=physicals[0];iproc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);iproc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties")(physical,&count,families.data());
        unsigned family{};for(;family<count;++family){VkBool32 present{};iproc<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR")(physical,family,surface,&present);if(present && (families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))break;}require(family<count,"graphics/present queue");
        float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
        const char* de[]={"VK_KHR_swapchain"};VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;di.enabledExtensionCount=1;di.ppEnabledExtensionNames=de;
        VkDevice device{};require(iproc<PFN_vkCreateDevice>("vkCreateDevice")(physical,&di,nullptr,&device)==VK_SUCCESS,"device");
        auto gd=iproc<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");require(api.initialize(instance,physical,device,gi,gd),"dispatch");
        VkQueue queue{};proc<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device,family,0,&queue);
        VkSurfaceCapabilitiesKHR caps{};iproc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(physical,surface,&caps);extent=caps.currentExtent;
        require((caps.supportedUsageFlags&(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT))==(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT),"surface readback support");
        iproc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR")(physical,surface,&count,nullptr);std::vector<VkSurfaceFormatKHR> formats(count);iproc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR")(physical,surface,&count,formats.data());
        VkSurfaceFormatKHR format{};for(const auto& f:formats)if(f.format==VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)format=f;require(format.format!=0,"SDR surface format");
        VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};si.surface=surface;si.minImageCount=(std::max)(2U,caps.minImageCount);si.imageFormat=format.format;si.imageColorSpace=format.colorSpace;si.imageExtent=extent;si.imageArrayLayers=1;si.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;si.preTransform=caps.currentTransform;si.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;si.presentMode=VK_PRESENT_MODE_FIFO_KHR;si.clipped=VK_TRUE;
        require(proc<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(device,&si,nullptr,&chain)==VK_SUCCESS,"swapchain");proc<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR")(device,chain,&count,nullptr);images.resize(count);proc<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR")(device,chain,&count,images.data());
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;require(proc<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device,&pi,nullptr,&pool)==VK_SUCCESS,"command pool");VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;require(proc<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device,&ai,&cmd)==VK_SUCCESS,"command buffer");
        pixels.resize(extent.width*extent.height*4);VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=pixels.size();bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;require(api.CreateBuffer(device,&bi,nullptr,&readback)==VK_SUCCESS,"readback buffer");VkMemoryRequirements mr{};api.GetBufferMemoryRequirements(device,readback,&mr);unsigned type{};for(;type<api.memory.memoryTypeCount;++type)if((mr.memoryTypeBits&(1U<<type)) && (api.memory.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))break;require(type<api.memory.memoryTypeCount,"readback type");VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=type;require(api.AllocateMemory(device,&ma,nullptr,&memory)==VK_SUCCESS && api.BindBufferMemory(device,readback,memory,0)==VK_SUCCESS,"readback memory");
        VkSemaphore acquired{},rendered{};VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};require(proc<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device,&semaphore,nullptr,&acquired)==VK_SUCCESS && proc<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device,&semaphore,nullptr,&rendered)==VK_SUCCESS,"semaphores");
        const OverlayRuntime runtime{1,command,snapshot,"Vulkan test"};
        original_present=proc<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
        if(layer) {
            const auto host=GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");require(host!=nullptr,"layer host");
            const auto target=GetProcAddress(host,"CheekyHost_VulkanPresent");require(target!=nullptr,"layer present export");
            require(MH_Initialize()==MH_OK,"test hook init");void* previous{};
            require(MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(&capture_layer_present),&previous)==MH_OK && MH_EnableHook(reinterpret_cast<void*>(target))==MH_OK,"test-only foreground adapter");
        }
        for(unsigned frame=0;frame<9;++frame) {
            std::printf("Overlay Vulkan frame %u\n",frame);
            if(frame==4) {test_foreground=nullptr;SendMessageW(window,WM_KILLFOCUS,0,0);SendMessageW(window,WM_ACTIVATEAPP,FALSE,0);}
            if(frame==6) {test_foreground=window;SendMessageW(window,WM_SETFOCUS,0,0);SendMessageW(window,WM_ACTIVATEAPP,TRUE,0);}
            if(frame==1 || frame==8)SendMessageW(window,WM_KEYDOWN,VK_F8,1);
            unsigned index{};require(proc<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR")(device,chain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index)==VK_SUCCESS,"acquire");
            require(proc<PFN_vkResetCommandPool>("vkResetCommandPool")(device,pool,0)==VK_SUCCESS,"reset clear");VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};require(proc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cmd,&begin)==VK_SUCCESS,"begin clear");
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=images[index];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;api.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);VkClearColorValue black{};proc<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cmd,images[index],barrier.newLayout,&black,1,&barrier.subresourceRange);barrier.oldLayout=barrier.newLayout;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;api.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);require(proc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cmd)==VK_SUCCESS,"end clear");
            VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&acquired;submit.pWaitDstStageMask=&stage;submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&rendered;require(proc<PFN_vkQueueSubmit>("vkQueueSubmit")(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS,"clear submit");require(proc<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue)==VK_SUCCESS,"clear idle");
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&rendered;present.swapchainCount=1;present.pSwapchains=&chain;present.pImageIndices=&index;
            CheekyVulkanPresent p;p.instance=instance;p.physical=physical;p.device=device;p.queue=queue;p.queue_family=family;p.gipa=gi;p.gdpa=gd;p.window=window;p.swapchain=chain;p.format=format.format;p.color_space=format.colorSpace;p.extent=extent;p.image_count=count;p.images=images.data();p.present=&present;p.next=read_present;
            require((layer?proc<PFN_vkQueuePresentKHR>("vkQueuePresentKHR")(queue,&present):overlay_vulkan_present(p,runtime))==VK_SUCCESS,"overlay present");proc<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue);
            unsigned visible{};for(unsigned i=0;i<pixels.size();i+=4)visible+=pixels[i]>5 || pixels[i+1]>5 || pixels[i+2]>5;
            std::printf("Overlay pixels=%u snapshots=%u\n",visible,snapshots);
            if(frame>=2 && frame<8)require(visible>10000 && snapshots>0,"F8 overlay pixels missing");else if(frame==0 || frame==8)require(!visible,"closed overlay changed pixels");
        }
        overlay_vulkan_destroy(device,chain);api.DestroyBuffer(device,readback,nullptr);api.FreeMemory(device,memory,nullptr);proc<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device,acquired,nullptr);proc<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device,rendered,nullptr);proc<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device,pool,nullptr);proc<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR")(device,chain,nullptr);proc<PFN_vkDestroyDevice>("vkDestroyDevice")(device,nullptr);iproc<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(instance,surface,nullptr);iproc<PFN_vkDestroyInstance>("vkDestroyInstance")(instance,nullptr);DestroyWindow(window);test_foreground=nullptr;
        std::puts("PASS native Vulkan F8 open/close, actual swapchain pixels, semaphore reuse and destruction");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL Vulkan overlay: %s\n",e.what());return 1;}
}
