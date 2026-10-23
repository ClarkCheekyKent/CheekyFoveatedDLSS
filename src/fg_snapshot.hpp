#pragma once
#include "d3d12_native.hpp"
#include "../shared/cheeky_fg_bridge.hpp"
#include "../shared/cheeky_gpu_span.hpp"
#include "../shared/fg_resource_memory.hpp"
#include "fg_prepared_guides.hpp"
#include "../shared/fg_capture_pair_token.hpp"
#include <memory>
#include <vector>
#include <algorithm>

namespace cheeky::foveated_dlss {
// Producer-private metadata: physical eye identities never depend on NGX call
// order once calibration is available. No change to the layer's leased ABI.
struct FgGuideIdentity {
    static constexpr unsigned unknown=~0u;
    unsigned eye{unknown};
    std::uint64_t mapping_revision{};
};
// Copies are recorded beside the original DLSS evaluation, before either eye's
// engine textures can be reused. Submission observation, not CPU record order,
// determines which evaluations are eligible for a released OpenXR color image.
class FgSnapshots {
    template<class T> using ComPtr=Microsoft::WRL::ComPtr<T>;
    struct Shot {
        ComPtr<ID3D12Resource> depth,motion,source_depth,source_motion;
        FgPreparedGuides::Buffers prepared;
        D3D12_RESOURCE_DESC raw_depth_desc{},raw_motion_desc{};
        ComPtr<ID3D12GraphicsCommandList> list;
        CheekyFgEye meta;
        FgGuideIdentity identity;
        std::uint64_t recording{};
        std::uint64_t retired{};
        std::uint64_t last_used_tick{};
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
    // A UEVR build may return a command-queue facade (or a different queue)
    // from its renderer accessor. The first *matching guide recording* proves
    // which queue executed the captured GPU commands.
    bool submission_queue_discovered{};
    std::vector<std::shared_ptr<Shot>> shots;
    struct Latch {std::uint64_t token;std::shared_ptr<Shot> a,b;};
    std::vector<Latch> latched;
    std::uint64_t ordinal{},serial{},consumed{};
    std::uint64_t failed_record{};
    std::uint64_t timestamp_frequency{};
    cheeky::gpu::Groups gpu_cost;
    FgPreparedGuides preparation;
    std::uint64_t prepared_captures{},raw_captures{},preparation_fallbacks{};
    std::uint64_t next_trim_tick{};
    CheekyFgEye previous_raw[2];
    FgGuideIdentity previous_identity[2];
    bool history_valid{};
    std::uint64_t history_mapping{},history_features[2]{};
    unsigned history_first_eye{FgGuideIdentity::unknown};
    static bool compatible_eyes(const Shot& a,const Shot& b) {
        return a.identity.mapping_revision==b.identity.mapping_revision &&
            ((a.identity.eye==FgGuideIdentity::unknown&&b.identity.eye==FgGuideIdentity::unknown)||
             (a.identity.eye<2&&b.identity.eye<2&&a.identity.eye!=b.identity.eye));
    }
    static void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition.pResource=r;
        v.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;v.Transition.StateBefore=a;v.Transition.StateAfter=b;list->ResourceBarrier(1,&v);
    }
    struct Region {unsigned x{},y{},w{},h{};bool full{};};
    static Region region(ID3D12Resource* source,unsigned x,unsigned y,unsigned w,unsigned h) {
        const auto d=source->GetDesc();
        // D3D12 requires whole-subresource copies for depth/stencil resources.
        // Keep their original shape; ordinary NGX guide textures can be cropped.
        if((d.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)||d.MipLevels!=1||d.DepthOrArraySize!=1)
            return {0,0,unsigned(d.Width),d.Height,true};
        if(x>=d.Width||y>=d.Height)return {};
        return {x,y,std::min(w?w:unsigned(d.Width),unsigned(d.Width)-x),std::min(h?h:d.Height,d.Height-y),false};
    }
    bool allocate(ID3D12Resource* source,ComPtr<ID3D12Resource>& out,const Region& region) {
        auto d=source->GetDesc();
        d.Width=region.w;d.Height=region.h;
        if(out){auto p=out->GetDesc();if(p.Width==d.Width&&p.Height==d.Height&&p.Format==d.Format)return true;}
        // Keep the original format/planes; the layer converts the owned copy.
        d.Flags=D3D12_RESOURCE_FLAG_NONE;d.Alignment=0;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;out.Reset();
        return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)));
    }
    void copy(ID3D12GraphicsCommandList* list,ID3D12Resource* from,ID3D12Resource* to,const Region& region) {
        // Native NGX's depth/MV contract is NON_PIXEL_SHADER_RESOURCE. Restore
        // it exactly; do not install descriptor heaps or alter compute bindings.
        barrier(list,from,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(list,to,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        if(region.full)list->CopyResource(to,from);
        else {
            D3D12_FEATURE_DATA_FORMAT_INFO info{from->GetDesc().Format,1};device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&info,sizeof(info));
            for(unsigned plane=0;plane<std::max(1u,unsigned(info.PlaneCount));++plane){
                D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=from;b.pResource=to;
                a.Type=b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;a.SubresourceIndex=b.SubresourceIndex=plane;
                D3D12_BOX box{region.x,region.y,0,region.x+region.w,region.y+region.h,1};list->CopyTextureRegion(&b,0,0,0,&a,&box);
            }
        }
        barrier(list,to,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        barrier(list,from,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
public:
    void memory(CheekyFgMemory& out,fg_memory::Collector& collect) const {
        const auto done=fence?fence->GetCompletedValue():0;
        for(const auto& shot:shots){
            const bool reusable=shot.use_count()==1&&(shot->cancelled||
                (shot->retired&&shot->retired<=done&&shot->meta.evaluation+2<ordinal+1&&!shot->foreign));
            collect.add(out.categories[fg_memory::guide_depth],shot->depth.Get(),reusable);
            collect.add(out.categories[fg_memory::guide_motion],shot->motion.Get(),reusable);
            for(const auto& p:shot->prepared.passes){
                collect.add(out.categories[fg_memory::guide_depth],p.depth.Get(),reusable);
                collect.add(out.categories[fg_memory::guide_motion],p.motion.Get(),reusable);
            }
        }
    }
    void guide_diagnostics(std::uint64_t& prepared,std::uint64_t& raw,std::uint64_t& fallbacks,unsigned& slots,unsigned& leased,std::uint64_t& raw_bytes) const {
        prepared+=prepared_captures;raw+=raw_captures;fallbacks+=preparation_fallbacks;slots+=unsigned(shots.size());
        for(const auto& s:shots){leased+=s.use_count()>1?1u:0u;
            for(const auto& desc:{s->raw_depth_desc,s->raw_motion_desc})if(desc.Width){
                const auto bytes=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;if(bytes!=UINT64_MAX)raw_bytes+=bytes;
            }
        }
    }
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
    // Peek only the UNCONSUMED capture interval (no latch/consume). A first
    // eye may be released before the second eye has been evaluated. In
    // hook-free mode only, predict the completed pair token as ordinal+1;
    // xrEndFrame MUST verify the actual token and the final crop geometry.
    bool peek_current_crops(CheekyFgCaptureCropsV2& out,std::uint64_t expected_configuration) const noexcept {
        if(failed_record>consumed)return false;
        const auto token=cheeky::fg::pending_stereo_crop_token(ordinal,consumed,!out.render_frame);
        if(!token)return false;
        const auto count=ordinal-consumed;
        if(count==1&&(out.render_frame||out.token))return false;
        const Shot* a=nullptr;const Shot* b=nullptr;
        for(const auto& shot:shots)if(shot->meta.evaluation>consumed){
            if(!shot->submitted||shot->foreign||shot->cancelled)return false;
            if(shot->meta.evaluation==ordinal-1&&count==2)a=shot.get();
            else if(shot->meta.evaluation==ordinal)b=shot.get();
            else return false;
        }
        if(!b||b->identity.eye>=2||b->meta.configuration!=expected_configuration||
           b->meta.render_frame!=out.render_frame)return false;
        if(count==2&&(!a||!compatible_eyes(*a,*b)||a->identity.eye>=2||
           a->meta.configuration!=expected_configuration||a->meta.width!=b->meta.width||
           a->meta.height!=b->meta.height||a->meta.render_frame!=out.render_frame))return false;
        if(out.token&&out.token!=token)return false;
        out.token=token;out.width=b->meta.width;out.height=b->meta.height;
        out.valid_mask=1u<<b->identity.eye;
        out.eye[b->identity.eye]={b->meta.focus_width,b->meta.focus_height,b->meta.focus_x,b->meta.focus_y};
        if(a){
            out.valid_mask|=1u<<a->identity.eye;
            out.eye[a->identity.eye]={a->meta.focus_width,a->meta.focus_height,a->meta.focus_x,a->meta.focus_y};
        }
        return true;
    }
    static inline constexpr GUID recording_key{0xf654ab8c,0x35ac,0x4b73,{0x95,0x23,0x7c,0x55,0x78,0x41,0x87,0xe9}};
    static std::uint64_t recording_identity(ID3D12GraphicsCommandList* list) noexcept {
        if(!list)return 0;std::uint64_t tag{};UINT size=sizeof(tag);
        __try{return SUCCEEDED(list->GetPrivateData(recording_key,&size,&tag))&&size==sizeof(tag)?tag:0;}
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION||GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return 0;}
    }
    static std::uint64_t tag_recording(ID3D12GraphicsCommandList* list){
        if(!list)return 0;
        // NGX can receive a forwarding D3D12 list whereas ExecuteCommandLists
        // receives a different interface for the same underlying recording.
        // Stamp both only when their existing identities agree. Never overwrite
        // another live recording's ID or infer equality from an adapter/device.
        auto native=native_d3d12_interface(list);
        const auto a=recording_identity(list),b=native?recording_identity(native.Get()):0;
        if(a&&b&&a!=b)return 0;
        static std::atomic<std::uint64_t> next{};
        const auto id=a?a:b?b:++next;
        if(!a&&FAILED(list->SetPrivateData(recording_key,sizeof(id),&id)))return 0;
        if(native&&native.Get()!=list&&!b&&
           FAILED(native->SetPrivateData(recording_key,sizeof(id),&id)))return 0;
        return id;
    }
    static bool tagged(ID3D12GraphicsCommandList* list) noexcept {
        if(recording_identity(list))return true;
        auto native=native_d3d12_interface(list);
        return native&&native.Get()!=list&&recording_identity(native.Get())!=0;
    }
    bool matches(ID3D12CommandQueue* q) const{return same_d3d12_queue(queue.Get(),q);}
    bool has_discovered_submission_queue() const noexcept {return submission_queue_discovered;}
    bool same_queue_device(ID3D12CommandQueue* q) const noexcept {
        auto native=native_d3d12_interface(q);
        Microsoft::WRL::ComPtr<ID3D12Device> candidate;
        return device&&native&&SUCCEEDED(native->GetDevice(IID_PPV_ARGS(&candidate)))&&
            same_d3d12_device(device.Get(),candidate.Get());
    }
    bool pending_submissions() const {
        for(const auto& s:shots)if(!s->submitted&&!s->cancelled)return true;
        return false;
    }
    struct SubmissionStats {unsigned pending{},submitted{},foreign{},retired{},cancelled{};std::uint64_t pending_id{},pending_recording{};};
    SubmissionStats submission_stats() const {
        SubmissionStats d;
        for(const auto& s:shots){
            if(s->cancelled)++d.cancelled;
            else if(s->foreign)++d.foreign;
            else if(!s->submitted){++d.pending;d.pending_id=s->meta.evaluation;d.pending_recording=s->recording;}
            else if(!s->retired)++d.submitted;
            else ++d.retired;
        }
        return d;
    }
    void invalidate(){consumed=ordinal;latched.clear();previous_raw[0]={};previous_raw[1]={};history_valid=false;}
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
        submission_queue_discovered=false;
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
    bool record(ID3D12GraphicsCommandList* commands,CheekyFgEye meta,FgGuideIdentity identity={}) {
        const auto id=++ordinal; // Failed/dropped captures must leave a gap.
        if(!queue||!fence||!commands||!meta.depth||!meta.motion){failed_record=id;return false;}
        const auto recording=tag_recording(commands);if(!recording){failed_record=id;return false;}
        std::shared_ptr<Shot> slot;
        const auto done=fence->GetCompletedValue();
        const auto now=GetTickCount64();
        if(now>=next_trim_tick){
            // Reclaim high-water allocations after transient queue/frame
            // pressure. Leases, pending recordings and the most recent stereo
            // pair remain protected; no producer or consumer GPU wait is added.
            collect_completed();
            std::erase_if(shots,[&](const auto& s){return s.use_count()==1&&!s->foreign&&
                (s->cancelled||(s->retired&&s->retired<=done))&&s->meta.evaluation+2<id&&
                now>=s->last_used_tick&&now-s->last_used_tick>=3000;});
            next_trim_tick=now+1000;
        }
        // Keep the most recent stereo pair until a later color handoff, even
        // when its GPU work has already completed.
        for(auto& s:shots)if(s.use_count()==1&&(s->cancelled||(s->retired&&s->retired<=done&&s->meta.evaluation+2<id&&!s->foreign))){slot=s;break;}
        if(!slot){if(shots.size()>=24){failed_record=id;return false;}slot=std::make_shared<Shot>();shots.push_back(slot);}
        slot->last_used_tick=now;
        if(slot->retired&&slot->retired<=done) {
            double ms{};if(slot->gpu_span.collect(timestamp_frequency,ms))gpu_cost.add(slot->meta.render_frame,ms,false,2);
            gpu_cost.commit();
        }
        const auto dr=region(meta.depth,meta.depth_base_x,meta.depth_base_y,meta.depth_width,meta.depth_height),
                   mr=region(meta.motion,meta.motion_base_x,meta.motion_base_y,meta.motion_width,meta.motion_height);
        if(!dr.w||!dr.h||!mr.w||!mr.h){failed_record=id;return false;}
        slot->raw_depth_desc=meta.depth->GetDesc();slot->raw_depth_desc.Width=dr.w;slot->raw_depth_desc.Height=dr.h;slot->raw_depth_desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        slot->raw_motion_desc=meta.motion->GetDesc();slot->raw_motion_desc.Width=mr.w;slot->raw_motion_desc.Height=mr.h;slot->raw_motion_desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        slot->source_depth=meta.depth;slot->source_motion=meta.motion;slot->list=native_d3d12_interface(commands);
        slot->recording=recording;
        slot->submitted=slot->foreign=slot->cancelled=false;slot->retired=0;
        const auto history_eye=identity.eye<2?identity.eye:unsigned((id-1)%2);
        auto& prior=previous_raw[history_eye];
        const auto prior_identity=previous_identity[history_eye];
        if(meta.motion_jittered){
            // Without a UEVR frame ticket, the consecutive left/right
            // evaluation ordinal is the same-eye temporal identity. Never
            // compare two identical zero frame IDs as native frame numbers.
            const bool previous_eye=meta.render_frame ? prior.render_frame+1==meta.render_frame :
                prior.render_frame==0 && prior.evaluation+2==id;
            if(previous_eye&&prior.motion_jittered&&prior_identity.eye==identity.eye&&prior_identity.mapping_revision==identity.mapping_revision&&
               prior.feature==meta.feature&&prior.motion_width==meta.motion_width&&
               prior.motion_height==meta.motion_height&&!meta.reset){
                meta.jitter_correction_x=(prior.jitter_x-meta.jitter_x)/std::max(1u,meta.motion_width);
                meta.jitter_correction_y=(prior.jitter_y-meta.jitter_y)/std::max(1u,meta.motion_height);
            }else meta.reset=1;
        }
        meta.evaluation=id;
        slot->identity=identity;previous_identity[history_eye]=identity;
        prior=meta;prior.depth=prior.motion=nullptr;prior.peripheral_depth=prior.peripheral_motion=nullptr;
        tag_recording(commands);
        if(timestamp_frequency)slot->gpu_span.begin(device.Get(),commands);
        const bool prepared=meta.guide_capture_mode==1u&&preparation.record(device.Get(),commands,meta,slot->prepared);
        if(prepared){slot->depth.Reset();slot->motion.Reset();++prepared_captures;}
        else {
            // Never silently pass a multi-slice raw depth texture to the FG
            // consumer, which expects one 2D guide per physical eye.
            if(meta.depth->GetDesc().DepthOrArraySize>1){failed_record=id;return false;}
            if(meta.guide_capture_mode==1u)++preparation_fallbacks;
            slot->prepared.clear();
            if(!allocate(meta.depth,slot->depth,dr)||!allocate(meta.motion,slot->motion,mr)){failed_record=id;return false;}
            copy(commands,meta.depth,slot->depth.Get(),dr);copy(commands,meta.motion,slot->motion.Get(),mr);++raw_captures;
        }
        if(timestamp_frequency)slot->gpu_span.end(commands);
        if(!prepared){
            if(!dr.full){meta.depth_base_x=meta.depth_base_y=0;meta.depth_width=dr.w;meta.depth_height=dr.h;}
            if(!mr.full){meta.motion_base_x=meta.motion_base_y=0;meta.motion_width=mr.w;meta.motion_height=mr.h;}
            meta.depth=slot->depth.Get();meta.motion=slot->motion.Get();meta.prepared_guides=0;
        }
        slot->meta=meta;return true;
    }
    unsigned submitted(ID3D12CommandQueue* q,ID3D12GraphicsCommandList* commands,bool discover_queue=false) {
        unsigned found{};
        const auto native_list=native_d3d12_interface(commands);
        const auto recording=recording_identity(commands);
        const auto native_recording=native_list?recording_identity(native_list.Get()):0;
        for(auto& s:shots)if(!s->submitted&&
            ((recording&&s->recording==recording)||(native_recording&&s->recording==native_recording)||
             (!recording&&!native_recording&&same_d3d12_list(s->list.Get(),commands)))) {
            if(discover_queue&&!submission_queue_discovered){
                // Only this explicitly recognized NGX recording may select the
                // executing queue. Mere same-device, timing, or queue priority
                // NEVER proves that a command list was submitted here.
                // Fence resources are per-device, so the existing unsignaled
                // fence may safely be signaled by this queue instead.
                // Do not move a queue once any earlier guide was submitted:
                // it could otherwise signal before that earlier GPU work.
                const bool previous_submits=std::any_of(shots.begin(),shots.end(),
                    [](const auto& shot){return shot->submitted&&!shot->cancelled;});
                if(!previous_submits&&same_queue_device(q)){
                    auto executing=native_d3d12_interface(q);
                    if(executing){
                        queue=std::move(executing);
                        timestamp_frequency=0;
                        queue->GetTimestampFrequency(&timestamp_frequency);
                        submission_queue_discovered=true;
                    }
                }
            }
            s->submitted=true;s->foreign=!matches(q);s->list.Reset();++found;
        }
        return found;
    }
    void reset(ID3D12GraphicsCommandList* commands) {
        const auto recording=recording_identity(commands);
        for(auto& s:shots)if(!s->submitted&&
            (recording?s->recording==recording:same_d3d12_list(s->list.Get(),commands))){
            s->cancelled=true;s->submitted=true;s->list.Reset();s->source_depth.Reset();s->source_motion.Reset();
            s->gpu_span.pending=false;
            failed_record=std::max(failed_record,s->meta.evaluation);
        }
    }
    std::uint64_t token(std::uint64_t render_frame=0,bool hookless=false) {
        retire();last_match=inspect(render_frame);
        if(hookless){
            // Exactly two consecutive, submitted guide recordings since the
            // last consumed COLOR interval. Never silently take the newest
            // two from a 1/3/4-eye interval or across a failed recording.
            last_match={};last_match.consumed=consumed;last_match.failed=failed_record;
            last_match.latest_frame=0;last_match.frame=0;
            for(const auto& s:shots)if(s->meta.evaluation>consumed){
                if(s->cancelled)++last_match.cancelled;
                else if(s->foreign)++last_match.foreign;
                else if(!s->submitted)++last_match.pending;
                else {++last_match.eligible;last_match.latest=std::max(last_match.latest,s->meta.evaluation);}
            }
            if(ordinal<consumed||ordinal-consumed!=2||failed_record>consumed||
                last_match.eligible!=2||last_match.pending||last_match.foreign||last_match.cancelled||
                last_match.latest!=ordinal){
                last_match.reason=failed_record>consumed?"Guide capture failed":
                    last_match.pending?"Guides await submission":
                    last_match.foreign?"Guide queue mismatch":
                    last_match.cancelled?"Guide commands cancelled":
                    "Color interval has not exactly two fresh eyes";
                return 0;
            }
        }
        const auto latest=last_match.latest;
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
        if(!a||!b||(render_frame&&(a->meta.render_frame!=render_frame||b->meta.render_frame!=render_frame))||
            (hookless&&(a->meta.render_frame||b->meta.render_frame))){last_match.reason="Guide pair is interrupted";return 0;}
        if(!compatible_eyes(*a,*b)){last_match.reason="Guide eye mapping changed or stereo identities are ambiguous";return 0;}
        // Pin guides at COLOR release, not later at EndFrame. A future DLSS
        // recording must not recycle either snapshot while color awaits submit.
        if(latched.size()>=4)latched.erase(latched.begin());
        latched.push_back({latest,a,b});return latest;
    }
    void discard_hookless_interval() {
        // A partial/ambiguous color handoff consumes the interval without
        // generating FG. Start fresh with the next native stereo pair rather
        // than permanently starving on an extra or failed evaluation.
        consumed=ordinal;
        history_valid=false;
        std::erase_if(latched,[&](const Latch& l){return l.token<=consumed;});
    }
    bool acquire(CheekyFgPair& out) {
        const auto expected=out.capture_token;
        if(!expected||expected<=consumed||!queue)return false;
        std::shared_ptr<Shot> a,b;
        for(auto& latch:latched)if(latch.token==expected){a=latch.a;b=latch.b;break;}
        if(!a||!b||a->meta.width!=b->meta.width||a->meta.height!=b->meta.height||a->meta.configuration!=b->meta.configuration)return false;
        if(out.render_frame&&(a->meta.render_frame!=out.render_frame||b->meta.render_frame!=out.render_frame))return false;
        if(!compatible_eyes(*a,*b))return false;
        const auto first_eye=a->identity.eye;
        if(first_eye==1)std::swap(a,b);
        auto* lease=new(std::nothrow) Lease(a,b);if(!lease)return false;
        out.eyes[0]=a->meta;out.eyes[1]=b->meta;
        // Keep both NGX and peripheral histories tied to physical eyes. Even
        // unjittered native motion must reset after a calibration/order switch.
        const bool reset=!history_valid||history_mapping!=a->identity.mapping_revision||history_first_eye!=first_eye||
            history_features[0]!=a->meta.feature||history_features[1]!=b->meta.feature||
            (consumed&&expected!=consumed+2)||out.eyes[0].reset||out.eyes[1].reset;
        if(reset)for(auto& e:out.eyes)e.reset=1;
        history_valid=true;history_mapping=a->identity.mapping_revision;history_first_eye=first_eye;
        history_features[0]=a->meta.feature;history_features[1]=b->meta.feature;
        for(auto& e:out.eyes)for(auto* resource:{e.depth,e.motion,e.peripheral_depth,e.peripheral_motion})if(resource)resource->AddRef();
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
    struct Pending {ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Resource> color;Tag tag;std::uint64_t recording{};bool conflicted{},hookless{};};
    static inline constexpr GUID key{0x41c548c2,0x8d8a,0x4f23,{0x83,0x24,0x90,0xbe,0xa5,0xf8,0x22,0x7d}};
    std::vector<Pending> pending;
    // In mode 3, color can be copied multiple times during one native frame.
    // Queue submission only establishes provenance; choose a stereo token at
    // the FINAL OpenXR color handoff, not at each intermediate copy.
    std::vector<ComPtr<ID3D12Resource>> hookless_submitted;
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
    void invalidate(){++generation;pending.clear();hookless_submitted.clear();}
    bool has_pending() const {return !pending.empty();}
    std::uint64_t token(ID3D12Resource* color,FgSnapshots* guides=nullptr,bool hookless=false) {
        if(!color)return 0;Tag tag;
        if(!read(color,tag)||tag.generation!=generation){
            tag={generation,0};color->SetPrivateData(key,sizeof(tag),&tag);return 0;
        }
        if(hookless&&guides){
            const auto it=std::find_if(hookless_submitted.begin(),hookless_submitted.end(),
                [&](const auto& r){return r.Get()==color;});
            if(it!=hookless_submitted.end()){
                // Only a submitted copy of THIS OpenXR color image authorizes
                // pairing. token() refuses incomplete/extra/pending guides.
                const auto matched=guides->token(0,true);
                if(!matched){
                    // The OpenXR handoff is the sole rejection boundary.
                    // Incomplete/extra work cannot poison subsequent frames.
                    guides->discard_hookless_interval();
                    hookless_submitted.erase(it);
                    return 0;
                }
                tag={generation,matched,0};
                if(FAILED(color->SetPrivateData(key,sizeof(tag),&tag))){
                    guides->discard_hookless_interval();
                    hookless_submitted.erase(it);
                    return 0;
                }
                hookless_submitted.erase(it); // latch pins snapshots until acquire
            }
        }
        return tag.token;
    }
    std::uint64_t frame(ID3D12Resource* color) {Tag tag;return read(color,tag)&&tag.generation==generation?tag.render_frame:0;}
    void observe(ID3D12GraphicsCommandList* list,ID3D12Resource* color,FgSnapshots& guides,
                 std::uint64_t render_frame=0,bool hookless=false) {
        if(!registered(color))return;
        auto native=native_d3d12_interface(list);
        const Tag tag{generation,hookless?0:render_frame?guides.token(render_frame):0,render_frame};
        const auto recording=FgSnapshots::tag_recording(list);if(!recording)return;
        for(auto& p:pending)if(p.recording==recording&&p.color.Get()==color){
            if(p.hookless!=hookless||p.tag.render_frame!=tag.render_frame||
               (p.tag.token!=tag.token&&(!render_frame||(p.tag.token&&tag.token)))){
                p.conflicted=true;p.tag.token=0;
            }else if(!p.conflicted&&tag.token)p.tag.token=tag.token;
            return;
        }
        if(pending.size()>=128){Tag invalid{generation,0};color->SetPrivateData(key,sizeof(invalid),&invalid);return;}
        pending.push_back({native,color,tag,recording,false,hookless});
    }
    void submitted(ID3D12CommandQueue* queue,ID3D12GraphicsCommandList* list,FgSnapshots& guides) {
        const auto recording=FgSnapshots::recording_identity(list);
        std::erase_if(pending,[&](Pending& p){
            if(recording?p.recording!=recording:!same_d3d12_list(p.list.Get(),list))return false;
            if(p.tag.generation!=generation||!guides.matches(queue))p.tag={generation,0};
            else if(!p.conflicted){
                if(!p.hookless&&!p.tag.token&&p.tag.render_frame)
                    // Guides and final color can be recorded in the SAME command
                    // list. They become eligible only after this enqueue.
                    p.tag.token=guides.token(p.tag.render_frame);
            }
            if(p.hookless){
                // A previous frame's token must not survive a new color copy.
                // Preserve the unconsumed guide interval until the OpenXR
                // handoff. Intermediate copies no longer destroy a stereo pair.
                p.tag={generation,0,0};
                if(SUCCEEDED(p.color->SetPrivateData(key,sizeof(p.tag),&p.tag)) &&
                   !p.conflicted&&guides.matches(queue)){
                    if(std::none_of(hookless_submitted.begin(),hookless_submitted.end(),
                       [&](const auto& r){return r.Get()==p.color.Get();})){
                        if(hookless_submitted.size()>=16)hookless_submitted.erase(hookless_submitted.begin());
                        hookless_submitted.push_back(p.color);
                    }
                }
            }else p.color->SetPrivateData(key,sizeof(p.tag),&p.tag);
            return true;
        });
    }
    void reset(ID3D12GraphicsCommandList* list) {
        const auto recording=FgSnapshots::recording_identity(list);
        std::erase_if(pending,[&](Pending& p){return recording?p.recording==recording:
            same_d3d12_list(p.list.Get(),list);});
    }
};
}
