#pragma once
#include "../timing_list_alias.hpp"

void fg_snapshot_validation(ID3D12Device* device) {
    using Microsoft::WRL::ComPtr;
    using cheeky::foveated_dlss::FgSnapshots;
    // The same physical head translation must reproject to the same pixel for
    // either native depth convention. Previously reversed-Z used forward-Z math.
    for(bool reversed:{false,true}) {
        XrCompositionLayerProjectionView previous{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        previous.pose.orientation.w=1;previous.fov={-.785398163f,.785398163f,.785398163f,-.785398163f};
        auto current=previous;current.pose.position.x=.2f;
        auto camera=cheeky::fg::camera(current,&previous,reversed);
        const float reversed_depth=(10000.f/2.f-1.f)/(10000.f/.05f-1.f);
        const float z=reversed?reversed_depth:1-reversed_depth;
        const auto& m=camera.clip_to_prev.m;
        const float x=m[2]*z+m[3],w=m[14]*z+m[15];
        check(camera.valid&&std::abs(x/w-.1f)<.0001f,"Head translation must use the native depth convention");
    }
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Snapshot queue");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"Snapshot commands");
    ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
    check(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))),"Snapshot descriptors");
    const auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);
    auto texture=[&](unsigned i,DXGI_FORMAT format) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=16;d.Format=format;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> result;
        check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&result))),"Snapshot texture");
        auto rtv=heap->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=i*stride;device->CreateRenderTargetView(result.Get(),nullptr,rtv);return result;
    };
    auto depth=texture(0,DXGI_FORMAT_R32_FLOAT),motion=texture(1,DXGI_FORMAT_R32G32_FLOAT);
    auto clear=[&](float value) {
        unsigned i{};for(auto* r:{depth.Get(),motion.Get()}) {
            cheeky::fg::color_transition(list.Get(),r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto rtv=heap->GetCPUDescriptorHandleForHeapStart();rtv.ptr+=i++*stride;const float v[]{value,value,0,0};list->ClearRenderTargetView(rtv,v,0,nullptr);
            cheeky::fg::color_transition(list.Get(),r,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    };
    FgSnapshots capture;check(capture.initialize(queue.Get()),"Snapshot initialize");
    auto record=[&](float value) {
        clear(value);CheekyFgEye eye;eye.depth=depth.Get();eye.motion=motion.Get();eye.width=eye.height=32;
        eye.depth_width=eye.depth_height=eye.motion_width=eye.motion_height=16;
        check(capture.record(list.Get(),eye),"Record immutable evaluation-time guides");
    };
    record(.25f);record(.75f);clear(.125f);
    check(capture.token()==0,"CPU-recorded guides must not be eligible before queue submission");
    check(SUCCEEDED(list->Close()),"Snapshot close");
    capture.submitted(queue.Get(),list.Get());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    const auto color_token=capture.token();check(color_token==2,"Color token follows submitted evaluations");
    // Fence completion is needed only by this readback test, never by capture.
    auto wait=[&] {
        ComPtr<ID3D12Fence> f;auto hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f));
        if(SUCCEEDED(hr))hr=queue->Signal(f.Get(),1);
        if(FAILED(hr))std::cerr<<"Snapshot readback fence hr="<<std::hex<<unsigned(hr)<<" removed="<<unsigned(device->GetDeviceRemovedReason())<<std::dec<<'\n';
        check(SUCCEEDED(hr),"Snapshot readback fence");
        HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(e!=nullptr,"Snapshot event");
        const bool done=SUCCEEDED(f->SetEventOnCompletion(1,e))&&WaitForSingleObject(e,10000)==WAIT_OBJECT_0;CloseHandle(e);check(done,"Snapshot readback wait");
    };wait();
    allocator->Reset();list->Reset(allocator.Get(),nullptr);
    record(.4f);record(.6f);list->Close();queue->ExecuteCommandLists(1,lists);capture.submitted(queue.Get(),list.Get());wait();
    CheekyFgPair pair;pair.capture_token=color_token;
    check(capture.acquire(pair)&&pair.eyes[0].evaluation==1&&pair.eyes[1].evaluation==2,
        "A pair matched at xrEndFrame keeps its exact guides after newer guides are submitted");
    check(capture.token()==4,"The next complete interval matches once the earlier pair is consumed");
    auto release=[&](CheekyFgPair& p){for(auto& eye:p.eyes){eye.depth->Release();eye.motion->Release();}p.producer_queue->Release();p.lease->Release();};
    cheeky::fg::FrameGuides12 retained;
    retained.retain(pair);release(pair);pair=retained.pair;
    check(retained.lease&&pair.eyes[0].evaluation==1&&pair.eyes[1].evaluation==2,
        "Native FG job must retain the exact original stereo guide lease after releasing the bridge response");
    // Read every snapshot after the engine has reused the SAME original guide
    // textures for both eyes and a later frame. No constant-source mock passes.
    const auto d=pair.eyes[0].depth->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};
    device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
    bytes=(bytes+D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1)&~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1);
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC rd{};
    rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=bytes*4;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb))),"Snapshot readback buffer");
    auto read_retained=[&] {
    allocator->Reset();list->Reset(allocator.Get(),nullptr);
    unsigned index{};
    for(auto& eye:pair.eyes)for(auto* source:{eye.depth,eye.motion}) {
        auto desc=source->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 size{};
        device->GetCopyableFootprints(&desc,0,1,bytes*index, &footprint,nullptr,nullptr,&size);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=source;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        b.pResource=rb.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=footprint;
        cheeky::fg::color_transition(list.Get(),source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
        cheeky::fg::color_transition(list.Get(),source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);++index;
    }
    list->Close();queue->ExecuteCommandLists(1,lists);wait();
    void* mapped{};D3D12_RANGE range{0,SIZE_T(bytes*4)};check(SUCCEEDED(rb->Map(0,&range,&mapped)),"Snapshot readback map");
    // Guides are prepared at capture: depth is copied, motion is normalized to
    // UV displacement (scale 1 over a 16-pixel motion grid).
    for(unsigned i=0;i<4;++i)check(std::abs(*reinterpret_cast<float*>(static_cast<char*>(mapped)+bytes*i)-(i<2?.25f:.75f)/(i%2?16.f:1.f))<.0001f,
        "Depth/motion snapshot must retain the correct eye and native frame despite source reuse");
    D3D12_RANGE empty{};rb->Unmap(0,&empty);
    };read_retained();
    CheekyFgPair duplicate;duplicate.capture_token=color_token;check(!capture.acquire(duplicate),"Old guides cannot be reused for another native frame");
    CheekyFgPair next;next.capture_token=4;check(capture.acquire(next),"Next exact frame pair");release(next);
    // Recycle the producer pool for 32 newer stereo frames after returning the
    // bridge response. Holding resource pointers alone would not prevent its
    // Shot objects from being selected and overwritten by the producer.
    for(unsigned frame=0;frame<32;++frame) {
        allocator->Reset();list->Reset(allocator.Get(),nullptr);capture.reset(list.Get());
        record(.1f);record(.9f);
        list->Close();queue->ExecuteCommandLists(1,lists);capture.submitted(queue.Get(),list.Get());
        CheekyFgPair recycled;recycled.capture_token=capture.token();
        check(recycled.capture_token&&capture.acquire(recycled),"Guide pool must remain usable while one old stereo job is retained");
        release(recycled);
        wait();capture.retire();wait();
    }
    read_retained();retained=cheeky::fg::FrameGuides12{};
    check(!retained.lease&&!retained.depth[0]&&!retained.motion[1],"Retired native job releases guide leases and resources");
    capture.invalidate();check(capture.token()==0,"Configuration/session reset rejects previous-frame guides");
    // Exercise physical eye identity with different GPU pixels while call order
    // and calibration change independently, as they can across ACC scenes.
    using cheeky::foveated_dlss::FgGuideIdentity;
    FgSnapshots mapped;check(mapped.initialize(queue.Get()),"Calibrated snapshot queue");
    auto start_mapped=[&]{allocator->Reset();list->Reset(allocator.Get(),nullptr);mapped.reset(list.Get());};
    auto record_mapped=[&](unsigned eye,std::uint64_t revision,std::uint64_t feature=0) {
        clear(eye==0?.25f:.75f);
        CheekyFgEye meta;meta.depth=depth.Get();meta.motion=motion.Get();meta.width=meta.height=32;
        meta.depth_width=meta.depth_height=meta.motion_width=meta.motion_height=16;
        meta.feature=feature?feature:101+eye;
        check(mapped.record(list.Get(),meta,{eye,revision}),"Record calibrated physical-eye guides");
    };
    auto submit_mapped=[&]{list->Close();queue->ExecuteCommandLists(1,lists);mapped.submitted(queue.Get(),list.Get());wait();};
    auto acquire_mapped=[&](bool reset) {
        pair={};pair.capture_token=mapped.token();
        check(pair.capture_token&&mapped.acquire(pair),"Acquire a complete calibrated stereo pair");
        check(cheeky_fg_pair_matches_color(pair,pair.capture_token,0),
            "The OpenXR handoff must accept physical eye order with either evaluation order");
        auto invalid=pair;invalid.eyes[0].evaluation=invalid.eyes[1].evaluation;
        check(!cheeky_fg_pair_matches_color(invalid,pair.capture_token,0)&&
            !cheeky_fg_pair_matches_color(pair,pair.capture_token,1),
            "Reordering must not weaken exact evaluation/color-frame validation");
        check(pair.eyes[0].reset==unsigned(reset)&&pair.eyes[1].reset==unsigned(reset),
            "Both physical-eye histories reset together only on discontinuities");
        // This checks GPU depth AND motion pixels, not just metadata ordering.
        read_retained();release(pair);mapped.retire();wait();
    };
    start_mapped();record_mapped(0,11);record_mapped(1,11);submit_mapped();acquire_mapped(true);
    mapped.collect_completed();const auto standalone_guide_gpu=mapped.capture_gpu();
    check(standalone_guide_gpu.samples==1&&standalone_guide_gpu.mean>0,
        "Standalone guide timestamps are grouped by the matched stereo pair even without a game-frame ticket");
    start_mapped();record_mapped(0,11);record_mapped(1,11);submit_mapped();acquire_mapped(false);
    start_mapped();record_mapped(1,11);record_mapped(0,11);submit_mapped();acquire_mapped(true);
    start_mapped();record_mapped(1,11);record_mapped(0,11);submit_mapped();acquire_mapped(false);
    // Same evaluation order but the handles now represent opposite eyes.
    start_mapped();record_mapped(1,12,101);record_mapped(0,12,102);submit_mapped();acquire_mapped(true);
    start_mapped();record_mapped(0,12);record_mapped(1,13);submit_mapped();
    check(!mapped.token(),"A calibration switch between evaluations rejects the whole stereo pair");
    mapped.discard_interval();
    start_mapped();record_mapped(0,13);record_mapped(1,13);submit_mapped();acquire_mapped(true);
    start_mapped();record_mapped(0,13);record_mapped(0,13);submit_mapped();
    check(!mapped.token(),"Two guides for one physical eye cannot masquerade as stereo");
    mapped.discard_interval();
    start_mapped();record_mapped(0,13);record_mapped(1,13);submit_mapped();acquire_mapped(true);
    start_mapped();record_mapped(FgGuideIdentity::unknown,13);record_mapped(1,13);submit_mapped();
    check(!mapped.token(),"A partial calibrated/unknown pair cannot use ordinal fallback");
    mapped.discard_interval();
    start_mapped();record_mapped(1,14);record_mapped(0,14);submit_mapped();acquire_mapped(true);
    // Physical-eye jitter must not cross a changed mapping, even when native
    // frame IDs, dimensions and the NGX handle address are unchanged.
    auto record_jitter=[&](unsigned eye,std::uint64_t revision,float jitter) {
        CheekyFgEye meta;meta.depth=depth.Get();meta.motion=motion.Get();meta.width=meta.height=32;
        meta.depth_width=meta.depth_height=meta.motion_width=meta.motion_height=16;
        meta.feature=101+eye;meta.motion_jittered=1;meta.jitter_x=jitter;
        check(mapped.record(list.Get(),meta,{eye,revision}),"Record physical-eye jitter history");
    };
    for(unsigned frame=0;frame<3;++frame) {
        start_mapped();record_jitter(1,frame<2?14:15,.25f+.5f*frame);record_jitter(0,frame<2?14:15,.25f+.5f*frame);submit_mapped();
        CheekyFgPair jitter_pair;jitter_pair.capture_token=mapped.token();
        check(jitter_pair.capture_token&&mapped.acquire(jitter_pair),"Calibrated jitter pair");
        for(auto& eye:jitter_pair.eyes)
            check(std::abs(eye.jitter_correction_x-(frame==1?-.5f/16:0.f))<.00001f,
                "Jitter history follows physical eyes and clears on mapping changes");
        release(jitter_pair);mapped.retire();wait();
    }
    std::cout<<"PASS calibrated FG eye-order switches, mixed-mapping rejection, GPU eye pixels and history resets\n";
    allocator->Reset();list->Reset(allocator.Get(),nullptr);
    unsigned accepted{};for(unsigned i=0;i<32;++i){
        CheekyFgEye eye;eye.depth=depth.Get();eye.motion=motion.Get();eye.width=eye.height=32;
        accepted+=capture.record(list.Get(),eye)?1u:0u;
    }
    check(accepted<32&&FgSnapshots::tagged(list.Get()),"Bounded recordings are tagged for serialized submit/reset observation");
    list->Close();list->Reset(allocator.Get(),nullptr);capture.reset(list.Get());
    check(capture.token()==0,"Discarded, unsubmitted commands must never become eligible guides");
    CheekyFgEye retry;retry.depth=depth.Get();retry.motion=motion.Get();retry.width=retry.height=32;
    check(capture.record(list.Get(),retry),"Discarded recordings release bounded snapshot slots for reuse");
    list->Close();list->Reset(allocator.Get(),nullptr);capture.reset(list.Get());list->Close();
    capture.retire();wait();
    // NGX can see an opaque command-list facade while ExecuteCommandLists
    // receives its native object. Shared private data proves the association.
    allocator->Reset();list->Reset(allocator.Get(),nullptr);
    FgSnapshots aliased;check(aliased.initialize(queue.Get()),"Alias snapshot queue");
    TimingListAlias alias(list.Get());CheekyFgEye meta;meta.depth=depth.Get();meta.motion=motion.Get();
    meta.width=meta.height=32;
    meta.depth_base_x=2;meta.depth_base_y=3;meta.depth_width=7;meta.depth_height=9;
    meta.motion_base_x=4;meta.motion_base_y=5;meta.motion_width=8;meta.motion_height=6;
    check(aliased.record(alias.get(),meta)&&aliased.record(alias.get(),meta),"Capture through opaque command-list facade");
    list->Close();queue->ExecuteCommandLists(1,lists);
    aliased.submitted(queue.Get(),list.Get());wait();
    check(aliased.token()==2,"Native submission must commit guides recorded through an opaque alias");
    CheekyFgPair alias_pair;alias_pair.capture_token=2;
    check(aliased.acquire(alias_pair),"Opaque alias must preserve the exact native frame's guide lease");
    // Prepared guides cover only the active depth/motion rectangles, on the
    // shared grid (the smaller of the two: 7x6).
    check(alias_pair.eyes[0].prepared_guides&&alias_pair.eyes[0].depth->GetDesc().Width==7&&alias_pair.eyes[0].depth->GetDesc().Height==6&&
        alias_pair.eyes[0].motion->GetDesc().Width==7&&alias_pair.eyes[0].motion->GetDesc().Height==6,
        "Snapshots retain only the active guide rectangles");release(alias_pair);
    check(alias.references==0,"Alias guide/color command references retire after native enqueue");
    aliased.retire();const auto guide_gpu=aliased.capture_gpu();
    check(guide_gpu.samples==1&&guide_gpu.mean>0,"Both completed eye snapshots must contribute one measured stereo guide-copy sample");
    std::cout<<"Guide alignment: reused source pixels, delayed consumption, bounded/reset recovery and reversed-Z head translation passed\n";
}
