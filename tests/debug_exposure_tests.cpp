// Run through CheekyTests --debug-exposure. Readbacks here are test-only.
#include "../src/debug_marker12.hpp"
#include "../src/d3d12_composite_shader.hpp"
#include "../src/composite_constants.hpp"
#include <dxgi1_4.h>
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <limits>
#include <source_location>
namespace {
using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
void check(HRESULT h, std::source_location where=std::source_location::current()) { if(FAILED(h)) { std::fprintf(stderr,"HRESULT=%08X line=%u\n",unsigned(h),where.line()); throw std::runtime_error("GPU operation failed"); } }
void barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
 D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,0,a,b};l->ResourceBarrier(1,&v);
}
ComPtr<ID3D12Resource> buffer(ID3D12Device* d,UINT64 bytes,D3D12_HEAP_TYPE type) {
 D3D12_HEAP_PROPERTIES h{};h.Type=type;
 D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;r.Width=bytes;r.Height=r.DepthOrArraySize=r.MipLevels=1;r.SampleDesc.Count=1;r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 ComPtr<ID3D12Resource> b;check(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&r,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&b)));return b;
}
void run(float pre,float scale,float exposure_value,float expected,bool normalize=true,DXGI_FORMAT output_format=DXGI_FORMAT_R32G32B32A32_FLOAT) {
 ComPtr<IDXGIFactory4> f;check(CreateDXGIFactory1(IID_PPV_ARGS(&f)));ComPtr<IDXGIAdapter> a;check(f->EnumWarpAdapter(IID_PPV_ARGS(&a)));
 ComPtr<ID3D12Device> d;check(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
 ComPtr<ID3D12CommandQueue> q;D3D12_COMMAND_QUEUE_DESC qd{};check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
 ComPtr<ID3D12CommandAllocator> allocator;check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
 ComPtr<ID3D12GraphicsCommandList> l;check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&l)));
 std::vector<ComPtr<ID3D12Resource>> uploads;
 auto texture=[&](unsigned side,DXGI_FORMAT format,float value,bool uav) {
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=side;desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=format;desc.SampleDesc.Count=1;desc.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
  D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> t;check(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&t)));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};d->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);auto upload=buffer(d.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD);
  unsigned char* p{};check(upload->Map(0,nullptr,reinterpret_cast<void**>(&p)));std::memset(p,0,size_t(bytes));
  unsigned channels=format==DXGI_FORMAT_R11G11B10_FLOAT?1:format==DXGI_FORMAT_R32G32_FLOAT?2:4;
  for(unsigned y=0;y<side;++y) for(unsigned x=0;x<side;++x) {
   auto* pixel=reinterpret_cast<float*>(p+y*fp.Footprint.RowPitch)+x*channels;
   for(unsigned c=0;c<channels;++c) pixel[c]=c==3?1:value;
   if(channels==1) { const std::uint32_t quarter=0x340U|(0x340U<<11)|(0x1a0U<<22); std::memcpy(pixel,&quarter,4); }
   if(channels==2) pixel[1]=999; // Only red contains exposure.
  }
  upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=t.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  barrier(l.Get(),t.Get(),D3D12_RESOURCE_STATE_COPY_DEST,uav?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);uploads.push_back(upload);return t;
 };
 auto exposure=texture(1,DXGI_FORMAT_R32G32_FLOAT,exposure_value,false);
 auto color=texture(128,DXGI_FORMAT_R32G32B32A32_FLOAT,.25F,false);
 auto output=texture(128,output_format,.25F,true);
 DebugExposureScope scope(normalize?DebugExposure{exposure.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,pre,scale}:DebugExposure{});
 D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};ComPtr<ID3D12DescriptorHeap> heap;check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
 const auto stride=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
 D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2DArray.MipLevels=srv.Texture2DArray.ArraySize=1;
 d->CreateShaderResourceView(color.Get(),&srv,cpu);cpu.ptr+=stride;d->CreateShaderResourceView(color.Get(),&srv,cpu);cpu.ptr+=stride;
 D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=output_format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;uav.Texture2DArray.ArraySize=1;d->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);cpu.ptr+=stride;
 srv={};srv.Format=DXGI_FORMAT_R32G32_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;d->CreateShaderResourceView(exposure.Get(),&srv,cpu);
 D3D12_DESCRIPTOR_RANGE srvs[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,2,0,3}},ur{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
 D3D12_ROOT_PARAMETER params[3]{};params[0].ParameterType=params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,srvs};params[1].DescriptorTable={1,&ur};params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,44};
 D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=3;rd.pParameters=params;ComPtr<ID3DBlob> sig,shader,errors;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&errors));ComPtr<ID3D12RootSignature> root;check(d->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root)));
 check(D3DCompile(composite_shader_source,sizeof(composite_shader_source)-1,nullptr,nullptr,nullptr,"CompositeMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&errors));
 D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={shader->GetBufferPointer(),shader->GetBufferSize()};ComPtr<ID3D12PipelineState> pipeline;check(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
 CompositeConstants c{};c.output_size[0]=c.output_size[1]=c.input_size[0]=c.input_size[1]=c.rect_size[0]=c.rect_size[1]=128;c.shape_width=c.shape_height=1;c.show_alignment_border=1;c.exposure_white_multiplier=normalize?pre/scale:0;
 ID3D12DescriptorHeap* heaps[]{heap.Get()};l->SetDescriptorHeaps(1,heaps);l->SetComputeRootSignature(root.Get());l->SetPipelineState(pipeline.Get());auto gpu=heap->GetGPUDescriptorHandleForHeapStart();l->SetComputeRootDescriptorTable(0,gpu);gpu.ptr+=2*stride;l->SetComputeRootDescriptorTable(1,gpu);l->SetComputeRoot32BitConstants(2,44,&c,0);l->Dispatch(8,8,1);
 D3D12_RESOURCE_BARRIER order{};order.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;order.UAV.pResource=output.Get();l->ResourceBarrier(1,&order);
 DebugMarker12 marker;std::uint64_t allocations{};
 if(normalize) { if(!marker.prepare(d.Get(),output.Get(),allocations)) throw std::runtime_error("Marker preparation failed");marker.draw(l.Get(),0,16,16,0,true); }
 else if(marker.prepare(d.Get(),output.Get(),allocations)) throw std::runtime_error("Missing exposure must leave marker path unchanged");
 barrier(l.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
 auto read=buffer(d.Get(),128*128*16,D3D12_HEAP_TYPE_READBACK);D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Footprint={output_format,128,128,1,128*calibration_pixel_bytes(output_format)};src.pResource=output.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);check(l->Close());ID3D12CommandList* lists[]{l.Get()};q->ExecuteCommandLists(1,lists);
 ComPtr<ID3D12Fence> fence;check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));check(q->Signal(fence.Get(),1));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(fence->SetEventOnCompletion(1,event));if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");CloseHandle(event);
 float* pixels{};check(read->Map(0,nullptr,reinterpret_cast<void**>(&pixels)));
 auto at=[&](unsigned x,unsigned y,unsigned channel){ const auto pixel=calibration_decode(reinterpret_cast<unsigned char*>(pixels)+(y*128+x)*calibration_pixel_bytes(output_format),output_format); return channel==0?pixel.r:channel==1?pixel.g:channel==2?pixel.b:pixel.a; };
 if(std::abs(at(0,90,0)-expected)>expected*.001F || at(0,90,1)!=0 || at(0,90,2)!=0 || at(64,90,0)!=.25F) throw std::runtime_error("Border/scene pixels incorrect");
 if(normalize && (std::abs(at(2,2,0)-expected)>expected*.001F || at(12,12,0)!=0 || at(2,2,3)!=1)) throw std::runtime_error("Marker pixels incorrect");
 read->Unmap(0,nullptr);std::printf("PASS GPU border/stamp exposure: pre=%g scale=%g exposure=%g white=%g enabled=%d\n",pre,scale,exposure_value,expected,normalize);
}
} // namespace
int run_debug_exposure_tests(){try {
 run(.0625F,1,.000625F,100);run(.0625F,1,.000625F,100,true,DXGI_FORMAT_R11G11B10_FLOAT);run(16,1,16,1);run(.0625F,2,.000625F,50);
 run(1,1,0,1);run(1,1,std::numeric_limits<float>::quiet_NaN(),1);run(1,1,1,1,false);
 return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
