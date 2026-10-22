#include "../src/debug_number12.hpp"
#include "../src/d3d12_output_contract.hpp"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace cheeky::foveated_dlss;
static void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D12 test call failed");}
static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Gpu {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12InfoQueue> messages;
    Gpu(){ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC q{};check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));}
    ComPtr<ID3D12Resource> buffer(UINT64 bytes,D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;}
    ComPtr<ID3D12Resource> texture(unsigned w,unsigned h,unsigned slices,unsigned mips,DXGI_FORMAT format,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=UINT16(slices);d.MipLevels=UINT16(mips);d.Format=format;d.Flags=flags;d.SampleDesc.Count=1;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;}
    void barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,0,from,to};list->ResourceBarrier(1,&b);}
    void finish(){check(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);ComPtr<ID3D12Fence> done;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(queue->Signal(done.Get(),1));check(done->SetEventOnCompletion(1,event));WaitForSingleObject(event,10000);CloseHandle(event);
        if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){SIZE_T bytes{};messages->GetMessage(i,nullptr,&bytes);std::vector<char> storage(bytes);auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            check(messages->GetMessage(i,m,&bytes));if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"%s\n",m->pDescription);throw std::runtime_error("D3D12 validation error");}}}
};
int main(){try{
    Gpu gpu;
    auto exposure=gpu.texture(1,1,1,1,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    auto exposure_desc=exposure->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT efoot{};UINT64 bytes{};gpu.device->GetCopyableFootprints(&exposure_desc,0,1,0,&efoot,nullptr,nullptr,&bytes);
    auto upload=gpu.buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);void* data{};check(upload->Map(0,nullptr,&data));*static_cast<float*>(data)=1.F/256;upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION esrc{},edst{};esrc.pResource=upload.Get();esrc.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;esrc.PlacedFootprint=efoot;
    edst.pResource=exposure.Get();edst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;gpu.list->CopyTextureRegion(&edst,0,0,0,&esrc,nullptr);
    gpu.barrier(exposure.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto target=gpu.texture(140,100,4,3,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    DebugExposureScope exposure_scope{{exposure.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,.5F,2.F}};
    DebugNumber12 number;require(number.prepare(gpu.device.Get(),target.Get()),"Prepare number renderer");
    number.draw(gpu.list.Get(),0,0,140,100,2,"00100" "01100" "00100" "00100" "00100" "00100" "01110");
    gpu.barrier(target.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto td=target->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};gpu.device->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,&bytes);
    auto readback=gpu.buffer(bytes,D3D12_HEAP_TYPE_READBACK);D3D12_TEXTURE_COPY_LOCATION from{},to{};
    from.pResource=target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
    gpu.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);

    // Feed the same four-slice/mip allocation to the actual peripheral shader.
    gpu.barrier(target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto reduced=gpu.texture(140,100,1,1,td.Format,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
    D3D12_ROOT_PARAMETER params[3]{};for(unsigned i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,6};D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=3;rd.pParameters=params;
    ComPtr<ID3DBlob> blob,errors;ComPtr<ID3D12RootSignature> root;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));check(gpu.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={d3d_shaders::peripheral12_color.data,d3d_shaders::peripheral12_color.size};ComPtr<ID3D12PipelineState> pipeline;check(gpu.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};ComPtr<ID3D12DescriptorHeap> descriptors;check(gpu.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&descriptors)));
    auto cpu=descriptors->GetCPUDescriptorHandleForHeapStart();const auto srv=d3d12_composite_srv(td.Format);gpu.device->CreateShaderResourceView(target.Get(),&srv,cpu);
    const auto stride=gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);cpu.ptr+=stride;D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=td.Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;gpu.device->CreateUnorderedAccessView(reduced.Get(),nullptr,&uav,cpu);
    ID3D12DescriptorHeap* heaps[]{descriptors.Get()};gpu.list->SetDescriptorHeaps(1,heaps);gpu.list->SetComputeRootSignature(root.Get());gpu.list->SetPipelineState(pipeline.Get());auto handle=descriptors->GetGPUDescriptorHandleForHeapStart();gpu.list->SetComputeRootDescriptorTable(0,handle);handle.ptr+=stride;gpu.list->SetComputeRootDescriptorTable(1,handle);
    unsigned constants[]{0,0,140,100,140,100};gpu.list->SetComputeRoot32BitConstants(2,6,constants,0);gpu.list->Dispatch(9,7,1);
    gpu.barrier(reduced.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto result=gpu.buffer(bytes,D3D12_HEAP_TYPE_READBACK);from.pResource=reduced.Get();to.pResource=result.Get();gpu.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    gpu.finish();
    for(auto* r:{readback.Get(),result.Get()}){check(r->Map(0,nullptr,&data));auto* p=static_cast<unsigned char*>(data);
        const auto white=*reinterpret_cast<float*>(p+9*footprint.Footprint.RowPitch+14*16);
        const auto black=*reinterpret_cast<float*>(p+8*footprint.Footprint.RowPitch+9*16);
        require(std::abs(white-64.F)<.01F,"Glyph must use pre/(scale*exposure), including through array SRV");require(black==0.F,"Glyph background stays black");r->Unmap(0,nullptr);}
    std::puts("Quad DX12: exposure-correct glyph pixels and four-slice/mip peripheral sampling passed");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
