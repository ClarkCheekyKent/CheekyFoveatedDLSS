#pragma once
#include "d3d12_native.hpp"
#include "../shared/cheeky_fg_bridge.hpp"
#include "../shared/cheeky_gpu_span.hpp"
#include <memory>
#include <vector>

namespace cheeky::foveated_dlss {
// Copies are recorded beside the original DLSS evaluation, before either eye's
// engine textures can be reused. Submission observation, not CPU record order,
// determines which evaluations are eligible for a released OpenXR color image.
class FgSnapshots {
    template<class T> using ComPtr=Microsoft::WRL::ComPtr<T>;
    struct Shot {
        ComPtr<ID3D12Resource> depth,motion,source_depth,source_motion;
        ComPtr<ID3D12GraphicsCommandList> list;
        CheekyFgEye meta;
        std::uint64_t recording{};
        std::uint64_t retired{};
        bool submitted{},foreign{},cancelled{};
        cheeky::gpu::Span gpu_span;
    };
    class Lease final : public IUnknown {
        std::atomic<ULONG> references{1};
        std::shared_ptr<Shot> eyes[2];
    public:
        Lease(std::shared_ptr<Shot> a,std::shared_ptr<Shot> b):eyes{std::move(a),std::move(b)}{}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
            if(!out)return E_POINTER;*out=nullptr;
            if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override {return ++references;}
        ULONG STDMETHODCALLTYPE Release() override {auto n=--references;if(!n)delete this;return n;}
    };
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::vector<std::shared_ptr<Shot>> shots;
    struct Latch {std::uint64_t token;std::shared_ptr<Shot> a,b;};
    std::vector<Latch> latched;
    std::uint64_t ordinal{},serial{},consumed{};
    std::uint64_t failed_record{};
    std::uint64_t timestamp_frequency{};
    cheeky::gpu::Groups gpu_cost;
    CheekyFgEye previous_raw[2];
    static void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition.pResource=r;
        v.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;v.Transition.StateBefore=a;v.Transition.StateAfter=b;list->ResourceBarrier(1,&v);
    }
    bool allocate(ID3D12Resource* source,ComPtr<ID3D12Resource>& out) {
        auto d=source->GetDesc();
        if(out){auto p=out->GetDesc();if(p.Width==d.Width&&p.Height==d.Height&&p.Format==d.Format)return true;}
        // Keep the original format/planes; the layer converts the owned copy.
        d.Flags=D3D12_RESOURCE_FLAG_NONE;d.Alignment=0;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;out.Reset();
        return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)));
    }
    static void copy(ID3D12GraphicsCommandList* list,ID3D12Resource* from,ID3D12Resource* to) {
        // Native NGX's depth/MV contract is NON_PIXEL_SHADER_RESOURCE. Restore
        // it exactly; do not install descriptor heaps or alter compute bindings.
        barrier(list,from,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(list,to,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyResource(to,from);
        barrier(list,to,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        barrier(list,from,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
public:
    cheeky::gpu::Summary capture_gpu() const {return gpu_cost.summary();}
    struct Match {
        std::uint64_t frame{},latest_frame{},latest{},consumed{},failed{};
        unsigned eligible{},pending{},foreign{},cancelled{};
        const char* reason{"No guides for this frame"};
    };
    Match last_match;
    Match inspect(std::uint64_t frame) const {
        Match result;result.frame=frame;result.consumed=consumed;result.failed=failed_record;
        for(auto& s:shots){result.latest_frame=std::max(result.latest_frame,s->meta.render_frame);
            if(frame&&s->meta.render_frame!=frame)continue;
            if(s->cancelled)++result.cancelled;
            else if(s->foreign)++result.foreign;
            else if(!s->submitted)++result.pending;
            else {++result.eligible;result.latest=std::max(result.latest,s->meta.evaluation);}
        }
        result.reason=result.pending?"Guides are not submitted":result.foreign?"Guides submitted on a different queue":
            result.cancelled?"Guide command list was reset":result.eligible!=2?(result.eligible?"Frame does not contain exactly two guides":"No guides with the color frame ID"):
            result.latest<=consumed?"Guide pair was already consumed":result.latest<=failed_record?"Guide capture failed before this pair":"Guides ready; color copy has no matching token";
        return result;
    }
    static inline constexpr GUID recording_key{0xf654ab8c,0x35ac,0x4b73,{0x95,0x23,0x7c,0x55,0x78,0x41,0x87,0xe9}};
    static std::uint64_t recording_identity(ID3D12GraphicsCommandList* list) noexcept {
        if(!list)return 0;std::uint64_t tag{};UINT size=sizeof(tag);
        __try{return SUCCEEDED(list->GetPrivateData(recording_key,&size,&tag))&&size==sizeof(tag)?tag:0;}
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION||GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return 0;}
    }
    static std::uint64_t tag_recording(ID3D12GraphicsCommandList* list){
        auto id=recording_identity(list);if(id)return id;static std::atomic<std::uint64_t> next{};id=++next;
        return SUCCEEDED(list->SetPrivateData(recording_key,sizeof(id),&id))?id:0;
    }
    static bool tagged(ID3D12GraphicsCommandList* list) noexcept {return recording_identity(list)!=0;}
    bool matches(ID3D12CommandQueue* q) const{return queue==native_d3d12_interface(q);}
    void invalidate(){consumed=ordinal;latched.clear();previous_raw[0]={};previous_raw[1]={};}
    bool idle() const {
        const auto done=fence?fence->GetCompletedValue():0;
        for(auto& s:shots)if(s.use_count()!=1||(!s->cancelled&&(!s->retired||s->retired>done)))return false;
        return true;
    }
    // Called under the capture mutex. No CPU GPU-fence waits on this path.
    bool initialize(ID3D12CommandQueue* q) {
        auto native=native_d3d12_interface(q);
        if(queue==native)return queue&&fence;
        // Queue changes must not free recorded or submitted GPU references.
        // This object is process-resident; retain old generations until exit.
        if(!shots.empty())return false;
        queue=native;device.Reset();fence.Reset();ordinal=serial=consumed=0;timestamp_frequency=0;
        if(queue)queue->GetTimestampFrequency(&timestamp_frequency);
        return queue&&SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))&&
            SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    }
    void retire() {
        if(!queue||!fence)return;
        bool need{};for(auto& s:shots)need|=s->submitted&&!s->retired&&!s->foreign;
        if(need&&SUCCEEDED(queue->Signal(fence.Get(),serial+1))) {
            ++serial;for(auto& s:shots)if(s->submitted&&!s->retired&&!s->foreign)s->retired=serial;
        }
        collect_completed();
    }
    void collect_completed() {
        if(!fence)return;
        const auto done=fence->GetCompletedValue();
        for(auto& s:shots)if(s->retired&&s->retired<=done){
            double ms{};if(s->gpu_span.collect(timestamp_frequency,ms))gpu_cost.add(s->meta.render_frame,ms,false,2);
            s->source_depth.Reset();s->source_motion.Reset();
        }
        gpu_cost.commit();
    }
    bool record(ID3D12GraphicsCommandList* commands,CheekyFgEye meta) {
        const auto id=++ordinal; // Failed/dropped captures must leave a gap.
        if(!queue||!fence||!commands||!meta.depth||!meta.motion){failed_record=id;return false;}
        const auto recording=tag_recording(commands);if(!recording){failed_record=id;return false;}
        std::shared_ptr<Shot> slot;
        const auto done=fence->GetCompletedValue();
        // Keep the most recent stereo pair until a later color handoff, even
        // when its GPU work has already completed.
        for(auto& s:shots)if(s.use_count()==1&&(s->cancelled||(s->retired&&s->retired<=done&&s->meta.evaluation+2<id&&!s->foreign))){slot=s;break;}
        if(!slot){if(shots.size()>=24){failed_record=id;return false;}slot=std::make_shared<Shot>();shots.push_back(slot);}
        if(slot->retired&&slot->retired<=done) {
            double ms{};if(slot->gpu_span.collect(timestamp_frequency,ms))gpu_cost.add(slot->meta.render_frame,ms,false,2);
            gpu_cost.commit();
        }
        if(!allocate(meta.depth,slot->depth)||!allocate(meta.motion,slot->motion)){failed_record=id;return false;}
        slot->source_depth=meta.depth;slot->source_motion=meta.motion;slot->list=native_d3d12_interface(commands);
        slot->recording=recording;
        slot->submitted=slot->foreign=slot->cancelled=false;slot->retired=0;
        auto& prior=previous_raw[(id-1)%2];
        if(meta.motion_jittered){
            if(prior.render_frame+1==meta.render_frame&&prior.feature==meta.feature&&prior.motion_width==meta.motion_width&&
               prior.motion_height==meta.motion_height&&!meta.reset){
                meta.jitter_correction_x=(prior.jitter_x-meta.jitter_x)/std::max(1u,meta.motion_width);
                meta.jitter_correction_y=(prior.jitter_y-meta.jitter_y)/std::max(1u,meta.motion_height);
            }else meta.reset=1;
        }
        prior=meta;prior.depth=prior.motion=nullptr;
        tag_recording(commands);
        if(timestamp_frequency)slot->gpu_span.begin(device.Get(),commands);
        copy(commands,meta.depth,slot->depth.Get());copy(commands,meta.motion,slot->motion.Get());
        if(timestamp_frequency)slot->gpu_span.end(commands);
        meta.depth=slot->depth.Get();meta.motion=slot->motion.Get();meta.evaluation=id;slot->meta=meta;return true;
    }
    void submitted(ID3D12CommandQueue* q,ID3D12GraphicsCommandList* commands) {
        const auto recording=recording_identity(commands);auto native=native_d3d12_interface(q);
        for(auto& s:shots)if(!s->submitted&&recording&&s->recording==recording) {
            s->submitted=true;s->foreign=native!=queue;s->list.Reset();
        }
    }
    void reset(ID3D12GraphicsCommandList* commands) {
        const auto recording=recording_identity(commands);
        for(auto& s:shots)if(!s->submitted&&recording&&s->recording==recording){
            s->cancelled=true;s->submitted=true;s->list.Reset();s->source_depth.Reset();s->source_motion.Reset();
            s->gpu_span.pending=false;
            failed_record=std::max(failed_record,s->meta.evaluation);
        }
    }
    std::uint64_t token(std::uint64_t render_frame=0) {
        retire();last_match=inspect(render_frame);const auto latest=last_match.latest;
        if(render_frame&&last_match.eligible!=2){
            last_match.reason=last_match.pending?"Guides are not submitted":last_match.foreign?"Guides submitted on a different queue":
                last_match.cancelled?"Guide command list was reset":last_match.eligible?"Frame does not contain exactly two guides":"No guides with the color frame ID";
            return 0;
        }
        if(latest<=consumed){last_match.reason="Guide pair was already consumed";return 0;}
        if(latest<=failed_record){last_match.reason="Guide capture failed before this pair";return 0;}
        last_match.reason="Matched";
        for(auto& latch:latched)if(latch.token==latest)return latest;
        std::shared_ptr<Shot> a,b;
        for(auto& s:shots)if(s->submitted&&!s->foreign&&!s->cancelled){if(s->meta.evaluation==latest-1)a=s;if(s->meta.evaluation==latest)b=s;}
        if(!a||!b||(render_frame&&(a->meta.render_frame!=render_frame||b->meta.render_frame!=render_frame))){last_match.reason="Guide pair is interrupted";return 0;}
        // Pin guides at COLOR release, not later at EndFrame. A future DLSS
        // recording must not recycle either snapshot while color awaits submit.
        if(latched.size()>=4)latched.erase(latched.begin());
        latched.push_back({latest,a,b});return latest;
    }
    bool acquire(CheekyFgPair& out) {
        const auto expected=out.capture_token;
        if(!expected||expected<=consumed||!queue)return false;
        std::shared_ptr<Shot> a,b;
        for(auto& latch:latched)if(latch.token==expected){a=latch.a;b=latch.b;break;}
        if(!a||!b||a->meta.width!=b->meta.width||a->meta.height!=b->meta.height||a->meta.configuration!=b->meta.configuration)return false;
        if(out.render_frame&&(a->meta.render_frame!=out.render_frame||b->meta.render_frame!=out.render_frame))return false;
        auto* lease=new(std::nothrow) Lease(a,b);if(!lease)return false;
        out.eyes[0]=a->meta;out.eyes[1]=b->meta;
        // Stable native-stereo ordering is assumed. An interrupted pair or a
        // missing native publication invalidates NGX's temporal history.
        if(consumed&&expected!=consumed+2)for(auto& e:out.eyes)e.reset=1;
        for(auto& e:out.eyes){e.depth->AddRef();e.motion->AddRef();}
        out.producer_queue=queue.Get();out.producer_queue->AddRef();out.lease=lease;
        consumed=expected;
        std::erase_if(latched,[&](const Latch& l){return l.token<=consumed;});return true;
    }
};

// An OpenXR image is registered on its first release. The existing native copy
// observer stamps subsequent game->OpenXR copies, then commits the stamp only
// after that exact command list is submitted to the producer queue. Newer DLSS
// evaluations between the color copy and xrReleaseSwapchainImage cannot retag it.
class FgColorProvenance {
    template<class T> using ComPtr=Microsoft::WRL::ComPtr<T>;
    struct Tag {std::uint64_t generation{},token{},render_frame{};};
    struct Pending {ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Resource> color;Tag tag;std::uint64_t recording{};bool conflicted{};};
    static inline constexpr GUID key{0x41c548c2,0x8d8a,0x4f23,{0x83,0x24,0x90,0xbe,0xa5,0xf8,0x22,0x7d}};
    std::vector<Pending> pending;
    std::uint64_t generation{1};
    static bool read(ID3D12Resource* color,Tag& tag) {
        if(!color)return false;UINT size=sizeof(tag);
        __try{return SUCCEEDED(color->GetPrivateData(key,&size,&tag))&&size==sizeof(tag);}
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION||GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
    }
public:
    static bool registered(ID3D12Resource* color) {
        Tag tag;return read(color,tag);
    }
    void invalidate(){++generation;pending.clear();}
    std::uint64_t token(ID3D12Resource* color) {
        if(!color)return 0;Tag tag;
        if(!read(color,tag)||tag.generation!=generation){
            tag={generation,0};color->SetPrivateData(key,sizeof(tag),&tag);return 0;
        }
        return tag.token;
    }
    std::uint64_t frame(ID3D12Resource* color) {Tag tag;return read(color,tag)&&tag.generation==generation?tag.render_frame:0;}
    void observe(ID3D12GraphicsCommandList* list,ID3D12Resource* color,FgSnapshots& guides,std::uint64_t render_frame=0) {
        if(!registered(color))return;
        auto native=native_d3d12_interface(list);
        const Tag tag{generation,render_frame?guides.token(render_frame):0,render_frame};
        const auto recording=FgSnapshots::tag_recording(list);if(!recording)return;
        for(auto& p:pending)if(p.recording==recording&&p.color.Get()==color){
            if(p.tag.render_frame!=tag.render_frame||(p.tag.token!=tag.token&&(!render_frame||(p.tag.token&&tag.token)))){
                p.conflicted=true;p.tag.token=0;
            }else if(!p.conflicted&&tag.token)p.tag.token=tag.token;
            return;
        }
        if(pending.size()>=128){Tag invalid{generation,0};color->SetPrivateData(key,sizeof(invalid),&invalid);return;}
        pending.push_back({native,color,tag,recording});
    }
    void submitted(ID3D12CommandQueue* queue,ID3D12GraphicsCommandList* list,FgSnapshots& guides) {
        const auto recording=FgSnapshots::recording_identity(list);
        std::erase_if(pending,[&](Pending& p){
            if(!recording||p.recording!=recording)return false;
            if(p.tag.generation!=generation||!guides.matches(queue))p.tag={generation,0};
            else if(!p.conflicted&&!p.tag.token&&p.tag.render_frame)
                // Guides and final color can be recorded in the SAME command
                // list. They become eligible only after this enqueue. Resolve
                // that exact frame now, never substitute a later frame.
                p.tag.token=guides.token(p.tag.render_frame);
            p.color->SetPrivateData(key,sizeof(p.tag),&p.tag);return true;
        });
    }
    void reset(ID3D12GraphicsCommandList* list) {
        const auto recording=FgSnapshots::recording_identity(list);
        std::erase_if(pending,[&](Pending& p){return recording&&p.recording==recording;});
    }
};
}
