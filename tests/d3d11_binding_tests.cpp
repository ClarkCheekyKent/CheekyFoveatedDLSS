#include "backend.hpp"
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using namespace cheeky::foveated_dlss;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
}
int run_d3d11_binding_tests() {
    try {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"DX11 device");
        ComPtr<ID3D11Texture2D> color,center,output,readback;
        const auto texture=[&](unsigned size,ComPtr<ID3D11Texture2D>& result) {
            D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_RENDER_TARGET;
            check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&result)),"DX11 texture");
        };
        texture(64,color);texture(64,center);texture(128,output);
        D3D11_TEXTURE2D_DESC d{};output->GetDesc(&d);d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&readback)),"DX11 readback");
        ComPtr<ID3D11UnorderedAccessView> color_uav,center_uav;
        ComPtr<ID3D11RenderTargetView> color_rtv;
        check(SUCCEEDED(device->CreateUnorderedAccessView(color.Get(),nullptr,&color_uav)) &&
            SUCCEEDED(device->CreateUnorderedAccessView(center.Get(),nullptr,&center_uav)) &&
            SUCCEEDED(device->CreateRenderTargetView(color.Get(),nullptr,&color_rtv)),"DX11 views");
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
            context->CopyResource(readback.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            check(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)),"DX11 map");
            const auto* outside=static_cast<const float*>(mapped.pData);
            const auto* inside=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+64*mapped.RowPitch)+64*4;
            const bool correct=outside[2]>.99F && outside[0]<.01F && inside[0]>.99F && inside[2]<.01F;
            context->Unmap(readback.Get(),0);
            std::printf("DX11 binding case %u: %s\n",mode,correct?"visible center and periphery":"black/missing input");
            check(correct,"compositor lost an input that was previously bound for writing");
            if(mode<3) {ComPtr<ID3D11UnorderedAccessView> restored;context->CSGetUnorderedAccessViews(slot,1,&restored);check(restored.Get()==writer,"CS UAV was not restored");}
            if(mode==3) {ComPtr<ID3D11RenderTargetView> restored;context->OMGetRenderTargets(1,&restored,nullptr);check(restored.Get()==color_rtv.Get(),"RTV was not restored");}
            if(mode==4) {ComPtr<ID3D11UnorderedAccessView> restored;context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,slot,1,&restored);check(restored.Get()==writer,"OM UAV was not restored");}
        }
        context->ClearState();release_d3d11_resources();
        std::puts("PASS DX11 composite pixels and write-binding restoration");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL DX11 bindings: %s\n",e.what());return 1;}
}
