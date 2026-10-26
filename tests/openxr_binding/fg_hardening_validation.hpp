#pragma once
#include <atomic>
#include <thread>
#include <variant>
#include <unordered_set>

// Invalid inputs must be rejected before they can advance FG's camera history.
void fg_camera_validation() {
    using namespace cheeky::fg;
    const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
    Matrix inverse_result;
    for(unsigned i=0;i<16;++i)for(float bad:{nan,inf,-inf}){
        auto m=identity();m.m[i]=bad;
        check(!inverse(m,inverse_result),"Matrix inversion rejects every non-finite input element");
    }
    check(!inverse(Matrix{},inverse_result),"Singular matrix is rejected");
    auto scaled=identity();scaled.m[0]=.25f;scaled.m[5]=4.f;
    check(inverse(scaled,inverse_result)&&inverse_result.m[0]==4.f&&inverse_result.m[5]==.25f,"Finite invertible matrices remain usable");
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose.orientation.w=1;view.fov={-.7f,.6f,.65f,-.55f};
    for(bool inverted:{false,true}){
        check(camera(view,nullptr,inverted).valid&&camera(view,&view,inverted).valid,"Valid asymmetric camera remains usable at either depth convention");
        auto rotation=view;rotation.pose.orientation.w=2;
        check(camera(rotation,&view,inverted).valid,"Finite non-unit quaternions remain normalized by camera math");
        for(bool previous:{false,true}){
            auto reject=[&](const XrCompositionLayerProjectionView& bad){
                check(!(previous?camera(view,&bad,inverted):camera(bad,&view,inverted)).valid,"Invalid current or previous camera is rejected");
            };
            for(float bad:{nan,inf,-inf}){
                for(unsigned axis=0;axis<3;++axis){auto broken=view;
                    if(axis==0)broken.pose.position.x=bad;else if(axis==1)broken.pose.position.y=bad;else broken.pose.position.z=bad;reject(broken);}
                for(unsigned axis=0;axis<4;++axis){auto broken=view;
                    if(axis==0)broken.pose.orientation.x=bad;else if(axis==1)broken.pose.orientation.y=bad;
                    else if(axis==2)broken.pose.orientation.z=bad;else broken.pose.orientation.w=bad;reject(broken);}
                for(unsigned edge=0;edge<4;++edge){auto broken=view;
                    if(edge==0)broken.fov.angleLeft=bad;else if(edge==1)broken.fov.angleRight=bad;
                    else if(edge==2)broken.fov.angleUp=bad;else broken.fov.angleDown=bad;reject(broken);}
            }
            auto broken=view;broken.pose.orientation={0,0,0,0};reject(broken);
            broken=view;broken.fov.angleRight=broken.fov.angleLeft;reject(broken);
            broken=view;broken.fov.angleUp=broken.fov.angleDown;reject(broken);
            broken=view;std::swap(broken.fov.angleRight,broken.fov.angleLeft);reject(broken);
            broken=view;broken.pose.orientation.x=std::numeric_limits<float>::max();reject(broken);
        }
    }
    std::cout<<"PASS FG camera validation: NaN/infinity, singular matrices, current/previous poses, rotations, FOV and both depth conventions\n";
}

