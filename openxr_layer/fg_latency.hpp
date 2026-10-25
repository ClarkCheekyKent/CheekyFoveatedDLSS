#pragma once
#include "fg_timing.hpp"
#include "../shared/cheeky_fg_bridge.hpp"
#include <Windows.h>

namespace cheeky::fg {
// Sparse diagnostic only. No additional CPU fence wait is used to collect.
// Queue markers bracket the producer dependency; FG markers bracket the work.
struct LatencyProbe {
    using Fence=Microsoft::WRL::ComPtr<ID3D12Fence>;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>,2> allocators;
    std::array<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>,2> lists;
    Fence own_fence,retire_fence;
    std::uint64_t serial{},retire_value{},frequency{},attempts{},samples{},rejected{};
    std::uint64_t before_gpu{},before_cpu{},enqueued_cpu{},submitted_cpu{};
    bool initialized{},enabled{},active{},end_recorded{},valid{};
    std::array<TimingAverage,7> stages;
    void clear_statistics() noexcept {stages={};attempts=samples=rejected=0;valid=false;}
    static std::uint64_t cpu_now() noexcept {LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
    static double cpu_frequency() noexcept {LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return double(f.QuadPart);}
    void reset() noexcept {
        if(retire_fence&&device)while(retire_fence->GetCompletedValue()<retire_value&&SUCCEEDED(device->GetDeviceRemovedReason()))Sleep(1);
        heap.Reset();readback.Reset();allocators={};lists={};own_fence.Reset();retire_fence.Reset();device.Reset();
        serial=retire_value=frequency=attempts=samples=rejected=0;initialized=enabled=active=end_recorded=valid=false;stages={};
    }
    bool initialize(ID3D12Device* dev,ID3D12CommandQueue* queue) noexcept {
        if(initialized)return true;
        heap.Reset();readback.Reset();own_fence.Reset();allocators={};lists={};
        device=dev;if(FAILED(queue->GetTimestampFrequency(&frequency))||!frequency)return false;
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=4;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=32;d.Height=1;
        d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(dev->CreateQueryHeap(&q,IID_PPV_ARGS(&heap)))||
           FAILED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)))||
           FAILED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&own_fence))))return false;
        for(unsigned i=0;i<2;++i){
            if(FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators[i])))||
               FAILED(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[i].Get(),nullptr,IID_PPV_ARGS(&lists[i])))||
               FAILED(lists[i]->Close()))return false;
        }
        initialized=true;return true;
    }
    HRESULT enqueue_wait(ID3D12Device* dev,ID3D12CommandQueue* queue,ID3D12Fence* dependency,
                         std::uint64_t value,bool requested) noexcept {
        active=end_recorded=false;
        if(enabled!=requested){enabled=requested;clear_statistics();}
        const bool sample=enabled&&(attempts++%17)==0;
        if(!sample)return queue->Wait(dependency,value);
        const auto unavailable=[&]{++rejected;valid=false;return queue->Wait(dependency,value);};
        if((retire_fence&&retire_fence->GetCompletedValue()<retire_value)||!initialize(dev,queue))return unavailable();
        for(unsigned i=0;i<2;++i){
            if(FAILED(allocators[i]->Reset())||FAILED(lists[i]->Reset(allocators[i].Get(),nullptr)))return unavailable();
            lists[i]->EndQuery(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,i);
            if(FAILED(lists[i]->Close()))return unavailable();
        }
        if(FAILED(queue->GetClockCalibration(&before_gpu,&before_cpu)))return unavailable();
        enqueued_cpu=cpu_now();submitted_cpu=0;
        ID3D12CommandList* a[]{lists[0].Get()};queue->ExecuteCommandLists(1,a);
        const HRESULT result=queue->Wait(dependency,value);
        ID3D12CommandList* b[]{lists[1].Get()};queue->ExecuteCommandLists(1,b);
        retire_fence=own_fence;retire_value=++serial;
        if(FAILED(queue->Signal(own_fence.Get(),serial)))return E_FAIL;
        active=SUCCEEDED(result);return result;
    }
    void begin(ID3D12GraphicsCommandList* list) noexcept {if(active)list->EndQuery(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2);}
    void end(ID3D12GraphicsCommandList* list) noexcept {
        if(!active)return;
        list->EndQuery(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,3);
        list->ResolveQueryData(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,4,readback.Get(),0);end_recorded=true;
    }
    void submitted(ID3D12Fence* fence,std::uint64_t value) noexcept {
        if(!active)return;retire_fence=fence;retire_value=value;if(!submitted_cpu)submitted_cpu=cpu_now();
    }
    bool collect(ID3D12CommandQueue* queue,std::uint64_t observed) noexcept {
        if(!active||!end_recorded||!submitted_cpu)return false;
        active=false;valid=false;
        if(!retire_fence||retire_fence->GetCompletedValue()<retire_value){++rejected;return false;}
        std::uint64_t gpu{},cpu{};void* mapped{};D3D12_RANGE range{0,32};
        if(FAILED(queue->GetClockCalibration(&gpu,&cpu))||FAILED(readback->Map(0,&range,&mapped))||!mapped){++rejected;return false;}
        std::array<std::uint64_t,4> t{};for(unsigned i=0;i<4;++i)t[i]=static_cast<std::uint64_t*>(mapped)[i];
        D3D12_RANGE written{0,0};readback->Unmap(0,&written);
        const double cf=cpu_frequency();
        auto delta_gpu=[&](std::uint64_t a,std::uint64_t b){return (double(a)-double(b))*1000./double(frequency);};
        auto delta_cpu=[&](std::uint64_t a,std::uint64_t b){return (double(a)-double(b))*1000./cf;};
        // Fresh calibration on both sides detects clock drift. Invalid timing
        // remains unavailable; it must not become an invented zero-ms result.
        const double drift=delta_cpu(cpu,before_cpu)-delta_gpu(gpu,before_gpu);
        if(std::abs(drift)>.5||!t[0]||t[1]<t[0]||t[2]<t[1]||t[3]<t[2]){++rejected;return false;}
        std::array<double,7> v{
            delta_cpu(cpu,enqueued_cpu)+delta_gpu(t[0],gpu),delta_gpu(t[1],t[0]),
            delta_gpu(t[2],t[1]),delta_gpu(t[3],t[2]),
            delta_cpu(observed,cpu)-delta_gpu(t[3],gpu),delta_cpu(observed,enqueued_cpu),delta_cpu(observed,submitted_cpu)};
        for(auto x:v)if(!std::isfinite(x)||x<-.1||x>1000){++rejected;return false;}
        for(unsigned i=0;i<v.size();++i)stages[i].add(std::max(0.,v[i]));
        ++samples;valid=true;return true;
    }
    void snapshot(CheekyFgExperimentDiagnostics& out) const noexcept {
        out.latency_enabled=enabled;out.latency_valid=valid;out.latency_samples=samples;out.latency_rejected=rejected;
        CheekyFgTimingValue* values[]{&out.queue_entry,&out.capture_dependency,&out.ready_to_fg,&out.gpu_work,
            &out.gpu_to_observed,&out.enqueue_to_observed,&out.submit_to_observed};
        for(unsigned i=0;i<stages.size();++i)*values[i]={stages[i].mean(),stages[i].last};
    }
};
}
