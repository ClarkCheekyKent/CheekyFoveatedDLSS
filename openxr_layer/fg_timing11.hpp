#pragma once
#include "fg_timing.hpp"
#include "../shared/cheeky_fg_bridge.hpp"
#include "../shared/cheeky_gpu_span.hpp"
#include <d3d11.h>
#include <atomic>
#include <memory>
#include <mutex>

namespace cheeky::fg {
// GPU commands and query-pool access require the immediate-context guard.
// Statistics have their own lock: snapshots must never acquire the context
// while BufferedPresenter::mx is held. Queries are polled without flushing.
struct ShaderTimings11 {
    struct Sample {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        std::array<Microsoft::WRL::ComPtr<ID3D11Query>,4> stamps;
        unsigned parts{},mask{},width{},height{},foveated_eyes{},peripheral_method{};
        std::uint64_t epoch{},sequence{},group{};
        unsigned expected{};
        bool open{},pending{};
    };
    using Ticket=std::shared_ptr<Sample>;
    std::array<Ticket,16> pool;
    Ticket active;
    std::atomic<bool> supported{};
    std::atomic<std::uint64_t> epoch{1};
    std::uint64_t configuration{},next_sequence{};
    unsigned configured_parts{},configured_width{},configured_height{},configured_focus{},configured_method{};
    mutable std::mutex statistics_mutex;
    TimingAverage total;
    std::uint64_t samples{},last_sequence{};
    unsigned width{},height{},foveated_eyes{},peripheral_method{};