namespace fg_completion_fixture {
struct Parameters final : NVSDK_NGX_Parameter {
    using Value=std::variant<unsigned long long,float,double,unsigned,int,ID3D11Resource*,ID3D12Resource*,void*>;
    std::unordered_map<std::string,Value> values;
#define FG_COMPLETION_PARAMETER(T) \
    void Set(const char* key,T value) override {values.insert_or_assign(key,value);} \
    NVSDK_NGX_Result Get(const char* key,T* out) const override { \
        const auto it=values.find(key);if(it==values.end()||!out)return NVSDK_NGX_Result_FAIL_InvalidParameter; \
        const auto* value=std::get_if<T>(&it->second);if(!value)return NVSDK_NGX_Result_FAIL_InvalidParameter; \
        *out=*value;return NVSDK_NGX_Result_Success;}
    FG_COMPLETION_PARAMETER(unsigned long long)
    FG_COMPLETION_PARAMETER(float)
    FG_COMPLETION_PARAMETER(double)
    FG_COMPLETION_PARAMETER(unsigned)
    FG_COMPLETION_PARAMETER(int)
    FG_COMPLETION_PARAMETER(ID3D11Resource*)
    FG_COMPLETION_PARAMETER(ID3D12Resource*)
    FG_COMPLETION_PARAMETER(void*)
#undef FG_COMPLETION_PARAMETER
    void Reset() override {values.clear();}
};
inline Microsoft::WRL::ComPtr<ID3D12Resource> flag_upload;
inline unsigned calls{},fail_at{},created{};
inline std::vector<unsigned> reset_flags;
inline std::vector<std::pair<unsigned,unsigned>> frame_indices;
inline bool suppress_second_only{};
NVSDK_NGX_Result NVSDK_CONV allocate(NVSDK_NGX_Parameter** out){*out=new Parameters;return NVSDK_NGX_Result_Success;}
NVSDK_NGX_Result NVSDK_CONV destroy(NVSDK_NGX_Parameter* p){delete static_cast<Parameters*>(p);return NVSDK_NGX_Result_Success;}
NVSDK_NGX_Result NVSDK_CONV create(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle** out){
    *out=reinterpret_cast<NVSDK_NGX_Handle*>(new unsigned(++created));return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV release(NVSDK_NGX_Handle* h){delete reinterpret_cast<unsigned*>(h);return NVSDK_NGX_Result_Success;}
NVSDK_NGX_Result NVSDK_CONV evaluate(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter* p,PFN_NVSDK_NGX_ProgressCallback){
    unsigned count{},index{};
    check(NVSDK_NGX_SUCCEED(p->Get("DLSSG.MultiFrameCount",&count))&&NVSDK_NGX_SUCCEED(p->Get("DLSSG.MultiFrameIndex",&index)),"NGX receives multi-frame parameters");
    frame_indices.emplace_back(count,index);
    unsigned reset{};check(NVSDK_NGX_SUCCEED(p->Get("DLSSG.Reset",&reset)),"Fixture receives actual NGX reset policy");reset_flags.push_back(reset);
    if(++calls==fail_at)return NVSDK_NGX_Result_FAIL_InvalidParameter;
    ID3D12Resource* disable{};check(NVSDK_NGX_SUCCEED(p->Get("DLSSG.OutputDisableInterpolation",&disable))&&disable,"Fixture receives disable-output buffer");
    cheeky::fg::color_transition(list,disable,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(disable,0,flag_upload.Get(),suppress_second_only&&(calls%2)?4:0,4);
    cheeky::fg::color_transition(list,disable,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return NVSDK_NGX_Result_Success;
}
}

// Real worker, command recording, queue submission and readback on WARP. Only
// the NGX backend is replaced, so failed and suppressed evaluations are deterministic.
void fg_completion_validation(ID3D12Device* device,ID3D12CommandQueue* queue) {
    using namespace cheeky::fg;using namespace fg_completion_fixture;using Microsoft::WRL::ComPtr;
    calls=fail_at=created=0;suppress_second_only=false;reset_flags.clear();
    Worker worker;worker.probe_attempted=true;worker.capability_ready=true;worker.probed_device=device;
    check(worker.initialize(device,queue,64,64,DXGI_FORMAT_R8G8B8A8_UNORM),"Initialize completion fixture without NVIDIA inference");
    worker.allocate=allocate;worker.destroy=destroy;worker.create=create;worker.release=release;worker.evaluate=evaluate;
    auto buffer=[&](D3D12_HEAP_TYPE heap_type,UINT64 bytes){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=heap_type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,heap_type==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&result))),"Completion upload buffer");return result;
    };
    flag_upload=buffer(D3D12_HEAP_TYPE_UPLOAD,8);
    auto flag=[&](unsigned value){void* data{};check(SUCCEEDED(flag_upload->Map(0,nullptr,&data)),"Map completion flag");
        std::memset(data,0,8);std::memcpy(data,&value,4);flag_upload->Unmap(0,nullptr);};flag(1);
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"Completion fixture initialization commands");
    ComPtr<ID3D12DescriptorHeap> rtvs;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=3;
    check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs))),"Completion RTV heap");
    const auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);
    auto texture=[&](DXGI_FORMAT format,unsigned index,const std::array<float,4>& value,unsigned size=64){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=size;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
        d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;ComPtr<ID3D12Resource> result;
        check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&result))),"Completion fixture texture");
        auto slot=rtvs->GetCPUDescriptorHandleForHeapStart();slot.ptr+=index*stride;device->CreateRenderTargetView(result.Get(),nullptr,slot);
        list->ClearRenderTargetView(slot,value.data(),0,nullptr);color_transition(list.Get(),result.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);return result;
    };
    auto color=texture(DXGI_FORMAT_R8G8B8A8_UNORM,0,{1,0,0,1}),depth=texture(DXGI_FORMAT_R32_FLOAT,1,{.5f,0,0,0}),motion=texture(DXGI_FORMAT_R32G32_FLOAT,2,{0,0,0,0});
    check(SUCCEEDED(list->Close()),"Close completion initialization");ID3D12CommandList* initial[]{list.Get()};queue->ExecuteCommandLists(1,initial);
    ComPtr<ID3D12Fence> initialized;check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&initialized)))&&SUCCEEDED(queue->Signal(initialized.Get(),1)),"Completion initialization fence");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    check(event&&SUCCEEDED(initialized->SetEventOnCompletion(1,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"Wait completion initialization");CloseHandle(event);
    CheekyFgPair guides;guides.producer_queue=queue;
    for(auto& g:guides.eyes){g.depth=depth.Get();g.motion=motion.Get();g.width=g.height=g.depth_width=g.depth_height=g.motion_width=g.motion_height=64;g.configuration=1;}
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};view.pose.orientation.w=1;view.fov={-.7f,.7f,.7f,-.7f};
    const std::array<Camera,2> cameras{camera(view,&view),camera(view,&view)};
    const std::array<ID3D12Resource*,2> colors{color.Get(),color.Get()};std::array<ID3D12Resource*,2> output{};unsigned submissions{};
    auto generate=[&](const std::array<Camera,2>& input,std::array<Worker::OutputCopy,2>* copies=nullptr){
        ++guides.render_frame;
        return worker.generate(guides,colors,colors,input,output,copies,[](void* p) noexcept {++*static_cast<unsigned*>(p);},&submissions);
    };
    check(!generate(cameras)&&worker.temporal_evaluation_completed&&!worker.failed&&submissions==1,"Suppressed NGX output is a completed submitted evaluation");
    check(!generate(cameras)&&worker.temporal_evaluation_completed&&!worker.failed&&created==2&&submissions==2,"Repeated warm-up preserves feature history");
    check(reset_flags.size()==4&&reset_flags[2]==0&&reset_flags[3]==0,"Warm-up is not trapped in repeated history resets");
    flag(0);check(generate(cameras)&&worker.temporal_evaluation_completed&&!worker.failed,"NGX resumes interpolation after warm-up");
    const auto serial=worker.serial,accepted_frame=worker.features[0].evaluated_frame;const auto submitted=submissions;
    auto broken=cameras;broken[0].valid=false;const auto calls_before=calls;
    check(!generate(broken)&&!worker.temporal_evaluation_completed&&!worker.failed&&calls==calls_before&&worker.serial==serial,"Input rejection clears stale completion without evaluating or submitting");
    broken=cameras;broken[unsigned(worker.gpu_timings.samples&1)^1u].view_to_clip.m[0]=std::numeric_limits<float>::quiet_NaN();
    check(!generate(broken)&&!worker.temporal_evaluation_completed&&!worker.failed&&worker.serial==serial&&submissions==submitted&&calls==calls_before+1,
        "Invalid second-eye projection discards the first eye's recorded NGX commands");
    check(worker.features[0].evaluated_frame==accepted_frame,"Rejected camera pair cannot advance submitted crop history");
    fail_at=calls+2;
    check(!generate(cameras)&&!worker.temporal_evaluation_completed&&worker.failed&&worker.serial==serial&&submissions==submitted,"Second-eye NGX failure discards the entire recorded batch");
    fail_at=0;for(auto& g:guides.eyes)++g.configuration;
    check(generate(cameras)&&worker.temporal_evaluation_completed&&!worker.failed,"Validated source change recovers after evaluation failure");
    const auto before_copy=worker.serial;const auto before_copy_calls=calls;
    // An incompatible-format publication target fails after both NGX calls,
    // before ExecuteCommandLists. No recorded evaluation can advance history.
    std::array<Worker::OutputCopy,2> copies{{{depth.Get()},{motion.Get()}}};
    check(!generate(cameras,&copies)&&!worker.temporal_evaluation_completed&&worker.failed&&worker.serial==before_copy&&calls==before_copy_calls+2,
        "Failed final publication discards recorded evaluations and clears completion");
    for(auto& g:guides.eyes)++g.configuration;
    check(generate(cameras)&&worker.temporal_evaluation_completed&&!worker.failed,"Recover after discarded publication");
    worker.reset_features();check(!worker.temporal_evaluation_completed,"Feature reset invalidates completion state");
    check(generate(cameras)&&worker.temporal_evaluation_completed,"Generation resumes after resetting features");
    for(unsigned count:{2u,3u}){
        const auto before=calls;worker.max_generated_frames=1;
        check(!worker.generate(guides,colors,colors,cameras,output,nullptr,nullptr,nullptr,nullptr,count,1)&&calls==before&&!worker.failed,
            "Unsupported DLSS multiplier rejects before evaluation without a sticky failure");
        worker.max_generated_frames=3;++guides.render_frame;
        const auto previous_frame=worker.features[0].evaluated_frame;
        for(unsigned index=1;index<=count;++index){
            frame_indices.clear();
            check(worker.generate(guides,colors,colors,cameras,output,nullptr,nullptr,nullptr,nullptr,count,index),"NGX multi-frame indexed evaluation");
            check(frame_indices.size()==2&&frame_indices[0]==std::pair{count,index}&&frame_indices[1]==std::pair{count,index},
                "Each eye receives the same count and successive index");
            check(worker.features[0].evaluated_frame==(index==count?guides.render_frame:previous_frame),
                "Crop history advances once per real frame after the last output");
        }
    }
    // Hold the real worker queue on a CPU-controlled fence. A DLSS phase must
    // hand over its immutable stereo outputs before the GPU can execute them.
    // Suppress only one eye: the queued GPU fallback must protect BOTH eyes.
    {
        std::array<Worker::OutputCopy,2> copies;
        std::array<ComPtr<ID3D12Resource>,2> publication;
        auto desc=color->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        for(unsigned eye=0;eye<2;++eye){
            check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,
                IID_PPV_ARGS(&publication[eye]))),"Early NGX publication texture");
            copies[eye].texture=publication[eye].Get();copies[eye].layout.width=copies[eye].layout.height=64;
        }
        ComPtr<ID3D12CommandQueue> consumer;D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&consumer))),"Early NGX consumer queue");
        ComPtr<ID3D12Fence> gate,consumed;
        check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)))&&
            SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&consumed))),"Early NGX handoff fences");
        flag(1);suppress_second_only=true;
        check(SUCCEEDED(queue->Wait(gate.Get(),1)),"Block NGX GPU work until early handoff");
        HANDLE handed_over=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        check(handed_over!=nullptr,"Early handoff watchdog event");std::atomic<bool> handoff_timed_out{};
        std::jthread handoff_watchdog([&]{if(WaitForSingleObject(handed_over,5000)!=WAIT_OBJECT_0){
            handoff_timed_out=true;gate->Signal(1);}});
        unsigned handoffs{};
        auto handoff=[&]() noexcept {
            ++handoffs;
            check(worker.fence->GetCompletedValue()<worker.serial&&worker.submitted_at!=TimingClock::time_point{},
                "DLSS stereo handoff occurs after submission but before GPU completion");
            for(const auto& copy:copies)check(copy.state==D3D12_RESOURCE_STATE_GENERIC_READ,"Handoff publishes final resource states");
            check(SUCCEEDED(consumer->Wait(worker.fence.Get(),worker.serial))&&SUCCEEDED(consumer->Signal(consumed.Get(),1)),
                "Consumer queues a GPU dependency without CPU completion");
            check(consumed->GetCompletedValue()==0,"Consumer cannot overtake the incomplete stereo output");
            SetEvent(handed_over);
            check(SUCCEEDED(gate->Signal(1)),"Release the fixture GPU after checking the handoff");
        };
        ++guides.render_frame;
        check(worker.generate(guides,colors,colors,cameras,output,&copies,nullptr,nullptr,nullptr,1,1,
            +[](void* context) noexcept {(*static_cast<decltype(handoff)*>(context))();},&handoff)&&handoffs==1,
            "Queued DLSS phase succeeds with a GPU native fallback for suppressed interpolation");
        handoff_watchdog.join();CloseHandle(handed_over);
        check(!handoff_timed_out,"DLSS publication did not wait for GPU completion");
        worker.wait_idle();suppress_second_only=false;flag(0);
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        check(event&&SUCCEEDED(consumed->SetEventOnCompletion(1,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0,
            "Consumer retires only after the stereo ready fence");CloseHandle(event);
        check(SUCCEEDED(allocator->Reset())&&SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"Queued fallback readback commands");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        std::array<ComPtr<ID3D12Resource>,2> readbacks;
        for(unsigned eye=0;eye<2;++eye){
            readbacks[eye]=buffer(D3D12_HEAP_TYPE_READBACK,bytes);
            color_transition(list.Get(),publication[eye].Get(),D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=publication[eye].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readbacks[eye].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        }
        check(SUCCEEDED(list->Close()),"Close queued fallback readback");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        check(SUCCEEDED(queue->Signal(consumed.Get(),2)),"Queued fallback readback fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        check(event&&SUCCEEDED(consumed->SetEventOnCompletion(2,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0,
            "Retire queued fallback readback");CloseHandle(event);
        for(const auto& readback:readbacks){void* data{};check(SUCCEEDED(readback->Map(0,nullptr,&data)),"Map native fallback pixels");
            const auto* pixel=static_cast<const unsigned char*>(data)+footprint.Offset+32*footprint.Footprint.RowPitch+32*4;
            check(pixel[0]==255&&pixel[1]==0&&pixel[2]==0,"One suppressed eye selects actual native pixels for both eyes");
            readback->Unmap(0,nullptr);
        }
    }
    // Keep the GPU blocked until the LAST index has been queued. Any CPU
    // completion wait between indices would deadlock before that callback.
    for(unsigned method:{0u,1u,2u})for(unsigned release:{0u,1u})for(unsigned count:{2u,3u}){
        for(auto& g:guides.eyes)g.center_method=method;
        guides.release_point=release;
        std::array<std::array<ComPtr<ID3D12Resource>,2>,3> images;
        std::array<std::array<Worker::OutputCopy,2>,3> copies;
        auto desc=color->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        for(unsigned phase=0;phase<count;++phase)for(unsigned eye=0;eye<2;++eye){
            check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,
                IID_PPV_ARGS(&images[phase][eye]))),"Queued multi-frame publication texture");
            copies[phase][eye].texture=images[phase][eye].Get();copies[phase][eye].layout.width=copies[phase][eye].layout.height=64;
        }
        ComPtr<ID3D12Fence> gate;
        check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)))&&SUCCEEDED(queue->Wait(gate.Get(),1)),
            "Block multi-frame GPU work until all indices are submitted");
        HANDLE all_queued=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        check(all_queued!=nullptr,"Queued NGX watchdog event");std::atomic<bool> queue_timed_out{};
        std::jthread queue_watchdog([&]{if(WaitForSingleObject(all_queued,5000)!=WAIT_OBJECT_0){
            queue_timed_out=true;gate->Signal(1);}});
        const auto created_before=created;
        const auto submitted_before=worker.serial;
        const auto handles=std::array{worker.features[0].handle,worker.features[1].handle};
        unsigned handoffs{};frame_indices.clear();++guides.render_frame;
        const auto previous_frame=worker.features[0].evaluated_frame;
        for(unsigned index=1;index<=count;++index){
            auto handoff=[&]() noexcept {
                ++handoffs;
                check(worker.fence->GetCompletedValue()<=submitted_before&&worker.serial==submitted_before+index*(release?2u:1u),
                    "Every generated phase queues its stereo fence while earlier GPU work is still blocked");
                if(index==count){SetEvent(all_queued);check(SUCCEEDED(gate->Signal(1)),"Release GPU only after the last indexed submission");}
            };
            check(worker.generate(guides,colors,colors,cameras,output,&copies[index-1],nullptr,nullptr,nullptr,count,index,
                +[](void* context) noexcept {(*static_cast<decltype(handoff)*>(context))();},&handoff),
                "Queue consecutive indices without an intervening GPU-to-CPU completion wait");
            check(worker.features[0].evaluated_frame==(index==count?guides.render_frame:previous_frame),
                "Queued NGX advances temporal crop history once at the final index");
            if(index<count)check(worker.fence->GetCompletedValue()<=submitted_before,
                "An early index returns to the CPU while the GPU remains blocked");
        }
        queue_watchdog.join();CloseHandle(all_queued);
        check(!queue_timed_out,"Every index queues without waiting for earlier GPU execution");
        check(handoffs==count&&created==created_before&&frame_indices.size()==(method?0u:2*count),
            "Every phase publishes once without creating additional temporal handles");
        if(!method)for(unsigned phase=0;phase<count;++phase)for(unsigned eye=0;eye<2;++eye){
            check(frame_indices[phase*2+eye]==std::pair{count,phase+1},"Queued NGX preserves multi-frame index ordering");
            if(phase){const auto& binding=worker.ngx_phases[phase-1].features[eye];
                check(binding.borrowed_handle&&binding.handle==handles[eye]&&binding.params!=worker.features[eye].params&&
                    binding.disable.Get()!=worker.features[eye].disable.Get()&&
                    worker.ngx_phases[phase-1].focus.inputs[eye][0].color.Get()!=worker.focus.inputs[eye][0].color.Get(),
                    "Queued indices share temporal history with independent parameters, suppression and prepared inputs");
            }
        }
        check(worker.batch_timings.snapshot().generated_count==count&&worker.batch_timings.snapshot().valid,
            "Final indexed fence retires all phase timing queries");
    }
    guides.release_point=0;
    worker.reset();check(!worker.temporal_evaluation_completed,"Worker reset clears completion state");flag_upload.Reset();
    worker.probe_attempted=true;worker.capability_ready=false;worker.failed=true;
    check(worker.initialize(device,queue,64,64,DXGI_FORMAT_R8G8B8A8_UNORM,false),"Shader-only D3D12 FG bypasses unavailable NGX capability");
    worker.create=nullptr;worker.evaluate=nullptr;worker.allocate=nullptr;
    for(unsigned method:{1u,2u})for(bool focused:{false,true}){
        for(auto& g:guides.eyes){g.center_method=method;g.peripheral_method=0;g.focus_width=g.focus_height=focused?.5f:1.f;++g.configuration;}
        check(!cheeky_fg_needs_ngx(guides),"Shader regions need no NGX initialization");
        check(generate(cameras)&&generate(cameras)&&worker.temporal_evaluation_completed,"Production worker interpolates both shader center methods without any NGX callbacks");
        check(!worker.features[0].handle&&!worker.features[1].handle,"Shader center creates no DLSS-G feature");
    }
    check(SUCCEEDED(allocator->Reset())&&SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"Shader center readback commands");
    const auto shader_desc=output[0]->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT shader_footprint{};UINT64 shader_bytes{};
    device->GetCopyableFootprints(&shader_desc,0,1,0,&shader_footprint,nullptr,nullptr,&shader_bytes);
    auto shader_readback=buffer(D3D12_HEAP_TYPE_READBACK,shader_bytes);
    color_transition(list.Get(),output[0],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION shader_from{},shader_to{};shader_from.pResource=output[0];shader_from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    shader_to.pResource=shader_readback.Get();shader_to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;shader_to.PlacedFootprint=shader_footprint;
    list->CopyTextureRegion(&shader_to,0,0,0,&shader_from,nullptr);
    color_transition(list.Get(),output[0],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    check(SUCCEEDED(list->Close()),"Close shader center readback");ID3D12CommandList* shader_lists[]{list.Get()};queue->ExecuteCommandLists(1,shader_lists);
    check(SUCCEEDED(queue->Signal(initialized.Get(),2)),"Shader center readback fence");event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    check(event&&SUCCEEDED(initialized->SetEventOnCompletion(2,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"Wait shader center pixels");CloseHandle(event);
    void* shader_data{};check(SUCCEEDED(shader_readback->Map(0,nullptr,&shader_data)),"Map shader center output");
    const auto* shader_pixel=static_cast<const unsigned char*>(shader_data)+shader_footprint.Offset+32*shader_footprint.Footprint.RowPitch+32*4;
    check(shader_pixel[0]==255&&shader_pixel[1]==0&&shader_pixel[2]==0,"D3D12 cropped shader center produces actual source pixels without NGX");
    shader_readback->Unmap(0,nullptr);
    worker.reset();
    // Exercise the production D3D12 backend's complete batch and output leases.
    for(unsigned multiplier:{3u,4u}){
        BufferedPresenter presenter;
        check(presenter.setup(h<XrSession>(2),buffered_probe::Runtime::functions(),device,queue),"D3D12 multi-frame backend setup");
        auto previous=std::make_shared<BufferedNative>(),current=std::make_shared<BufferedNative>();
        previous->id=1;current->id=2;previous->guide_token=2;current->guide_token=4;
        previous->source_configuration=current->source_configuration=guides.eyes[0].configuration;
        previous->views=current->views={view,view};current->multiplier=multiplier;current->policy.queue_priority=2;
        for(unsigned eye=0;eye<2;++eye){previous->color[eye]=current->color[eye]=std::make_shared<TransportImage>();
            current->color[eye]->texture=color;current->color[eye]->state=D3D12_RESOURCE_STATE_GENERIC_READ;}
        auto payload=std::make_shared<FrameGuides12>();payload->retain(guides);current->guides=payload;current->guides_valid=true;
        BufferedPair pair;pair.native_id=pair.current_id=current->id;pair.native=pair.current=current->color;
        pair.native_views=pair.current_views=current->views;
        bool reset=true;std::uint64_t logged{};std::shared_ptr<BufferedDirectRequest> direct;
        const auto generated=presenter.backend->generate(previous,current,pair,reset,logged,direct);
        check(generated.success&&generated.attempted&&pair.generated_count==multiplier-1,"D3D12 backend generates the complete indexed batch");
        std::unordered_set<ID3D12Resource*> outputs;
        for(unsigned index=0;index<pair.generated_count;++index)for(const auto& image:index?pair.extra_generated[index-1]:pair.generated){
            check(image&&image->state==D3D12_RESOURCE_STATE_GENERIC_READ&&outputs.insert(image->texture.Get()).second,
                "Every D3D12 output eye has its own retained publication image");}
        {
            std::lock_guard lock(presenter.mx);
            check(pair.progress&&presenter.ready.size()==1&&presenter.ready.front().progress==pair.progress,
                "DX12 publishes one shared interval while submitting its individual stereo phases");
            auto active=presenter.ready.front();
            check(active.native_id==previous->id&&active.current_id==current->id&&active.native==previous->color,
                "Early publication retains the previous native frame and current interval identity");
            for(unsigned index=0;index<pair.generated_count;++index){
                const auto& images=index?pair.extra_generated[index-1]:pair.generated;
                check(pair.progress->phases[index].images==images&&presenter.phase_ready(active,index),
                    "Each completed phase publishes both retained eyes with its own GPU fence");
            }
            struct Pending final : FrameCompletion {bool done{};bool ready() const noexcept override{return done;}};
            auto later=std::make_shared<Pending>();pair.progress->phases[1].completion=later;
            check(presenter.phase_ready(active,0)&&!presenter.phase_ready(active,1)&&
                presenter.fresh_content(active,true,true,1.),
                "G1 is presentable while G2 is pending, without waiting for the complete batch");
            active.generated_index=1;
            check(!presenter.fresh_content(active,true,true,1.25),"Pending G2 cannot be skipped for a later completed phase");
            later->done=true;auto right=pair.progress->phases[1].images[1];pair.progress->phases[1].images[1].reset();
            check(!presenter.phase_ready(active,1),"A generated phase requires both stereo eyes");
            pair.progress->phases[1].images[1]=right;
            check(presenter.fresh_content(active,true,true,1.25),"G2 becomes presentable at its own completion");
            presenter.inflight_id=current->id;presenter.submitted_native_id=current->id;
            presenter.inflight_release_point=6;presenter.awaiting_generated_submission=current->id;
            check(presenter.release_gate_active(),"First-generated submission policy initially holds the game");
            presenter.finish_generated_gate(current->id,true);
            check(!presenter.release_gate_active()&&!presenter.generated_submission_pending()&&presenter.inflight_id,
                "First generated submission releases the game while later phases remain in flight");
            presenter.discard_pending();
            check(!presenter.phase_ready(active,1)&&presenter.fresh_content(active,true,true,1.25),
                "Epoch cancellation retires a partial interval instead of stranding its presenter wait");
        }
        const auto batch=presenter.worker.batch_timings.snapshot();double sum{};
        for(unsigned index=0;index<multiplier-1;++index){
            check(batch.phase_last_ms[index]>0&&presenter.worker.phase_timings[index].samples==1,
                "Every generated stereo phase retains its own timing queries through batch retirement");
            sum+=batch.phase_last_ms[index];
        }
        check(batch.valid&&batch.generated_count==multiplier-1&&batch.samples==1&&batch.batch_last_ms+.01>=sum&&
            presenter.worker.gpu_timings.samples==multiplier-1,
            "Whole-batch GPU span contains all independent phase scopes and the legacy metric averages every phase");
        presenter.reset();
    }
    {
        Transport transport;check(transport.initialize(device,queue),"Application timing transport");
        ApplicationTimer12 timer;
        Sleep(20); // Deliberate pacing before the work bracket must be excluded.
        timer.begin(transport,true);Sleep(4);
        auto* c=transport.begin();check(c!=nullptr,"Application timing end packet");timer.end(c->list.Get());
        std::uint64_t ticket{};check(transport.execute(*c,ticket),"Submit application timing");timer.submitted(ticket);
        transport.drain();timer.begin(transport,false);
        const auto sample=timer.sample();check(sample.sequence&&sample.gpu_ms>0&&sample.cpu_ms>=3&&sample.cpu_ms<20,
            "D3D12 Auto measures GPU/CPU work and excludes the preceding pacing wait");
        timer.begin(transport,true);transport.drain();timer.reset(transport);
        check(!timer.sample().sequence,"Discard/reset retires an unfinished timestamp bracket safely");
    }
    {
        using Runtime=buffered_probe::Runtime;
        Runtime::device=device;Runtime::chains.clear();Runtime::direct_states.clear();Runtime::snapshots=nullptr;
        Runtime::owner=Runtime::phase=Runtime::next_chain=0;Runtime::capture_token=0;Runtime::errors=0;
        Runtime::waits=Runtime::begins=Runtime::ends=Runtime::generated=0;
        Runtime::paused=Runtime::reject_private=Runtime::block_generated_end=false;Runtime::upload_serial=nullptr;
        Runtime::guides=guides;Runtime::guides.release_point=3;Runtime::guides.fg_queue_priority=2;
        for(auto& g:Runtime::guides.eyes){g.center_method=2;g.peripheral_method=0;g.focus_width=g.focus_height=1;g.reset=0;}
        check(SUCCEEDED(allocator->Reset())&&SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"Auto native source state");
        color_transition(list.Get(),color.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
        check(SUCCEEDED(list->Close()),"Auto source state close");ID3D12CommandList* source_lists[]{list.Get()};queue->ExecuteCommandLists(1,source_lists);
        BufferedPresenter presenter;check(presenter.setup(h<XrSession>(2),Runtime::functions(),device,queue),"D3D12 Auto presenter setup");
        presenter.automatic_multiplier=true;presenter.guide_stamp=Runtime::stamp;
        presenter.guide_acquire=Runtime::acquire_guides;presenter.guide_release=Runtime::release_guides;
        std::array<XrCompositionLayerProjectionView,2> views{view,view},helper_views{view,view};
        for(unsigned eye=0;eye<2;++eye){views[eye].subImage={h<XrSwapchain>(200),{{0,0},{64,64}},0};helper_views[eye].subImage={h<XrSwapchain>(201),{{0,0},{4,4}},0};}
        XrCompositionLayerProjection scene{XR_TYPE_COMPOSITION_LAYER_PROJECTION},helper{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        scene.space=helper.space=h<XrSpace>(1);scene.views=views.data();helper.views=helper_views.data();scene.viewCount=helper.viewCount=2;
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};quad.space=scene.space;quad.pose.orientation.w=1;quad.size={1,1};
        quad.subImage={h<XrSwapchain>(201),{{0,0},{64,64}},0};
        const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&helper),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&scene),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
        const auto frames=[&](unsigned count,unsigned work){for(unsigned frame=0;frame<count;++frame){
            XrFrameState state{XR_TYPE_FRAME_STATE};check(XR_SUCCEEDED(presenter.wait_frame(nullptr,&state))&&XR_SUCCEEDED(presenter.begin_frame(nullptr)),"Auto D3D12 application frame");
            Sleep(work);Runtime::capture_token+=2;
            check(presenter.capture(h<XrSwapchain>(200),0,color.Get(),64,64,1,DXGI_FORMAT_R8G8B8A8_UNORM)&&
                presenter.capture(h<XrSwapchain>(201),0,color.Get(),64,64,1,DXGI_FORMAT_R8G8B8A8_UNORM),"Auto D3D12 capture");
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=state.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end.layerCount=3;end.layers=layers;check(XR_SUCCEEDED(presenter.end_frame(&end,{},&scene)),"Auto D3D12 handoff");
        }};
        frames(40,21);
        {std::lock_guard lock(presenter.mx);const auto status=presenter.automatic_status();
            check(status.valid&&status.switches&&status.selected>=3&&presenter.epoch==0&&presenter.history_reset_events<=1,
                "Real D3D12 Auto changes count under load while retaining shader color/depth history");}
        presenter.automatic_multiplier=false;frames(6,3);
        {std::lock_guard lock(presenter.mx);check(!presenter.automatic_status().enabled&&presenter.auto_multiplier.selected==2&&presenter.epoch==0,
            "Auto off restores the manual count without restarting D3D12 presentation");}
        presenter.reset();check(!Runtime::errors,"Auto D3D12 preserves runtime frame ownership and image ordering");
        std::cout<<"PASS live D3D12 Auto FG: measured load, 2x->3x transition, history continuity and manual recovery\n";
    }
    std::cout<<"PASS FG evaluation completion: warm-up/resume, early rejection, partial recording, NGX failure, failed publication, recovery and reset\n";
}

void fg_hardening_validation() {
    fg_camera_validation();
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))),"Hardening WARP adapter");
    Microsoft::WRL::ComPtr<ID3D12Device> device;Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    check(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))&&SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Hardening WARP device/queue");
    fg_completion_validation(device.Get(),queue.Get());
}
