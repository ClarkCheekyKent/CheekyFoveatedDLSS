#pragma once
#include "d3d12_native.hpp"
#include "../shared/fg_guide_layout.hpp"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstring>

namespace cheeky::foveated_dlss {
// Recorded at the end of NGX evaluation, inside NGX's existing command-state
// invalidation boundary. The engine restores its bindings after EvaluateFeature.
// NGX command-state contract: https://docs.nvidia.com/ngx/latest/pdf/Programming-Guide.pdf
// Source states are unchanged. Each Shot exclusively owns its descriptor heap
// until producer retirement AND the consumer lease permit reuse.
struct FgPreparedGuides {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Buffers {
        struct Pass {Ptr<ID3D12Resource> depth,motion;};
        std::array<Pass,2> passes;
        Ptr<ID3D12DescriptorHeap> descriptors;
        void clear(){passes={};descriptors.Reset();}
    };
    Ptr<ID3D12RootSignature> root;
    Ptr<ID3D12PipelineState> pipeline;
    struct Constants {float rect[4]{},offset[4]{},dimensions[4]{},motion[4]{},jitter[4]{},options[4]{};};
    bool initialize(ID3D12Device* device) {
        if(root&&pipeline)return true;
        constexpr char shader[]=R"HLSL(
Texture2D<float> Depth:register(t0);Texture2D<float2> Motion:register(t1);
RWTexture2D<float> OutDepth:register(u0);RWTexture2D<float2> OutMotion:register(u1);
cbuffer Params:register(b0){float4 Rect;float4 GuideOffset;float4 Dimensions;float4 MotionAdjust;float4 JitterCorrection;float4 Options;}
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 if(id.x>=uint(Options.z)||id.y>=uint(Options.w))return;
 float2 uv=Rect.xy+(float2(id.xy)+.5)/Options.zw*Rect.zw;
 if(Options.x>.5){uint dw,dh;Depth.GetDimensions(dw,dh);
  uint2 p=min(uint2(uv*Dimensions.xy+GuideOffset.xy),uint2(dw,dh)-1);OutDepth[id.xy]=Depth.Load(int3(p,0));}
 if(Options.y>.5){uint mw,mh;Motion.GetDimensions(mw,mh);
  uint2 p=min(uint2(uv*Dimensions.zw+GuideOffset.zw),uint2(mw,mh)-1);
  float2 mv=Motion.Load(int3(p,0));bool good=all(isfinite(mv))&&all(abs(mv)<65504.0);
  mv=clamp(mv*MotionAdjust.xy-JitterCorrection.xy,-MotionAdjust.z,MotionAdjust.z);
  OutMotion[id.xy]=good?mv:float2(MotionAdjust.z,MotionAdjust.z);}
})HLSL";
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,2}};
        D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,sizeof(Constants)/4};
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=2;desc.pParameters=params;
        Ptr<ID3DBlob> blob,error,code;
        if(!root&&(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))||
            FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)))))return false;
        if(FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"Main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error)))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS={code->GetBufferPointer(),code->GetBufferSize()};
        return SUCCEEDED(device->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipeline)));
    }
    static bool allocate(ID3D12Device* device,Ptr<ID3D12Resource>& out,unsigned w,unsigned h,DXGI_FORMAT format) {
        if(out){const auto d=out->GetDesc();if(d.Width==w&&d.Height==h&&d.Format==format)return true;}
        out.Reset();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
        desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)));
    }
    static void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
        if(!texture||from==to)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
    }
    bool record(ID3D12Device* device,ID3D12GraphicsCommandList* list,CheekyFgEye& g,Buffers& buffers) {
        const auto depth=native_d3d12_interface(g.depth),motion=native_d3d12_interface(g.motion);
        if(!depth||!motion)return false;
        const auto dd=depth->GetDesc(),md=motion->GetDesc();
        const auto df=cheeky::fg::guide_depth_format(dd.Format),mf=cheeky::fg::guide_motion_format(md.Format);
        if(df==DXGI_FORMAT_UNKNOWN||mf==DXGI_FORMAT_UNKNOWN||
           ((dd.Flags|md.Flags)&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||!initialize(device))return false;
        auto active=[](unsigned total,unsigned base,unsigned requested){return base<total?std::min(requested?requested:total,total-base):0u;};
        const unsigned dw=active(unsigned(dd.Width),g.depth_base_x,g.depth_width),dh=active(dd.Height,g.depth_base_y,g.depth_height),
            mw=active(unsigned(md.Width),g.motion_base_x,g.motion_width),mh=active(md.Height,g.motion_base_y,g.motion_height);
        if(!dw||!dh||!mw||!mh)return false;
        const std::array<cheeky::fg::GuideGrid,2> grids{cheeky::fg::guide_grid(g,dw,dh,mw,mh,0),cheeky::fg::guide_grid(g,dw,dh,mw,mh,1)};
        // Complete all fallible allocation/validation before altering the list.
        for(unsigned pass=0;pass<2;++pass){const auto& grid=grids[pass];auto& p=buffers.passes[pass];
            if(!grid.depth)p.depth.Reset();if(!grid.motion)p.motion.Reset();
            if((grid.depth&&!allocate(device,p.depth,grid.width,grid.height,grid.depth_bits==16?DXGI_FORMAT_R16_FLOAT:DXGI_FORMAT_R32_FLOAT))||
               (grid.motion&&!allocate(device,p.motion,grid.width,grid.height,grid.motion_bits==16?DXGI_FORMAT_R16G16_FLOAT:DXGI_FORMAT_R32G32_FLOAT)))return false;
        }
        if(!buffers.descriptors){D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;desc.NumDescriptors=8;desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if(FAILED(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&buffers.descriptors))))return false;}
        const unsigned stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu=buffers.descriptors->GetCPUDescriptorHandleForHeapStart();
        for(unsigned pass=0;pass<2;++pass){const auto& p=buffers.passes[pass];
            for(unsigned input=0;input<2;++input){D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=input?mf:df;
                view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
                device->CreateShaderResourceView(input?motion.Get():depth.Get(),&view,cpu);cpu.ptr+=stride;}
            unsigned i{};for(auto* texture:{p.depth.Get(),p.motion.Get()}){D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
                view.Format=texture?texture->GetDesc().Format:(i?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R32_FLOAT);++i;
                view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(texture,nullptr,&view,cpu);cpu.ptr+=stride;}
        }
        ID3D12DescriptorHeap* heaps[]{buffers.descriptors.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
        for(unsigned pass=0;pass<2;++pass){const auto& grid=grids[pass];const auto& p=buffers.passes[pass];if(!grid.motion&&!grid.depth)continue;
            Constants c;c.rect[0]=float(grid.x)/g.width;c.rect[1]=float(grid.y)/g.height;c.rect[2]=float(grid.w)/g.width;c.rect[3]=float(grid.h)/g.height;
            c.offset[0]=float(g.depth_base_x);c.offset[1]=float(g.depth_base_y);c.offset[2]=float(g.motion_base_x);c.offset[3]=float(g.motion_base_y);
            c.dimensions[0]=float(dw);c.dimensions[1]=float(dh);c.dimensions[2]=float(mw);c.dimensions[3]=float(mh);
            c.motion[0]=g.motion_scale_x/(mw*c.rect[2]);c.motion[1]=g.motion_scale_y/(mh*c.rect[3]);c.motion[2]=grid.motion_bits==16?65504.f:3.402823466e38f;
            c.jitter[0]=g.jitter_correction_x/c.rect[2];c.jitter[1]=g.jitter_correction_y/c.rect[3];
            c.options[0]=grid.depth?1.f:0.f;c.options[1]=grid.motion?1.f:0.f;c.options[2]=float(grid.width);c.options[3]=float(grid.height);
            for(auto* texture:{p.depth.Get(),p.motion.Get()})barrier(list,texture,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            auto gpu=buffers.descriptors->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=pass*4*stride;list->SetComputeRootDescriptorTable(0,gpu);
            list->SetComputeRoot32BitConstants(1,sizeof(Constants)/4,&c,0);list->Dispatch((grid.width+7)/8,(grid.height+7)/8,1);
            for(auto* texture:{p.depth.Get(),p.motion.Get()})barrier(list,texture,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        }
        g.depth_width=dw;g.depth_height=dh;g.motion_width=mw;g.motion_height=mh;
        g.depth=buffers.passes[0].depth.Get();g.motion=buffers.passes[0].motion.Get();
        g.peripheral_depth=buffers.passes[1].depth.Get();g.peripheral_motion=buffers.passes[1].motion.Get();
        g.prepared_guides=1;return true;
    }
};
}
