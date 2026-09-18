#include "vulkan_observer.hpp"
#include "vulkan_backend.hpp"
#include "ngx_frame_contract.hpp"
#include "ngx_parameter_overlay.hpp"
#include "runtime.hpp"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace cheeky::foveated_dlss {
namespace {
struct Route {
    HMODULE module{};
    VulkanNgxCallbacks callbacks{};

};
std::array<Route,8> routes{};
std::mutex hooks_mutex,features_mutex;
using Scope=VulkanNgxScope;
struct GameFeature {unsigned type{},width{},height{},out_width{},out_height{},flags{},quality{};};
std::unordered_map<const NgxHandle*,GameFeature> features;
void created(const NgxHandle* handle,unsigned type,const NgxParameters* p) {
    if(!handle || !p)return;
    std::lock_guard lock(features_mutex);
    features[handle]={type,get_ui(p,"Width"),get_ui(p,"Height"),get_ui(p,"OutWidth"),get_ui(p,"OutHeight"),
        get_ngx_integer_bits(p,"DLSS.Feature.Create.Flags"),get_ngx_integer_bits(p,"PerfQualityValue")};
    if(type==1)register_stereo_view(reinterpret_cast<std::uintptr_t>(handle));
}
template<unsigned I> NgxResult create(VkCommandBuffer cmd,unsigned type,NgxParameters* p,NgxHandle** out) {
    Scope scope;const auto result=routes[I].callbacks.create(cmd,type,p,out);
    if(vulkan_ngx_private_depth==1 && ngx_succeeded(result) && out)created(*out,type,p);
    return result;
}
template<unsigned I> NgxResult create1(VkDevice device,VkCommandBuffer cmd,unsigned type,NgxParameters* p,NgxHandle** out) {
    Scope scope;const auto result=routes[I].callbacks.create1(device,cmd,type,p,out);
    if(vulkan_ngx_private_depth==1 && ngx_succeeded(result) && out)created(*out,type,p);
    return result;
}
template<unsigned I> NgxResult evaluate(VkCommandBuffer cmd,const NgxHandle* handle,const NgxParameters* p,NgxProgressCallback callback) {
    Scope scope;
    auto& route=routes[I];
    if(vulkan_ngx_private_depth==1 && p && route.callbacks.create && route.callbacks.release) {
        GameFeature feature{};
        {std::lock_guard lock(features_mutex);const auto it=features.find(handle);if(it!=features.end())feature=it->second;}
        if(feature.type==1) {
            if(const auto api=vulkan_command_device(cmd))vulkan_runtime_frame(api->device);
            NgxParameterOverlay parameters(p);
            // Creation-time dimensions can be absent from per-frame parameter maps.
            for(const auto& pair:{std::pair{"Width",feature.width},{"Height",feature.height},{"OutWidth",feature.out_width},{"OutHeight",feature.out_height}})
                if(!get_ui(p,pair.first))parameters.Set(pair.first,pair.second);
            unsigned value{};
            if(!try_get_ngx_integer_bits(p,"DLSS.Feature.Create.Flags",value))parameters.Set("DLSS.Feature.Create.Flags",feature.flags);
            if(!try_get_ngx_integer_bits(p,"PerfQualityValue",value))parameters.Set("PerfQualityValue",feature.quality);
            DlssFrameContract frame{};NgxResult result{};
            if(read_ngx_frame_contract(&parameters,reinterpret_cast<std::uintptr_t>(handle),feature.type,frame) &&
                evaluate_vulkan_backend(cmd,&parameters,frame,current_settings(),route.callbacks,result,handle,callback))return result;
        }
    }
    return route.callbacks.evaluate(cmd,handle,p,callback);
}
template<unsigned I> NgxResult release(NgxHandle* handle) {
    Scope scope;
    if(vulkan_ngx_private_depth==1){vulkan_release_view(reinterpret_cast<std::uintptr_t>(handle));unregister_stereo_view(reinterpret_cast<std::uintptr_t>(handle));std::lock_guard lock(features_mutex);features.erase(handle);}
    return routes[I].callbacks.release(handle);
}
template<unsigned I> bool install(HMODULE module) {
    auto& route=routes[I];route.module=module;
    const auto hook=[&](const char* name,auto replacement,auto& original) {
        const auto target=GetProcAddress(module,name);if(!target)return false;
        void* trampoline{};
        if(MH_CreateHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(replacement),&trampoline)!=MH_OK)return false;
        original=reinterpret_cast<std::remove_reference_t<decltype(original)>>(trampoline);
        if(MH_EnableHook(reinterpret_cast<void*>(target))!=MH_OK)return false;
        return true;
    };
    const bool c=hook("NVSDK_NGX_VULKAN_CreateFeature",create<I>,route.callbacks.create);
    hook("NVSDK_NGX_VULKAN_CreateFeature1",create1<I>,route.callbacks.create1);
    const bool e=hook("NVSDK_NGX_VULKAN_EvaluateFeature",evaluate<I>,route.callbacks.evaluate);
    const bool r=hook("NVSDK_NGX_VULKAN_ReleaseFeature",release<I>,route.callbacks.release);
    trace_event("Vulkan NGX hook route=%u create=%u evaluate=%u release=%u",I,c,e,r);
    return c&&e&&r;
}
}
void vulkan_install_ngx_hooks(HMODULE module) noexcept {
    static thread_local bool installing{};
    if(installing)return;
    struct Guard { bool& value; Guard(bool& v):value(v){value=true;} ~Guard(){value=false;} } guard(installing);
    if(!module || !GetProcAddress(module,"NVSDK_NGX_VULKAN_EvaluateFeature"))return;
    std::lock_guard lock(hooks_mutex);
    for(const auto& route:routes)if(route.module==module)return;
    unsigned slot{};while(slot<routes.size() && routes[slot].module)++slot;
    if(slot==routes.size())return;
    const auto mh=MH_Initialize();if(mh!=MH_OK && mh!=MH_ERROR_ALREADY_INITIALIZED)return;
    HMODULE pinned{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(module),&pinned))return;
    using Install=bool(*)(HMODULE);
    static constexpr Install installers[]={install<0>,install<1>,install<2>,install<3>,install<4>,install<5>,install<6>,install<7>};
    installers[slot](module);
}
}
