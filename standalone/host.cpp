#include "host_api.hpp"
#include "vulkan_overlay.hpp"
#include "runtime_host_api.hpp"
#include "overlay.hpp"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <MinHook.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>
#include <utility>

using Microsoft::WRL::ComPtr;
namespace {
HMODULE module{};
std::mutex start_mutex, hooks_mutex, frame_mutex;
std::mutex log_mutex;
HANDLE host_log{INVALID_HANDLE_VALUE};
std::atomic<bool> started{};
std::uint32_t active_host{};
std::uint64_t attachment{};
CheekyRuntimeTickFn runtime_tick{};
CheekyRuntimeCommandFn runtime_command{};
CheekyRuntimeSnapshotFn runtime_snapshot{};
CheekyRuntimeDetachFn runtime_detach{};
thread_local bool internal{};
struct InternalScope { bool prior{internal}; InternalScope() { internal = true; } ~InternalScope() { internal = prior; } };
struct Hook { void* target; void* handler; };
std::vector<Hook> hooks;
// A hook's continuation belongs to its entry point, not the object's current
// vtable. Other injectors may replace that table and forward back through us.
constexpr std::size_t max_method_hooks=128;
std::array<std::atomic<void*>,max_method_hooks> continuations{};
template<auto Handler,class Signature> struct MethodEntry;
template<auto Handler,class Result,class... Args>
struct MethodEntry<Handler,Result(STDMETHODCALLTYPE*)(Args...)> {
    using Function=Result(STDMETHODCALLTYPE*)(Args...);
    template<std::size_t Index> static Result STDMETHODCALLTYPE call(Args... args) {
        const auto next=reinterpret_cast<Function>(continuations[Index].load(std::memory_order_acquire));
        return Handler(next,args...);
    }
    template<std::size_t... Index> static auto entries(std::index_sequence<Index...>) {
        return std::array<Function,sizeof...(Index)>{&call<Index>...};
    }
};
constexpr GUID queue_key{0x1a149631,0x8908,0x4f69,{0xb8,0x35,0x75,0xf9,0x01,0xb2,0x98,0x31}};
constexpr GUID color_key{0x721c2760,0x327d,0x46cf,{0x94,0x82,0x55,0x3d,0x2e,0xf9,0x13,0xe6}};
// Deliberately not the presentation queue: only used to discover native observer
// methods after late attach. The runtime tracks the actual submitted list queues.
ComPtr<ID3D12Device> observer_device;
ComPtr<ID3D12CommandQueue> observer_queue;

void log_host(const char* message) noexcept {
    std::lock_guard lock(log_mutex);
    OutputDebugStringA(message);
    if (host_log == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME time{}; GetLocalTime(&time);
    char line[1024]{};
    const auto count=snprintf(line,sizeof(line),"%02u:%02u:%02u.%03u %s\r\n",time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,message);
    if (count>0) { DWORD written{}; WriteFile(host_log,line,static_cast<DWORD>((std::min)(static_cast<std::size_t>(count),sizeof(line)-1)),&written,nullptr); }
}

void* method(void* object, unsigned slot) { return (*static_cast<void***>(object))[slot]; }
template<class Signature,auto Handler> bool hook_method(void* object, unsigned slot) {
    const auto target = method(object, slot);
    std::lock_guard lock(hooks_mutex);
    const auto handler=reinterpret_cast<void*>(Handler);
    for (const auto& hook : hooks) if (hook.target == target) return hook.handler==handler;
    if(hooks.size()==max_method_hooks)return false;
    const auto index=hooks.size();
    static const auto entries=MethodEntry<Handler,Signature>::entries(std::make_index_sequence<max_method_hooks>{});
    const auto detour=reinterpret_cast<void*>(entries[index]);
    void* trampoline{};
    if (MH_CreateHook(target, detour, &trampoline) != MH_OK) return false;
    // Publish before enabling: another game thread can enter immediately.
    continuations[index].store(trampoline,std::memory_order_release);
    hooks.push_back({target, handler});
    if (MH_EnableHook(target) == MH_OK) return true;
    hooks.pop_back(); MH_RemoveHook(target); return false;
}
ComPtr<ID3D12CommandQueue> chain_queue(IDXGISwapChain* chain) {
    ComPtr<ID3D12CommandQueue> result;
    UINT bytes = sizeof(ID3D12CommandQueue*);
    chain->GetPrivateData(queue_key, &bytes, result.GetAddressOf());
    return result;
}
void tick(IDXGISwapChain* chain, ID3D12CommandQueue* presentation_queue) {
    ComPtr<ID3D11Device> d11;
    if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&d11)))) {
        runtime_tick(attachment, 0, d11.Get(), nullptr);
        return;
    }
    ComPtr<ID3D12Device> d12;
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&d12)))) return;
    if (!presentation_queue) {
        if (observer_device.Get() != d12.Get()) {
            observer_queue.Reset(); observer_device = d12;
            D3D12_COMMAND_QUEUE_DESC desc{};
            d12->CreateCommandQueue(&desc, IID_PPV_ARGS(&observer_queue));
        }
        runtime_tick(attachment, 1, d12.Get(), observer_queue.Get());
    } else runtime_tick(attachment, 1, d12.Get(), presentation_queue);
}
void present_frame(IDXGISwapChain* chain, UINT flags) noexcept {
    if (internal || !started || (flags & DXGI_PRESENT_TEST)) return;
    InternalScope guard;
    try {
        std::lock_guard lock(frame_mutex);
        auto queue = chain_queue(chain);
        tick(chain, queue.Get());
        UINT color{}, size=sizeof(color);
        if (SUCCEEDED(chain->GetPrivateData(color_key,&size,&color)))
            cheeky::standalone::overlay_set_color_space(chain,color);
        cheeky::standalone::OverlayRuntime ui{attachment, runtime_command, runtime_snapshot,
            active_host == 2 ? "OptiScaler" : "Standalone"};
        cheeky::standalone::overlay_present(chain, queue.Get(), ui);
        static const char* last_status{};
        const auto status=cheeky::standalone::overlay_status();
        if (status!=last_status) { log_host(status); last_status=status; }
    } catch (...) { log_host("Standalone presentation failed; game presentation continues"); }
}
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*);
using ColorSpaceFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,DXGI_COLOR_SPACE_TYPE);
HRESULT color_space(ColorSpaceFn next, IDXGISwapChain3* chain, DXGI_COLOR_SPACE_TYPE value) {
    auto result = next ? next(chain,value) : E_UNEXPECTED;
    if (SUCCEEDED(result)) {
        const auto color=static_cast<UINT>(value);
        chain->SetPrivateData(color_key,sizeof(color),&color);
    }
    return result;
}
HRESULT present(PresentFn next, IDXGISwapChain* chain, UINT sync, UINT flags) {
    present_frame(chain, flags);
    InternalScope guard;
    return next ? next(chain, sync, flags) : E_UNEXPECTED;
}
HRESULT present1(Present1Fn next, IDXGISwapChain1* chain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    present_frame(chain, flags);
    InternalScope guard;
    return next ? next(chain, sync, flags, params) : E_UNEXPECTED;
}
HRESULT resize(ResizeFn next, IDXGISwapChain* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    if (internal) return next ? next(chain,count,width,height,format,flags) : E_UNEXPECTED;
    InternalScope guard;
    std::lock_guard lock(frame_mutex);
    cheeky::standalone::overlay_before_resize(chain);
    return next ? next(chain, count, width, height, format, flags) : E_UNEXPECTED;
}
HRESULT resize1(Resize1Fn next, IDXGISwapChain3* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format,
        UINT flags, const UINT* masks, IUnknown* const* queues) {
    if (internal) return next ? next(chain,count,width,height,format,flags,masks,queues) : E_UNEXPECTED;
    InternalScope guard;
    std::lock_guard lock(frame_mutex);
    cheeky::standalone::overlay_before_resize(chain);
    auto result = next ? next(chain,count,width,height,format,flags,masks,queues) : E_UNEXPECTED;
    if (SUCCEEDED(result) && queues) {
        DXGI_SWAP_CHAIN_DESC desc{}; chain->GetDesc(&desc);
        const auto actual_count = count ? count : desc.BufferCount;
        ComPtr<ID3D12CommandQueue> queue;
        bool same = actual_count != 0 && queues[0] && SUCCEEDED(queues[0]->QueryInterface(IID_PPV_ARGS(&queue)));
        for (UINT i=1; same && i<actual_count; ++i) {
            ComPtr<ID3D12CommandQueue> other;
            same = queues[i] && SUCCEEDED(queues[i]->QueryInterface(IID_PPV_ARGS(&other))) && other.Get() == queue.Get();
        }
        chain->SetPrivateDataInterface(queue_key, same ? queue.Get() : nullptr);
    }
    return result;
}
bool observe_chain(IDXGISwapChain* chain, IUnknown* source) {
    if (!chain) return false;
    if (source) {
        ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&queue))) && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            chain->SetPrivateDataInterface(queue_key, queue.Get());
    }
    bool ok = hook_method<PresentFn,present>(chain,8);
    ok = hook_method<ResizeFn,resize>(chain,13) && ok;
    ComPtr<IDXGISwapChain1> chain1;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1))))
        ok = hook_method<Present1Fn,present1>(chain1.Get(),22) && ok;
    ComPtr<IDXGISwapChain3> chain3;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3))))
        ok = hook_method<Resize1Fn,resize1>(chain3.Get(),39) && ok;
    if (chain3) ok = hook_method<ColorSpaceFn,color_space>(chain3.Get(),38) && ok;
    if (!internal && started) {
        std::lock_guard lock(frame_mutex); auto queue = chain_queue(chain); tick(chain,queue.Get());
        char text[192]{};
        snprintf(text,sizeof(text),"Observed swap chain=%p direct queue=%p presentation hooks=%s",static_cast<void*>(chain),static_cast<void*>(queue.Get()),ok ? "ready" : "incomplete");
        log_host(text);
    }
    return ok;
}
using CreateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*,IUnknown*,DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**);
using HwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
using CoreFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,IUnknown*,const DXGI_SWAP_CHAIN_DESC1*,IDXGIOutput*,IDXGISwapChain1**);
using CompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,const DXGI_SWAP_CHAIN_DESC1*,IDXGIOutput*,IDXGISwapChain1**);
HRESULT create_chain(CreateFn next,IDXGIFactory* f,IUnknown* d,DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** out) {
    if (!internal && desc) cheeky::standalone::overlay_before_create(desc->OutputWindow);
    auto result = next ? next(f,d,desc,out) : E_UNEXPECTED;
    if (SUCCEEDED(result) && out && *out) { try { observe_chain(*out,d); } catch (...) {} }
    return result;
}
HRESULT create_hwnd(HwndFn next,IDXGIFactory2* f,IUnknown* d,HWND w,const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* output,IDXGISwapChain1** out) {
    if (!internal) cheeky::standalone::overlay_before_create(w);
    auto result = next ? next(f,d,w,desc,fullscreen,output,out) : E_UNEXPECTED;
    if (SUCCEEDED(result) && out && *out) { try { observe_chain(*out,d); } catch (...) {} }
    return result;
}
HRESULT create_core(CoreFn next,IDXGIFactory2* f,IUnknown* d,IUnknown* w,const DXGI_SWAP_CHAIN_DESC1* desc,IDXGIOutput* output,IDXGISwapChain1** out) {
    auto result = next ? next(f,d,w,desc,output,out) : E_UNEXPECTED;
    if (SUCCEEDED(result) && out && *out) { try { observe_chain(*out,d); } catch (...) {} }
    return result;
}
HRESULT create_composition(CompositionFn next,IDXGIFactory2* f,IUnknown* d,const DXGI_SWAP_CHAIN_DESC1* desc,IDXGIOutput* output,IDXGISwapChain1** out) {
    auto result = next ? next(f,d,desc,output,out) : E_UNEXPECTED;
    if (SUCCEEDED(result) && out && *out) { try { observe_chain(*out,d); } catch (...) {} }
    return result;
}
bool install_graphics_hooks() {
    InternalScope guard;
    const auto mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) return false;
    // Use an absolute system module, never recursively load our dxgi proxy.
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system,MAX_PATH)) return false;
    auto dxgi = LoadLibraryExW((std::filesystem::path(system)/L"dxgi.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi) return false;
    auto factory_fn = reinterpret_cast<HRESULT(WINAPI*)(REFIID,void**)>(GetProcAddress(dxgi,"CreateDXGIFactory1"));
    ComPtr<IDXGIFactory2> factory;
    if (!factory_fn || FAILED(factory_fn(IID_PPV_ARGS(&factory)))) return false;
    if (!hook_method<CreateFn,create_chain>(factory.Get(),10) ||
        !hook_method<HwndFn,create_hwnd>(factory.Get(),15) ||
        !hook_method<CoreFn,create_core>(factory.Get(),16) ||
        !hook_method<CompositionFn,create_composition>(factory.Get(),24)) return false;
    // Discover Present implementations for swapchains created before ASI attach.
    // The window is hidden, created and destroyed on this initialization thread.
    HWND window = CreateWindowExW(0,L"STATIC",L"Cheeky graphics discovery",WS_POPUP,0,0,16,16,nullptr,nullptr,module,nullptr);
    if (!window) return false;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width=16; desc.Height=16; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<ID3D11Device> d11;
    bool observed{};
    if (SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,nullptr)) ||
        SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,nullptr))) {
        ComPtr<IDXGISwapChain1> chain;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(d11.Get(),window,&desc,nullptr,nullptr,&chain)))
            observed = observe_chain(chain.Get(),d11.Get());
    }
    ComPtr<ID3D12Device> d12;
    if (SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12)))) {
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        if (SUCCEEDED(d12->CreateCommandQueue(&qdesc,IID_PPV_ARGS(&queue)))) {
            ComPtr<IDXGISwapChain1> chain;
            if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&chain)))
                observed = observe_chain(chain.Get(),queue.Get()) || observed;
        }
    }
    DestroyWindow(window);
    return observed;
}
template<class T> bool load(HMODULE dll,const char* name,T& out) {
    out=reinterpret_cast<T>(GetProcAddress(dll,name)); return out!=nullptr;
}
}

