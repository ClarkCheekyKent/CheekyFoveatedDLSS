#pragma once
#include "../../third_party/ngx-sdk/include/nvsdk_ngx_helpers.h"
#include <filesystem>

// Opt-in isolation probe: real D3D11 DLSS SR on the render thread, native FG
// copies/uploads/fence signals on another thread. No game or OpenXR runtime.
inline void fg_shader11_ngx_probe(const std::filesystem::path& models,const std::string& mode){
    using namespace shader11_fixture;using namespace cheeky::fg;
    check(mode=="gated"||mode=="ungated"||mode=="lifecycle-gated"||mode=="lifecycle-ungated"||
        mode=="multithread"||mode=="external-context"||mode=="external-context-protected","Unknown real-NGX probe mode");
    const bool gated=mode!="ungated"&&mode!="multithread";
    const bool churn_features=mode.find("lifecycle-")==0,gate_lifecycle=mode!="lifecycle-ungated";
    const bool external_context=mode.find("external-context")==0;
    const bool protect_evaluation=mode=="multithread"||mode=="external-context-protected";
    check(std::filesystem::exists(models/L"nvngx_dlss.dll"),"NGX probe requires a directory containing nvngx_dlss.dll");
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"NGX probe hardware device");
    std::recursive_mutex gate;Backend11 backend;
    check(backend.initialize(device.Get(),7,gated?&gate:nullptr),"NGX probe native backend");
    const auto cache=std::filesystem::absolute(L"build/fg-diagnostics/ngx-probe-cache");std::filesystem::create_directories(cache);
    const auto directory=models.wstring();const wchar_t* paths[]{directory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};info.PathListInfo.Path=paths;info.PathListInfo.Length=1;
    auto success=[](NVSDK_NGX_Result result,const char* stage){
        std::cout<<stage<<" = 0x"<<std::hex<<unsigned(result)<<std::dec<<'\n';check(NVSDK_NGX_SUCCEED(result),stage);};
    success(NVSDK_NGX_D3D11_Init_with_ProjectID("eec05a21-0120-42fa-a51a-eafae09e4cdc",NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "CheekyFoveatedDLSS",cache.c_str(),device.Get(),&info,NVSDK_NGX_Version_API),"Real D3D11 NGX init");
    struct Ngx {
        NVSDK_NGX_Parameter* params{};std::array<NVSDK_NGX_Handle*,2> features{};
        ~Ngx(){for(auto* f:features)if(f)NVSDK_NGX_D3D11_ReleaseFeature(f);
            if(params)NVSDK_NGX_D3D11_DestroyParameters(params);NVSDK_NGX_D3D11_Shutdown1(device.Get());}
    } ngx;
    success(NVSDK_NGX_D3D11_GetCapabilityParameters(&ngx.params),"Real NGX capabilities");
    int available{};ngx.params->Get(NVSDK_NGX_Parameter_SuperSampling_Available,&available);check(available!=0,"Real DLSS SR available");
    constexpr unsigned input_size=640,output_size=1280;
    auto make=[&](DXGI_FORMAT format,unsigned size,const std::array<float,4>& clear){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11RenderTargetView> r;
        check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&t))&&SUCCEEDED(device->CreateRenderTargetView(t.Get(),nullptr,&r)),"NGX probe texture");
        context->ClearRenderTargetView(r.Get(),clear.data());return t;
    };
    auto color=make(DXGI_FORMAT_R16G16B16A16_FLOAT,input_size,{.8f,.1f,.05f,1});
    auto depth=make(DXGI_FORMAT_R32_FLOAT,input_size,{.5f,0,0,0});
    auto motion=make(DXGI_FORMAT_R32G32_FLOAT,input_size,{0,0,0,0});
    std::array<ComPtr<ID3D11Texture2D>,2> output,target;
    NVSDK_NGX_DLSS_Create_Params create{};create.Feature={input_size,input_size,output_size,output_size,NVSDK_NGX_PerfQuality_Value_MaxPerf};
    create.InFeatureCreateFlags=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    for(unsigned eye=0;eye<2;++eye){
        output[eye]=make(DXGI_FORMAT_R16G16B16A16_FLOAT,output_size,{0,0,0,0});
        target[eye]=make(DXGI_FORMAT_R16G16B16A16_FLOAT,output_size,{0,0,0,0});
        success(NGX_D3D11_CREATE_DLSS_EXT(context.Get(),&ngx.features[eye],ngx.params,&create),"Real DLSS SR feature");
    }
    ComPtr<ID3D11Device5> device5;ComPtr<ID3D11DeviceContext4> context4;ComPtr<ID3D11Fence> fence;
    check(SUCCEEDED(device.As(&device5))&&SUCCEEDED(context.As(&context4))&&
        SUCCEEDED(device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence))),"NGX probe shared fence");
    std::atomic<unsigned> copies{};std::atomic<bool> failed{};
    std::jthread presenter([&](std::stop_token stop){
        while(!stop.stop_requested()){
            for(unsigned eye=0;eye<2;++eye){auto image=backend.copy(output[eye].Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
                if(!image||!backend.upload(target[eye].Get(),image)||FAILED(backend.runtime_submit([&]{return context4->Signal(fence.Get(),copies.load()+1);})))
                    {failed=true;return;}
                ++copies;
            }
            Sleep(1);
        }
    });
    ComPtr<ID3D11Fence> external_fence;std::atomic<unsigned> external_signals{};
    std::jthread external;
    if(external_context){
        check(SUCCEEDED(device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&external_fence))),"NGX probe external fence");
        external=std::jthread([&](std::stop_token stop){while(!stop.stop_requested()){
            {Backend11::Guard lock(backend.protection.Get());
                if(FAILED(context4->Signal(external_fence.Get(),++external_signals))){failed=true;return;}
                context->Flush();}
            Sleep(1);
        }});
    }
    unsigned lifecycle_count{};
    NVSDK_NGX_D3D11_DLSS_Eval_Params evaluate{};evaluate.Feature.pInColor=color.Get();evaluate.pInDepth=depth.Get();evaluate.pInMotionVectors=motion.Get();
    evaluate.InRenderSubrectDimensions={input_size,input_size};evaluate.InMVScaleX=evaluate.InMVScaleY=1;
    evaluate.InPreExposure=evaluate.InExposureScale=1;evaluate.InFrameTimeDeltaInMsec=16.67f;
    for(unsigned frame=0;frame<1500&&!failed;++frame){
        if(churn_features&&frame%25==0){
            // An unused feature has no in-flight evaluations or output lifetime
            // dependency. Vary its dimensions so NGX cannot alias either eye.
            std::unique_lock lock(gate,std::defer_lock);if(gate_lifecycle)lock.lock();
            auto temporary_create=create;temporary_create.Feature.InTargetWidth+=16;
            NVSDK_NGX_Handle* temporary{};
            check(NVSDK_NGX_SUCCEED(NGX_D3D11_CREATE_DLSS_EXT(context.Get(),&temporary,ngx.params,&temporary_create)),
                "Real NGX concurrent feature creation");
            check(temporary&&temporary!=ngx.features[0]&&temporary!=ngx.features[1],"NGX lifecycle probe distinct feature");
            check(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D11_ReleaseFeature(temporary)),"Real NGX concurrent feature release");
            ++lifecycle_count;
        }
        for(unsigned eye=0;eye<2;++eye){
            std::unique_lock lock(gate,std::defer_lock);if(gated)lock.lock();
            std::optional<Backend11::Guard> context_lock;if(protect_evaluation)context_lock.emplace(backend.protection.Get());
            evaluate.Feature.pInOutput=output[eye].Get();evaluate.InReset=frame==0;
            const auto result=NGX_D3D11_EVALUATE_DLSS_EXT(context.Get(),ngx.features[eye],ngx.params,&evaluate);
            if(NVSDK_NGX_FAILED(result)){failed=true;std::cout<<"NGX evaluation failed: 0x"<<std::hex<<unsigned(result)<<std::dec<<'\n';break;}
        }
        if(frame%250==0)std::cout<<"Real NGX frames="<<frame<<" concurrent uploads="<<copies.load()<<" gate="<<gated<<'\n';
        Sleep(4);
    }
    external.request_stop();if(external.joinable())external.join();
    presenter.request_stop();presenter.join();
    const auto final_value=std::uint64_t(copies.load())+1;
    check(SUCCEEDED(context4->Signal(fence.Get(),final_value)),"NGX probe final signal");context->Flush();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(fence->GetCompletedValue()<final_value&&std::chrono::steady_clock::now()<deadline)Sleep(1);
    check(!failed&&copies>0&&fence->GetCompletedValue()>=final_value,"Real NGX and background native uploads complete");
    std::cout<<"PASS real D3D11 NGX SR + native presenter: 3000 evaluations, "<<copies.load()<<" uploads, gate="<<gated
        <<" feature_lifecycles="<<lifecycle_count<<" lifecycle_gate="<<gate_lifecycle
        <<" external_signals="<<external_signals.load()<<" evaluation_context_lock="<<protect_evaluation<<'\n';
}
