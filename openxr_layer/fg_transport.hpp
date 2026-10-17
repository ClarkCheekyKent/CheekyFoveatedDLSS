#pragma once
#include "fg_present.hpp"
#include <vector>

namespace cheeky::fg {
// Each command allocator and every referenced image stays leased until its
// fence retires. The producer only polls for a free context; it never waits.
struct TransportImage {ComPtr<ID3D12Resource> texture;D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};DXGI_FORMAT presentation_format{DXGI_FORMAT_UNKNOWN};};
using Image=std::shared_ptr<TransportImage>;
struct Transport {
    struct Context {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::vector<Image> images;
        std::vector<ComPtr<ID3D12Resource>> borrowed;
        std::vector<ComPtr<IUnknown>> leases;
        std::uint64_t retired{};
    };
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::array<Context,16> contexts;
    std::vector<Image> pool;
    std::uint64_t serial{};
    bool initialize(ID3D12Device* d,ID3D12CommandQueue* q) {
        device=d;queue=q;return SUCCEEDED(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    }
    bool completed(std::uint64_t value) const {return fence&&fence->GetCompletedValue()>=value;}
    Context* begin() {
        if(!device||FAILED(device->GetDeviceRemovedReason()))return nullptr;
        const auto done=fence->GetCompletedValue();
        for(auto& c:contexts)if(c.retired<=done){c.images.clear();c.borrowed.clear();c.leases.clear();}
        for(auto& c:contexts)if(c.retired<=done) {
            if(!c.allocator) {
                if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&c.allocator)))||
                   FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,c.allocator.Get(),nullptr,IID_PPV_ARGS(&c.list))))return nullptr;
                if(FAILED(c.list->Close()))return nullptr;
            }
            if(SUCCEEDED(c.allocator->Reset())&&SUCCEEDED(c.list->Reset(c.allocator.Get(),nullptr)))return &c;
        }
        return nullptr;
    }
    bool execute(Context& c,std::uint64_t& ticket) {
        if(FAILED(c.list->Close()))return false;
        ID3D12CommandList* lists[]{c.list.Get()};queue->ExecuteCommandLists(1,lists);
        // Even a failed signal must not make this allocator reusable.
        c.retired=UINT64_MAX;
        if(FAILED(queue->Signal(fence.Get(),++serial)))return false;
        c.retired=ticket=serial;return true;
    }
    Image image(D3D12_RESOURCE_DESC desc) {
        desc.Flags=D3D12_RESOURCE_FLAG_NONE;desc.SampleDesc.Count=1;desc.SampleDesc.Quality=0;
        for(auto& p:pool)if(p.use_count()==1) {
            const auto d=p->texture->GetDesc();
            if(d.Width==desc.Width&&d.Height==desc.Height&&d.DepthOrArraySize==desc.DepthOrArraySize&&d.Format==desc.Format){p->presentation_format=DXGI_FORMAT_UNKNOWN;return p;}
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
        c.list->CopyResource(destination->texture.Get(),source);
        color_transition(c.list.Get(),destination->texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        color_transition(c.list.Get(),source,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
        destination->state=D3D12_RESOURCE_STATE_COMMON;c.borrowed.emplace_back(source);c.images.push_back(destination);return destination;
    }
    Image crop(Context& c,const Image& source,XrRect2Di rect,unsigned layer) {
        if(!source)return {};auto desc=source->texture->GetDesc();
        if(rect.offset.x<0||rect.offset.y<0||rect.extent.width<=0||rect.extent.height<=0||
           std::uint64_t(rect.offset.x)+rect.extent.width>desc.Width||
           std::uint64_t(rect.offset.y)+rect.extent.height>desc.Height||layer>=desc.DepthOrArraySize)return {};
        desc.Width=rect.extent.width;desc.Height=rect.extent.height;desc.DepthOrArraySize=1;desc.MipLevels=1;
        auto destination=image(desc);if(!destination)return {};
        auto* from=source->texture.Get();auto* to=destination->texture.Get();
        color_transition(c.list.Get(),from,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        color_transition(c.list.Get(),to,destination->state,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=from;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        a.SubresourceIndex=layer*from->GetDesc().MipLevels;b.pResource=to;b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box{unsigned(rect.offset.x),unsigned(rect.offset.y),0,unsigned(rect.offset.x+rect.extent.width),unsigned(rect.offset.y+rect.extent.height),1};
        c.list->CopyTextureRegion(&b,0,0,0,&a,&box);
        color_transition(c.list.Get(),to,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        color_transition(c.list.Get(),from,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        destination->state=D3D12_RESOURCE_STATE_COMMON;c.images.push_back(source);c.images.push_back(destination);return destination;
    }
    bool upload(const Image& source,ID3D12Resource* target) {
        if(!source)return false;
        auto* c=begin();
        // Only the background presenter uses upload(). Allocator pressure here
        // may wait for retirement without turning a transient busy ring into a
        // permanent presentation failure. Producer begin() remains nonblocking.
        if(!c){drain();c=begin();}if(!c)return false;
        if(!compatible_color(target->GetDesc().Format,source->texture->GetDesc().Format)){c->list->Close();return false;}
        const auto read_state=(source->state&D3D12_RESOURCE_STATE_COPY_SOURCE)?source->state:D3D12_RESOURCE_STATE_COPY_SOURCE;
        color_transition(c->list.Get(),source->texture.Get(),source->state,read_state);
        color_transition(c->list.Get(),target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
        c->list->CopyResource(target,source->texture.Get());
        color_transition(c->list.Get(),target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
        color_transition(c->list.Get(),source->texture.Get(),read_state,source->state);
        c->images.push_back(source);c->borrowed.emplace_back(target);std::uint64_t ticket{};return execute(*c,ticket);
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
        for(auto& c:contexts){c.images.clear();c.borrowed.clear();c.leases.clear();}
    }
    ~Transport(){drain();}
};
}
