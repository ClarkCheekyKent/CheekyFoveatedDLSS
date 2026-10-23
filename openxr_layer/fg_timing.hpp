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
    double percentile(unsigned percent) const noexcept {
        if(!count)return 0;auto ordered=values;std::sort(ordered.begin(),ordered.begin()+count);
        const auto rank=std::clamp((count*std::min(percent,100u)+99u)/100u,1u,count);
        return ordered[rank-1];
    }
};
struct CadenceWindow {
    // Counters: game submissions, distinct native, generated, runtime submissions.
    struct Sample {double ms{};std::array<std::uint64_t,4> counts{};};
    std::array<Sample,256> samples{};unsigned count{},next{};
    void add(double ms,std::array<std::uint64_t,4> counts) {
        samples[next]={ms,counts};next=(next+1)%samples.size();count=std::min<unsigned>(count+1,unsigned(samples.size()));
    }
    bool rates(std::array<double,4>& result) const {
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
    // Processing [0,1], preparation/NGX/periphery pairs for each eye, then
    // immutable output publication [14,15] outside the processing subtotal.
    // [16,23] are four NGX boundaries per eye (always recorded).
    // [24,39] are four preparation boundaries per eye/pass, sampled 1 in 17.
    static constexpr unsigned main_query_count=16,ngx_detail_count=8;
    static constexpr unsigned always_query_count=main_query_count+ngx_detail_count;
    static constexpr unsigned query_count=always_query_count+16;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    std::uint64_t frequency{},samples{};
    TimingAverage preparation,center,periphery,total,publication;
    std::array<TimingAverage,2> eye_preparation;
    // These use the existing NGX timestamp pairs [4,5] (left) and [10,11]
    // (right), with no additional queries or synchronization. Separate eye
    // identity from evaluation order: one eye alternates into the first slot.
    std::array<TimingAverage,2> eye_center,center_by_position;
    std::array<std::array<TimingAverage,3>,2> ngx_parts,ngx_by_position;
    std::array<TimingAverage,2> preparation_by_first;
    std::array<std::array<TimingAverage,2>,2> eye_preparation_by_first,center_dispatch_by_first,eye_center_by_first;
    std::array<std::uint64_t,2> order_samples{},order_detail_samples{};
    unsigned first_eye{};
    // Sampled independently of the main 60-frame window: eye * 2 + pass,
    // then transitions/setup before dispatch, dispatch, transitions after.
    std::array<std::array<TimingAverage,3>,4> preparation_parts;
    TimingAverage sampled_preparation,preparation_gaps;
    std::uint64_t preparation_samples{};
    bool record_preparation{},preparation_valid{};
    unsigned foveated_eyes{};
    bool valid{};
    void reset() noexcept {
        heap.Reset();readback.Reset();frequency=samples=0;
        preparation={};center={};periphery={};total={};publication={};eye_preparation={};
        eye_center={};center_by_position={};eye_center_by_first={};ngx_parts=ngx_by_position={};foveated_eyes=0;valid=false;
        preparation_parts={};sampled_preparation={};preparation_gaps={};preparation_samples=0;
        preparation_by_first={};eye_preparation_by_first={};center_dispatch_by_first={};order_samples=order_detail_samples={};first_eye=0;
        record_preparation=preparation_valid=false;
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
    void ngx_stamp(ID3D12GraphicsCommandList* list,unsigned eye,unsigned phase) noexcept {
        stamp(list,main_query_count+eye*4+phase);
    }
    void begin(ID3D12GraphicsCommandList* list) noexcept {
        valid=preparation_valid=false;record_preparation=supported()&&(samples%17)==0;
        first_eye=unsigned(samples&1);
        stamp(list,0);
    }
    void preparation_stamp(ID3D12GraphicsCommandList* list,unsigned eye,unsigned pass,unsigned phase) noexcept {
        if(record_preparation)stamp(list,always_query_count+(eye*2+pass)*4+phase);
    }
    void resolve(ID3D12GraphicsCommandList* list) noexcept {
        if(supported())list->ResolveQueryData(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,
            record_preparation?query_count:always_query_count,readback.Get(),0);
    }
    // Caller has already completed the ordinary generation fence wait. Reading
    // telemetry must not add another wait, flush or command-list submission.
    bool collect(unsigned focused_eyes,bool copied_output=true) noexcept {
        valid=preparation_valid=false;if(!supported())return false;
        const unsigned count=record_preparation?query_count:always_query_count;
        void* mapped{};D3D12_RANGE range{0,count*sizeof(std::uint64_t)};
        if(FAILED(readback->Map(0,&range,&mapped))||!mapped)return false;
        std::array<std::uint64_t,query_count> ticks{};
        for(unsigned i=0;i<count;++i)ticks[i]=static_cast<const std::uint64_t*>(mapped)[i];
        const D3D12_RANGE written{0,0};readback->Unmap(0,&written);
        for(unsigned i=0;i<main_query_count;i+=2)
            if(!ticks[i]||ticks[i+1]<ticks[i])return false;
        auto ms=[&](unsigned start){return double(ticks[start+1]-ticks[start])*1000.0/double(frequency);};
        const double whole=ms(0),prep=ms(2)+ms(8),ngx=ms(4)+ms(10),edge=ms(6)+ms(12);
        // Reject incomplete/unordered samples instead of showing fabricated 0ms.
        if(!std::isfinite(whole)||whole<=0||prep+ngx+edge>whole+0.001)return false;
        if(samples&&foveated_eyes!=focused_eyes) {
            preparation={};center={};periphery={};total={};publication={};eye_preparation={};
            eye_center={};center_by_position={};eye_center_by_first={};ngx_parts=ngx_by_position={};samples=0;
            preparation_parts={};sampled_preparation={};preparation_gaps={};preparation_samples=0;
            preparation_by_first={};eye_preparation_by_first={};center_dispatch_by_first={};order_samples=order_detail_samples={};
        }
        // The subregions must remain ordered and bounded by the legacy NGX
        // interval; discard a malformed window instead of logging bad timing.
        std::array<std::array<double,3>,2> ngx_sub{};
        for(unsigned eye=0;eye<2;++eye) {
            const unsigned base=main_query_count+eye*4,outer=4+eye*6;
            if(!ticks[base]||ticks[base]<ticks[outer]||ticks[base+3]>ticks[outer+1])return false;
            for(unsigned part=0;part<3;++part) {
                if(ticks[base+part+1]<ticks[base+part])return false;
                ngx_sub[eye][part]=double(ticks[base+part+1]-ticks[base+part])*1000.0/double(frequency);
            }
        }
        preparation.add(prep);center.add(ngx);periphery.add(edge);total.add(whole);
        for(unsigned eye=0;eye<2;++eye)
            for(unsigned part=0;part<3;++part) {
                ngx_parts[eye][part].add(ngx_sub[eye][part]);
                ngx_by_position[eye==first_eye?0:1][part].add(ngx_sub[eye][part]);
            }
        // Direct composition has no publication copy. Its final transition is
        // already inside the measured composition scope; adjacent timestamps
        // must not turn an omitted operation into a fabricated copy cost.
        publication.add(copied_output?ms(14):0);
        eye_preparation[0].add(ms(2));eye_preparation[1].add(ms(8));
        // Preserve the exact same sample admission rules as the HUD's combined
        // center metric, so left+right averages remain directly comparable.
        const double left_center=ms(4),right_center=ms(10);
        eye_center[0].add(left_center);eye_center[1].add(right_center);
        center_by_position[0].add(first_eye?right_center:left_center);
        center_by_position[1].add(first_eye?left_center:right_center);
        eye_center_by_first[first_eye][0].add(left_center);
        eye_center_by_first[first_eye][1].add(right_center);
        preparation_by_first[first_eye].add(prep);++order_samples[first_eye];
        for(unsigned eye=0;eye<2;++eye)eye_preparation_by_first[first_eye][eye].add(ms(2+eye*6));
        if(record_preparation) {
            std::array<std::array<double,3>,4> parts{};double covered{};bool ordered=true;
            for(unsigned pass=0;pass<4;++pass) {
                const auto base=always_query_count+pass*4,outer=2+(pass/2)*6;
                ordered&=ticks[base]>=ticks[outer]&&ticks[base+3]<=ticks[outer+1];
                for(unsigned phase=0;phase<3;++phase) {
                    if(!ticks[base+phase]||ticks[base+phase+1]<ticks[base+phase]){ordered=false;continue;}
                    const double value=double(ticks[base+phase+1]-ticks[base+phase])*1000./double(frequency);
                    parts[pass][phase]=value;covered+=value;
                }
            }
            if(ordered&&covered<=prep+.001) {
                for(unsigned pass=0;pass<4;++pass)for(unsigned phase=0;phase<3;++phase)
                    preparation_parts[pass][phase].add(parts[pass][phase]);
                sampled_preparation.add(prep);preparation_gaps.add(std::max(0.,prep-covered));
                ++order_detail_samples[first_eye];
                for(unsigned eye=0;eye<2;++eye)center_dispatch_by_first[first_eye][eye].add(parts[eye*2][1]);
                ++preparation_samples;preparation_valid=true;
            }
        }
        foveated_eyes=focused_eyes;++samples;valid=true;return true;
    }
};
}
