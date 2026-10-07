#include "backend.hpp"
#include "d3d11_peripheral_dlaa.hpp"
#include "mock_ngx_parameters.hpp"
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using namespace cheeky::foveated_dlss;
namespace cheeky::foveated_dlss {
extern "C" void register_d3d11_game_feature(const NgxHandle*, unsigned,
    D3D11PeripheralCreateFeatureFn, D3D11PeripheralReleaseFeatureFn, NgxOutputExtent, bool) noexcept;
extern "C" D3D11Evaluation* prepare_d3d11_private(ID3D11DeviceContext*, const NgxHandle*,
    const NgxParameters*, const Settings&) noexcept;
extern "C" const NgxHandle* d3d11_private_handle(const D3D11Evaluation*) noexcept;
}
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
unsigned creates{}, created_width{}, created_output_width{}, created_dlaa_preset{};
NgxResult create_feature(ID3D11DeviceContext*, unsigned, NgxParameters* p, NgxHandle** handle) {
    p->Get("Width", &created_width); p->Get("OutWidth", &created_output_width);
    p->Get("DLSS.Hint.Render.Preset.DLAA", &created_dlaa_preset);
    *handle=reinterpret_cast<NgxHandle*>(static_cast<uintptr_t>(++creates)); return 1;
}
NgxResult release_feature(NgxHandle*) { return 1; }
// Stub only NGX inference. All allocation, downsampling and composition use production GPU code.
NgxResult evaluate_feature(ID3D11DeviceContext* context, const NgxHandle*,
    const NgxParameters* p, NgxProgressCallback) {
    ID3D11Resource* output{}; p->Get("Output", &output);
    ID3D11Resource* color{}; p->Get("Color", &color);
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (!output || !color || FAILED(device->CreateUnorderedAccessView(output,nullptr,&uav))) return 0xBAD00005U;
    context->CopyResource(output,color); return 1;
}
void check_pixels(ID3D11DeviceContext* context, ID3D11Texture2D* output, ID3D11Texture2D* readback) {
    context->CopyResource(readback,output); D3D11_MAPPED_SUBRESOURCE mapped{};
    check(SUCCEEDED(context->Map(readback,0,D3D11_MAP_READ,0,&mapped)),"DX11 map");
    const auto* outside=static_cast<const float*>(mapped.pData);
    const auto* inside=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+64*mapped.RowPitch)+64*4;
    const bool correct=outside[2]>.99F && outside[0]<.01F && inside[0]>.99F && inside[2]<.01F;
    context->Unmap(readback,0); check(correct,"compositor lost center or peripheral pixels");
}
}
int run_d3d11_binding_tests() {
    try {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"DX11 device");
        for (const auto format : {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_TYPELESS}) {
        ComPtr<ID3D11Texture2D> color,center,output,readback;
        const auto texture=[&](unsigned size,ComPtr<ID3D11Texture2D>& result) {
            D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=format;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_RENDER_TARGET;
            check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&result)),"DX11 texture");
        };
        texture(64,color);texture(64,center);texture(128,output);
        D3D11_TEXTURE2D_DESC d{};output->GetDesc(&d);d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&readback)),"DX11 readback");
        ComPtr<ID3D11UnorderedAccessView> color_uav,center_uav;
        ComPtr<ID3D11RenderTargetView> color_rtv;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;ud.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=ud.Format;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        check(SUCCEEDED(device->CreateUnorderedAccessView(color.Get(),&ud,&color_uav)) &&
            SUCCEEDED(device->CreateUnorderedAccessView(center.Get(),&ud,&center_uav)) &&
            SUCCEEDED(device->CreateRenderTargetView(color.Get(),&rd,&color_rtv)),"DX11 views");
        const float blue[]{0,0,1,1},red[]{1,0,0,1};
        context->ClearUnorderedAccessViewFloat(color_uav.Get(),blue);context->ClearUnorderedAccessViewFloat(center_uav.Get(),red);
        DlssFrameContract contract{};contract.render_width=contract.render_height=64;contract.output_width=contract.output_height=128;
        CropGeometry crop{};crop.input_width=crop.input_height=32;crop.input_base_x=crop.input_base_y=16;
        crop.output_width=crop.output_height=64;crop.output_base_x=crop.output_base_y=32;
        Settings settings;settings.width=settings.height=.5F;settings.x_offset=settings.height_offset=0;settings.roundness=settings.transition_width=0;
        for(unsigned mode=0;mode<5;++mode) {
            context->ClearState();
            auto* writer=mode==2?center_uav.Get():color_uav.Get();const unsigned slot=mode==0?0:3;
            if(mode<3)context->CSSetUnorderedAccessViews(slot,1,&writer,nullptr);
            if(mode==3)context->OMSetRenderTargets(1,color_rtv.GetAddressOf(),nullptr);
            if(mode==4)context->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,slot,1,&writer,nullptr);
            check(composite_d3d11_crop(context.Get(),color.Get(),output.Get(),center.Get(),contract,crop,settings),"DX11 composite failed");
            check_pixels(context.Get(),output.Get(),readback.Get());
            std::printf("DX11 format %u binding case %u: visible center and periphery\n",unsigned(format),mode);
            if(mode<3) {ComPtr<ID3D11UnorderedAccessView> restored;context->CSGetUnorderedAccessViews(slot,1,&restored);check(restored.Get()==writer,"CS UAV was not restored");}
            if(mode==3) {ComPtr<ID3D11RenderTargetView> restored;context->OMGetRenderTargets(1,&restored,nullptr);check(restored.Get()==color_rtv.Get(),"RTV was not restored");}
            if(mode==4) {ComPtr<ID3D11UnorderedAccessView> restored;context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,slot,1,&restored);check(restored.Get()==writer,"OM UAV was not restored");}
        }
        context->ClearState();
        ComPtr<ID3D11Texture2D> depth,motion;
        d={};d.Width=d.Height=64;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        d.Format=DXGI_FORMAT_R32_FLOAT;check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&depth)),"depth texture");
        d.Format=DXGI_FORMAT_R32G32_TYPELESS;check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&motion)),"motion texture");
        ComPtr<ID3D11UnorderedAccessView> depth_uav,motion_uav;
        ud.Format=DXGI_FORMAT_R32_FLOAT;check(SUCCEEDED(device->CreateUnorderedAccessView(depth.Get(),&ud,&depth_uav)),"depth view");
        ud.Format=DXGI_FORMAT_R32G32_FLOAT;check(SUCCEEDED(device->CreateUnorderedAccessView(motion.Get(),&ud,&motion_uav)),"motion view");
        const float zero[]{0,0,0,0};context->ClearUnorderedAccessViewFloat(depth_uav.Get(),zero);
        context->ClearUnorderedAccessViewFloat(motion_uav.Get(),zero);
        MockNgxParameters p;
        p.Set("Color",static_cast<ID3D11Resource*>(color.Get()));p.Set("Output",static_cast<ID3D11Resource*>(output.Get()));
        p.Set("Depth",static_cast<ID3D11Resource*>(depth.Get()));p.Set("MotionVectors",static_cast<ID3D11Resource*>(motion.Get()));
        p.Set("Width",64U);p.Set("Height",64U);p.Set("OutWidth",128U);p.Set("OutHeight",128U);
        p.Set("PerfQualityValue",3U);p.Set("DLSS.Feature.Create.Flags",2U);p.Set("Reset",0);
        p.Set("DLSS.Hint.Render.Preset.DLAA",13U);
        settings.auto_stereo_alignment=false;settings.peripheral_dlaa_scale=.5F;
        settings.peripheral_dlaa_preset=5U;
        const auto* game_handle=reinterpret_cast<NgxHandle*>(uintptr_t{0x1000});
        register_d3d11_game_feature(game_handle,1,create_feature,release_feature,{128,128},false);
        auto* evaluation=prepare_d3d11_private(context.Get(),game_handle,&p,settings);
        check(evaluation && d3d11_private_handle(evaluation),"DX11 private center preparation failed");
        check(created_width==32 && created_output_width==64 && created_dlaa_preset==13,"center creation contract/preset changed");
        ID3D11Resource* private_output{};p.Get("Output",&private_output);
        ComPtr<ID3D11UnorderedAccessView> private_uav;
        check(SUCCEEDED(device->CreateUnorderedAccessView(private_output,nullptr,&private_uav)),"private FLOAT output view");
        context->ClearUnorderedAccessViewFloat(private_uav.Get(),red);
        finish_d3d11(context.Get(),&p,evaluation,1);
        ID3D11Resource* restored_output{};unsigned restored_width{};
        p.Get("Output",&restored_output);p.Get("Width",&restored_width);
        check(restored_output==output.Get() && restored_width==64,"center parameters were not restored");
        check_pixels(context.Get(),output.Get(),readback.Get());
        D3D11PeripheralDlaaResult peripheral{};
        check(evaluate_d3d11_peripheral_dlaa(context.Get(),game_handle,&p,settings,
            create_feature,evaluate_feature,release_feature,peripheral),"DX11 peripheral DLAA preparation failed");
        check(peripheral.output_srv && peripheral.working_width==32 && peripheral.working_height==32 &&
            created_width==32 && created_output_width==32 && created_dlaa_preset==5,"peripheral contract/preset changed");
        p.Get("Output",&restored_output);p.Get("Width",&restored_width);p.Get("DLSS.Hint.Render.Preset.DLAA",&created_dlaa_preset);
        check(restored_output==output.Get() && restored_width==64 && created_dlaa_preset==13,"peripheral parameters were not restored");
        evaluation=prepare_d3d11_private(context.Get(),game_handle,&p,settings);
        check(evaluation!=nullptr,"center preparation with peripheral base failed");
        p.Get("Output",&private_output);
        private_uav.Reset();
        check(SUCCEEDED(device->CreateUnorderedAccessView(private_output,nullptr,&private_uav)),"center output view");
        context->ClearUnorderedAccessViewFloat(private_uav.Get(),red);
        d3d11_set_composite_base(evaluation,peripheral.output_srv,32,32);
        finish_d3d11(context.Get(),&p,evaluation,1);
        check_pixels(context.Get(),output.Get(),readback.Get());
        release_d3d11_peripheral_dlaa_result(peripheral);
        release_d3d11_peripheral_dlaa_resources();
        context->ClearState();release_d3d11_resources();
        std::printf("PASS DX11 format %u private center and reduced peripheral DLAA\n",unsigned(format));
        }
        std::puts("PASS DX11 composite pixels and write-binding restoration");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL DX11 bindings: %s\n",e.what());return 1;}
}
