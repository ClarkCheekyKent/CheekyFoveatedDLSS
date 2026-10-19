#pragma once
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>

namespace cheeky::foveated_dlss {
// DXGI reports this process's usage and OS-assigned budget on the selected
// adapter/node. It does not report whole-system usage or identify NGX allocations.
struct GpuMemory {
    struct Segment {
        bool valid{};
        HRESULT result{E_NOINTERFACE};
        std::uint64_t usage{},budget{},headroom{},over_budget{},peak{},over_budget_samples{};
        void update(HRESULT hr,const DXGI_QUERY_VIDEO_MEMORY_INFO& info) noexcept {
            result=hr;valid=SUCCEEDED(hr);
            usage=valid?info.CurrentUsage:0;budget=valid?info.Budget:0;
            headroom=budget>usage?budget-usage:0;over_budget=usage>budget?usage-budget:0;
            if(valid){peak=(std::max)(peak,usage);if(over_budget)++over_budget_samples;}
        }
    } local,nonlocal;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    HRESULT adapter_result{E_NOINTERFACE};
    LUID luid{};unsigned node{};char name[256]{};
    std::uint64_t dedicated_capacity{},samples{},sampled_at{},next_poll{};
    bool baseline_valid{},baseline_nonlocal_valid{},baseline_fg_enabled{};
    std::uint64_t baseline_local{},baseline_nonlocal{},baseline_at{};

    void attach(unsigned renderer,void* device,void* queue) noexcept {
        *this={};if(!device||renderer>1)return;
        Microsoft::WRL::ComPtr<IDXGIAdapter> base;
        if(renderer==1){
            auto* dev=static_cast<ID3D12Device*>(device);luid=dev->GetAdapterLuid();
            if(queue){auto mask=static_cast<ID3D12CommandQueue*>(queue)->GetDesc().NodeMask;
                while(mask&&!(mask&1u)){++node;mask>>=1;}}
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            adapter_result=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            if(SUCCEEDED(adapter_result))adapter_result=factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&base));
        }else{
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
            adapter_result=static_cast<ID3D11Device*>(device)->QueryInterface(IID_PPV_ARGS(&dxgi));
            if(SUCCEEDED(adapter_result))adapter_result=dxgi->GetAdapter(&base);
        }
        if(FAILED(adapter_result)||!base)return;
        DXGI_ADAPTER_DESC desc{};
        if(SUCCEEDED(base->GetDesc(&desc))){
            luid=desc.AdapterLuid;dedicated_capacity=desc.DedicatedVideoMemory;
            WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,sizeof(name),nullptr,nullptr);
        }
        adapter_result=base.As(&adapter);
    }
    bool poll(std::uint64_t now,bool force=false) noexcept {
        if(!force&&samples&&now<next_poll)return false;
        next_poll=now+1000;sampled_at=now;++samples;
        DXGI_QUERY_VIDEO_MEMORY_INFO a{},b{};
        local.update(adapter?adapter->QueryVideoMemoryInfo(node,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&a):adapter_result,a);
        nonlocal.update(adapter?adapter->QueryVideoMemoryInfo(node,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&b):adapter_result,b);
        return true;
    }
    bool mark_baseline(std::uint64_t now,bool fg_enabled) noexcept {
        poll(now,true);baseline_valid=local.valid;
        baseline_nonlocal_valid=local.valid&&nonlocal.valid;
        baseline_fg_enabled=fg_enabled;baseline_at=now;
        baseline_local=local.usage;baseline_nonlocal=nonlocal.usage;
        return baseline_valid;
    }
};
}
