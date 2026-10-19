#pragma once
#include "fg_present.hpp"
#include "../shared/cheeky_gpu_span.hpp"
#include <vector>
#include <atomic>

namespace cheeky::fg {
// Each command allocator and every referenced image stays leased until its
// fence retires. The producer only polls for a free context; it never waits.
struct TransportImage {ComPtr<ID3D12Resource> texture;D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};DXGI_FORMAT presentation_format{DXGI_FORMAT_UNKNOWN};};
using Image=std::shared_ptr<TransportImage>;
struct ColorReadyStamp {
    std::atomic<std::uint64_t> ready_qpc{};
    std::uint64_t captured_cpu{},before_gpu{},before_cpu{};
    bool calibrated{};
};
using ColorReady=std::shared_ptr<ColorReadyStamp>;
struct Transport {
    struct Context {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::vector<Image> images;
        std::vector<ComPtr<ID3D12Resource>> borrowed;
        std::vector<ComPtr<IUnknown>> leases;
        std::uint64_t retired{};
        cheeky::gpu::Span gpu_span;
        ColorReady color_ready;
        std::uint64_t frame{};
        bool closes_frame{},profile_pending{};
    };
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::array<Context,16> contexts;
    std::vector<Image> pool;
    std::uint64_t serial{};
    std::uint64_t timestamp_frequency{};
    std::atomic<std::uint64_t> full_copies{},region_copies{},upload_copies{},submissions{};
    cheeky::gpu::Groups gpu_cost;
    bool initialize(ID3D12Device* d,ID3D12CommandQueue* q) {
        device=d;queue=q;timestamp_frequency=0;q->GetTimestampFrequency(&timestamp_frequency);
        return SUCCEEDED(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    }
    bool completed(std::uint64_t value) const {return fence&&fence->GetCompletedValue()>=value;}
    void collect(std::uint64_t done) {
        for(auto& c:contexts)if(c.retired<=done&&c.profile_pending) {
            c.profile_pending=false;double ms{};std::uint64_t start{};
            if(c.gpu_span.collect(timestamp_frequency,ms,&start)){
                gpu_cost.add(c.frame,ms,c.closes_frame);
                if(c.color_ready&&c.color_ready->calibrated){
                    std::uint64_t gpu{},cpu{};auto& stamp=*c.color_ready;
                    if(SUCCEEDED(queue->GetClockCalibration(&gpu,&cpu))){
                        const double cpu_rate=LatencyProbe::cpu_frequency(),gpu_rate=double(timestamp_frequency);
                        const double drift=(double(cpu)-double(stamp.before_cpu))*1000./cpu_rate-
                            (double(gpu)-double(stamp.before_gpu))*1000./gpu_rate;
                        const double ready=double(cpu)+(double(start)-double(gpu))*cpu_rate/gpu_rate;
                        if(std::abs(drift)<=.5&&std::isfinite(ready)&&ready>0&&ready<=double(cpu)+cpu_rate*.0001)
                            stamp.ready_qpc.store(std::uint64_t(ready));
                    }
                }
            }
        }
        gpu_cost.commit();
    }
    void close_profile_frame(std::uint64_t frame) {
        Context* last{};
        for(auto& c:contexts)if(c.profile_pending&&c.frame==frame&&(!last||last->retired<c.retired))last=&c;
        if(last)last->closes_frame=true;
        else {gpu_cost.add(frame,0,true);gpu_cost.commit();} // Unchanged images need no upload.
    }
    Context* begin(std::uint64_t frame=0,bool closes_frame=false) {
        if(!device||FAILED(device->GetDeviceRemovedReason()))return nullptr;
        const auto done=fence->GetCompletedValue();
        collect(done);
        for(auto& c:contexts)if(c.retired<=done){c.images.clear();c.borrowed.clear();c.leases.clear();}
        for(auto& c:contexts)if(c.retired<=done) {
            if(!c.allocator) {
                if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&c.allocator)))||
                   FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,c.allocator.Get(),nullptr,IID_PPV_ARGS(&c.list))))return nullptr;
                if(FAILED(c.list->Close()))return nullptr;
            }
            if(SUCCEEDED(c.allocator->Reset())&&SUCCEEDED(c.list->Reset(c.allocator.Get(),nullptr))) {
                c.frame=frame;c.closes_frame=closes_frame;c.profile_pending=false;c.color_ready.reset();
                if(frame&&timestamp_frequency)c.gpu_span.begin(device.Get(),c.list.Get());
                return &c;
            }
        }
        return nullptr;
    }
    ColorReady mark_color_ready(Context& c,std::uint64_t captured_cpu) {
        if(!c.gpu_span.heap||!timestamp_frequency)return {};
        auto stamp=std::make_shared<ColorReadyStamp>();stamp->captured_cpu=captured_cpu;
        stamp->calibrated=SUCCEEDED(queue->GetClockCalibration(&stamp->before_gpu,&stamp->before_cpu));
        c.color_ready=stamp;return stamp;
    }
    bool execute(Context& c,std::uint64_t& ticket) {
        if(c.frame&&timestamp_frequency)c.gpu_span.end(c.list.Get());
        if(FAILED(c.list->Close()))return false;
        ID3D12CommandList* lists[]{c.list.Get()};queue->ExecuteCommandLists(1,lists);++submissions;
        // Even a failed signal must not make this allocator reusable.
        c.retired=UINT64_MAX;
        if(FAILED(queue->Signal(fence.Get(),++serial)))return false;
        c.retired=ticket=serial;c.profile_pending=c.frame&&c.gpu_span.pending;return true;
    }
    Image image(D3D12_RESOURCE_DESC desc,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
        desc.Flags=flags;desc.SampleDesc.Count=1;desc.SampleDesc.Quality=0;
        for(auto& p:pool)if(p.use_count()==1) {
            const auto d=p->texture->GetDesc();
            if(d.Width==desc.Width&&d.Height==desc.Height&&d.DepthOrArraySize==desc.DepthOrArraySize&&d.MipLevels==desc.MipLevels&&
               d.Format==desc.Format&&d.Flags==desc.Flags){p->presentation_format=DXGI_FORMAT_UNKNOWN;return p;}
        }
        // Bounded storage even if the consumer stops or shapes keep changing.
        if(pool.size()>=48) {
            const auto free=std::find_if(pool.begin(),pool.end(),[](const Image& p){return p.use_count()==1;});
            if(free==pool.end())return {};
            pool.erase(free);
        }
        auto result=std::make_shared<TransportImage>();
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,
            nullptr,IID_PPV_ARGS(&result->texture))))return {};
        pool.push_back(result);return result;
    }
    Image copy(Context& c,ID3D12Resource* source,D3D12_RESOURCE_STATES state,DXGI_FORMAT typed=DXGI_FORMAT_UNKNOWN) {
        auto desc=source->GetDesc();if(typed!=DXGI_FORMAT_UNKNOWN)desc.Format=typed;
        auto destination=image(desc);if(!destination)return {};
        color_transition(c.list.Get(),source,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        color_transition(c.list.Get(),destination->texture.Get(),destination->state,D3D12_RESOURCE_STATE_COPY_DEST);
        c.list->CopyResource(destination->texture.Get(),source);++full_copies;
        color_transition(c.list.Get(),destination->texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        color_transition(c.list.Get(),source,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
        destination->state=D3D12_RESOURCE_STATE_COMMON;c.borrowed.emplace_back(source);c.images.push_back(destination);return destination;
    }
    Image region_copy(Context& c,ID3D12Resource* from,D3D12_RESOURCE_STATES from_state,
                      XrRect2Di rect,unsigned layer,DXGI_FORMAT typed=DXGI_FORMAT_UNKNOWN) {
        if(!from)return {};auto desc=from->GetDesc();
        if(rect.offset.x<0||rect.offset.y<0||rect.extent.width<=0||rect.extent.height<=0||
           std::uint64_t(rect.offset.x)+rect.extent.width>desc.Width||
           std::uint64_t(rect.offset.y)+rect.extent.height>desc.Height||layer>=desc.DepthOrArraySize)return {};
        const auto mip=desc.MipLevels;
        desc.Width=rect.extent.width;desc.Height=rect.extent.height;desc.DepthOrArraySize=1;desc.MipLevels=1;
        if(typed!=DXGI_FORMAT_UNKNOWN)desc.Format=typed;
        auto destination=image(desc);if(!destination)return {};
        auto* to=destination->texture.Get();
        color_transition(c.list.Get(),from,from_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        color_transition(c.list.Get(),to,destination->state,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=from;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        a.SubresourceIndex=layer*mip;b.pResource=to;b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box{unsigned(rect.offset.x),unsigned(rect.offset.y),0,unsigned(rect.offset.x+rect.extent.width),unsigned(rect.offset.y+rect.extent.height),1};
        c.list->CopyTextureRegion(&b,0,0,0,&a,&box);++region_copies;
        color_transition(c.list.Get(),to,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        color_transition(c.list.Get(),from,D3D12_RESOURCE_STATE_COPY_SOURCE,from_state);
        destination->state=D3D12_RESOURCE_STATE_COMMON;c.images.push_back(destination);c.borrowed.emplace_back(from);return destination;
    }
    Image crop(Context& c,const Image& source,XrRect2Di rect,unsigned layer) {
        if(!source)return {};
        auto result=region_copy(c,source->texture.Get(),source->state,rect,layer);
        if(result)c.images.push_back(source);return result;
    }
    bool record_upload(Context& c,const Image& source,ID3D12Resource* target) {
        if(!source||!target||target==source->texture.Get())return false;
        const auto a=source->texture->GetDesc(),b=target->GetDesc();
        if(!compatible_color(b.Format,a.Format)||a.Width!=b.Width||a.Height!=b.Height||
           a.DepthOrArraySize!=b.DepthOrArraySize||a.MipLevels!=b.MipLevels)return false;
        const auto read_state=(source->state&D3D12_RESOURCE_STATE_COPY_SOURCE)?source->state:D3D12_RESOURCE_STATE_COPY_SOURCE;
        color_transition(c.list.Get(),source->texture.Get(),source->state,read_state);
        color_transition(c.list.Get(),target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
        c.list->CopyResource(target,source->texture.Get());++upload_copies;
        color_transition(c.list.Get(),target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
        color_transition(c.list.Get(),source->texture.Get(),read_state,source->state);
        c.images.push_back(source);c.borrowed.emplace_back(target);return true;
    }
    bool upload(const Image& source,ID3D12Resource* target,std::uint64_t frame=0) {
        if(!source)return false;
        auto* c=begin(frame);
        // Only the background presenter uses upload(). Allocator pressure here
        // may wait for retirement without turning a transient busy ring into a
        // permanent presentation failure. Producer begin() remains nonblocking.
        if(!c){drain();c=begin(frame);}if(!c)return false;
        if(!record_upload(*c,source,target)){c->list->Close();return false;}
        std::uint64_t ticket{};return execute(*c,ticket);
    }
    void freeze(Context& c,const Image& image) {
        color_transition(c.list.Get(),image->texture.Get(),image->state,D3D12_RESOURCE_STATE_GENERIC_READ);
        image->state=D3D12_RESOURCE_STATE_GENERIC_READ;c.images.push_back(image);
    }
    void drain() {
        if(!fence||!device)return;
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        while(!completed(serial)&&SUCCEEDED(device->GetDeviceRemovedReason())) {
            if(event&&SUCCEEDED(fence->SetEventOnCompletion(serial,event)))WaitForSingleObject(event,100);else Sleep(1);
        }
        if(event)CloseHandle(event);
        collect(fence->GetCompletedValue());
        for(auto& c:contexts){c.images.clear();c.borrowed.clear();c.leases.clear();}
    }
    ~Transport(){drain();}
};
}
