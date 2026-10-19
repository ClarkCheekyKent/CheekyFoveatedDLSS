#pragma once
#include "cheeky_fg_bridge.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>

namespace cheeky::fg_memory {
// Cache the driver's allocation requirement on the resource itself. This does
// not create, retain, map or wait for GPU work. Residency remains a DXGI metric.
inline std::uint64_t allocation_bytes(ID3D12Device* device,ID3D12Resource* resource) noexcept {
    if(!resource)return 0;
    static constexpr GUID key{0x755a6b15,0x0627,0x4aac,{0x91,0x44,0x44,0x13,0x88,0xa6,0x34,0xe0}};
    std::uint64_t bytes{};UINT size=sizeof(bytes);
    if(SUCCEEDED(resource->GetPrivateData(key,&size,&bytes))&&size==sizeof(bytes)&&bytes)return bytes;
    Microsoft::WRL::ComPtr<ID3D12Device> owned_device;
    if(!device){if(FAILED(resource->GetDevice(IID_PPV_ARGS(&owned_device))))return 0;device=owned_device.Get();}
    const auto desc=resource->GetDesc();D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
    const unsigned nodes=device->GetNodeCount()==1?0u:
        SUCCEEDED(resource->GetHeapProperties(&heap,&flags))?heap.VisibleNodeMask:1u;
    const auto info=device->GetResourceAllocationInfo(nodes,1,&desc);
    if(!info.SizeInBytes||info.SizeInBytes==UINT64_MAX)return 0;
    bytes=info.SizeInBytes;resource->SetPrivateData(key,sizeof(bytes),&bytes);return bytes;
}
struct Collector {
    ID3D12Device* device{};
    std::array<IUnknown*,512> seen{};unsigned seen_count{};
    explicit Collector(ID3D12Device* d=nullptr):device(d){}
    void add(CheekyFgMemoryBucket& bucket,ID3D12Resource* resource,bool reusable=false) {
        if(!resource)return;
        Microsoft::WRL::ComPtr<IUnknown> identity;
        IUnknown* key=resource;
        if(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&identity))))key=identity.Get();
        if(std::find(seen.begin(),seen.begin()+seen_count,key)!=seen.begin()+seen_count)return;
        if(seen_count==seen.size()){++bucket.resources;++bucket.unknown_resources;return;}
        seen[seen_count++]=key;
        const auto bytes=allocation_bytes(device,resource);++bucket.resources;bucket.bytes+=bytes;
        if(!bytes)++bucket.unknown_resources;
        if(reusable){++bucket.reusable_resources;bucket.reusable_bytes+=bytes;}
    }
};
}
