#pragma once
#include "fg_color_format.hpp"
#include "../shared/fg_compact.hpp"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <cstring>

namespace cheeky::fg {
// Bindings belong to a transport command context, so no descriptor is rewritten
// until that context's GPU fence retires. PSOs are shared within one queue owner.
struct ColorDrawBindings {
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv,rtv;
    unsigned used{};
};
struct ColorDraw {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12RootSignature> root;
    Ptr<ID3DBlob> vertex,pixel;
    struct Pipeline {DXGI_FORMAT format;Ptr<ID3D12PipelineState> state;};
    std::vector<Pipeline> pipelines;
    struct Constants {float region[4]{0,0,1,1},crop[4]{},spans[4]{},texel[4]{},options[4]{};};
    bool initialize(ID3D12Device* d) {
        if(root&&vertex&&pixel)return true;
        constexpr char shader[]=CHEEKY_FG_COMPACT_HLSL R"HLSL(
Texture2DArray<float4> Source:register(t0);SamplerState LinearClamp:register(s0);
cbuffer Params:register(b0){float4 Region;float4 Crop;float4 Spans;float4 Texel;float4 Options;}
struct V {float4 position:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){V v;v.uv=float2((id<<1)&2,id&2);v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float4 PS(V v):SV_Target{
 float2 uv=Region.xy+v.uv*Region.zw;
 float4 c;
 if(Spans.x>0){float fade=centerFade(uv,Crop,Texel);
  // The covered context interior can be unwritten. Select the center before
  // sampling context: lerp(NaN,center,1) would still propagate NaN in HDR.
  [branch]if(fade>=1)c=Source.SampleLevel(LinearClamp,float3(centerUV(uv,Crop,Spans,Texel),0),0);
  else{float4 b=Source.SampleLevel(LinearClamp,float3(contextUV(uv,Spans,Texel),0),0);
   [branch]if(fade<=0)c=b;
   else c=lerp(b,Source.SampleLevel(LinearClamp,float3(centerUV(uv,Crop,Spans,Texel),0),0),fade);}}
 else c=Source.SampleLevel(LinearClamp,float3(uv,0),0);
 // SDR FG uses gamma-encoded UNORM. A typed sRGB source was decoded by its SRV;
 // re-encode only when the render target itself is not sRGB.
 if(Options.x>.5)c.rgb=lerp(12.92*c.rgb,1.055*pow(max(c.rgb,0),1.0/2.4)-.055,step(.0031308,c.rgb));
 return c;
})HLSL";
        Ptr<ID3DBlob> error;
        if(!vertex&&FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"VS","vs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vertex,&error)))return false;
        if(!pixel&&FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"PS","ps_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&pixel,&error)))return false;
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
        D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&range};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,sizeof(Constants)/4};
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=2;desc.pParameters=params;desc.NumStaticSamplers=1;desc.pStaticSamplers=&sampler;
        Ptr<ID3DBlob> blob;
        return SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))&&
            SUCCEEDED(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    }
    ID3D12PipelineState* pipeline(ID3D12Device* d,DXGI_FORMAT format) {
        for(auto& p:pipelines)if(p.format==format)return p.state.Get();
        if(!initialize(d))return nullptr;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();
        desc.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};desc.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable=TRUE;desc.DepthStencilState.DepthEnable=FALSE;desc.DepthStencilState.StencilEnable=FALSE;
        desc.SampleMask=UINT_MAX;desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets=1;desc.RTVFormats[0]=format;desc.SampleDesc.Count=1;
        Pipeline p{format,{}};if(FAILED(d->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&p.state))))return nullptr;
        pipelines.push_back(std::move(p));return pipelines.back().state.Get();
    }
    bool record(ID3D12Device* d,ID3D12GraphicsCommandList* list,ColorDrawBindings& bindings,
                ID3D12Resource* source,DXGI_FORMAT source_format,unsigned layer,ID3D12Resource* target,
                DXGI_FORMAT target_format,const D3D12_VIEWPORT& viewport,const Constants& constants) {
        if(!source||!target||source==target||bindings.used>=32)return false;
        auto* pso=pipeline(d,target_format);if(!pso)return false;
        if(!bindings.srv){D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=32;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if(FAILED(d->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&bindings.srv))))return false;}
        if(!bindings.rtv){D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=32;
            if(FAILED(d->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&bindings.rtv))))return false;}
        const unsigned slot=bindings.used++;
        auto srv=bindings.srv->GetCPUDescriptorHandleForHeapStart();srv.ptr+=slot*d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto gpu=bindings.srv->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=slot*d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto rtv=bindings.rtv->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=slot*d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=source_format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2DArray.MipLevels=1;
        view.Texture2DArray.FirstArraySlice=layer;view.Texture2DArray.ArraySize=1;d->CreateShaderResourceView(source,&view,srv);
        D3D12_RENDER_TARGET_VIEW_DESC out{};out.Format=target_format;out.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;d->CreateRenderTargetView(target,&out,rtv);
        ID3D12DescriptorHeap* heaps[]{bindings.srv.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetGraphicsRootSignature(root.Get());list->SetPipelineState(pso);list->SetGraphicsRootDescriptorTable(0,gpu);
        list->SetGraphicsRoot32BitConstants(1,sizeof(Constants)/4,&constants,0);
        list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);list->RSSetViewports(1,&viewport);
        D3D12_RECT scissor{LONG(viewport.TopLeftX),LONG(viewport.TopLeftY),LONG(viewport.TopLeftX+viewport.Width),LONG(viewport.TopLeftY+viewport.Height)};
        list->RSSetScissorRects(1,&scissor);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);return true;
    }
};
}
