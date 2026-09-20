#include "host_api.hpp"
#include "late_attach_tests.hpp"
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool b,const char* why) { if (!b) throw std::runtime_error(why); }
void check(HRESULT hr,const char* why) { require(SUCCEEDED(hr),why); }
template<class T> T proc(HMODULE m,const char* name) {
    auto result=reinterpret_cast<T>(GetProcAddress(m,name)); require(result!=nullptr,name); return result;
}
ComPtr<IDXGISwapChain1> chain;
using HwndFn=HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
HwndFn chained_hwnd{};
HRESULT STDMETHODCALLTYPE wrapper_hwnd(IDXGIFactory2* factory,IUnknown* device,HWND window,const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* output,IDXGISwapChain1** result) {
    return chained_hwnd(factory,device,window,desc,fullscreen,output,result);
}
bool (*send)(const char*);
void command(const char* text) { require(send(text),"Host command accepted"); }
void frame() { check(chain->Present(0,0),"Host presentation callback"); }
std::string snapshot(CheekyHostSnapshotFn get) {
    std::array<char,32768> out{}; require(get(out.data(),static_cast<std::uint32_t>(out.size())),"Host snapshot"); return out.data();
}
}
int main(int argc,char** argv) {
    try {
        const std::string mode=argc>1 ? argv[1] : "dx12";
        const bool dx11=mode.find("dx11")!=std::string::npos;
        const bool chained_table=mode.find("method-chain")!=std::string::npos;
        const bool streamline=mode.find("streamline")!=std::string::npos;
        const bool c_callback=mode.ends_with("-c");
        const unsigned host=argc>2 && std::string(argv[2])=="optiscaler" ? 2U : 1U;
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
        const auto bin=std::filesystem::path(executable).parent_path();
        const auto root=bin/"standalone-host-tests"/(mode+"-"+std::to_string(host)+"-"+std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        for (const auto* name : {L"CheekyFoveatedDLSSHost.dll",L"CheekyFoveatedDLSSRuntime.dll"})
            std::filesystem::copy_file(bin/"CheekyFoveatedDLSS"/name,root/name);
        if(!chained_table)std::filesystem::copy_file(bin/"test-fixtures"/"nvngx_dlss.dll",root/"nvngx_dlssnr.dll");
        // Exercise the direct DX11 route here. A separate suite exercises its
        // optional DX12 transport and NR; settings must not select it implicitly.
        std::ofstream(root/"CheekyFoveatedDLSS.ini") << "[CheekyFoveatedDLSS]\nD3D11D3D12Transport=false\n";
        ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory");
        ComPtr<IDXGIAdapter> warp; check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP adapter");
        ComPtr<ID3D12Device> d12; ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D11Device> d11;
        if (dx11) check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,nullptr),"DX11");
        else {
            check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12)),"DX12");
            D3D12_COMMAND_QUEUE_DESC desc{}; check(d12->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"Queue");
        }
        if(!chained_table)prepare_late_attach_test(bin,d11.Get(),d12.Get(),queue.Get(),c_callback,streamline);
        auto dll=LoadLibraryExW((root/L"CheekyFoveatedDLSSHost.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(dll!=nullptr,"Load actual standalone host");
        const auto start=proc<CheekyHostStartFn>(dll,"CheekyHost_Start");
        const auto get=proc<CheekyHostSnapshotFn>(dll,"CheekyHost_Snapshot");
        send=proc<bool(*)(const char*)>(dll,"CheekyHost_Command");
        require(start(host),"Start actual standalone host");
        require(start(host),"Start idempotent"); require(!start(host==1 ? 2 : 1),"Reject conflicting loader");
        HWND window=CreateWindowExW(0,L"STATIC",L"Cheeky host test",WS_POPUP,0,0,320,240,nullptr,nullptr,nullptr,nullptr);
        require(window!=nullptr,"Test window");
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=320; desc.Height=240; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2;
        desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // A second mod can replace an object's vtable entry while forwarding
        // to the native method Cheeky already detoured. The hook must select
        // its trampoline by its entry point, not the now-replaced vtable entry.
        auto** native_table=*reinterpret_cast<void***>(factory.Get());
        std::array<void*,28> wrapper_table{};
        if(chained_table) {
            std::copy_n(native_table,wrapper_table.size(),wrapper_table.begin());
            chained_hwnd=reinterpret_cast<HwndFn>(native_table[15]);wrapper_table[15]=reinterpret_cast<void*>(&wrapper_hwnd);
            *reinterpret_cast<void***>(factory.Get())=wrapper_table.data();
        }
        const auto created=factory->CreateSwapChainForHwnd(dx11 ? static_cast<IUnknown*>(d11.Get()) : queue.Get(),window,&desc,nullptr,nullptr,&chain);
        if(chained_table)*reinterpret_cast<void***>(factory.Get())=native_table;
        std::printf("Game swapchain creation: 0x%08lX\n",static_cast<unsigned long>(created));
        check(created,"Observed game swapchain");
        if(chained_table){
            frame();check(chain->ResizeBuffers(2,400,300,DXGI_FORMAT_UNKNOWN,0),"Chained host resize");frame();
            chain.Reset();DestroyWindow(window);
            std::puts("PASS: chained factory vtable preserves creation, presentation and resize");return 0;
        }
        frame();
        auto state=snapshot(get);
        require(state.find("\"ready\":true")!=state.npos,"Graphics independently discovered");
        require(state.find(host==2 ? "\"host\":\"optiscaler\"" : "\"host\":\"standalone\"")!=state.npos,"Correct host diagnostics");
        command("1\n2\nset\nEnabled=true\nPeripheralDlaa=false\nAutoStereoAlignment=false\nCenterMode=0\nNrEnabled=false");
        // Let discovery's bounded late-load stability window expire.
        Sleep(300);
        verify_late_attach_test(get,command,frame);
        check(chain->ResizeBuffers(2,400,300,DXGI_FORMAT_UNKNOWN,0),"Resize without retained backbuffers"); frame();
        ComPtr<IDXGISwapChain3> chain3;
        if (!dx11 && SUCCEEDED(chain.As(&chain3))) {
            UINT masks[]{1,1}; IUnknown* queues[]{queue.Get(),queue.Get()};
            check(chain3->ResizeBuffers1(2,320,240,DXGI_FORMAT_UNKNOWN,0,masks,queues),"ResizeBuffers1 queues"); frame();
        }
        chain3.Reset(); chain.Reset(); DestroyWindow(window);
        std::printf("PASS: actual %s host %s processing, presentation and resize\n",host==2 ? "OptiScaler" : "standalone",mode.c_str());
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n",error.what()); return 1; }
}
