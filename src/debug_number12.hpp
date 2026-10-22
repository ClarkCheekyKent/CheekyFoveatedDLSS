#pragma once
#include "debug_exposure.hpp"
#include "d3d_shaders.hpp"
#include <wrl/client.h>
namespace cheeky::foveated_dlss {
// Descriptor bindings are immutable until the owning calibration frame retires.
struct DebugNumber12 {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> output,exposure;
    bool prepare(ID3D12Device* device,ID3D12Resource* target) {
        const auto d=target->GetDesc();
        if(!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)||
            (d.Format!=DXGI_FORMAT_R11G11B10_FLOAT&&d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&
             d.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT&&d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM))return false;
        if(!pipeline){
            D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
            D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
            params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,12};
            D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=params;
            Microsoft::WRL::ComPtr<ID3DBlob> signature,errors;
            if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors))||
               FAILED(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={d3d_shaders::debug_number12.data,d3d_shaders::debug_number12.size};
            if(FAILED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline))))return false;
        }
        if(!heap){D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))))return false;}
        output=target;exposure=debug_exposure_supported(debug_exposure)?debug_exposure.texture:nullptr;
        auto cpu=heap->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format=exposure?exposure->GetDesc().Format:DXGI_FORMAT_R32_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(exposure.Get(),&srv,cpu);cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=d.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;uav.Texture2DArray.ArraySize=1;
        device->CreateUnorderedAccessView(target,nullptr,&uav,cpu);return true;
    }
    void draw(ID3D12GraphicsCommandList* list,unsigned x,unsigned y,unsigned width,unsigned height,unsigned cell,const char* glyph) {
        uint64_t code{};for(unsigned i=0;i<35;++i)if(glyph[i]=='1')code|=uint64_t(1)<<i;
        float multiplier=debug_exposure.pre/debug_exposure.scale;
        if(!std::isfinite(multiplier)||multiplier<=0)multiplier=1;
        struct Constants {unsigned x,y,width,height,cell,lo,hi,has_exposure;float multiplier;unsigned pad[3];};
        const Constants c{x,y,width,height,cell,unsigned(code),unsigned(code>>32),unsigned(bool(exposure)),multiplier,{}};
        ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());
        list->SetPipelineState(pipeline.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRoot32BitConstants(1,12,&c,0);list->Dispatch((5*cell+9)/8,(7*cell+9)/8,35);
    }
};
inline constexpr char debug_number12_source[] = R"(
Texture2D<float4> Exposure : register(t0);
RWTexture2DArray<float4> Output : register(u0);
cbuffer Params : register(b0) { uint2 Origin; uint2 Size; uint Cell; uint Lo; uint Hi; uint HasExposure; float Multiplier; uint3 Pad; };
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 uint2 glyphSize=uint2(5,7)*Cell+2;
 if(any(id.xy>=glyphSize))return;
 bool light=false;
 if(all(id.xy>0)&&all(id.xy<glyphSize-1)) {
  uint2 p=(id.xy-1)/Cell;uint bit=p.y*5+p.x;
  light=bit<32?((Lo>>bit)&1)!=0:((Hi>>(bit-32))&1)!=0;
 }
 float white=Multiplier;
 if(HasExposure){float e=Exposure.Load(int3(0,0,0)).r;if(isfinite(e)&&e>0)white/=e;}
 white=clamp(white,0.0001,1024.0);
 uint2 grid=uint2(id.z%7,id.z/7);
 uint2 position=Origin+(Size-glyphSize)*(2*grid+1)/uint2(14,10)+id.xy;
 Output[uint3(position,0)]=float4(light?white.xxx:0.0.xxx,1.0);
})";
}
