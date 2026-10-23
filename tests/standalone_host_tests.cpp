#include "host_api.hpp"
#include "late_attach_tests.hpp"
#include "mock_ngx_parameters.hpp"
#include "../shared/cheeky_fg_bridge.hpp"
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
void verify_fg(ID3D12Device* device, ID3D12CommandQueue* queue) {
    using namespace cheeky::foveated_dlss;
    const auto runtime = GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
    const auto needs_hook = proc<bool(*)()>(runtime,"CheekyFG_NeedsUEVRFrameSourceV1");
    const auto complete = proc<CheekyFgCompleteHooklessFrameFn>(runtime,"CheekyFG_CompleteHooklessFrameV1");
    const auto acquire = proc<CheekyFgAcquireFn>(runtime,"CheekyFG_AcquirePair");
    const auto release = proc<CheekyFgReleaseFn>(runtime,"CheekyFG_ReleasePair");
    command("1\n200\nset\nEnabled=false\nNrEnabled=false\nFgEnabled=true");
    require(!needs_hook(),"Standalone matches FG guide pairs at xrEndFrame without UEVR hooks");
    frame();
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"FG allocator");
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"FG list");
    auto texture = [&](DXGI_FORMAT format) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=desc.Height=64; desc.DepthOrArraySize=desc.MipLevels=1; desc.SampleDesc.Count=1; desc.Format=format;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&result)),"FG texture");
        return result;
    };
    auto depth=texture(DXGI_FORMAT_R32_FLOAT), motion=texture(DXGI_FORMAT_R32G32_FLOAT);
    auto color=texture(DXGI_FORMAT_R8G8B8A8_UNORM), output=texture(DXGI_FORMAT_R8G8B8A8_UNORM);
    MockNgxParameters p;
    p.Set("Width",64u); p.Set("Height",64u); p.Set("OutWidth",64u); p.Set("OutHeight",64u);
    p.Set("DLSS.Feature.Create.Flags",14u); p.Set("Depth",depth.Get()); p.Set("MotionVectors",motion.Get());
    p.Set("Color",color.Get()); p.Set("Output",output.Get());
    const auto ngx=GetModuleHandleW(L"nvngx_dlss.dll");
    using Create=NgxResult(*)(ID3D12GraphicsCommandList*,unsigned,NgxParameters*,NgxHandle**);
    using Evaluate=NgxResult(*)(ID3D12GraphicsCommandList*,const NgxHandle*,const NgxParameters*,NgxProgressCallback);
    const auto evaluate=proc<Evaluate>(ngx,"NVSDK_NGX_D3D12_EvaluateFeature");
    NgxHandle* handle{}; require(ngx_succeeded(proc<Create>(ngx,"NVSDK_NGX_D3D12_CreateFeature")(list.Get(),1,&p,&handle)),"FG native feature");
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"FG fence");
    std::uint64_t serial{};
    auto guides = [&](unsigned eyes) {
        for(unsigned i=0;i<eyes;++i) require(ngx_succeeded(evaluate(list.Get(),handle,&p,nullptr)),"Intercepted FG guide evaluation");
        check(list->Close(),"FG list close"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
        check(queue->Signal(fence.Get(),++serial),"FG signal");
        const auto deadline=GetTickCount64()+10000;
        while(fence->GetCompletedValue()<serial && GetTickCount64()<deadline) Sleep(1);
        require(fence->GetCompletedValue()>=serial,"FG GPU completion");
        check(allocator->Reset(),"FG allocator reset"); check(list->Reset(allocator.Get(),nullptr),"FG list reset");
    };
    auto pair = [&] {
        const auto token=complete(1); require(token!=0,"Standalone xrEndFrame claims a submitted stereo interval");
        CheekyFgPair value; value.capture_token=token;
        require(acquire(&value)&&!value.enable_frame_id&&!value.render_frame&&value.release_point==3&&
            value.eyes[0].evaluation+1==value.eyes[1].evaluation,"Standalone frame-free stereo pair preserves scheduling");
        release(&value);
    };
    guides(2); pair(); require(!complete(1),"Consumed stereo guides cannot repeat");
    guides(1); require(!complete(1),"Partial stereo frame falls back");
    guides(3); require(!complete(1),"Ambiguous stereo frame falls back");
    guides(2); pair();
    command("1\n201\nset\nFgEnabled=false"); frame();
    guides(2); require(!complete(1),"Disabled FG cannot capture guides");
    command("1\n202\nset\nFgEnabled=true"); frame();
    guides(2); pair();
    command("1\n203\ndefaults_fg\n"); frame();
    require(!needs_hook()&&!complete(1),"FG defaults retain automatic standalone matching and disable generation");
    require(ngx_succeeded(proc<NgxResult(*)(NgxHandle*)>(ngx,"NVSDK_NGX_D3D12_ReleaseFeature")(handle)),"FG release feature");
    check(list->Close(),"Close unused FG recording");
    std::puts("PASS: standalone FG capture without UEVR hooks, stereo rejection/recovery, disable/re-enable and defaults");
}
void verify_fg11(ID3D11Device* device) {
    using namespace cheeky::foveated_dlss;
    const auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
    const auto complete=proc<CheekyFgCompleteHooklessFrameFn>(runtime,"CheekyFG_CompleteHooklessFrameV1");
    const auto acquire=proc<CheekyFgAcquireFn>(runtime,"CheekyFG_AcquirePair");
    const auto release=proc<CheekyFgReleaseFn>(runtime,"CheekyFG_ReleasePair");
    command("1\n300\nset\nFgEnabled=true\nFgFoveated=false\nD3D11D3D12Transport=false\nEnabled=true\nWidth=0.5\nHeight=0.5\nNrEnabled=false\nPeripheralDlaa=false");
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    auto texture=[&](DXGI_FORMAT format,unsigned size=128) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=size;desc.ArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;
        desc.Format=format;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> result;check(device->CreateTexture2D(&desc,nullptr,&result),"DX11 FG texture");return result;
    };
    auto color=texture(DXGI_FORMAT_R8G8B8A8_UNORM,64),output=texture(DXGI_FORMAT_R8G8B8A8_UNORM);
    auto depth=texture(DXGI_FORMAT_R32_FLOAT,64),motion=texture(DXGI_FORMAT_R16G16_FLOAT);
    MockNgxParameters p;p.Set("Width",64u);p.Set("Height",64u);p.Set("OutWidth",128u);p.Set("OutHeight",128u);
    p.Set("DLSS.Feature.Create.Flags",0x49u);p.Set("PerfQualityValue",2u); // ACC: input depth, output-resolution MV.
    p.Set("MV.Scale.X",1.f);p.Set("MV.Scale.Y",1.f);p.Set("Color",static_cast<ID3D11Resource*>(color.Get()));
    p.Set("Output",static_cast<ID3D11Resource*>(output.Get()));p.Set("Depth",static_cast<ID3D11Resource*>(depth.Get()));
    p.Set("MotionVectors",static_cast<ID3D11Resource*>(motion.Get()));
    const auto ngx=GetModuleHandleW(L"nvngx_dlss.dll");
    using Create=NgxResult(*)(ID3D11DeviceContext*,unsigned,NgxParameters*,NgxHandle**);
    using Evaluate=NgxResult(*)(ID3D11DeviceContext*,const NgxHandle*,const NgxParameters*,NgxProgressCallback);
    using Init=NgxResult(*)(unsigned long long,const wchar_t*,ID3D11Device*,const void*,unsigned);
    require(ngx_succeeded(proc<Init>(ngx,"NVSDK_NGX_D3D11_Init")(42,L".",device,nullptr,1)),"Observe DX11 NGX initialization for private transport");
    NgxHandle* handle{};require(ngx_succeeded(proc<Create>(ngx,"NVSDK_NGX_D3D11_CreateFeature")(context.Get(),1,&p,&handle)),"DX11 FG game feature");
    const auto evaluate=proc<Evaluate>(ngx,"NVSDK_NGX_D3D11_EvaluateFeature");
    ComPtr<ID3D11RenderTargetView> depth_rtv,motion_rtv;
    check(device->CreateRenderTargetView(depth.Get(),nullptr,&depth_rtv),"DX11 FG depth fixture RTV");
    check(device->CreateRenderTargetView(motion.Get(),nullptr,&motion_rtv),"DX11 FG output-resolution motion RTV");
    auto eyes=[&](unsigned count) {
        for(unsigned i=0;i<count;++i) {
            const float depth_value[]{i%2?.75f:.25f,0,0,0};context->ClearRenderTargetView(depth_rtv.Get(),depth_value);
            const float motion_value[]{i%2?-16.f:8.f,i%2?2.f:-4.f,0,0};context->ClearRenderTargetView(motion_rtv.Get(),motion_value);
            require(ngx_succeeded(evaluate(context.Get(),handle,&p,nullptr)),"DX11 transported FG evaluation");
        }
        context->Flush();frame(); // A desktop DX11 tick must retain the private guide queue.
    };
    auto pair=[&] {
        CheekyFgPair value;value.capture_token=complete(1);
        require(value.capture_token&&acquire(&value),"DX11 native evaluations provide an OpenXR stereo guide pair");
        require(value.eyes[0].depth_width==64&&value.eyes[1].motion_width==128&&!value.enable_frame_id,
            "DX11 FG keeps full-eye guides independently of the cropped SR resolution");
        require(value.eyes[0].prepared_guides&&value.eyes[1].prepared_guides&&
            value.eyes[0].depth->GetDesc().Format==DXGI_FORMAT_R32_FLOAT&&
            value.eyes[0].motion->GetDesc().Format==DXGI_FORMAT_R32G32_FLOAT,
            "Default prepared guides use 32-bit depth and normalized 32-bit motion");
        ComPtr<ID3D12Device> gpu;check(value.producer_queue->GetDevice(IID_PPV_ARGS(&gpu)),"DX11 FG snapshot device");
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        check(gpu->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"DX11 FG read allocator");
        check(gpu->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"DX11 FG read commands");
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        auto desc=value.eyes[0].depth->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        gpu->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes*2;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> read;check(gpu->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)),"DX11 FG readback");
        auto motion_desc=value.eyes[0].motion->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT motion_footprint{};UINT64 motion_bytes{};
        gpu->GetCopyableFootprints(&motion_desc,0,1,0,&motion_footprint,nullptr,nullptr,&motion_bytes);
        buffer.Width=motion_bytes*2;ComPtr<ID3D12Resource> motion_read;
        check(gpu->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&motion_read)),"DX11 FG motion readback");
        for(unsigned eye=0;eye<2;++eye) {
            auto* source=value.eyes[eye].depth;D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource=source;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1,&barrier);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=source;
            to.pResource=read.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;to.PlacedFootprint.Offset=eye*bytes;
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
            source=value.eyes[eye].motion;barrier.Transition.pResource=source;
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
            from.pResource=source;to.pResource=motion_read.Get();to.PlacedFootprint=motion_footprint;to.PlacedFootprint.Offset=eye*motion_bytes;
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
        }
        check(list->Close(),"DX11 FG close readback");ID3D12CommandList* commands[]{list.Get()};value.producer_queue->ExecuteCommandLists(1,commands);
        ComPtr<ID3D12Fence> fence;check(gpu->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"DX11 FG read fence");
        check(value.producer_queue->Signal(fence.Get(),1),"DX11 FG read signal");
        while(fence->GetCompletedValue()<1)Sleep(1);
        void* mapped{};D3D12_RANGE range{0,static_cast<SIZE_T>(bytes*2)};check(read->Map(0,&range,&mapped),"DX11 FG map depth snapshots");
        for(unsigned eye=0;eye<2;++eye)for(unsigned y:{0u,desc.Height-1})for(unsigned x:{0u,unsigned(desc.Width)-1}) {
            const auto* pixel=reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped)+eye*bytes+y*footprint.Footprint.RowPitch+x*4);
            require(*pixel==(eye?.75f:.25f),"DX11 FG full-depth corner pixels preserve each eye beyond the SR crop");
        }
        read->Unmap(0,nullptr);
        D3D12_RANGE motion_range{0,static_cast<SIZE_T>(motion_bytes*2)};
        check(motion_read->Map(0,&motion_range,&mapped),"DX11 FG motion map");
        for(unsigned eye=0;eye<2;++eye)for(unsigned y:{0u,motion_desc.Height-1})for(unsigned x:{0u,unsigned(motion_desc.Width)-1}) {
            const auto* pixel=reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped)+eye*motion_bytes+y*motion_footprint.Footprint.RowPitch+x*8);
            const auto& guides=value.eyes[eye];
            require(pixel[0]==(eye?-.125f:.0625f)/guides.focus_width&&pixel[1]==(eye?.015625f:-.03125f)/guides.focus_height,
                "Prepared ACC motion preserves each eye's signed vectors normalized to its FG region, independently of SR/depth crop");
        }
        motion_read->Unmap(0,nullptr);
        release(&value);
    };
    eyes(2);pair();eyes(1);require(!complete(1),"DX11 incomplete stereo falls back");eyes(2);pair();
    command("1\n301\nset\nFgFoveated=true");eyes(2);pair();
    command("1\n302\nset\nEnabled=false");eyes(2);pair();
    command("1\n303\nset\nFgEnabled=false");eyes(2);require(!complete(1),"DX11 FG disabled capture");
    command("1\n304\nset\nFgEnabled=true");eyes(2);pair();
    command("1\n305\ndefaults_fg\n");frame();
    require(ngx_succeeded(proc<NgxResult(*)(NgxHandle*)>(ngx,"NVSDK_NGX_D3D11_ReleaseFeature")(handle)),"DX11 FG game feature release");
    std::puts("PASS: DX11 FG automatic transport, full-eye guides, native stereo matching, SR off and disabled capture");
}
}
int main(int argc,char** argv) {
    try {
        const std::string mode=argc>1 ? argv[1] : "dx12";
        const bool dx11=mode.find("dx11")!=std::string::npos;
        const bool realvr=mode.starts_with("realvr-");
        const bool fg=mode=="fg",fg11=mode=="fg-dx11";
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
        if(fg11) {
            std::filesystem::copy_file(bin/"test-fixtures/nvngx_dlss.dll",root/"_nvngx.dll");
            require(LoadLibraryW((root/"_nvngx.dll").c_str())!=nullptr,"DX11 transport NGX core fixture");
        }
        if(realvr) prepare_afw_test(bin,root,d12.Get(),queue.Get(),"--"+mode);
        else if(fg) require(LoadLibraryW((bin/"test-fixtures/nvngx_dlss.dll").c_str())!=nullptr,"FG fixture library");
        else if(!chained_table)prepare_late_attach_test(bin,d11.Get(),d12.Get(),queue.Get(),c_callback,streamline);
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
        if(fg11) verify_fg11(d11.Get());
        else if(fg) verify_fg(d12.Get(),queue.Get());
        else if(realvr) verify_realvr_test(get,command);
        else verify_late_attach_test(get,command,frame);
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
