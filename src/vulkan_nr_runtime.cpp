#include "vulkan_nr_runtime.hpp"
#include "nr_runtime_module.hpp"
#include "nr_parameters.hpp"
#include "ngx_runtime_discovery.hpp"
#include "runtime.hpp"
#include <mutex>
#include <unordered_map>

namespace cheeky::foveated_dlss {
namespace {
struct Runtime {
    VulkanNgxCallbacks callbacks;
    std::uint64_t generation{};
    NgxResult result{};
    bool initialized{},attempted{};
};
std::mutex mutex;
std::unordered_map<VkDevice,Runtime> runtimes;
template<class T>T proc(HMODULE module,const char* name){return reinterpret_cast<T>(GetProcAddress(module,name));}
}
bool vulkan_nr_runtime(const VulkanDeviceApi& a,VulkanNgxCallbacks& callbacks,NgxResult& result) noexcept {
    try {
        std::lock_guard lock(mutex);
        auto& r=runtimes[a.device];const auto generation=requested_nr_reset_generation.load(std::memory_order_relaxed);
        if(!r.initialized && (!r.attempted || r.generation!=generation)) {
            r.attempted=true;r.generation=generation;
            const auto model=load_nr_runtime_module();
            r.result=model.error?model.error:0xBAD00001U;
            if(model.module) {
                using Init=NgxResult(*)(unsigned long long,const wchar_t*,VkInstance,VkPhysicalDevice,VkDevice,unsigned,const void*);
                using Init2=NgxResult(*)(unsigned long long,const wchar_t*,VkInstance,VkPhysicalDevice,VkDevice,PFN_vkGetInstanceProcAddr,PFN_vkGetDeviceProcAddr,unsigned,const void*);
                const auto init=proc<Init>(model.module,"NVSDK_NGX_VULKAN_Init_Ext");
                const auto init2=proc<Init2>(model.module,"NVSDK_NGX_VULKAN_Init_Ext2");
                auto& c=r.callbacks;
                c.create=proc<VulkanNgxCreate>(model.module,"NVSDK_NGX_VULKAN_CreateFeature");
                c.evaluate=proc<VulkanNgxEvaluate>(model.module,"NVSDK_NGX_VULKAN_EvaluateFeature");
                c.release=proc<VulkanNgxRelease>(model.module,"NVSDK_NGX_VULKAN_ReleaseFeature");
                auto allocator=model.module;
                if(!GetProcAddress(allocator,"NVSDK_NGX_VULKAN_AllocateParameters"))allocator=find_loaded_ngx_core_runtime();
                if(allocator) {
                    c.allocate_parameters=proc<decltype(c.allocate_parameters)>(allocator,"NVSDK_NGX_VULKAN_AllocateParameters");
                    c.destroy_parameters=proc<decltype(c.destroy_parameters)>(allocator,"NVSDK_NGX_VULKAN_DestroyParameters");
                }
                if(c.create && c.evaluate && c.release && c.allocate_parameters && c.destroy_parameters) {
                    VulkanNgxScope private_call;
                    if(init2)r.result=init2(0x0876232cULL,model.directory.data(),a.instance,a.physical,a.device,a.gipa,a.gdpa,0x15,nullptr);
                    else if(init)r.result=init(0x0876232cULL,model.directory.data(),a.instance,a.physical,a.device,0x15,nullptr);
                    r.initialized=ngx_succeeded(r.result);
                }
                trace_event("Vulkan NR initialize device=%p result=0x%08X ready=%u",a.device,r.result,r.initialized);
            }
        }
        result=r.result;if(r.initialized)callbacks=r.callbacks;return r.initialized;
    }catch(...){result=0xBAD00001U;return false;}
}
void vulkan_nr_release_device(VkDevice d) noexcept {std::lock_guard lock(mutex);runtimes.erase(d);}
bool vulkan_nr_extensions(std::vector<std::string>& extensions,VkInstance instance,VkPhysicalDevice physical) noexcept {
    try {
        const auto model=load_nr_runtime_module();if(!model.module)return false;
        // Public NVSDK_NGX_FeatureDiscoveryInfo ABI; only ApplicationId is used.
        struct Discovery {
            unsigned version{0x15},feature{18};
            struct {unsigned type{};union {unsigned long long id;struct {const char* project;unsigned engine;const char* version;} project;} value{0x0876232cULL};} identifier;
            const wchar_t* directory{};const NgxFeatureCommonInfo* common{};
        } discovery;
        static_assert(sizeof(Discovery)==56);
        discovery.directory=model.directory.data();
        unsigned count{};VkExtensionProperties* properties{};NgxResult result{};
        if(physical) {
            using Query=NgxResult(*)(VkInstance,VkPhysicalDevice,const Discovery*,unsigned*,VkExtensionProperties**);
            auto query=proc<Query>(model.module,"NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements");if(!query)return false;
            result=query(instance,physical,&discovery,&count,&properties);
        }else {
            using Query=NgxResult(*)(const Discovery*,unsigned*,VkExtensionProperties**);
            auto query=proc<Query>(model.module,"NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements");if(!query)return false;
            result=query(&discovery,&count,&properties);
        }
        if(!ngx_succeeded(result) || count>128 || (count&&!properties))return false;
        for(unsigned i=0;i<count;++i)extensions.emplace_back(properties[i].extensionName);
        return true;
    }catch(...){return false;}
}
}
