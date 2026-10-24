#pragma once
#include "fg_auto.hpp"
#include "fg_transport.hpp"
#include "fg_timing11.hpp"
#include <mutex>

namespace cheeky::fg {
// Query storage remains owned until its GPU fence/query retires. Neither timer
// waits or flushes to retrieve data. Only Auto mode records these brackets.
struct ApplicationTimer12 {
    struct Ticket {cheeky::gpu::Span span;std::uint64_t ready{},sequence{};double cpu_ms{};TimingClock::time_point start;};
    std::array<Ticket,8> tickets;
    Ticket* open{};
    std::uint64_t sequence{};
    mutable std::mutex mutex;
    ApplicationTiming latest;
    void collect(Transport& transport) {
        for(auto& t:tickets)if(t.ready&&transport.completed(t.ready)){
            double gpu{};
            if(t.span.collect(transport.timestamp_frequency,gpu)){
                std::lock_guard lock(mutex);if(t.sequence>latest.sequence)latest={t.sequence,t.cpu_ms,gpu,
                    std::chrono::duration<double,std::milli>(t.start.time_since_epoch()).count()+t.cpu_ms};
            }
            t.ready=0;
        }
    }
    void begin(Transport& transport,bool enabled) {
        collect(transport);
        // A discarded application interval still owns its queued first stamp.
        if(open){open->span.pending=false;open=nullptr;}
        if(!enabled)return;
        for(auto& t:tickets)if(!t.ready){
            auto* c=transport.begin();if(!c)return;
            const bool recorded=t.span.begin(transport.device.Get(),c->list.Get());
            std::uint64_t ready{};
            if(!transport.execute(*c,ready)||!recorded)return;
            t.ready=ready;t.sequence=++sequence;t.start=TimingClock::now();open=&t;return;
        }
    }
    void end(ID3D12GraphicsCommandList* commands) {if(open){open->cpu_ms=elapsed_ms(open->start);open->span.end(commands);}}
    void submitted(std::uint64_t ready) {if(open){open->ready=ready;open=nullptr;}}
    void cancel() {if(open){open->span.pending=false;open=nullptr;}}
    void reset(Transport& transport) {cancel();collect(transport);std::lock_guard lock(mutex);latest={};}
    ApplicationTiming sample() const {std::lock_guard lock(mutex);return latest;}
};
struct ApplicationTimer11 {
    ShaderTimings11 queries;
    ShaderTimings11::Ticket open;
    TimingClock::time_point start;
    std::map<std::uint64_t,std::pair<double,double>> cpu;
    mutable std::mutex mutex;
    ApplicationTiming latest;
    void collect(ID3D11DeviceContext* context) {
        queries.collect(context,[&](const auto& ticket,double gpu){
            const auto found=cpu.find(ticket.sequence);if(found==cpu.end())return;
            std::lock_guard lock(mutex);if(ticket.sequence>latest.sequence)latest={ticket.sequence,found->second.first,gpu,found->second.second};cpu.erase(found);
        });
    }
    void begin(ID3D11Device* device,ID3D11DeviceContext* context,bool enabled) {
        if(open){queries.cancel(context);open.reset();}
        collect(context);if(!enabled)return;
        open=queries.reserve(device,1,0,0,0,0,1);
        if(!queries.begin(context,open,0)){open.reset();return;}start=TimingClock::now();
    }
    void end(ID3D11DeviceContext* context) {
        if(!open)return;
        cpu[open->sequence]={elapsed_ms(start),std::chrono::duration<double,std::milli>(TimingClock::now().time_since_epoch()).count()};
        while(cpu.size()>64)cpu.erase(cpu.begin());
        queries.end(context,open,0);open.reset();
    }
    void reset(ID3D11DeviceContext* context) {
        if(open){queries.cancel(context);open.reset();}queries.reset();std::lock_guard lock(mutex);latest={};
    }
    ApplicationTiming sample() const {std::lock_guard lock(mutex);return latest;}
};
}
