#include <Windows.h>
#include <cwchar>
#define VK_NO_PROTOTYPES
#include "../third_party/vulkan/include/vulkan/vk_layer.h"

namespace {
HMODULE self{};
INIT_ONCE once=INIT_ONCE_STATIC_INIT;
using Negotiate=VkResult(VKAPI_PTR*)(VkNegotiateLayerInterface*);
Negotiate negotiate{};
BOOL CALLBACK initialize(PINIT_ONCE,void*,void**) {
    wchar_t library[32768]{},executable[32768]{};
    auto length=GetModuleFileNameW(self,library,ARRAYSIZE(library));
    auto exe_length=GetModuleFileNameW(nullptr,executable,ARRAYSIZE(executable));
    if(!length || length>=ARRAYSIZE(library) || !exe_length || exe_length>=ARRAYSIZE(executable))return TRUE;
    auto* file=std::wcsrchr(library,L'\\');auto* exe=std::wcsrchr(executable,L'\\');
    if(!file || !exe)return TRUE;
    *file=*exe=0;
    // An implicit layer is discovered globally, but its processing host only
    // loads in the installation directory. No hooks or GPU discovery elsewhere.
    if(_wcsicmp(library,executable)!=0)return TRUE;
    const wchar_t* roots[]={L"CheekyFoveatedDLSS",L"OptiScaler\\plugins\\CheekyFoveatedDLSS"};
    for(unsigned i=0;i<2;++i) {
        wchar_t path[32768]{};
        if(swprintf_s(path,L"%s\\%s\\CheekyFoveatedDLSSHost.dll",library,roots[i])<0)continue;
        const auto host=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!host)continue;
        using Start=bool(*)(unsigned);
        const auto start=reinterpret_cast<Start>(GetProcAddress(host,"CheekyHost_StartVulkan"));
        if(!start || !start(i+1))return TRUE;
        if(swprintf_s(path,L"%s\\%s\\CheekyFoveatedDLSSRuntime.dll",library,roots[i])<0)return TRUE;
        const auto runtime=GetModuleHandleW(path);
        if(!runtime)return TRUE;
        negotiate=reinterpret_cast<Negotiate>(GetProcAddress(runtime,"CheekyVkNegotiateLoaderLayerInterfaceVersion"));
        HMODULE pinned{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(self),&pinned);
        return TRUE;
    }
    OutputDebugStringW(L"Cheeky Vulkan: matching host/runtime pair was not found.\n");
    return TRUE;
}
}
extern "C" __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL CheekyVulkanNegotiate(VkNegotiateLayerInterface* info) {
    if(!info)return VK_ERROR_INITIALIZATION_FAILED;
    InitOnceExecuteOnce(&once,initialize,nullptr,nullptr);
    return negotiate?negotiate(info):VK_ERROR_INITIALIZATION_FAILED;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){self=module;DisableThreadLibraryCalls(module);}return TRUE;
}
