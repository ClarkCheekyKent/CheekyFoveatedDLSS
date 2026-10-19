#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <cstdint>

namespace cheeky::gpu {
struct Summary {std::uint64_t samples{};double mean{},last{};};
// Merge GPU command batches into an application frame or presentation. A
// closure is committed only after all completed contexts have been collected.
class Groups {
    struct Group {double ms{};unsigned count{};bool closed{};};
    mutable std::mutex mutex;
    std::map<std::uint64_t,Group> pending;
    std::array<double,60> window{};
    unsigned count{},next{};
    std::uint64_t samples{};
    double sum{},last{};
public:
    void add(std::uint64_t frame,double ms,bool closed=false,unsigned expected=0) {
        if(!frame||!std::isfinite(ms)||ms<0)return;
        std::lock_guard lock(mutex);auto& g=pending[frame];g.ms+=ms;++g.count;
        g.closed|=closed||(expected&&g.count==expected);
        while(pending.size()>64)pending.erase(pending.begin());
    }
    void commit() {
        std::lock_guard lock(mutex);
        for(auto it=pending.begin();it!=pending.end();) {
            if(!it->second.closed){++it;continue;}
            if(count==window.size())sum-=window[next];else ++count;
            window[next]=last=it->second.ms;sum+=last;next=(next+1)%window.size();++samples;
            it=pending.erase(it);
        }
    }
    Summary summary() const {std::lock_guard lock(mutex);return {samples,count?sum/count:0,last};}
    void reset(){std::lock_guard lock(mutex);pending.clear();window={};count=next=0;samples=0;sum=last=0;}
};

// The owner supplies an already-retired fence before collect/reuse. Profiling
// never submits another list, waits for a fence, or changes the work's queue.
struct Span {
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    bool attempted{},pending{};
    bool begin(ID3D12Device* device,ID3D12GraphicsCommandList* list) {
        if(!attempted) {
            attempted=true;D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=2;
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC d{};
            d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=16;d.Height=1;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;
            d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if(FAILED(device->CreateQueryHeap(&q,IID_PPV_ARGS(&heap)))||
               FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)))) {
                heap.Reset();readback.Reset();
            }
        }
        pending=false;if(!heap||!readback)return false;
        list->EndQuery(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);return true;
    }
    void end(ID3D12GraphicsCommandList* list) {
        if(!heap||!readback)return;
        list->EndQuery(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        list->ResolveQueryData(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,readback.Get(),0);pending=true;
    }
    bool collect(std::uint64_t frequency,double& ms,std::uint64_t* begin_tick=nullptr) {
        if(!pending||!frequency)return false;pending=false;
        void* mapped{};D3D12_RANGE range{0,16};if(FAILED(readback->Map(0,&range,&mapped))||!mapped)return false;
        const auto* t=static_cast<const std::uint64_t*>(mapped);const auto a=t[0],b=t[1];
        D3D12_RANGE written{0,0};readback->Unmap(0,&written);
        if(!a||b<a)return false;if(begin_tick)*begin_tick=a;
        ms=double(b-a)*1000./double(frequency);return std::isfinite(ms);
    }
};
}
