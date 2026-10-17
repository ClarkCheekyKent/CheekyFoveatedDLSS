#pragma once
#include <MinHook.h>
#include <mutex>
#include <functional>
#include <map>
// A real MSVC RTTI/vtable fixture for the existing UEVR VRRuntime ABI. The
// inline hook must intercept an actual virtual call, then preserve forwarding.
struct VRRuntime {
    virtual ~VRRuntime()=default;
    virtual unsigned name(){return 1;}
    virtual unsigned ready(){return 2;}
    virtual unsigned char type(){return 0;}
    virtual unsigned destroy(){return 4;}
    virtual unsigned synchronize(){return 5;}
    virtual unsigned fix(){return 6;}
    virtual unsigned poses(){return 7;}
    virtual unsigned extent(){return 8;}
    virtual unsigned events(){return 9;}
    virtual unsigned width(){return 10;}
    virtual unsigned height(){return 11;}
    virtual unsigned matrices(){return 12;}
    virtual unsigned input(){return 13;}
    virtual void enqueue_render_poses(std::uint32_t)=0;
    virtual unsigned depth(){return 15;}
    virtual unsigned cylinder(){return 16;}
    virtual unsigned config_load(){return 17;}
    virtual unsigned config_save(){return 18;}
    virtual unsigned ui(){return 19;}
    virtual unsigned reset(){return 20;}
    virtual unsigned game(){return 21;}
    virtual unsigned render(){return 22;}
    virtual unsigned rhi(){return 23;}
};
namespace runtimes {
struct OpenXR final:VRRuntime {
    std::mutex assignment;
    volatile std::uint32_t frame{};bool has_frame{};
    unsigned char type() override{return 1;}
    __declspec(noinline) void enqueue_render_poses(std::uint32_t value) override {
        std::lock_guard lock(assignment);frame=value;has_frame=true;
    }
};
}
inline runtimes::OpenXR fg_test_runtime;
inline std::map<int,void*> fg_test_hook_targets;
inline unsigned fg_test_hook_installs{},fg_test_hook_removals{};
inline int fg_test_install_hook(void* target,void* detour,void** original) {
    auto status=MH_Initialize();if(status!=MH_OK&&status!=MH_ERROR_ALREADY_INITIALIZED)return -1;
    if(MH_CreateHook(target,detour,original)!=MH_OK)return -1;
    if(MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);return -1;}
    const auto id=37+int(fg_test_hook_installs++);fg_test_hook_targets[id]=target;return id;
}
inline void fg_test_remove_hook(int id) {
    if(auto it=fg_test_hook_targets.find(id);it!=fg_test_hook_targets.end()){
        MH_RemoveHook(it->second);fg_test_hook_targets.erase(it);++fg_test_hook_removals;
    }
}
inline void fg_test_render_frame(std::uint64_t frame) {
    auto* base=static_cast<VRRuntime*>(&fg_test_runtime);
    auto function=reinterpret_cast<void(*)(void*,std::uint32_t)>((*reinterpret_cast<void***>(base))[14]);
    function(base,std::uint32_t(frame-1));
    if(fg_test_runtime.frame!=frame-1)throw std::runtime_error("Frame hook did not forward the original UEVR call");
}
inline const char* volatile fg_test_last_log{};
__declspec(noinline) inline void fg_test_log(const char* value){fg_test_last_log=value;}
inline auto volatile fg_test_logger=&fg_test_log;
inline std::function<void()> fg_test_render_body;
inline std::function<void()> fg_test_before_render_body;
inline unsigned fg_test_command_frame{};
__declspec(noinline) inline void* fg_test_rhi_command(void*,void*,void*,void*,void*,void*,void*,void*,void*,void*,void*,void* last) {
    fg_test_logger("[ISceneViewExtension] Successfully hijacked command list! {}");
    fg_test_logger("[ISceneViewExtension] Command list frame count: {}");
    fg_test_logger("[ISceneViewExtension] Original vtable: {:x}");
    if(fg_test_before_render_body)fg_test_before_render_body();
    if(fg_test_render_body)fg_test_render_body(); // DLSS happens BEFORE the pose marker, as in UEVR.
    fg_test_render_frame(fg_test_command_frame);
    return last;
}
inline void fg_test_execute_frame(unsigned frame){
    fg_test_command_frame=frame;auto volatile invoke=&fg_test_rhi_command;
    auto last=reinterpret_cast<void*>(0xc);
    if(invoke(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,last)!=last)
        throw std::runtime_error("RHI hook did not preserve stack arguments and return value");
}
