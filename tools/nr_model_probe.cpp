// Opt-in hardware experiment against an explicitly supplied NVIDIA NR DLL.
// Uses generated test pixels and private resources; never attaches to a game.
#include "../src/ngx_abi.hpp"
#include <Windows.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
using DirectX::PackedVector::HALF;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;
namespace {
void check(HRESULT r) { if(FAILED(r)) throw std::runtime_error("D3D12 call failed"); }
void ngx(NgxResult r) { if(!ngx_succeeded(r)) { std::printf("NGX failed %08x\n",r);throw std::runtime_error("NGX failure"); } }
template<class T> T proc(HMODULE m,const char* name) { auto p=reinterpret_cast<T>(GetProcAddress(m,name));if(!p)throw std::runtime_error(name);return p; }
struct Tuning { const char* name; float intensity{1}, tone{1}, structure{1}, skin{1}; unsigned mask{}, style{}, preset{}, ui{}; };
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) { std::puts("Usage: nvngx.dll-probe.exe CORE_DLL NR_DLL"); return 2; }
    try {
        const auto core=LoadLibraryW(argv[1]), nr=LoadLibraryW(argv[2]);
        if(!core||!nr) throw std::runtime_error("Load supplied DLLs");
        const auto allocate=proc<NgxResult(*)(NgxParameters**)>(core,"NVSDK_NGX_D3D12_AllocateParameters");
        const auto destroy=proc<NgxResult(*)(NgxParameters*)>(core,"NVSDK_NGX_D3D12_DestroyParameters");
        const auto init=proc<NgxResult(*)(unsigned long long,const wchar_t*,ID3D12Device*,unsigned,const NgxParameters*)>(nr,"NVSDK_NGX_D3D12_Init_Ext");
        const auto create=proc<NgxResult(*)(ID3D12GraphicsCommandList*,unsigned,NgxParameters*,NgxHandle**)>(nr,"NVSDK_NGX_D3D12_CreateFeature");
        const auto evaluate=proc<NgxResult(*)(ID3D12GraphicsCommandList*,const NgxHandle*,const NgxParameters*,NgxProgressCallback)>(nr,"NVSDK_NGX_D3D12_EvaluateFeature");
        const auto release=proc<NgxResult(*)(NgxHandle*)>(nr,"NVSDK_NGX_D3D12_ReleaseFeature");
        ComPtr<ID3D12Device> device; check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{};check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));UINT64 sequence{};
        const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event) throw std::runtime_error("Create event");
        const auto flush=[&] {
            check(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),++sequence));
            check(fence->SetEventOnCompletion(sequence,event));if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU timeout");
            check(allocator->Reset());check(list->Reset(allocator.Get(),nullptr));
        };
        const auto barrier=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
            D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,0,a,b};list->ResourceBarrier(1,&v);
        };
        const auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE type) {
            D3D12_HEAP_PROPERTIES h{};h.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width=bytes;d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;
        };
        constexpr unsigned size=512;
        const auto texture=[&](DXGI_FORMAT format,bool output) {
            D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width=d.Height=size;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=output?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
            ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,output?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;
        };
        auto color=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,false), output=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,true);
        auto motion=texture(DXGI_FORMAT_R32G32_FLOAT,false), depth=texture(DXGI_FORMAT_R32_FLOAT,false);
        std::vector<float> original(size*size*3);
        std::vector<ComPtr<ID3D12Resource>> uploads;
        for(auto* r : {color.Get(),motion.Get(),depth.Get()}) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};auto desc=r->GetDesc();device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
            auto upload=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);unsigned char* data{};check(upload->Map(0,nullptr,reinterpret_cast<void**>(&data)));std::fill(data,data+bytes,static_cast<unsigned char>(0));
            for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
                if(r==depth.Get()) reinterpret_cast<float*>(data+fp.Offset+y*fp.Footprint.RowPitch)[x]=0.5F;
                if(r!=color.Get()) continue;
                const float u=float(x)/size,v=float(y)/size;
                const float detail=((x/12+y/12)%2)*0.08F;
                std::array<float,3> rgb{0.08F+0.7F*u,0.1F+0.65F*v,0.16F+detail};
                const float dx=(u-0.5F)/0.18F,dy=(v-0.5F)/0.27F;
                if(dx*dx+dy*dy<1) rgb={0.63F+0.1F*dx,0.4F+0.08F*dx,0.28F+0.05F*dx};
                auto* pixel=reinterpret_cast<HALF*>(data+fp.Offset+y*fp.Footprint.RowPitch)+x*4;
                for(unsigned c=0;c<3;++c){pixel[c]=XMConvertFloatToHalf(rgb[c]);original[(y*size+x)*3+c]=XMConvertHalfToFloat(pixel[c]);}pixel[3]=XMConvertFloatToHalf(1);
            }
            upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=upload.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=fp;b.pResource=r;b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&b,0,0,0,&a,nullptr);barrier(r,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);uploads.push_back(upload);
        }
        flush();ngx(init(0x0876232cULL,L".",device.Get(),0x15,nullptr));std::puts("Real NR initialized");std::fflush(stdout);
        std::vector<float> baseline;
        for(const auto& t : {Tuning{"baseline"},Tuning{"intensity0",0},Tuning{"intensity0.5",0.5F},Tuning{"intensity2",2},
            Tuning{"tone0",1,0},Tuning{"tone2",1,2},Tuning{"structure0",1,1,0},Tuning{"structure2",1,1,2},
            Tuning{"skin0-mask-off",1,1,1,0,0},Tuning{"mask-on",1,1,1,1,1},Tuning{"skin0-mask-on",1,1,1,0,1},
            Tuning{"skin2-mask-on",1,1,1,2,1},Tuning{"skin-auto",1,1,1,-1,1},
            Tuning{"style1",1,1,1,1,0,1},Tuning{"style2",1,1,1,1,0,2},
            Tuning{"preset1",1,1,1,1,0,0,1},Tuning{"preset2",1,1,1,1,0,0,2},
            Tuning{"preset3",1,1,1,1,0,0,3},Tuning{"preset4",1,1,1,1,0,0,4},
            Tuning{"preset5",1,1,1,1,0,0,5},Tuning{"preset6",1,1,1,1,0,0,6},
            Tuning{"preset7",1,1,1,1,0,0,7},Tuning{"ui-correction",1,1,1,1,0,0,0,1}}) {
            NgxParameters* p{};ngx(allocate(&p));
            for(const char* k: {"Width","Height","OutWidth","OutHeight","DLSSNR.Width","DLSSNR.Height"})p->Set(k,size);
            p->Set("DLSSNR.Enabled",1U);p->Set("CreationNodeMask",1U);p->Set("VisibilityNodeMask",1U);p->Set("DLSSNR.Hint.Render.Preset",t.preset);
            p->Set("DLSSNR.Intensity",t.intensity);p->Set("DLSSNR.LocalToneStrength",t.tone);p->Set("DLSSNR.LocalStructureStrength",t.structure);p->Set("DLSSNR.SkinStructureStrength",t.skin);
            p->Set("DLSSNR.UseAutoMask",t.mask);p->Set("DLSSNR.ControlMask",static_cast<void*>(nullptr));p->Set("DLSSNR.Style",t.style);p->Set("DLSSNR.UICorrection",t.ui);
            NgxHandle* handle{};ngx(create(list.Get(),18,p,&handle));if(!handle)throw std::runtime_error("No handle");flush();
            p->Set("DLSSNR.Color",color.Get());p->Set("DLSSNR.Output",output.Get());p->Set("DLSSNR.Depth",depth.Get());p->Set("DLSSNR.MVec",motion.Get());
            for(const char* resource : {"Color","Output","Depth","MVec"}) {
                const std::string prefix=std::string("DLSSNR.")+resource+"Subrect";
                p->Set((prefix+"BaseX").c_str(),0U);p->Set((prefix+"BaseY").c_str(),0U);p->Set((prefix+"Width").c_str(),size);p->Set((prefix+"Height").c_str(),size);
            }
            p->Set("DLSSNR.MVecScaleX",float(size));p->Set("DLSSNR.MVecScaleY",float(size));p->Set("DLSSNR.DepthInverted",0U);
            for(unsigned frame=0;frame<4;++frame){p->Set("DLSSNR.Reset",frame==0?1U:0U);ngx(evaluate(list.Get(),handle,p,nullptr));flush();}
            auto desc=output->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
            auto readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK);barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=output.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=readback.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=fp;
            list->CopyTextureRegion(&b,0,0,0,&a,nullptr);barrier(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);flush();
            unsigned char* data{};check(readback->Map(0,nullptr,reinterpret_cast<void**>(&data)));std::vector<float> pixels(original.size());
            for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)for(unsigned c=0;c<3;++c)pixels[(y*size+x)*3+c]=XMConvertHalfToFloat(reinterpret_cast<HALF*>(data+fp.Offset+y*fp.Footprint.RowPitch)[x*4+c]);
            readback->Unmap(0,nullptr);if(baseline.empty())baseline=pixels;
            double input_diff{},base_diff{};float max_diff{};unsigned nonfinite{};
            for(size_t i=0;i<pixels.size();++i){if(!std::isfinite(pixels[i]))++nonfinite;input_diff+=std::abs(pixels[i]-original[i]);base_diff+=std::abs(pixels[i]-baseline[i]);max_diff=std::max(max_diff,std::abs(pixels[i]-baseline[i]));}
            std::printf("%-20s inputMAE=%.9f baselineMAE=%.9f max=%.9f nonfinite=%u\n",t.name,input_diff/pixels.size(),base_diff/pixels.size(),max_diff,nonfinite);std::fflush(stdout);
            ngx(release(handle));ngx(destroy(p));
        }
        CloseHandle(event);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
