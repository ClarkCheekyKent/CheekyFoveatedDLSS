#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace cheeky::fg {
// A bounded arithmetic mean, with independent windows for GPU samples and
// native-frame CPU waits. Invalid samples never enter the displayed averages.
struct TimingAverage {
    std::array<double,60> values{};
    unsigned count{},next{};
    double sum{},last{};
    void add(double ms) noexcept {
        if(!std::isfinite(ms)||ms<0)return;
        if(count==values.size())sum-=values[next];else ++count;
        values[next]=last=ms;sum+=ms;next=(next+1)%values.size();
    }
    double mean() const noexcept {return count?sum/count:0;}
};
struct CadenceWindow {
    // Counters: game submissions, distinct native, generated, repeated, runtime
    // submissions. Repeated native pixels must never inflate unique-frame FPS.
    struct Sample {double ms{};std::array<std::uint64_t,5> counts{};};
    std::array<Sample,256> samples{};unsigned count{},next{};
    void add(double ms,std::array<std::uint64_t,5> counts) {
        samples[next]={ms,counts};next=(next+1)%samples.size();count=std::min<unsigned>(count+1,unsigned(samples.size()));
    }
    bool rates(std::array<double,5>& result) const {
        result={};if(count<2)return false;
        const auto& newest=samples[(next+samples.size()-1)%samples.size()];auto oldest=newest;
        for(unsigned i=2;i<=count;++i){const auto& candidate=samples[(next+samples.size()-i)%samples.size()];
            if(newest.ms-candidate.ms>2000)break;oldest=candidate;}
        const auto elapsed=newest.ms-oldest.ms;if(elapsed<250)return false;
        for(unsigned i=0;i<result.size();++i){if(newest.counts[i]<oldest.counts[i])return false;
            result[i]=1000.*double(newest.counts[i]-oldest.counts[i])/elapsed;}
        return true;
    }
};
using TimingClock=std::chrono::steady_clock;
inline double elapsed_ms(TimingClock::time_point since) noexcept {
    return std::chrono::duration<double,std::milli>(TimingClock::now()-since).count();
}
struct AddElapsed {
    double& destination;
    TimingClock::time_point begin{TimingClock::now()};
    ~AddElapsed(){destination+=elapsed_ms(begin);}
};
struct CommitWait {
    TimingAverage& average;
    double& accumulator;
    ~CommitWait(){average.add(accumulator);accumulator=0;}
};

struct GpuTimings {
    // Whole worker [0,1], then preparation/NGX/periphery pairs for each eye.
    static constexpr unsigned query_count=14;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    std::uint64_t frequency{},samples{};
    TimingAverage preparation,center,periphery,total;
    unsigned foveated_eyes{};
    bool valid{};
    void reset() noexcept {
        heap.Reset();readback.Reset();frequency=samples=0;
        preparation={};center={};periphery={};total={};foveated_eyes=0;valid=false;
    }
    bool initialize(ID3D12Device* device,ID3D12CommandQueue* queue) noexcept {
        reset();
        if(!device||!queue||FAILED(queue->GetTimestampFrequency(&frequency))||!frequency)return false;
        D3D12_QUERY_HEAP_DESC hd{};hd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;hd.Count=query_count;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width=query_count*sizeof(std::uint64_t);rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;
        rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(device->CreateQueryHeap(&hd,IID_PPV_ARGS(&heap)))||
           FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,
               D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)))) {
            reset();return false;
        }
        return true;
    }
    bool supported() const noexcept {return heap&&readback&&frequency;}
    void stamp(ID3D12GraphicsCommandList* list,unsigned index) noexcept {
        if(supported())list->EndQuery(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index);
    }
    void resolve(ID3D12GraphicsCommandList* list) noexcept {
        if(supported())list->ResolveQueryData(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,query_count,readback.Get(),0);
    }
    // Caller has already completed the ordinary generation fence wait. Reading
    // telemetry must not add another wait, flush or command-list submission.
    bool collect(unsigned focused_eyes) noexcept {
        valid=false;if(!supported())return false;
        void* mapped{};D3D12_RANGE range{0,query_count*sizeof(std::uint64_t)};
        if(FAILED(readback->Map(0,&range,&mapped))||!mapped)return false;
        std::array<std::uint64_t,query_count> ticks{};
        for(unsigned i=0;i<query_count;++i)ticks[i]=static_cast<const std::uint64_t*>(mapped)[i];
        const D3D12_RANGE written{0,0};readback->Unmap(0,&written);
        for(unsigned i=0;i<query_count;i+=2)
            if(!ticks[i]||ticks[i+1]<ticks[i])return false;
        auto ms=[&](unsigned start){return double(ticks[start+1]-ticks[start])*1000.0/double(frequency);};
        const double whole=ms(0),prep=ms(2)+ms(8),ngx=ms(4)+ms(10),edge=ms(6)+ms(12);
        // Reject incomplete/unordered samples instead of showing fabricated 0ms.
        if(!std::isfinite(whole)||whole<=0||prep+ngx+edge>whole+0.001)return false;
        if(samples&&foveated_eyes!=focused_eyes) {
            preparation={};center={};periphery={};total={};samples=0;
        }
        preparation.add(prep);center.add(ngx);periphery.add(edge);total.add(whole);
        foveated_eyes=focused_eyes;++samples;valid=true;return true;
    }
};
}
