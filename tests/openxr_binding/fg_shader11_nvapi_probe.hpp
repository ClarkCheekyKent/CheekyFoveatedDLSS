#pragma once
#include "../../src/nvapi_d3d11_guard.hpp"
// Opt-in isolation probe: a UE4-style render thread toggles the NVAPI D3D11
// depth-bounds test between draws while the native FG presenter copies,
// uploads and signals on another thread. NVAPI enters the driver directly,
// so ID3D11Multithread protection does not order it against our calls unless
// the caller holds the D3D11 lock. Modes: "ungated", "locked", or "guarded"
// (the runtime's NVAPI detours installed after the entry point was cached).
inline void fg_shader11_nvapi_probe(const std::string& mode){
    using namespace shader11_fixture;using namespace cheeky::fg;
    check(mode=="ungated"||mode=="locked"||mode=="guarded"||mode=="ids","Unknown NVAPI probe mode");
    const bool locked=mode=="locked";
    using QueryInterface=void*(__cdecl*)(unsigned);
    using Initialize=int(__cdecl*)();
    using SetDepthBoundsTest=int(__cdecl*)(IUnknown*,unsigned,float,float);
    const auto nvapi=LoadLibraryW(L"nvapi64.dll");check(nvapi!=nullptr,"NVAPI probe requires nvapi64.dll");
    const auto query=reinterpret_cast<QueryInterface>(GetProcAddress(nvapi,"nvapi_QueryInterface"));
    check(query!=nullptr,"NVAPI query interface");
    const auto initialize=reinterpret_cast<Initialize>(query(0x0150E828u));
    const auto depth_bounds=reinterpret_cast<SetDepthBoundsTest>(query(0x7AAF7A04u));
    check(initialize&&depth_bounds&&initialize()==0,"NVAPI initialize and NvAPI_D3D11_SetDepthBoundsTest");
    if(mode=="guarded"){const auto hooks=MH_Initialize();
        check((hooks==MH_OK||hooks==MH_ERROR_ALREADY_INITIALIZED)&&cheeky::foveated_dlss::nvapi_guard::install(),"Runtime NVAPI D3D11 guard installs");}
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),
        "NVAPI probe hardware device");
    Backend11 backend;check(backend.initialize(device.Get(),7,nullptr),"NVAPI probe native backend");
    check(depth_bounds(device.Get(),1,0.f,1.f)==0&&depth_bounds(device.Get(),0,0.f,1.f)==0,"NVAPI depth bounds supported");
    using UavOverlap=int(__cdecl*)(IUnknown*);
    const auto begin_overlap=reinterpret_cast<UavOverlap>(query(0x65B93CA8u)),end_overlap=reinterpret_cast<UavOverlap>(query(0x2216A357u));
    if(mode=="ids"){
        std::cout<<"NvAPI_D3D11_BeginUAVOverlap="<<(begin_overlap?begin_overlap(device.Get()):-999)
            <<" EndUAVOverlap="<<(end_overlap?end_overlap(device.Get()):-999)<<'\n';
        device.Reset();context.Reset();return;}
    constexpr unsigned size=1024;
    auto make=[&](DXGI_FORMAT format,ComPtr<ID3D11RenderTargetView>* view=nullptr){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11RenderTargetView> r;
        check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&t))&&SUCCEEDED(device->CreateRenderTargetView(t.Get(),nullptr,&r)),"NVAPI probe texture");
        if(view)*view=r;return t;};
    ComPtr<ID3D11RenderTargetView> scene_view;auto scene=make(DXGI_FORMAT_R8G8B8A8_UNORM,&scene_view);
    std::array<ComPtr<ID3D11Texture2D>,2> target{make(DXGI_FORMAT_R8G8B8A8_UNORM),make(DXGI_FORMAT_R8G8B8A8_UNORM)};
    ComPtr<ID3D11Device5> device5;ComPtr<ID3D11DeviceContext4> context4;ComPtr<ID3D11Fence> fence;
    check(SUCCEEDED(device.As(&device5))&&SUCCEEDED(context.As(&context4))&&
        SUCCEEDED(device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence))),"NVAPI probe shared fence");
    std::atomic<unsigned> copies{};std::atomic<bool> failed{};
    std::jthread presenter([&](std::stop_token stop){
        while(!stop.stop_requested()){
            for(unsigned eye=0;eye<2;++eye){auto image=backend.copy(scene.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
                if(!image||!backend.upload(target[eye].Get(),image)||FAILED(backend.runtime_submit([&]{return context4->Signal(fence.Get(),copies.load()+1);})))
                    {failed=true;return;}
                ++copies;}
            Sleep(1);}
    });
    std::uint64_t toggles{};
    for(unsigned frame=0;frame<1500&&!failed;++frame){
        // A deferred-lighting pass: one bounds change per light around its draw.
        for(unsigned light=0;light<64;++light){
            const float color[]{float(light)/64.f,float(frame%7)/7.f,0,1};
            const D3D11_RECT rect{LONG(light%8*128),LONG(light/8*128),LONG(light%8*128+128),LONG(light/8*128+128)};
            std::optional<Backend11::Guard> lock;if(locked)lock.emplace(backend.protection.Get());
            depth_bounds(device.Get(),1,float(light)/128.f,1.f);
            context4->ClearView(scene_view.Get(),color,&rect,1);
            depth_bounds(device.Get(),0,0.f,1.f);toggles+=2;
        }
        context->Flush();
        if(frame%250==0)std::cout<<"NVAPI frames="<<frame<<" toggles="<<toggles<<" concurrent uploads="<<copies.load()<<" locked="<<locked<<'\n';
        Sleep(2);
    }
    presenter.request_stop();presenter.join();
    const auto final_value=std::uint64_t(copies.load())+1;
    check(SUCCEEDED(context4->Signal(fence.Get(),final_value)),"NVAPI probe final signal");context->Flush();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(fence->GetCompletedValue()<final_value&&std::chrono::steady_clock::now()<deadline)Sleep(1);
    check(!failed&&copies>0&&fence->GetCompletedValue()>=final_value,"NVAPI render thread and background native uploads complete");
    device.Reset();context.Reset();
    if(mode=="guarded")check(cheeky::foveated_dlss::nvapi_guard::guarded_calls.load()>=toggles,"Cached NVAPI entry points run through the guard");
    std::cout<<"PASS NVAPI depth-bounds render thread + native presenter: "<<toggles<<" toggles, "<<copies.load()<<" uploads, locked="<<locked<<'\n';
}
