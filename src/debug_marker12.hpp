#pragma once
#include "debug_exposure.hpp"
#include "eye_calibration_pixels.hpp"
#include <d3dcompiler.h>
#include <wrl/client.h>
namespace cheeky::foveated_dlss {
// Owned by the calibration frame, whose existing fences protect every resource.
struct DebugMarker12 {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> output, exposure;
    bool bound{};
    bool prepare(ID3D12Device* device, ID3D12Resource* target, std::uint64_t& allocations) {
        const auto d=target->GetDesc();
        if (!debug_exposure_supported(debug_exposure) || !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
            (d.Format!=DXGI_FORMAT_R11G11B10_FLOAT && d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT && d.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT)) return false;
        if (bound) return output.Get()==target && exposure.Get()==debug_exposure.texture;
        if (!pipeline) {
            const D3D12_DESCRIPTOR_RANGE ranges[]{
                {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},
                {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
            D3D12_ROOT_PARAMETER params[2]{};
            params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[0].DescriptorTable={2,ranges};
            params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[1].Constants={0,0,8};
            D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=params;
            Microsoft::WRL::ComPtr<ID3DBlob> signature,errors;
            if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors)) ||
               FAILED(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)))) return false;
            static constexpr char source[]=R"(
Texture2D<float4> Exposure : register(t0);
RWTexture2DArray<float4> Output : register(u0);
cbuffer Params : register(b0) { uint2 Origin; uint Side; uint Code; uint Locator; float Multiplier; uint2 Pad; };
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=Side)) return;
 uint2 p=id.xy;
 bool light;
 if(Locator && (any(p<16)||any(p>=56))) light=any(p<8)||any(p>=64);
 else { if(Locator) p-=16; light=((Code>>((p.y/8)*5+p.x/8))&1)!=0; }
 float white=1.0, e=Exposure.Load(int3(0,0,0)).r;
 if(isfinite(e)&&e>0) white=clamp(Multiplier/e,0.0001,1024.0);
 Output[uint3(Origin+id.xy,0)]=float4(light?white.xxx:0.0.xxx,1.0);
})";
            static const auto code = [] {
                Microsoft::WRL::ComPtr<ID3DBlob> result;
                D3DCompile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&result,nullptr);
                return result;
            }();
            if (!code) return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
            if(FAILED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)))) return false;
            allocations+=2;
        }
        if(!heap) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)))) return false;
            ++allocations;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=debug_exposure.texture->GetDesc().Format;
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateShaderResourceView(debug_exposure.texture,&srv,cpu);
        cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=d.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;uav.Texture2DArray.ArraySize=1;
        device->CreateUnorderedAccessView(target,nullptr,&uav,cpu);
        output=target;exposure=debug_exposure.texture;bound=true;
        return true;
    }
    void draw(ID3D12GraphicsCommandList* list,unsigned candidate,unsigned x,unsigned y,std::uint32_t code,bool locator) {
        if(!code) for(unsigned yy=0;yy<5;++yy) for(unsigned xx=0;xx<5;++xx)
            code|=unsigned(calibration_pattern_bit(candidate,xx,yy))<<(yy*5+xx);
        const unsigned pad=locator?16:0,side=locator?72:40;
        struct Constants { unsigned x,y,side,code,locator;float multiplier;unsigned pad[2]; };
        const Constants c{x-pad,y-pad,side,code,unsigned(locator),debug_exposure.pre/debug_exposure.scale,{}};
        ID3D12DescriptorHeap* heaps[]{heap.Get()};
        list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
        list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRoot32BitConstants(1,8,&c,0);
        list->Dispatch((side+7)/8,(side+7)/8,1);
        // Small source images can have overlapping locator rectangles. Match
        // the original ordered copies instead of racing successive writes.
        D3D12_RESOURCE_BARRIER order{};
        order.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        order.UAV.pResource=output.Get();
        list->ResourceBarrier(1,&order);
    }
};
}
