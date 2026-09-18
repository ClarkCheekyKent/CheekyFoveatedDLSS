#include "vulkan_observer.hpp"
#include "../third_party/vulkan/include/vulkan/vk_layer.h"
#include "runtime.hpp"
#include "vulkan_nr_runtime.hpp"
#include "gaze_foveation.hpp"
#include "../shared/runtime_host_api.hpp"
#include "../shared/vulkan_overlay_api.hpp"
#include "../third_party/vulkan/include/vulkan/vulkan_win32.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cheeky::foveated_dlss {
namespace {
struct Instance { PFN_vkGetInstanceProcAddr next{}; };
struct ImageKey {
    VkImage image{}; unsigned aspect{}, mip{}, layer{};
    auto operator<=>(const ImageKey&) const = default;
};
using Layouts=std::map<ImageKey,VkImageLayout>;
struct Device { std::shared_ptr<VulkanDeviceApi> api; Layouts layouts; PFN_vkSetDeviceLoaderData set_loader_data{}; };
struct Command { VkDevice device{}; VkCommandPool pool{}; Layouts layouts; std::vector<GazeCopyEdge> copies; };
std::mutex registry_mutex;
std::unordered_map<VkInstance,Instance> instances;
std::unordered_map<VkPhysicalDevice,VkInstance> physicals;
std::unordered_map<VkDevice,Device> devices;
std::unordered_map<VkCommandBuffer,Command> commands;
struct Queue {VkDevice device{};unsigned family{};VkQueueFlags flags{};bool protected_queue{};};
std::unordered_map<VkQueue,Queue> queues;
struct Surface {VkInstance instance{};HWND window{};};
struct Swapchain {VkDevice device{};HWND window{};VkFormat format{};VkColorSpaceKHR color_space{};VkExtent2D extent{};std::vector<VkImage> images;bool drawable{};};
std::unordered_map<VkSurfaceKHR,Surface> surfaces;
std::unordered_map<VkSwapchainKHR,std::shared_ptr<Swapchain>> swapchains;
std::once_flag startup;

void start_layer_runtime() {
    std::call_once(startup,[] {
        if(GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll"))return;
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&start_layer_runtime),&module)) return;
        auto start=reinterpret_cast<CheekyRuntimeStartFn>(GetProcAddress(module,"CheekyRuntime_Start"));
        if (!start) return;
        wchar_t filename[32768]{}; GetModuleFileNameW(module,filename,ARRAYSIZE(filename));
        const auto directory=std::filesystem::path(filename).parent_path().wstring();
        static std::uint64_t attachment{};
        CheekyRuntimeStart input; input.host=CheekyRuntimeHost::standalone;
        input.config_directory=directory.c_str(); input.attachment=&attachment;
        // A pre-existing OptiScaler/standalone host retains ownership. The layer
        // observes graphics independently and never detaches another host.
        start(&input);
    });
}
std::shared_ptr<VulkanDeviceApi> device_api(VkDevice device) {
    std::lock_guard lock(registry_mutex);
    const auto it=devices.find(device); return it==devices.end()?nullptr:it->second.api;
}
template<class T> T proc(const VulkanDeviceApi& a,const char* name) { return reinterpret_cast<T>(a.gdpa(a.device,name)); }
template<class T> T iproc(VkInstance instance,const char* name) {
    PFN_vkGetInstanceProcAddr next{};
    { std::lock_guard lock(registry_mutex); const auto it=instances.find(instance); if(it!=instances.end()) next=it->second.next; }
    return next?reinterpret_cast<T>(next(instance,name)):nullptr;
}
void observe_physical(VkInstance instance,unsigned count,const VkPhysicalDevice* values) {
    std::lock_guard lock(registry_mutex); for(unsigned i=0;i<count;++i) physicals[values[i]]=instance;
}
Queue describe_queue(const VulkanDeviceApi& a,unsigned family,bool protected_queue=false) {
    const auto get=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(a.gipa(a.instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
    unsigned count{};get(a.physical,&count,nullptr);std::vector<VkQueueFamilyProperties> props(count);get(a.physical,&count,props.data());
    return {a.device,family,family<count?props[family].queueFlags:0U,protected_queue};
}
void destroy_overlay(VkDevice device,VkSwapchainKHR chain) {
    const auto host=GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
    const auto destroy=host?reinterpret_cast<CheekyVulkanDestroyFn>(GetProcAddress(host,"CheekyHost_VulkanDestroy")):nullptr;
    if(destroy)destroy(device,chain);
}
void record_layout(Layouts& layouts,VkImage image,const VkImageSubresourceRange& range,VkImageLayout layout) {
    // NGX images refer to finite subresources. Unbounded ranges are represented
    // by a wildcard and resolved only if no more specific transition exists.
    if(range.levelCount==VK_REMAINING_MIP_LEVELS || range.layerCount==VK_REMAINING_ARRAY_LAYERS) {
        for(auto it=layouts.begin();it!=layouts.end();) {
            if(it->first.image==image && (it->first.aspect&range.aspectMask) && it->first.mip>=range.baseMipLevel && it->first.layer>=range.baseArrayLayer) it=layouts.erase(it);else ++it;
        }
        // A whole-image range is the only wildcard whose bounds are unambiguous.
        if(range.baseMipLevel==0 && range.baseArrayLayer==0) layouts[{image,range.aspectMask,UINT32_MAX,UINT32_MAX}]=layout;
        return;
    }
    if(range.levelCount>32 || range.layerCount>2048) return;
    for(unsigned m=0;m<range.levelCount;++m) for(unsigned l=0;l<range.layerCount;++l)
        layouts[{image,range.aspectMask,range.baseMipLevel+m,range.baseArrayLayer+l}]=layout;
}
}

std::shared_ptr<VulkanDeviceApi> vulkan_command_device(VkCommandBuffer cmd) noexcept {
    std::lock_guard lock(registry_mutex); const auto c=commands.find(cmd);
    if(c==commands.end()) return {};
    const auto d=devices.find(c->second.device);return d==devices.end()?nullptr:d->second.api;
}
bool vulkan_observe_device(VkInstance instance,VkPhysicalDevice physical,VkDevice device,PFN_vkGetInstanceProcAddr gi,PFN_vkGetDeviceProcAddr gd) {
    auto api=std::make_shared<VulkanDeviceApi>();
    if(!api->initialize(instance,physical,device,gi,gd))return false;
    std::lock_guard lock(registry_mutex);devices[device]={api,{}};return true;
}
void vulkan_runtime_frame(VkDevice device,VkQueue queue) noexcept {
    using Frame=void(*)(void*,void*);
    static const auto frame=[] {
        HMODULE module{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&vulkan_runtime_frame),&module);
        return reinterpret_cast<Frame>(GetProcAddress(module,"CheekyRuntime_VulkanFrame"));
    }();
    if(frame)frame(device,queue);
}
bool vulkan_image_layout(VkCommandBuffer cmd,const VulkanImageInfo& image,VkImageLayout& result) noexcept {
    std::lock_guard lock(registry_mutex); const auto c=commands.find(cmd);
    if(c==commands.end() || image.range.levelCount!=1 || image.range.layerCount!=1) return false;
    const auto d=devices.find(c->second.device);if(d==devices.end()) return false;
    const ImageKey key{image.image,image.range.aspectMask,image.range.baseMipLevel,image.range.baseArrayLayer};
    const ImageKey wildcard{image.image,image.range.aspectMask,UINT32_MAX,UINT32_MAX};
    for(const auto* map:{&c->second.layouts,&d->second.layouts}) {
        auto it=map->find(key);if(it==map->end()) it=map->find(wildcard);
        if(it!=map->end()) { result=it->second;return result!=VK_IMAGE_LAYOUT_UNDEFINED; }
    }
    return false;
}
void vulkan_forget_command(VkCommandBuffer cmd) noexcept {
    vulkan_backend_forget_command(cmd);
    reset_gaze_copies(reinterpret_cast<std::uint64_t>(cmd));
    std::lock_guard lock(registry_mutex); const auto it=commands.find(cmd);if(it!=commands.end()) {it->second.layouts.clear();it->second.copies.clear();}
}

extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CheekyVkGetInstanceProcAddr(VkInstance,const char*);
extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CheekyVkGetDeviceProcAddr(VkDevice,const char*);

VKAPI_ATTR VkResult VKAPI_CALL layer_create_instance(const VkInstanceCreateInfo* info,const VkAllocationCallbacks* alloc,VkInstance* out) {
    auto* link=reinterpret_cast<const VkLayerInstanceCreateInfo*>(info->pNext);
    while(link && (link->sType!=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || link->function!=VK_LAYER_LINK_INFO)) link=reinterpret_cast<const VkLayerInstanceCreateInfo*>(link->pNext);
    if(!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto next=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const_cast<VkLayerInstanceCreateInfo*>(link)->u.pLayerInfo=link->u.pLayerInfo->pNext;
    start_layer_runtime();
    const auto create=reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE,"vkCreateInstance"));
    auto expanded=*info;
    std::vector<std::string> required;vulkan_nr_extensions(required);
    std::vector<const char*> extensions(info->ppEnabledExtensionNames,info->ppEnabledExtensionNames+info->enabledExtensionCount);
    // Instance-extension enumeration is a loader-global command, not part of
    // the next layer's instance dispatch before that instance exists.
    const auto enumerate=reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),"vkEnumerateInstanceExtensionProperties"));
    unsigned count{};std::vector<VkExtensionProperties> supported;
    if(enumerate && enumerate(nullptr,&count,nullptr)==VK_SUCCESS) {supported.resize(count);enumerate(nullptr,&count,supported.data());}
    for(const auto& name:required)if(std::none_of(extensions.begin(),extensions.end(),[&](auto n){return name==n;}) &&
        std::any_of(supported.begin(),supported.end(),[&](const auto& p){return name==p.extensionName;}))extensions.push_back(name.c_str());
    expanded.enabledExtensionCount=static_cast<unsigned>(extensions.size());expanded.ppEnabledExtensionNames=extensions.data();
    const auto result=create(&expanded,alloc,out);
    if(result==VK_SUCCESS) { std::lock_guard lock(registry_mutex);instances[*out]={next}; }
    return result;
}
VKAPI_ATTR void VKAPI_CALL layer_destroy_instance(VkInstance instance,const VkAllocationCallbacks* alloc) {
    auto destroy=iproc<PFN_vkDestroyInstance>(instance,"vkDestroyInstance");
    if(destroy) destroy(instance,alloc);
    std::lock_guard lock(registry_mutex);instances.erase(instance);
    std::erase_if(physicals,[&](const auto& item){return item.second==instance;});
    std::erase_if(surfaces,[&](const auto& item){return item.second.instance==instance;});
}
VKAPI_ATTR VkResult VKAPI_CALL layer_enumerate_physical(VkInstance instance,unsigned* count,VkPhysicalDevice* out) {
    const auto result=iproc<PFN_vkEnumeratePhysicalDevices>(instance,"vkEnumeratePhysicalDevices")(instance,count,out);
    if(out && (result==VK_SUCCESS || result==VK_INCOMPLETE)) observe_physical(instance,*count,out);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL layer_enumerate_groups(VkInstance instance,unsigned* count,VkPhysicalDeviceGroupProperties* out) {
    auto next=iproc<PFN_vkEnumeratePhysicalDeviceGroups>(instance,"vkEnumeratePhysicalDeviceGroups");
    if(!next)next=iproc<PFN_vkEnumeratePhysicalDeviceGroups>(instance,"vkEnumeratePhysicalDeviceGroupsKHR");
    const auto result=next(instance,count,out);
    if(out && (result==VK_SUCCESS || result==VK_INCOMPLETE)) for(unsigned i=0;i<*count;++i) observe_physical(instance,out[i].physicalDeviceCount,out[i].physicalDevices);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL layer_create_device(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,const VkAllocationCallbacks* alloc,VkDevice* out) {
    VkInstance instance{};
    {std::lock_guard lock(registry_mutex);const auto it=physicals.find(physical);if(it!=physicals.end()) instance=it->second;}
    auto* link=reinterpret_cast<const VkLayerDeviceCreateInfo*>(info->pNext);
    while(link && (link->sType!=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || link->function!=VK_LAYER_LINK_INFO)) link=reinterpret_cast<const VkLayerDeviceCreateInfo*>(link->pNext);
    if(!instance || !link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto gi=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto gd=link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkSetDeviceLoaderData set_loader_data{};
    for(auto* entry=reinterpret_cast<const VkLayerDeviceCreateInfo*>(info->pNext);entry;
        entry=reinterpret_cast<const VkLayerDeviceCreateInfo*>(entry->pNext))
        if(entry->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && entry->function==VK_LOADER_DATA_CALLBACK)
            set_loader_data=entry->u.pfnSetDeviceLoaderData;
    const_cast<VkLayerDeviceCreateInfo*>(link)->u.pLayerInfo=link->u.pLayerInfo->pNext;
    const auto create=reinterpret_cast<PFN_vkCreateDevice>(gi(instance,"vkCreateDevice"));
    auto expanded=*info;
    std::vector<std::string> required;vulkan_nr_extensions(required,instance,physical);
    std::vector<const char*> extensions(info->ppEnabledExtensionNames,info->ppEnabledExtensionNames+info->enabledExtensionCount);
    const auto enumerate=reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gi(instance,"vkEnumerateDeviceExtensionProperties"));
    unsigned count{};std::vector<VkExtensionProperties> supported;
    if(enumerate && enumerate(physical,nullptr,&count,nullptr)==VK_SUCCESS) {supported.resize(count);enumerate(physical,nullptr,&count,supported.data());}
    for(const auto& name:required)if(std::none_of(extensions.begin(),extensions.end(),[&](auto n){return name==n;}) &&
        std::any_of(supported.begin(),supported.end(),[&](const auto& p){return name==p.extensionName;}))extensions.push_back(name.c_str());
    expanded.enabledExtensionCount=static_cast<unsigned>(extensions.size());expanded.ppEnabledExtensionNames=extensions.data();
    const auto result=create(physical,&expanded,alloc,out);
    if(result==VK_SUCCESS) {
        vulkan_observe_device(instance,physical,*out,gi,gd);
        {std::lock_guard lock(registry_mutex);devices.at(*out).set_loader_data=set_loader_data;}
        trace_event("Vulkan device observed device=%p",*out);
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL layer_destroy_device(VkDevice device,const VkAllocationCallbacks* alloc) {
    const auto a=device_api(device);if(!a) return;
    destroy_overlay(device,VK_NULL_HANDLE);
    vulkan_backend_release_device(device);
    proc<PFN_vkDestroyDevice>(*a,"vkDestroyDevice")(device,alloc);
    std::lock_guard lock(registry_mutex);devices.erase(device);
    std::erase_if(commands,[&](const auto& i){return i.second.device==device;});
    std::erase_if(queues,[&](const auto& i){return i.second.device==device;});
    std::erase_if(swapchains,[&](const auto& i){return i.second->device==device;});
}
VKAPI_ATTR void VKAPI_CALL layer_get_queue(VkDevice d,unsigned family,unsigned index,VkQueue* out) {
    auto a=device_api(d);proc<PFN_vkGetDeviceQueue>(*a,"vkGetDeviceQueue")(d,family,index,out);
    const auto metadata=describe_queue(*a,family);std::lock_guard lock(registry_mutex);queues[*out]=metadata;
}
VKAPI_ATTR void VKAPI_CALL layer_get_queue2(VkDevice d,const VkDeviceQueueInfo2* info,VkQueue* out) {
    auto a=device_api(d);proc<PFN_vkGetDeviceQueue2>(*a,"vkGetDeviceQueue2")(d,info,out);
    const auto metadata=describe_queue(*a,info->queueFamilyIndex,(info->flags&VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT)!=0);std::lock_guard lock(registry_mutex);queues[*out]=metadata;
}
VKAPI_ATTR VkResult VKAPI_CALL layer_allocate_commands(VkDevice d,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {
    auto a=device_api(d);const auto result=proc<PFN_vkAllocateCommandBuffers>(*a,"vkAllocateCommandBuffers")(d,info,out);
    if(result==VK_SUCCESS) {std::lock_guard lock(registry_mutex);for(unsigned i=0;i<info->commandBufferCount;++i) commands[out[i]]={d,info->commandPool,{}};}
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL allocate_private_commands(VkDevice device,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out) {
    std::shared_ptr<VulkanDeviceApi> api;PFN_vkSetDeviceLoaderData set_loader_data{};
    {std::lock_guard lock(registry_mutex);const auto found=devices.find(device);
        if(found==devices.end())return VK_ERROR_INITIALIZATION_FAILED;
        api=found->second.api;set_loader_data=found->second.set_loader_data;}
    const auto result=proc<PFN_vkAllocateCommandBuffers>(*api,"vkAllocateCommandBuffers")(device,info,out);
    if(result!=VK_SUCCESS)return result;
    // Layer-created dispatchable objects do not pass through the loader's
    // allocation trampoline. Initialize them explicitly before recording or
    // handing them to another overlay/layer.
    if(set_loader_data)for(unsigned i=0;i<info->commandBufferCount;++i) {
        const auto initialized=set_loader_data(device,out[i]);
        if(initialized!=VK_SUCCESS) {
            proc<PFN_vkFreeCommandBuffers>(*api,"vkFreeCommandBuffers")(device,info->commandPool,info->commandBufferCount,out);
            std::fill_n(out,info->commandBufferCount,VK_NULL_HANDLE);return initialized;
        }
    }
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL private_device_proc(VkDevice device,const char* name) {
    if(!name)return nullptr;
    if(std::strcmp(name,"vkAllocateCommandBuffers")==0)return reinterpret_cast<PFN_vkVoidFunction>(&allocate_private_commands);
    const auto api=device_api(device);return api?api->gdpa(device,name):nullptr;
}
VKAPI_ATTR void VKAPI_CALL layer_free_commands(VkDevice d,VkCommandPool pool,unsigned count,const VkCommandBuffer* list) {
    auto a=device_api(d);
    for(unsigned i=0;i<count;++i) vulkan_forget_command(list[i]);
    proc<PFN_vkFreeCommandBuffers>(*a,"vkFreeCommandBuffers")(d,pool,count,list);
    std::lock_guard lock(registry_mutex);for(unsigned i=0;i<count;++i) commands.erase(list[i]);
}
VKAPI_ATTR VkResult VKAPI_CALL layer_begin(VkCommandBuffer cmd,const VkCommandBufferBeginInfo* info) {
    auto a=vulkan_command_device(cmd);auto result=proc<PFN_vkBeginCommandBuffer>(*a,"vkBeginCommandBuffer")(cmd,info);
    if(result==VK_SUCCESS) vulkan_forget_command(cmd);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL layer_reset(VkCommandBuffer cmd,VkCommandBufferResetFlags flags) {
    auto a=vulkan_command_device(cmd);auto result=proc<PFN_vkResetCommandBuffer>(*a,"vkResetCommandBuffer")(cmd,flags);
    if(result==VK_SUCCESS) vulkan_forget_command(cmd);return result;
}
void forget_pool(VkDevice device,VkCommandPool pool,bool destroy) {
    std::vector<VkCommandBuffer> list;
    {std::lock_guard lock(registry_mutex);for(const auto& [cmd,c]:commands)if(c.device==device && c.pool==pool)list.push_back(cmd);}
    for(auto cmd:list)vulkan_forget_command(cmd);
    if(destroy){std::lock_guard lock(registry_mutex);for(auto cmd:list)commands.erase(cmd);}
}
VKAPI_ATTR VkResult VKAPI_CALL layer_reset_pool(VkDevice device,VkCommandPool pool,VkCommandPoolResetFlags flags) {
    auto a=device_api(device);const auto result=proc<PFN_vkResetCommandPool>(*a,"vkResetCommandPool")(device,pool,flags);
    if(result==VK_SUCCESS)forget_pool(device,pool,false);return result;
}
VKAPI_ATTR void VKAPI_CALL layer_destroy_pool(VkDevice device,VkCommandPool pool,const VkAllocationCallbacks* alloc) {
    auto a=device_api(device);forget_pool(device,pool,true);proc<PFN_vkDestroyCommandPool>(*a,"vkDestroyCommandPool")(device,pool,alloc);
}
VKAPI_ATTR void VKAPI_CALL layer_destroy_image(VkDevice device,VkImage image,const VkAllocationCallbacks* alloc) {
    auto a=device_api(device);proc<PFN_vkDestroyImage>(*a,"vkDestroyImage")(device,image,alloc);
    forget_gaze_resource(reinterpret_cast<std::uint64_t>(image));
    std::lock_guard lock(registry_mutex);
    const auto clear=[&](Layouts& map){std::erase_if(map,[&](const auto& entry){return entry.first.image==image;});};
    clear(devices.at(device).layouts);for(auto& [_,c]:commands)if(c.device==device)clear(c.layouts);
}
VKAPI_ATTR void VKAPI_CALL layer_barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
    unsigned mc,const VkMemoryBarrier* m,unsigned bc,const VkBufferMemoryBarrier* b,unsigned ic,const VkImageMemoryBarrier* images) {
    auto a=vulkan_command_device(cmd);a->CmdPipelineBarrier(cmd,src,dst,flags,mc,m,bc,b,ic,images);
    std::lock_guard lock(registry_mutex);auto it=commands.find(cmd);if(it!=commands.end())
        for(unsigned i=0;i<ic;++i) record_layout(it->second.layouts,images[i].image,images[i].subresourceRange,images[i].newLayout);
}
template<bool Core> VKAPI_ATTR void VKAPI_CALL layer_barrier2(VkCommandBuffer cmd,const VkDependencyInfoKHR* info) {
    auto a=vulkan_command_device(cmd);proc<PFN_vkCmdPipelineBarrier2KHR>(*a,Core?"vkCmdPipelineBarrier2":"vkCmdPipelineBarrier2KHR")(cmd,info);
    std::lock_guard lock(registry_mutex);auto it=commands.find(cmd);if(it!=commands.end())
        for(unsigned i=0;i<info->imageMemoryBarrierCount;++i) {
            const auto& b=info->pImageMemoryBarriers[i];record_layout(it->second.layouts,b.image,b.subresourceRange,b.newLayout);
        }
}
VKAPI_ATTR void VKAPI_CALL layer_execute(VkCommandBuffer cmd,unsigned count,const VkCommandBuffer* secondary) {
    auto a=vulkan_command_device(cmd);proc<PFN_vkCmdExecuteCommands>(*a,"vkCmdExecuteCommands")(cmd,count,secondary);
    std::lock_guard lock(registry_mutex);auto primary=commands.find(cmd);if(primary==commands.end())return;
    for(unsigned i=0;i<count;++i)if(const auto child=commands.find(secondary[i]);child!=commands.end())
        {
            for(const auto& entry:child->second.layouts)primary->second.layouts[entry.first]=entry.second;
            for(const auto& copy:child->second.copies)if(primary->second.copies.size()<512)primary->second.copies.push_back(copy);
        }
}
void observe_submission(VkDevice device,VkCommandBuffer cmd) {
    std::vector<GazeCopyEdge> copies;
    {
        std::lock_guard lock(registry_mutex);
        const auto c=commands.find(cmd);if(c==commands.end())return;
        auto& global=devices.at(device).layouts;
        for(const auto& entry:c->second.layouts)global[entry.first]=entry.second;
        copies=c->second.copies;
    }
    const auto identity=reinterpret_cast<std::uint64_t>(cmd);
    reset_gaze_copies(identity);
    for(const auto& copy:copies)record_gaze_copy(identity,copy);
    submit_gaze_copies(identity);
}
template<bool Core> VKAPI_ATTR VkResult VKAPI_CALL layer_submit2(VkQueue queue,unsigned count,const VkSubmitInfo2KHR* submits,VkFence fence) {
    std::shared_ptr<VulkanDeviceApi> a;
    {std::lock_guard lock(registry_mutex);const auto q=queues.find(queue);if(q==queues.end())return VK_ERROR_INITIALIZATION_FAILED;a=devices.at(q->second.device).api;}
    const auto result=proc<PFN_vkQueueSubmit2KHR>(*a,Core?"vkQueueSubmit2":"vkQueueSubmit2KHR")(queue,count,submits,fence);
    if(result==VK_SUCCESS)for(unsigned i=0;i<count;++i)for(unsigned j=0;j<submits[i].commandBufferInfoCount;++j)
        observe_submission(a->device,submits[i].pCommandBufferInfos[j].commandBuffer);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL layer_submit(VkQueue queue,unsigned count,const VkSubmitInfo* submits,VkFence fence) {
    std::shared_ptr<VulkanDeviceApi> a;
    {std::lock_guard lock(registry_mutex);const auto q=queues.find(queue);if(q==queues.end())return VK_ERROR_INITIALIZATION_FAILED;a=devices.at(q->second.device).api;}
    const auto result=proc<PFN_vkQueueSubmit>(*a,"vkQueueSubmit")(queue,count,submits,fence);
    if(result==VK_SUCCESS)for(unsigned i=0;i<count;++i)for(unsigned j=0;j<submits[i].commandBufferCount;++j)
        observe_submission(a->device,submits[i].pCommandBuffers[j]);
    return result;
}
void observe_copy(VkCommandBuffer cmd,VkImage source,VkImage destination,const VkImageSubresourceLayers& src,
    const VkImageSubresourceLayers& dst,VkOffset3D from,VkOffset3D to,VkExtent3D extent) {
    if(src.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT || dst.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT ||
        src.mipLevel || dst.mipLevel || src.baseArrayLayer || dst.baseArrayLayer ||
        src.layerCount!=1 || dst.layerCount!=1 || from.z || to.z || extent.depth!=1 ||
        from.x<0 || from.y<0 || to.x<0 || to.y<0 || !extent.width || !extent.height)return;
    std::lock_guard lock(registry_mutex);auto c=commands.find(cmd);
    if(c==commands.end() || c->second.copies.size()>=512)return;
    c->second.copies.push_back({{reinterpret_cast<std::uint64_t>(source),0,static_cast<unsigned>(from.x),static_cast<unsigned>(from.y),extent.width,extent.height},
        {reinterpret_cast<std::uint64_t>(destination),0,static_cast<unsigned>(to.x),static_cast<unsigned>(to.y),extent.width,extent.height}});
}
VKAPI_ATTR void VKAPI_CALL layer_copy(VkCommandBuffer cmd,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,unsigned count,const VkImageCopy* regions) {
    auto a=vulkan_command_device(cmd);proc<PFN_vkCmdCopyImage>(*a,"vkCmdCopyImage")(cmd,src,sl,dst,dl,count,regions);
    for(unsigned i=0;i<count;++i){const auto& r=regions[i];observe_copy(cmd,src,dst,r.srcSubresource,r.dstSubresource,r.srcOffset,r.dstOffset,r.extent);}
}
VKAPI_ATTR void VKAPI_CALL layer_blit(VkCommandBuffer cmd,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,unsigned count,const VkImageBlit* regions,VkFilter filter) {
    auto a=vulkan_command_device(cmd);proc<PFN_vkCmdBlitImage>(*a,"vkCmdBlitImage")(cmd,src,sl,dst,dl,count,regions,filter);
    for(unsigned i=0;i<count;++i) {
        const auto& r=regions[i];const auto width=r.srcOffsets[1].x-r.srcOffsets[0].x,height=r.srcOffsets[1].y-r.srcOffsets[0].y;
        if(width<=0 || height<=0 || width!=r.dstOffsets[1].x-r.dstOffsets[0].x || height!=r.dstOffsets[1].y-r.dstOffsets[0].y ||
            r.srcOffsets[1].z!=1 || r.dstOffsets[1].z!=1)continue;
        observe_copy(cmd,src,dst,r.srcSubresource,r.dstSubresource,r.srcOffsets[0],r.dstOffsets[0],{static_cast<unsigned>(width),static_cast<unsigned>(height),1});
    }
}
VKAPI_ATTR VkResult VKAPI_CALL layer_create_surface(VkInstance instance,const VkWin32SurfaceCreateInfoKHR* info,const VkAllocationCallbacks* alloc,VkSurfaceKHR* surface) {
    const auto result=iproc<PFN_vkCreateWin32SurfaceKHR>(instance,"vkCreateWin32SurfaceKHR")(instance,info,alloc,surface);
    if(result==VK_SUCCESS){std::lock_guard lock(registry_mutex);surfaces[*surface]={instance,info->hwnd};}return result;
}
VKAPI_ATTR void VKAPI_CALL layer_destroy_surface(VkInstance instance,VkSurfaceKHR surface,const VkAllocationCallbacks* alloc) {
    iproc<PFN_vkDestroySurfaceKHR>(instance,"vkDestroySurfaceKHR")(instance,surface,alloc);
    std::lock_guard lock(registry_mutex);surfaces.erase(surface);
}
VKAPI_ATTR VkResult VKAPI_CALL layer_create_swapchain(VkDevice device,const VkSwapchainCreateInfoKHR* info,const VkAllocationCallbacks* alloc,VkSwapchainKHR* out) {
    auto a=device_api(device);const auto result=proc<PFN_vkCreateSwapchainKHR>(*a,"vkCreateSwapchainKHR")(device,info,alloc,out);
    if(result==VK_SUCCESS) {
        auto chain=std::make_shared<Swapchain>();chain->device=device;chain->format=info->imageFormat;chain->color_space=info->imageColorSpace;chain->extent=info->imageExtent;
        chain->drawable=info->imageArrayLayers==1 && (info->imageUsage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(info->flags&VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR);
        const auto images=proc<PFN_vkGetSwapchainImagesKHR>(*a,"vkGetSwapchainImagesKHR");unsigned count{};
        if(images(device,*out,&count,nullptr)==VK_SUCCESS){chain->images.resize(count);if(images(device,*out,&count,chain->images.data())!=VK_SUCCESS)chain->drawable=false;}
        std::lock_guard lock(registry_mutex);const auto surface=surfaces.find(info->surface);if(surface!=surfaces.end())chain->window=surface->second.window;
        swapchains[*out]=chain;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL layer_destroy_swapchain(VkDevice device,VkSwapchainKHR chain,const VkAllocationCallbacks* alloc) {
    auto a=device_api(device);destroy_overlay(device,chain);
    proc<PFN_vkDestroySwapchainKHR>(*a,"vkDestroySwapchainKHR")(device,chain,alloc);
    std::lock_guard lock(registry_mutex);swapchains.erase(chain);
}
VKAPI_ATTR VkResult VKAPI_CALL layer_present(VkQueue queue,const VkPresentInfoKHR* info) {
    std::shared_ptr<VulkanDeviceApi> a;Queue q;std::shared_ptr<Swapchain> chain;unsigned index{};
    {std::lock_guard lock(registry_mutex);const auto found=queues.find(queue);if(found==queues.end())return VK_ERROR_INITIALIZATION_FAILED;
        q=found->second;a=devices.at(q.device).api;
        for(;index<info->swapchainCount;++index) {
            const auto swap=swapchains.find(info->pSwapchains[index]);
            if(swap!=swapchains.end() && swap->second->drawable && swap->second->window){chain=swap->second;break;}
        }
    }
    const auto next=proc<PFN_vkQueuePresentKHR>(*a,"vkQueuePresentKHR");
    vulkan_runtime_frame(a->device,queue);
    const auto host=GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
    const auto overlay=host?reinterpret_cast<CheekyVulkanPresentFn>(GetProcAddress(host,"CheekyHost_VulkanPresent")):nullptr;
    if(overlay && chain && (q.flags&VK_QUEUE_GRAPHICS_BIT) && !q.protected_queue) {
        CheekyVulkanPresent p;p.instance=a->instance;p.physical=a->physical;p.device=a->device;p.queue=queue;p.queue_family=q.family;
        p.gipa=a->gipa;p.gdpa=private_device_proc;p.window=chain->window;p.swapchain=info->pSwapchains[index];p.format=chain->format;p.color_space=chain->color_space;p.extent=chain->extent;
        p.image_count=static_cast<unsigned>(chain->images.size());p.images=chain->images.data();p.present_index=index;p.present=info;p.next=next;
        return overlay(&p);
    }
    return next(queue,info);
}
namespace {
PFN_vkVoidFunction intercepted(const char* name) {
#define VK_HOOK(api, fn) if(std::strcmp(name,"vk" #api)==0) return reinterpret_cast<PFN_vkVoidFunction>(&fn);
    VK_HOOK(GetInstanceProcAddr,CheekyVkGetInstanceProcAddr) VK_HOOK(GetDeviceProcAddr,CheekyVkGetDeviceProcAddr)
    VK_HOOK(CreateInstance,layer_create_instance) VK_HOOK(DestroyInstance,layer_destroy_instance)
    VK_HOOK(EnumeratePhysicalDevices,layer_enumerate_physical) VK_HOOK(EnumeratePhysicalDeviceGroups,layer_enumerate_groups)
    VK_HOOK(EnumeratePhysicalDeviceGroupsKHR,layer_enumerate_groups)
    VK_HOOK(CreateDevice,layer_create_device) VK_HOOK(DestroyDevice,layer_destroy_device)
    VK_HOOK(GetDeviceQueue,layer_get_queue) VK_HOOK(GetDeviceQueue2,layer_get_queue2)
    VK_HOOK(AllocateCommandBuffers,layer_allocate_commands) VK_HOOK(FreeCommandBuffers,layer_free_commands)
    VK_HOOK(BeginCommandBuffer,layer_begin) VK_HOOK(ResetCommandBuffer,layer_reset)
    VK_HOOK(CmdCopyImage,layer_copy) VK_HOOK(CmdBlitImage,layer_blit)
    VK_HOOK(CmdPipelineBarrier,layer_barrier) VK_HOOK(QueueSubmit,layer_submit)
    VK_HOOK(ResetCommandPool,layer_reset_pool) VK_HOOK(DestroyCommandPool,layer_destroy_pool) VK_HOOK(DestroyImage,layer_destroy_image)
    VK_HOOK(CmdPipelineBarrier2,layer_barrier2<true>) VK_HOOK(CmdPipelineBarrier2KHR,layer_barrier2<false>)
    VK_HOOK(QueueSubmit2,layer_submit2<true>) VK_HOOK(QueueSubmit2KHR,layer_submit2<false>) VK_HOOK(CmdExecuteCommands,layer_execute)
    VK_HOOK(CreateWin32SurfaceKHR,layer_create_surface) VK_HOOK(DestroySurfaceKHR,layer_destroy_surface)
    VK_HOOK(CreateSwapchainKHR,layer_create_swapchain) VK_HOOK(DestroySwapchainKHR,layer_destroy_swapchain) VK_HOOK(QueuePresentKHR,layer_present)
#undef VK_HOOK
    return nullptr;
}
}
extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CheekyVkGetInstanceProcAddr(VkInstance instance,const char* name) {
    if(!name) return nullptr;
    if(std::strcmp(name,"vkGetInstanceProcAddr")==0)return reinterpret_cast<PFN_vkVoidFunction>(&CheekyVkGetInstanceProcAddr);
    if(std::strcmp(name,"vkCreateInstance")==0)return reinterpret_cast<PFN_vkVoidFunction>(&layer_create_instance);
    const auto original=instance?iproc<PFN_vkVoidFunction>(instance,name):nullptr;
    if(!original)return nullptr;
    const auto hook=intercepted(name);return hook?hook:original;
}
extern "C" __declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CheekyVkGetDeviceProcAddr(VkDevice device,const char* name) {
    if(!name) return nullptr;
    const auto a=device_api(device);if(!a) return nullptr;
    const auto original=a->gdpa(device,name);if(!original) return nullptr;
    const auto hook=intercepted(name);return hook?hook:original;
}
extern "C" __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL CheekyVkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info) {
    if(!info || info->loaderLayerInterfaceVersion<2) return VK_ERROR_INITIALIZATION_FAILED;
    info->loaderLayerInterfaceVersion=2;
    info->pfnGetInstanceProcAddr=CheekyVkGetInstanceProcAddr;
    info->pfnGetDeviceProcAddr=CheekyVkGetDeviceProcAddr;
    info->pfnGetPhysicalDeviceProcAddr=nullptr;
    return VK_SUCCESS;
}
}
