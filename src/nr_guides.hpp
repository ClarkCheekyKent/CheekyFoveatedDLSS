#pragma once
#include <initializer_list>
#include <cstdio>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace cheeky::foveated_dlss {
// Resource ownership is supplied by the NR cache's recording/fence lifetime.
struct NrGuideConstants {
    unsigned output[2], region_base[2], region_size[2], processing[2];
    unsigned motion_full[2], depth_full[2];
    float motion_origin[2], depth_origin[2];
    float motion_scale[2], motion_offset[2];
};
static_assert(sizeof(NrGuideConstants) == 80);
inline constexpr char nr_guide_shader[] = R"(
Texture2D<float2> Motion : register(t0);
Texture2D<float> Depth : register(t1);
RWTexture2D<float2> OutMotion : register(u0);
RWTexture2D<float> OutDepth : register(u1);
cbuffer Constants : register(b0) {
 uint2 Size, RegionBase, RegionSize, Processing;
 uint2 MotionFull, DepthFull;
 float2 MotionOrigin, DepthOrigin, MotionScale, MotionOffset;
};
// Exact floor(a*b/divisor) without optional shader-double support. The float
// estimate is within one at texture-sized quotients; verify it with 64-bit
// products expressed as pairs of uints.
void multiplyWide(uint a,uint b,out uint lo,out uint hi) {
 uint p0=(a&65535)*(b&65535), p1=(a>>16)*(b&65535);
 uint p2=(a&65535)*(b>>16), p3=(a>>16)*(b>>16);
 uint middle=(p0>>16)+(p1&65535)+(p2&65535);
 lo=(p0&65535)|(middle<<16); hi=p3+(p1>>16)+(p2>>16)+(middle>>16);
}
bool greater(uint lo, uint hi, uint otherLo, uint otherHi) {
 return hi > otherHi || (hi == otherHi && lo > otherLo);
}
uint divideProduct(uint a, uint b, uint divisor) {
 uint lo,hi,ql,qh; multiplyWide(a,b,lo,hi);
 uint q=uint(float(a)*float(b)/float(divisor));
 multiplyWide(q,divisor,ql,qh);
 if(greater(ql,qh,lo,hi)) --q;
 else { multiplyWide(q+1,divisor,ql,qh); if(!greater(ql,qh,lo,hi)) ++q; }
 return q;
}
uint sampleAxis(uint pixel,uint base,uint region,uint outputSize,uint fullSize,uint processingSize) {
 return divideProduct(2*base*outputSize+(2*pixel+1)*region,fullSize,2*outputSize*processingSize);
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
 if (any(id.xy >= Size)) return;
 // Use full-view pixel centers, never the rounded enclosing guide rectangle.
 uint mw,mh,dw,dh; Motion.GetDimensions(mw,mh); Depth.GetDimensions(dw,dh);
 uint2 mpFull=uint2(sampleAxis(id.x,RegionBase.x,RegionSize.x,Size.x,MotionFull.x,Processing.x),
                    sampleAxis(id.y,RegionBase.y,RegionSize.y,Size.y,MotionFull.y,Processing.y));
 uint2 dpFull=uint2(sampleAxis(id.x,RegionBase.x,RegionSize.x,Size.x,DepthFull.x,Processing.x),
                    sampleAxis(id.y,RegionBase.y,RegionSize.y,Size.y,DepthFull.y,Processing.y));
 int2 mp=clamp(int2(MotionOrigin)+int2(mpFull),int2(0,0),int2(mw,mh)-1);
 int2 dp=clamp(int2(DepthOrigin)+int2(dpFull),int2(0,0),int2(dw,dh)-1);
 float2 mv=Motion.Load(int3(mp,0));
 OutMotion[id.xy] = any(!isfinite(mv)) || any(abs(mv)>1e15) ? mv : mv*MotionScale+MotionOffset;
 OutDepth[id.xy] = Depth.Load(int3(dp,0));
}
)";
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
    bool initialize(ID3D12Resource* mv, ID3D12Resource* z, unsigned w, unsigned h) {
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
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0}; ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,2};
        D3D12_ROOT_PARAMETER params[2]{}; params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable={2,ranges};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[1].Constants={0,0,sizeof(NrGuideConstants)/4};
        D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=2; rd.pParameters=params;
        Ptr<ID3DBlob> blob,errors,code;
        if (FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors)) ||
            FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))) ||
            FAILED(D3DCompile(nr_guide_shader,sizeof(nr_guide_shader),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,&errors))) { if(errors) std::fprintf(stderr,"%s\n",static_cast<const char*>(errors->GetBufferPointer())); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get(); pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
        return SUCCEEDED(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
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