    void reset() {
        std::lock_guard lock(statistics_mutex);++epoch;total={};samples=last_sequence=0;
        width=height=foveated_eyes=peripheral_method=0;
    }
    static double duration(const D3D11_QUERY_DATA_TIMESTAMP_DISJOINT& clock,
                           const std::array<UINT64,4>& ticks,unsigned parts) noexcept {
        if(clock.Disjoint||!clock.Frequency||parts<1||parts>2)return NAN;
        double ms{};
        for(unsigned p=0;p<parts;++p){
            if(ticks[p*2+1]<ticks[p*2])return NAN;
            ms+=double(ticks[p*2+1]-ticks[p*2])*1000./double(clock.Frequency);
        }
        return ms;
    }
    Ticket reserve(ID3D11Device* device,unsigned parts,unsigned w,unsigned h,unsigned focused,
                   unsigned method,std::uint64_t source_configuration) {
        if(parts<1||parts>2)return {};
        if(configuration!=source_configuration||configured_parts!=parts||configured_width!=w||configured_height!=h||
           configured_focus!=focused||configured_method!=method){
            configuration=source_configuration;configured_parts=parts;configured_width=w;configured_height=h;
            configured_focus=focused;configured_method=method;reset();
        }
        for(auto& slot:pool){
            if(!slot)slot=std::make_shared<Sample>();
            if(slot.use_count()!=1||slot->open||slot->pending)continue;
            D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
            if(!slot->disjoint&&FAILED(device->CreateQuery(&desc,&slot->disjoint)))return {};
            desc.Query=D3D11_QUERY_TIMESTAMP;
            for(unsigned i=0;i<parts*2;++i)
                if(!slot->stamps[i]&&FAILED(device->CreateQuery(&desc,&slot->stamps[i])))return {};
            supported=true;slot->parts=parts;slot->mask=0;slot->width=w;slot->height=h;
            slot->foveated_eyes=focused;slot->peripheral_method=method;slot->epoch=epoch.load();slot->sequence=++next_sequence;
            return slot;
        }
        return {}; // Never wait for a free query slot or outstanding GPU work.
    }
    bool begin(ID3D11DeviceContext* context,const Ticket& ticket,unsigned part) {
        if(!ticket||part>=ticket->parts||(ticket->mask&(1u<<part)))return false;
        if(active&&active!=ticket)return false; // Skip instrumentation, never the draw.
        if(!active){if(part!=0)return false;active=ticket;ticket->open=true;context->Begin(ticket->disjoint.Get());}
        context->End(ticket->stamps[part*2].Get());return true;
    }
    void end(ID3D11DeviceContext* context,const Ticket& ticket,unsigned part) {
        context->End(ticket->stamps[part*2+1].Get());ticket->mask|=1u<<part;
        if(ticket->mask==(1u<<ticket->parts)-1u){
            context->End(ticket->disjoint.Get());ticket->open=false;ticket->pending=true;active.reset();
        }
    }
    void cancel(ID3D11DeviceContext* context) {
        if(active){context->End(active->disjoint.Get());active->open=false;
            // Retire the disjoint query before reusing a partially recorded ticket.
            active->pending=true;active.reset();}
    }
    void collect(ID3D11DeviceContext* context) {collect(context,[](const Sample&,double){});}
    template<class Complete> void collect(ID3D11DeviceContext* context,Complete&& complete) {
        for(const auto& slot:pool){
            if(!slot||!slot->pending)continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
            const auto ready=context->GetData(slot->disjoint.Get(),&clock,sizeof(clock),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(ready==S_FALSE)continue;
            if(FAILED(ready)||clock.Disjoint||!clock.Frequency||slot->mask!=(1u<<slot->parts)-1u){slot->pending=false;continue;}
            std::array<UINT64,4> ticks{};bool waiting{},failed{};
            for(unsigned i=0;i<slot->parts*2;++i){
                const auto hr=context->GetData(slot->stamps[i].Get(),&ticks[i],sizeof(ticks[i]),D3D11_ASYNC_GETDATA_DONOTFLUSH);
                waiting|=hr==S_FALSE;failed|=FAILED(hr);
            }
            if(waiting&&!failed)continue;
            slot->pending=false;if(failed)continue;
            const auto ms=duration(clock,ticks,slot->parts);if(!std::isfinite(ms))continue;
            std::lock_guard lock(statistics_mutex);
            if(slot->epoch!=epoch.load())continue;
            const auto last=total.last;total.add(ms);
            if(slot->sequence<last_sequence)total.last=last;else last_sequence=slot->sequence;
            ++samples;width=slot->width;height=slot->height;
            foveated_eyes=slot->foveated_eyes;peripheral_method=slot->peripheral_method;
            complete(*slot,ms);
        }
    }
    CheekyFgPerformance snapshot() const {
        std::lock_guard lock(statistics_mutex);CheekyFgPerformance out;
        out.gpu_supported=supported.load();out.gpu_valid=total.count!=0;
        out.gpu_samples=samples;out.window_samples=total.count;out.foveated_eyes=foveated_eyes;
        // Native D3D11 fuses center and periphery shading. The UI labels the
        // combined interval explicitly instead of inventing separate stage times.
        out.total_ms=total.mean();out.total_last_ms=total.last;
        out.output_width=width;out.output_height=height;out.peripheral_method=peripheral_method;
        out.shared_gpu_queue=1;return out;
    }
};

// Short GPU stages grouped by their actual capture interval. Each measurement
// surrounds only its GPU commands; collection never waits or flushes.
struct StageTimings11 {
    ShaderTimings11 queries;
    cheeky::gpu::Groups groups;
    std::uint64_t serial{};
    void collect(ID3D11DeviceContext* context) {
        queries.collect(context,[&](const auto& sample,double ms){groups.add(sample.group,ms,false,sample.expected);});
        groups.commit();
    }
    template<class Record> bool measure(ID3D11Device* device,ID3D11DeviceContext* context,
        std::uint64_t group,unsigned expected,Record&& record) {
        collect(context);
        auto ticket=queries.reserve(device,1,0,0,0,0,1);
        if(ticket){ticket->group=group?group:++serial;ticket->expected=expected;}
        const bool measured=queries.begin(context,ticket,0);
        const bool result=record();
        if(measured)queries.end(context,ticket,0);
        return result;
    }
    void reset(){queries.reset();groups.reset();}
    cheeky::gpu::Summary summary() const {return groups.summary();}
};
}
