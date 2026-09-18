#pragma once
#include "nr_shader_cache.hpp"
#include "nr_guide_shader.hpp"
#include <initializer_list>
#include <cstdio>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace cheeky::foveated_dlss {
struct NrGuidePass {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Resource> source_motion, source_depth, motion, depth;
    Ptr<ID3D12DescriptorHeap> heap;
    Ptr<ID3D12RootSignature> root;
    Ptr<ID3D12PipelineState> pipeline;
    static DXGI_FORMAT readable(DXGI_FORMAT f) noexcept {
        switch(f) {
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return f;
        }
    }
    bool initialize(ID3D12Resource* mv, ID3D12Resource* z, unsigned w, unsigned h, const NrGuidePass* shared = nullptr) {
        source_motion=mv; source_depth=z;
        Ptr<ID3D12Device> device;
        if (FAILED(mv->GetDevice(IID_PPV_ARGS(&device)))) return false;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=w; desc.Height=h; desc.DepthOrArraySize=desc.MipLevels=1;
        desc.SampleDesc.Count=1; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        desc.Format=DXGI_FORMAT_R32G32_FLOAT;
        if (FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&motion)))) return false;
        desc.Format=DXGI_FORMAT_R32_FLOAT;
        if (FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&depth)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=4; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)))) return false;
        auto cpu=heap->GetCPUDescriptorHandleForHeapStart(); auto step=device->GetDescriptorHandleIncrementSize(hd.Type);
        for (auto* source : {mv,z}) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=readable(source->GetDesc().Format);
            sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels=1;
            device->CreateShaderResourceView(source,&sd,cpu); cpu.ptr+=step;
        }
        for (auto* target : {motion.Get(),depth.Get()}) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format=target->GetDesc().Format; ud.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(target,nullptr,&ud,cpu); cpu.ptr+=step;
        }
        if (shared && shared->root && shared->pipeline) {
            root = shared->root; pipeline = shared->pipeline; return true;
        }
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0}; ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,2};
        D3D12_ROOT_PARAMETER params[2]{}; params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable={2,ranges};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[1].Constants={0,0,sizeof(NrGuideConstants)/4};
        D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=2; rd.pParameters=params;
        Ptr<ID3DBlob> blob,errors,code;
        if (FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors)) ||
            FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))) ||
            FAILED(compile_nr_shader(nr_guide_shader,sizeof(nr_guide_shader),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,&errors))) { if(errors) std::fprintf(stderr,"%s\n",static_cast<const char*>(errors->GetBufferPointer())); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get(); pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
        return SUCCEEDED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
    }
    // Caller must prove all recordings using this heap have completed.
    void rebind(ID3D12Device* device, ID3D12Resource* mv, ID3D12Resource* z) {
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (auto* source : {mv, z}) {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = readable(source->GetDesc().Format);
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(source, &desc, cpu); cpu.ptr += step;
        }
        source_motion = mv; source_depth = z;
    }
    void dispatch(ID3D12GraphicsCommandList* list, const NrGuideConstants& c,
        D3D12_RESOURCE_STATES mv_state, D3D12_RESOURCE_STATES depth_state) {
        const auto barrier=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
            if(a==b) return;
            D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&v);
        };
        constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr auto write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier(source_motion.Get(),mv_state,read);
        if(source_depth.Get()!=source_motion.Get()) barrier(source_depth.Get(),depth_state,read);
        barrier(motion.Get(),read,write);barrier(depth.Get(),read,write);
        list->SetDescriptorHeaps(1,heap.GetAddressOf());list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
        list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRoot32BitConstants(1,sizeof(c)/4,&c,0);list->Dispatch((c.output[0]+7)/8,(c.output[1]+7)/8,1);
        barrier(motion.Get(),write,read);barrier(depth.Get(),write,read);
        barrier(source_motion.Get(),read,mv_state);
        if(source_depth.Get()!=source_motion.Get()) barrier(source_depth.Get(),read,depth_state);
    }
};
}
