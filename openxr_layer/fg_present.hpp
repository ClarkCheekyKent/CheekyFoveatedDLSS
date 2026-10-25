#pragma once
#include "dlssg_worker.hpp"
#include "fg_layers.hpp"
#include "fg_color_format.hpp"
#include "fg_d3d11.hpp"
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <memory>
#include <cstdio>
#include <unordered_map>
#include <cstring>

namespace cheeky::fg {
using Microsoft::WRL::ComPtr;
inline void color_transition(ID3D12GraphicsCommandList* list,ID3D12Resource* tex,
    D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) noexcept {
    if(from==to)return;
#ifdef CHEEKY_FG_BARRIER_OBSERVER
    CHEEKY_FG_BARRIER_OBSERVER(tex,from,to);
#endif
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=tex;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=from;b.Transition.StateAfter=to;list->ResourceBarrier(1,&b);
}
struct XrFns {
    XrInstance time_instance{XR_NULL_HANDLE};
    PFN_xrConvertTimeToWin32PerformanceCounterKHR time_to_qpc{};
    PFN_xrCreateSwapchain create{};
    PFN_xrDestroySwapchain destroy{};
    PFN_xrEnumerateSwapchainImages enumerate{};
    PFN_xrAcquireSwapchainImage acquire{};
    PFN_xrWaitSwapchainImage wait_image{};
    PFN_xrReleaseSwapchainImage release{};
    PFN_xrWaitFrame wait_frame{};
    PFN_xrBeginFrame begin_frame{};
    PFN_xrEndFrame end_frame{};
    PFN_xrLocateSpace locate_space{};
    RetainFrameSpace retain_space{};
};
struct ColorCopy {
    XrSwapchain source{XR_NULL_HANDLE};
    unsigned index{}, width{}, height{},array{};
    ComPtr<ID3D12Resource> color;
};
struct Presenter {
    std::recursive_mutex mx;
    XrFns xr{};
    XrSession session{XR_NULL_HANDLE};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    std::shared_ptr<D3D11Bridge> interop11;
    bool quad_input{}; // Separate raw quad color planes plus explicitly supplied guide atlases.
    virtual std::recursive_mutex* dispatch_mutex11() const noexcept {
        return interop11?interop11->dispatch_mutex:nullptr;
    }
    // NGX can bypass the D3D11 API's per-call protection. Serialize complete
    // driver submissions without holding ID3D11Multithread across a runtime
    // call (the runtime may use its own helper threads).
    template<class Submit> auto runtime_dispatch(Submit&& submit) {
        std::unique_lock<std::recursive_mutex> lock;
        if(auto* gate=dispatch_mutex11())lock=std::unique_lock(*gate);
        return submit();
    }
    D3D12_RESOURCE_STATES xr_image_state() const noexcept {
        return interop11 ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    bool enumerate_images(XrSwapchain chain, std::vector<XrSwapchainImageD3D12KHR>& images) {
        if (interop11) return interop11->enumerate(xr.enumerate, chain, images);
        unsigned count{};
        if (XR_FAILED(xr.enumerate(chain,0,&count,nullptr)) || !count) return false;
        images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        return XR_SUCCEEDED(xr.enumerate(chain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
    }
    XrResult release_image(XrSwapchain chain, unsigned index, const XrSwapchainImageReleaseInfo* info) {
        const bool copied = !interop11 || interop11->upload(chain,index);
        const auto submit=[&]{return xr.release(chain,info);};
        const auto result = runtime_dispatch(submit);
        return copied || XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE;
    }
    void destroy_chain(XrSwapchain chain) {
        if (interop11) interop11->forget(chain);
        if (xr.destroy) xr.destroy(chain);
    }
    virtual bool capture11(XrSwapchain chain, unsigned index, ID3D11Texture2D* source,
        unsigned width, unsigned height, unsigned array, DXGI_FORMAT format) {
        return interop11 && interop11->capture(chain,source,[&](ID3D12Resource* shared) {
            return capture(chain,index,shared,width,height,array,format);
        });
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence; HANDLE event{};std::uint64_t serial{};
    Worker worker;
    std::uint64_t configuration{};
    virtual void configure(std::uint64_t next) noexcept {
        std::lock_guard lock(mx);
        if(!next||next==configuration)return;
        if(configuration) {
            invalidate_temporal();worker.reset();worker.failed=false;
            ngx_logged=generation_logged=false;state=CheekyFgState::waiting_for_history;
        }
        configuration=next;
    }
    TimingAverage copy_wait,xr_wait;
    virtual CheekyFgCadence cadence() const noexcept {return {};}
    virtual CheekyFgPacingTimeline pacing_timeline() const noexcept {return {};}
    virtual CheekyFgNativeSubmissionLatency native_submission_latency() const noexcept {return {};}
    virtual CheekyFgOverhead overhead() const noexcept {return {};}
    virtual CheekyFgPresentation presentation() const noexcept {return {};}
    virtual CheekyFgExperimentDiagnostics experiments() const noexcept {return {};}
    virtual CheekyFgDirectOutput direct_output() const noexcept {return {};}
    virtual CheekyFgDirectOutputDetails direct_output_details() const noexcept {return {};}
    virtual CheekyFgMemory memory() const noexcept {return {};}
    virtual void note_app_image_wait(double) noexcept {}
    virtual void note_app_call(unsigned,double) noexcept {}
    virtual void set_native_frame_pending(bool) noexcept {}
    double copy_wait_accumulator{},xr_period_ms{};
    virtual CheekyFgPerformance performance() const noexcept {
        CheekyFgPerformance out;const auto& t=worker.gpu_timings;
        out.gpu_supported=t.supported();out.gpu_valid=t.valid;
        out.foveated_eyes=t.foveated_eyes;out.window_samples=t.total.count;out.gpu_samples=t.samples;
        out.center_ms=t.center.mean();out.center_last_ms=t.center.last;
        out.periphery_ms=t.periphery.mean();out.periphery_last_ms=t.periphery.last;
        out.preparation_ms=t.preparation.mean();out.preparation_last_ms=t.preparation.last;
        out.total_ms=t.total.mean();out.total_last_ms=t.total.last;
        out.completion_wait_ms=worker.completion_wait.mean();out.copy_wait_ms=copy_wait.mean();
        out.xr_wait_ms=xr_wait.mean();out.xr_period_ms=xr_period_ms;
        out.center_width=worker.center_width;out.center_height=worker.center_height;
        out.peripheral_width=worker.peripheral_width;out.peripheral_height=worker.peripheral_height;
        out.output_width=worker.width;out.output_height=worker.height;out.peripheral_method=worker.peripheral_method;
        out.source_depth_format=worker.source_depth_format;out.source_motion_format=worker.source_motion_format;
        return out;
    }
    virtual CheekyFgBatchTimingsV1 batch_timing() const noexcept {return {};}
    std::deque<ColorCopy> copies;
    std::array<ComPtr<ID3D12Resource>,2> held{};
    std::array<ComPtr<ID3D12Resource>,2> generated{}, fallback{};
    std::array<XrCompositionLayerProjectionView,2> held_views{}, generated_views{}, fallback_views{};
    std::array<XrSwapchain,2> real_chains{{XR_NULL_HANDLE,XR_NULL_HANDLE}};
    std::array<XrSwapchain,2> fg_chains{{XR_NULL_HANDLE,XR_NULL_HANDLE}};
    std::array<std::vector<XrSwapchainImageD3D12KHR>,2> real_images{}, fg_images{};
    bool held_valid{},generated_valid{};
    XrEnvironmentBlendMode blend{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    XrSpace space{XR_NULL_HANDLE};
    std::uint64_t frames{},generated_count{},fallback_count{};
    std::uint64_t native_count{};
    FrameLayers frame_layers;
    CheekyFgState state{CheekyFgState::waiting_for_history};
    void (*trace)(const char*) noexcept {};
    bool capture_logged{}, ngx_logged{}, generation_logged{}, layout_logged{}, last_layout_accepted{};
    const char* last_error_stage{};
    long long last_error_code{};
    void failure(const char* stage,long long code) noexcept {
        if(!trace||(last_error_stage&&!std::strcmp(last_error_stage,stage)&&last_error_code==code))return;
        char text[224]{};sprintf_s(text,"DLSS-FG %s failed: code=%lld (0x%llx); preserving native presentation",stage,code,code);
        trace(text);last_error_stage=stage;last_error_code=code;
    }
    virtual const char* diagnostic_text() const noexcept {return worker.diagnostic;}
    virtual ~Presenter(){reset();}
    virtual void invalidate_temporal() noexcept {
        std::lock_guard lock(mx);
        // Copies can still be queued when a submission is unsupported. Keep
        // their destinations alive until the queue has finished with them.
        if(fence&&serial)wait_copy();
        worker.wait_idle();
        held={};generated={};fallback={};copies.clear();
        held_valid=false;generated_valid=false;frames=0;space=XR_NULL_HANDLE;
        frame_layers.clear();
    }
    virtual void reject_frame(const XrFrameEndInfo* end) noexcept {
        std::lock_guard lock(mx);
        ++native_count;
        invalidate_temporal();state=CheekyFgState::unsupported_submission;
        log_layout(end,false);
    }
    void log_layout(const XrFrameEndInfo* end,bool accepted) noexcept {
        if(!trace||(layout_logged&&last_layout_accepted==accepted))return;
        char text[256]{};
        sprintf_s(text,"DLSS-FG first submission: layers=%u accepted=%u types=%u,%u,%u,%u",
            end?end->layerCount:0,unsigned(accepted),
            end&&end->layers&&end->layerCount>0&&end->layers[0]?unsigned(end->layers[0]->type):0,
            end&&end->layers&&end->layerCount>1&&end->layers[1]?unsigned(end->layers[1]->type):0,
            end&&end->layers&&end->layerCount>2&&end->layers[2]?unsigned(end->layers[2]->type):0,
            end&&end->layers&&end->layerCount>3&&end->layers[3]?unsigned(end->layers[3]->type):0);
        trace(text);layout_logged=true;last_layout_accepted=accepted;
    }
    virtual void reset() noexcept {
        std::lock_guard lock(mx);
        if(fence&&serial)wait_copy();
        worker.reset();
        for(auto* a:{&real_chains,&fg_chains})for(auto& chain:*a)if(chain)destroy_chain(chain);
        real_chains={XR_NULL_HANDLE,XR_NULL_HANDLE};fg_chains=real_chains;
        real_images={};fg_images={};held={};generated={};fallback={};copies.clear();
        frame_layers.clear();
        held_valid=generated_valid=false;
        if(event)CloseHandle(event);event=nullptr;
        allocator.Reset();list.Reset();fence.Reset();queue.Reset();device.Reset();serial=0;
    }
    virtual bool setup(XrSession s,const XrFns& f,ID3D12Device* dev,ID3D12CommandQueue* q) noexcept {
        if(!dev||!q||!f.create||!f.enumerate||!f.acquire||!f.wait_image||!f.release||!f.wait_frame||!f.begin_frame||!f.end_frame)return false;
        xr=f;session=s;device=dev;queue=q;
        if(FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||
           FAILED(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)))||
           FAILED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))return false;
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        return event&&SUCCEEDED(list->Close());
    }
    bool begin_copy() noexcept {
        if(!device || FAILED(device->GetDeviceRemovedReason()))return false;
        if(serial&&fence->GetCompletedValue()<serial){
            if(FAILED(fence->SetEventOnCompletion(serial,event))||WaitForSingleObject(event,1000)!=WAIT_OBJECT_0)return false;
        }
        return SUCCEEDED(allocator->Reset())&&SUCCEEDED(list->Reset(allocator.Get(),nullptr));
    }
    bool end_copy() noexcept {
        if(FAILED(list->Close()))return false;
        ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
        return SUCCEEDED(queue->Signal(fence.Get(),++serial));
    }
    bool wait_copy() noexcept {
        AddElapsed elapsed{copy_wait_accumulator};
        if(!fence||!device)return false;
        while(fence->GetCompletedValue()<serial) {
            if(FAILED(device->GetDeviceRemovedReason()))return false;
            // A timeout is not permission to free resources still in flight.
            // Poll device removal as well, so TDR cannot strand teardown.
            if(event && SUCCEEDED(fence->SetEventOnCompletion(serial,event)))
                WaitForSingleObject(event,100);
            else Sleep(1);
        }
        return SUCCEEDED(device->GetDeviceRemovedReason());
    }
    // Called BEFORE the downstream release. Every capture has an independently
    // owned allocation, so a later app acquisition cannot overwrite held color.
    virtual bool capture(XrSwapchain swapchain,unsigned index,ID3D12Resource* original,
                 unsigned width,unsigned height,unsigned array_size,
                 DXGI_FORMAT declared_format=DXGI_FORMAT_UNKNOWN) noexcept {
        std::lock_guard lock(mx);
        if(!original||!width||!height)return false;
        worker.trace=trace;
        if(worker.ngx_required&&!worker.probe(device.Get())) {state=worker.capability_ready?CheekyFgState::generation_failed:CheekyFgState::ngx_unavailable;return false;}
        auto desc=original->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||
            desc.SampleDesc.Count!=1||desc.DepthOrArraySize!=array_size)return false;
        const auto typed=capture_format(desc.Format,declared_format);
        if(typed==DXGI_FORMAT_UNKNOWN) {failure("typed color format",desc.Format);return false;}
        if(trace&&!capture_logged) {
            char details[192]{};
            sprintf_s(details,"DLSS-FG first color capture: %ux%u resource_format=%u declared_format=%u owned_format=%u array=%u",
                width,height,unsigned(desc.Format),unsigned(declared_format),unsigned(typed),array_size);
            trace(details);capture_logged=true;
        }
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        desc.Format=typed;
        desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> copy;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&copy)))||!begin_copy())return false;
        // XR_KHR_D3D12_enable requires color images in RENDER_TARGET at both
        // wait and release. COMMON is only for our privately owned copies.
        color_transition(list.Get(),original,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        color_transition(list.Get(),copy.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(copy.Get(),original);
        color_transition(list.Get(),copy.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        color_transition(list.Get(),original,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        if(!end_copy())return false;
        copies.push_back({swapchain,index,width,height,array_size,copy});
        while(copies.size()>12)copies.pop_front();return wait_copy();
    }
    ComPtr<ID3D12Resource> subrect(XrSwapchain sc,unsigned image,XrRect2Di rect,unsigned layer) noexcept {
        auto it=std::find_if(copies.rbegin(),copies.rend(),[&](const ColorCopy& c){return c.source==sc&&c.index==image;});
        if(it==copies.rend()||layer>=it->array||rect.extent.width<=0||rect.extent.height<=0||
           rect.offset.x<0||rect.offset.y<0||rect.offset.x+rect.extent.width>static_cast<int>(it->width)||
           rect.offset.y+rect.extent.height>static_cast<int>(it->height))return {};
        auto src=it->color;const auto d=src->GetDesc();
        D3D12_RESOURCE_DESC out=d;out.Width=rect.extent.width;out.Height=rect.extent.height;
        out.DepthOrArraySize=1;out.Flags=D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> target;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&out,
           D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&target)))||!begin_copy())return {};
        color_transition(list.Get(),src.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        color_transition(list.Get(),target.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=src.Get();to.pResource=target.Get();
        from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex=layer*d.MipLevels;
        to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=0;
        D3D12_BOX box{static_cast<UINT>(rect.offset.x),static_cast<UINT>(rect.offset.y),0,
            static_cast<UINT>(rect.offset.x+rect.extent.width),static_cast<UINT>(rect.offset.y+rect.extent.height),1};
        list->CopyTextureRegion(&to,0,0,0,&from,&box);
        color_transition(list.Get(),src.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        color_transition(list.Get(),target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        if(!end_copy()||!wait_copy())return {};
        return target;
    }
    bool chains(std::array<XrSwapchain,2>& chains,
        std::array<std::vector<XrSwapchainImageD3D12KHR>,2>& images,
        const std::array<ComPtr<ID3D12Resource>,2>& colors) noexcept {
        for(unsigned i=0;i<2;++i){
            if(!colors[i])return false;
            if(chains[i]!=XR_NULL_HANDLE){
                // Geometry/format changes require complete session-side reset.
                const auto& d=colors[i]->GetDesc();
                if(images[i].empty()||!images[i][0].texture||images[i][0].texture->GetDesc().Width!=d.Width||
                   images[i][0].texture->GetDesc().Height!=d.Height||
                   !compatible_color(images[i][0].texture->GetDesc().Format,d.Format))return false;
                continue;
            }
            auto d=colors[i]->GetDesc();
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            info.format=static_cast<std::int64_t>(d.Format);info.sampleCount=1;info.width=static_cast<unsigned>(d.Width);
            info.height=d.Height;info.faceCount=1;info.arraySize=1;info.mipCount=1;
            const auto created=xr.create(session,&info,&chains[i]);
            if(XR_FAILED(created)) {failure("xrCreateSwapchain",created);return false;}
            unsigned count{};if(XR_FAILED(xr.enumerate(chains[i],0,&count,nullptr))||!count)return false;
            auto& v=images[i];v.resize(count);
            for(auto& image:v)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            if(XR_FAILED(xr.enumerate(chains[i],count,&count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(v.data()))))return false;
        }
        return true;
    }
    bool upload(const std::array<ComPtr<ID3D12Resource>,2>& sources,
        std::array<XrSwapchain,2>& handles,
        std::array<std::vector<XrSwapchainImageD3D12KHR>,2>& images,
        std::array<unsigned,2>& indices) noexcept {
        if(!chains(handles,images,sources))return false;
        std::array<bool,2> acquired{};
        for(unsigned i=0;i<2;++i){
            XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if(XR_FAILED(xr.acquire(handles[i],&info,&indices[i])))break;
            acquired[i]=true;
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wi.timeout=XR_INFINITE_DURATION;
            if(XR_FAILED(xr.wait_image(handles[i],&wi)))break;
            if(indices[i]>=images[i].size()||!images[i][indices[i]].texture)break;
            if(!begin_copy())break;
            auto* out=images[i][indices[i]].texture;auto* src=sources[i].Get();
            if(!compatible_color(out->GetDesc().Format,src->GetDesc().Format))break;
            color_transition(list.Get(),src,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            color_transition(list.Get(),out,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(out,src);
            color_transition(list.Get(),out,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
            color_transition(list.Get(),src,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
            if(!end_copy()||!wait_copy())break;
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if(XR_FAILED(xr.release(handles[i],&ri)))break;
            acquired[i]=false;
            if(i==1)return true;
        }
        // Close any unclosed list before reuse and release acquired images.
        if(list)list->Close();
        for(unsigned j=0;j<2;++j)if(acquired[j]){
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xr.release(handles[j],&ri);
        }
        return false;
    }
    XrResult submit_original(const XrFrameEndInfo* upstream,XrTime timestamp) noexcept {
        if(upstream)return xr.end_frame(session,upstream);
        auto layers=frame_layers.original();
        XrFrameEndInfo original{XR_TYPE_FRAME_END_INFO};original.displayTime=timestamp;
        original.environmentBlendMode=blend;original.layerCount=static_cast<unsigned>(layers.size());original.layers=layers.data();
        return xr.end_frame(session,&original);
    }
    XrResult submit(const XrFrameEndInfo* upstream,XrTime timestamp,
                    const std::array<ComPtr<ID3D12Resource>,2>* buffers,
                    const std::array<XrCompositionLayerProjectionView,2>* views,
                    bool is_generated,bool* used_private=nullptr) noexcept {
        if(used_private)*used_private=false;
        if(!buffers||!views){XrFrameEndInfo blank{XR_TYPE_FRAME_END_INFO};
            blank.displayTime=timestamp;blank.environmentBlendMode=upstream?upstream->environmentBlendMode:blend;
            return xr.end_frame(session,&blank);
        }
        std::array<unsigned,2> indices{};
        auto& handles=is_generated?fg_chains:real_chains;
        auto& images=is_generated?fg_images:real_images;
        if(!upload(*buffers,handles,images,indices)) {
            failure("private image upload",XR_ERROR_RUNTIME_FAILURE);
            state=CheekyFgState::presentation_failed;
            return submit_original(upstream,timestamp);
        }
        std::array<XrCompositionLayerProjectionView,2> presentation=*views;
        for(unsigned i=0;i<2;++i){
            presentation[i].type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            presentation[i].next=nullptr;
            presentation[i].subImage.swapchain=handles[i];
            presentation[i].subImage.imageArrayIndex=0;
            presentation[i].subImage.imageRect.offset={0,0};
            auto d=(*buffers)[i]->GetDesc();
            presentation[i].subImage.imageRect.extent={static_cast<int>(d.Width),static_cast<int>(d.Height)};
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space=space;projection.viewCount=2;projection.views=presentation.data();
        projection.layerFlags=frame_layers.scene_flags;
        auto layers=frame_layers.replace_scene(projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=timestamp;
        end.environmentBlendMode=blend;end.layerCount=static_cast<unsigned>(layers.size());end.layers=layers.data();
        const auto result=xr.end_frame(session,&end);
        if(XR_FAILED(result)) {
            failure("private xrEndFrame",result);state=CheekyFgState::presentation_failed;
            return submit_original(upstream,timestamp);
        }
        if(used_private)*used_private=true;
        return result;
    }
    virtual XrResult begin_frame(const XrFrameBeginInfo* info) noexcept {return xr.begin_frame(session,info);}
    virtual XrResult end_frame(const XrFrameEndInfo* end,
        const std::array<unsigned,2>& released_indices,
        const XrCompositionLayerProjection* projection) noexcept {
        std::lock_guard lock(mx);
        CommitWait commit_copy_wait{copy_wait,copy_wait_accumulator};
        if(worker.failed) {
            ++native_count;state=worker.capability_ready?CheekyFgState::generation_failed:CheekyFgState::ngx_unavailable;
            return xr.end_frame(session,end);
        }
        if(!projection||projection->viewCount!=2||!projection->views||!frame_layers.prepare(end,projection)) {
            reject_frame(end);return xr.end_frame(session,end);
        }
        ++native_count;++frames;
        log_layout(end,true);
        std::array<ComPtr<ID3D12Resource>,2> colors{};
        for(unsigned i=0;i<2;++i){
            const auto& sub=projection->views[i].subImage;
            colors[i]=subrect(sub.swapchain,released_indices[i],sub.imageRect,sub.imageArrayIndex);
            if(!colors[i]){
                invalidate_temporal();state=CheekyFgState::unsupported_submission;
                log_layout(end,false);return xr.end_frame(session,end);
            }
        }
        copies.clear();
        const auto old_space=space;
        const auto previous_held=held;
        auto previous_views=held_views;
        auto have_previous=held_valid;
        held=colors;for(unsigned i=0;i<2;++i)held_views[i]=projection->views[i];
        blend=end->environmentBlendMode;space=projection->space;held_valid=true;
        generated_valid=false;fallback=previous_held;fallback_views=previous_views;
        // Keep the application's native scene until interpolation has actually
        // succeeded. Missing history/guides/support must never blank the view.
        if(!have_previous||old_space!=space){
            state=CheekyFgState::waiting_for_history;
            return xr.end_frame(session,end);
        }
        auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
        auto acquire=runtime?reinterpret_cast<CheekyFgAcquireFn>(GetProcAddress(runtime,"CheekyFG_AcquirePair")):nullptr;
        auto release=runtime?reinterpret_cast<CheekyFgReleaseFn>(GetProcAddress(runtime,"CheekyFG_ReleasePair")):nullptr;
        CheekyFgPair guides{};
        state=CheekyFgState::waiting_for_guides;
        if(!acquire||!release||!acquire(&guides))return xr.end_frame(session,end);
        auto same_queue=guides.producer_queue==queue.Get();
        state=CheekyFgState::queue_mismatch;
        if(same_queue){
            std::array<Camera,2> cameras{};
            for(unsigned i=0;i<2;++i)cameras[i]=camera(held_views[i],&previous_views[i],guides.eyes[i].depth_inverted!=0);
            auto d=colors[0]->GetDesc();
            state=CheekyFgState::ngx_unavailable;
            const bool log_ngx=trace&&!ngx_logged;
            if(log_ngx) {trace("DLSS-FG NGX initialization starting");ngx_logged=true;}
            const bool initialized=worker.initialize(device.Get(),queue.Get(),static_cast<unsigned>(d.Width),d.Height,d.Format,cheeky_fg_needs_ngx(guides));
            if(log_ngx)trace(initialized?"DLSS-FG NGX initialized":"DLSS-FG NGX initialization unavailable");
            if(initialized){
                std::array<ID3D12Resource*,2> input{colors[0].Get(),colors[1].Get()};
                std::array<ID3D12Resource*,2> output{};
                std::array<ID3D12Resource*,2> prev{previous_held[0].Get(),previous_held[1].Get()};
                state=CheekyFgState::generation_failed;
                const bool log_generate=trace&&!generation_logged;
                if(log_generate) {trace("DLSS-FG first generation starting");generation_logged=true;}
                const bool valid_cameras=std::all_of(cameras.begin(),cameras.end(),[](const auto& c){return c.valid;});
                if(!valid_cameras){worker.wait_idle();worker.reset_features();}
                const bool success=valid_cameras&&worker.generate(guides,input,prev,cameras,output);
                if(log_generate)trace(success?"DLSS-FG first generation succeeded":"DLSS-FG first generation failed");
                if(success){
                    for(unsigned i=0;i<2;++i){
                        generated[i]=output[i];generated_views[i]=held_views[i];
                        const auto& prev=previous_views[i].pose;const auto& cur=held_views[i].pose;
                        for(unsigned k=0;k<3;++k){
                            float a=(&prev.position.x)[k],b=(&cur.position.x)[k];
                            (&generated_views[i].pose.position.x)[k]=(a+b)*0.5f;
                        }
                        auto a=prev.orientation,b=cur.orientation;
                        float dot=a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;
                        if(dot<0){b.x=-b.x;b.y=-b.y;b.z=-b.z;b.w=-b.w;}
                        auto& o=generated_views[i].pose.orientation;
                        o={a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w};
                        float inv=1.0f/std::sqrt(o.x*o.x+o.y*o.y+o.z*o.z+o.w*o.w);
                        o.x*=inv;o.y*=inv;o.z*=inv;o.w*=inv;
                        const float *pf=&previous_views[i].fov.angleLeft,*cf=&held_views[i].fov.angleLeft;
                        float *of=&generated_views[i].fov.angleLeft;
                        for(unsigned k=0;k<4;++k)of[k]=0.5f*(pf[k]+cf[k]);
                    }
                    generated_valid=true;
                    state=CheekyFgState::generating;
                }
            }
        }
        release(&guides);
        if(!generated_valid)return xr.end_frame(session,end);
        bool used_private{};
        const auto submitted=submit(end,end->displayTime,&previous_held,&previous_views,false,&used_private);
        if(!used_private)generated_valid=false;
        if(XR_FAILED(submitted))invalidate_temporal();
        return submitted;
    }
    virtual XrResult wait_frame(const XrFrameWaitInfo* info,XrFrameState* state) noexcept {
        std::lock_guard lock(mx);
        double waiting{};
        CommitWait commit_xr_wait{xr_wait,waiting};
        auto wait=[&](const XrFrameWaitInfo* input,XrFrameState* output) {
            AddElapsed elapsed{waiting};return xr.wait_frame(session,input,output);
        };
        bool extra_presented{};
        if(generated_valid){
            XrFrameWaitInfo extra{XR_TYPE_FRAME_WAIT_INFO};XrFrameState extra_state{XR_TYPE_FRAME_STATE};
            auto r=wait(&extra,&extra_state);if(XR_FAILED(r))return r;
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};r=xr.begin_frame(session,&begin);if(XR_FAILED(r))return r;
            const auto* buf=&generated;
            const auto* views=&generated_views;
            if(extra_state.shouldRender!=XR_TRUE)buf=nullptr,views=nullptr;
            bool used_private{};
            r=submit(nullptr,extra_state.predictedDisplayTime,buf,views,true,&used_private);
            if(XR_FAILED(r))return r;
            extra_presented=true;
            if(used_private)++generated_count;
            else if(extra_state.shouldRender==XR_TRUE)++fallback_count;
            if((frames%120)==0){
                char msg[180]{};
                sprintf_s(msg,"Cheeky FG: native=%llu generated=%llu fallback=%llu\n",
                    static_cast<unsigned long long>(frames),
                    static_cast<unsigned long long>(generated_count),
                    static_cast<unsigned long long>(fallback_count));
                OutputDebugStringA(msg);
            }
            generated={};generated_valid=false;
        }
        const auto result=wait(info,state);
        if(XR_SUCCEEDED(result)&&state)xr_period_ms=double(state->predictedDisplayPeriod)/1000000.0;
        if(XR_SUCCEEDED(result)&&state&&extra_presented&&state->predictedDisplayPeriod>0 &&
           state->predictedDisplayPeriod<=INT64_MAX/2)
            state->predictedDisplayPeriod*=2;
        return result;
    }
};
struct AppFrameCallTimer {
    std::shared_ptr<Presenter>& presenter;unsigned stage;
    TimingClock::time_point start{TimingClock::now()};
    ~AppFrameCallTimer(){if(presenter)presenter->note_app_call(stage,elapsed_ms(start));}
};
}
