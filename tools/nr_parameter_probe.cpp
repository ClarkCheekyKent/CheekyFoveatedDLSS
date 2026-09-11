// Read-only driver ABI diagnostic. Does not initialize or evaluate a model.
#include "../src/ngx_abi.hpp"
#include <Windows.h>
#include <cstdio>
using namespace cheeky::foveated_dlss;
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto module = LoadLibraryW(argv[1]);
    if (!module) { std::printf("Load error %lu\n",GetLastError()); return 3; }
    using Allocate = NgxResult(*)(NgxParameters**);
    using Destroy = NgxResult(*)(NgxParameters*);
    const auto allocate = reinterpret_cast<Allocate>(GetProcAddress(module,"NVSDK_NGX_D3D12_AllocateParameters"));
    const auto destroy = reinterpret_cast<Destroy>(GetProcAddress(module,"NVSDK_NGX_D3D12_DestroyParameters"));
    if (!allocate || !destroy) { std::puts("No parameter exports"); return 4; }
    NgxParameters* p{};
    const auto result = allocate(&p);
    std::printf("Allocate: %08x object=%p\n",result,static_cast<void*>(p));
    if (!ngx_succeeded(result) || !p) return 5;
    p->Set("DLSSNR.Intensity",0.375F);
    float f{}; auto r = p->Get("DLSSNR.Intensity",&f);
    std::printf("Typed float: %08x %.9g\n",r,f);
    p->Set("DLSSNR.Enabled",1U);
    unsigned u{}; r = p->Get("DLSSNR.Enabled",&u);
    std::printf("Typed uint: %08x %u\n",r,u);
    // Use real resources so an implementation that retains COM references is safe.
    ID3D12Device* device{}; ID3D12Resource* resource{};
    if (SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=desc.Height=32; desc.DepthOrArraySize=desc.MipLevels=1;
        desc.SampleDesc.Count=1; desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&resource));
    }
    if (resource) {
        for (unsigned kind=0;kind<3;++kind) {
            const char* key = kind==0 ? "DLSSNR.Color" : kind==1 ? "DLSSNR.Output" : "DLSSNR.Depth";
            if(kind==0) p->Set(key,resource);
            if(kind==1) p->Set(key,static_cast<void*>(resource));
            if(kind==2) p->Set(key,reinterpret_cast<unsigned long long>(resource));
            void* v{}; unsigned long long ull{}; ID3D12Resource* typed{};
            const auto a=p->Get(key,&v), b=p->Get(key,&ull), c=p->Get(key,&typed);
            std::printf("Resource setter=%u expected=%p void=%08x/%p ull=%08x/%llx typed=%08x/%p\n",kind,resource,a,v,b,ull,c,typed);
        }
    }
    destroy(p);
    if(resource) resource->Release(); if(device) device->Release();
    return 0;
}
