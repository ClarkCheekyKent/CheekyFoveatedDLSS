#pragma once
#include "fg_color_format.hpp"
#include "../shared/fg_compact.hpp"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstring>

namespace cheeky::fg {
// A queued DLSS output cannot wait for CPU readback of OutputDisableInterpolation.
// Replace suppressed stereo outputs with the current native color on the GPU.
// Descriptors remain private to each eye/phase until the worker fence retires.
struct NgxFallback {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12RootSignature> root;
    Ptr<ID3D12PipelineState> pipeline;
    Ptr<ID3D12DescriptorHeap> descriptors;
    struct Constants {float source_crop[4]{},source_spans[4]{},source_texel[4]{},
        target_crop[4]{},target_spans[4]{},target_texel[4]{},options[4]{};};
    void reset() noexcept {descriptors.Reset();pipeline.Reset();root.Reset();}
    bool initialize(ID3D12Device* device) {
        if(root&&pipeline&&descriptors)return true;
        constexpr char shader[]=CHEEKY_FG_COMPACT_HLSL R"HLSL(
Texture2D<float4> Native:register(t0);
ByteAddressBuffer Disabled0:register(t1);ByteAddressBuffer Disabled1:register(t2);
ByteAddressBuffer Disabled2:register(t3);ByteAddressBuffer Disabled3:register(t4);
RWTexture2D<float4> Output:register(u0);SamplerState LinearClamp:register(s0);
cbuffer Params:register(b0){float4 SourceCrop;float4 SourceSpans;float4 SourceTexel;
 float4 TargetCrop;float4 TargetSpans;float4 TargetTexel;float4 Options;}
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 if(((Disabled0.Load(0)|Disabled1.Load(0)|Disabled2.Load(0)|Disabled3.Load(0))&255)==0)return;
 uint w,h;Output.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 float2 uv=(float2(id.xy)+.5)/float2(w,h);
 if(TargetSpans.x>0){
  float2 centerSize=round(TargetSpans.xy*float2(w,h));
  float2 contextSize=round(TargetSpans.zw*float2(w,h));
  if(id.y<centerSize.y){if(id.x>=centerSize.x)return;
   uv=TargetCrop.xy+(float2(id.xy)+.5)/centerSize*TargetCrop.zw;
  }else{if(id.x>=contextSize.x||id.y>=centerSize.y+contextSize.y)return;
   uv=(float2(id.x,id.y-centerSize.y)+.5)/contextSize;}
 }
 float4 c;
 if(SourceSpans.x<=0)c=Native.SampleLevel(LinearClamp,uv,0);
 else{float fade=centerFade(uv,SourceCrop,SourceTexel);
  [branch]if(fade>=1)c=Native.SampleLevel(LinearClamp,centerUV(uv,SourceCrop,SourceSpans,SourceTexel),0);
  else{float4 base=Native.SampleLevel(LinearClamp,contextUV(uv,SourceSpans,SourceTexel),0);
   [branch]if(fade<=0)c=base;
   else c=lerp(base,Native.SampleLevel(LinearClamp,centerUV(uv,SourceCrop,SourceSpans,SourceTexel),0),fade);}}
 if(Options.x>.5)c.rgb=lerp(12.92*c.rgb,1.055*pow(max(c.rgb,0),1.0/2.4)-.055,step(.0031308,c.rgb));
 Output[id.xy]=c;
})HLSL";
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,0,0,0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,5}};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={2,ranges};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[1].Constants={0,0,sizeof(Constants)/4};
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=parameters;
        signature.NumStaticSamplers=1;signature.pStaticSamplers=&sampler;
        Ptr<ID3DBlob> serialized,code,error;
        if(!root&&(FAILED(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error))||
            FAILED(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)))))return false;
        if(!pipeline){
            if(FAILED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"Main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error)))return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC state{};state.pRootSignature=root.Get();state.CS={code->GetBufferPointer(),code->GetBufferSize()};
            if(FAILED(device->CreateComputePipelineState(&state,IID_PPV_ARGS(&pipeline))))return false;
        }
        if(!descriptors){D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap.NumDescriptors=3*2*6;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&descriptors))))return false;}
        return true;
    }
    bool record(ID3D12Device* device,ID3D12GraphicsCommandList* list,unsigned phase,unsigned eye,
        ID3D12Resource* native,D3D12_RESOURCE_STATES native_state,const CompactLayout& source_layout,
        ID3D12Resource* output,const CompactLayout& target_layout,const std::array<ID3D12Resource*,4>& disabled,
        D3D12_RESOURCE_STATES output_state=D3D12_RESOURCE_STATE_GENERIC_READ,DXGI_FORMAT output_format=DXGI_FORMAT_UNKNOWN) {
        if(phase>=3||eye>=2||!native||!output||!initialize(device))return false;
        const auto src=native->GetDesc(),dst=output->GetDesc();
        if(src.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||dst.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||
            !(dst.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return false;
        const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto offset=(phase*2+eye)*6*stride;
        auto cpu=descriptors->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=offset;
        auto gpu=descriptors->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=offset;
        D3D12_SHADER_RESOURCE_VIEW_DESC color{};color.Format=src.Format;color.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        color.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;color.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(native,&color,cpu);cpu.ptr+=stride;
        for(auto* flag:disabled){D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer.NumElements=flag?unsigned(flag->GetDesc().Width/4):2;view.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
            device->CreateShaderResourceView(flag,&view,cpu);cpu.ptr+=stride;}
        if(output_format==DXGI_FORMAT_UNKNOWN)output_format=dst.Format;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=output_format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output,nullptr,&uav,cpu);
        Constants constants;source_layout.constants(constants.source_crop,constants.source_spans,constants.source_texel);
        target_layout.constants(constants.target_crop,constants.target_spans,constants.target_texel);
        constants.options[0]=srgb_color(src.Format)&&output_format==DXGI_FORMAT_R8G8B8A8_UNORM?1.f:0.f;
        auto transition=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
            if(!resource||from==to)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);};
        const auto readable=(native_state&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)?native_state:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        transition(native,native_state,readable);
        for(auto* flag:disabled)transition(flag,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(output,output_state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[]{descriptors.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());list->SetComputeRootDescriptorTable(0,gpu);
        list->SetComputeRoot32BitConstants(1,sizeof(Constants)/4,&constants,0);
        list->Dispatch((unsigned(dst.Width)+7)/8,(dst.Height+7)/8,1);
        transition(output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,output_state);
        for(auto* flag:disabled)transition(flag,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        transition(native,readable,native_state);
        return true;
    }
};
}