bool start_host(std::uint32_t host,bool vulkan) {
    try {
        if (host!=1 && host!=2) return false;
        std::lock_guard lock(start_mutex);
        if (started) return host==active_host;
        HMODULE resident{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&start_host),&resident)) return false;
        wchar_t path[32768]{};
        const auto length=GetModuleFileNameW(module,path,32768);
        if (!length || length>=32768) return false;
        const auto directory=std::filesystem::path(path).parent_path();
        if (host_log==INVALID_HANDLE_VALUE) host_log=CreateFileW((directory/L"CheekyFoveatedDLSS-Host.log").c_str(),FILE_APPEND_DATA,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        log_host(host==2 ? "Initializing OptiScaler host" : "Initializing standalone host");
        auto runtime=LoadLibraryExW((directory/L"CheekyFoveatedDLSSRuntime.dll").c_str(),nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        CheekyRuntimeStartFn start{};
        if (!runtime || !load(runtime,"CheekyRuntime_Start",start) || !load(runtime,"CheekyRuntime_Tick",runtime_tick) ||
            !load(runtime,"CheekyRuntime_Command",runtime_command) || !load(runtime,"CheekyRuntime_Snapshot",runtime_snapshot) ||
            !load(runtime,"CheekyRuntime_Detach",runtime_detach)) { log_host("Runtime missing or API incompatible"); return false; }
        CheekyRuntimeStart input;
        const auto config=directory.wstring();
        input.config_directory=config.c_str(); input.attachment=&attachment;
        input.host=static_cast<CheekyRuntimeHost>(host);
        if (!start(&input)) { log_host("Resident runtime refused startup; see its log"); return false; }
        if (!vulkan && !install_graphics_hooks()) { runtime_detach(attachment); attachment=0; log_host("Graphics hook discovery failed"); return false; }
        active_host=host; started=true;
        log_host("Host ready; waiting for game graphics. F8 opens settings.");
        return true;
    } catch (...) {
        if (runtime_detach && attachment && !started) { runtime_detach(attachment); attachment=0; }
        log_host("Host startup exception contained");
        return false;
    }
}
extern "C" __declspec(dllexport) bool CheekyHost_Start(std::uint32_t host) {return start_host(host,false);}
extern "C" __declspec(dllexport) void __cdecl CheekyOpenXRMenuLogDiagnostic(const char* message) noexcept {
    if (message) log_host(message);
}
extern "C" __declspec(dllexport) bool CheekyHost_StartVulkan(std::uint32_t host) {return start_host(host,true);}
extern "C" __declspec(dllexport) VkResult CheekyHost_VulkanPresent(const CheekyVulkanPresent* present) {
    if(!present || present->size!=sizeof(*present) || !present->next || !present->present)return VK_ERROR_INITIALIZATION_FAILED;
    const cheeky::standalone::OverlayRuntime api{attachment,runtime_command,runtime_snapshot,active_host==2?"OptiScaler":"Standalone"};
    return cheeky::standalone::overlay_vulkan_present(*present,api);
}
extern "C" __declspec(dllexport) void CheekyHost_VulkanDestroy(VkDevice device,VkSwapchainKHR chain) {
    cheeky::standalone::overlay_vulkan_destroy(device,chain);
}
extern "C" __declspec(dllexport) bool CheekyHost_Snapshot(char* out,std::uint32_t capacity) {
    return started && runtime_snapshot && runtime_snapshot(out,capacity);
}
extern "C" __declspec(dllexport) bool CheekyHost_Command(const char* command) {
    return started && runtime_command && runtime_command(attachment,command);
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) { module=instance; DisableThreadLibraryCalls(instance); }
    // Code and hooks are process-resident. No waits/graphics/loader work here.
    return TRUE;
}
