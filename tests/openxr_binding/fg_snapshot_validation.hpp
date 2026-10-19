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
    std::uint64_t recording_frame=77;
    auto record=[&](float value) {
        clear(value);CheekyFgEye eye;eye.depth=depth.Get();eye.motion=motion.Get();eye.width=eye.height=32;
        eye.render_frame=recording_frame;
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
    cheeky::foveated_dlss::FgColorProvenance provenance;
    check(provenance.token(depth.Get())==0,"Unobserved OpenXR color must not borrow the newest guides");
    allocator->Reset();list->Reset(allocator.Get(),nullptr);
    provenance.observe(list.Get(),depth.Get(),capture,77);
    recording_frame=78;record(.4f);record(.6f);list->Close();queue->ExecuteCommandLists(1,lists);capture.submitted(queue.Get(),list.Get());
    provenance.submitted(queue.Get(),list.Get(),capture);wait();
    check(capture.token()==4,"Later guides may be submitted before consumption of the captured color");
    check(provenance.token(depth.Get())==color_token,"Newer DLSS submissions must not retag a color copy recorded earlier");
    CheekyFgPair pair;pair.capture_token=provenance.token(depth.Get());
    check(capture.acquire(pair)&&pair.eyes[0].evaluation==1&&pair.eyes[1].evaluation==2,
        "Acquire must select the color's exact token, never substitute the newer pair");
    auto release=[&](CheekyFgPair& p){for(auto& eye:p.eyes){eye.depth->Release();eye.motion->Release();}p.producer_queue->Release();p.lease->Release();};
    cheeky::fg::BufferedPresenter::Native retained;
    retained.retain_guides(pair);release(pair);pair=retained.guides;
    check(retained.guide_lease&&pair.eyes[0].render_frame==77&&pair.eyes[1].render_frame==77,
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
    for(unsigned i=0;i<4;++i)check(std::abs(*reinterpret_cast<float*>(static_cast<char*>(mapped)+bytes*i)-(i<2?.25f:.75f))<.0001f,
        "Depth/motion snapshot must retain the correct eye and native frame despite source reuse");
    D3D12_RANGE empty{};rb->Unmap(0,&empty);
    };read_retained();
    CheekyFgPair duplicate;duplicate.capture_token=color_token;check(!capture.acquire(duplicate),"Old guides cannot be reused for another native frame");
    CheekyFgPair next;next.capture_token=4;check(capture.acquire(next),"Next exact frame pair");release(next);
    // Recycle the producer pool for 32 newer stereo frames after returning the
    // bridge response. Holding resource pointers alone would not prevent its
    // Shot objects from being selected and overwritten by the producer.
    for(unsigned frame=79;frame<111;++frame) {
        allocator->Reset();list->Reset(allocator.Get(),nullptr);capture.reset(list.Get());
        recording_frame=frame;record(.1f);record(.9f);
        list->Close();queue->ExecuteCommandLists(1,lists);capture.submitted(queue.Get(),list.Get());
        check(capture.token(frame)!=0,"Guide pool must remain usable while one old stereo job is retained");
        wait();capture.retire();wait();
    }
    read_retained();retained.clear_guides();
    check(!retained.guide_lease&&!retained.guide_depth[0]&&!retained.guide_motion[1],"Retired native job releases guide leases and resources");
    capture.invalidate();check(capture.token()==0,"Configuration/session reset rejects previous-frame guides");
    provenance.invalidate();check(provenance.token(depth.Get())==0,"Session reset clears color provenance");
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
    meta.width=meta.height=32;meta.render_frame=909;
    meta.depth_base_x=2;meta.depth_base_y=3;meta.depth_width=7;meta.depth_height=9;
    meta.motion_base_x=4;meta.motion_base_y=5;meta.motion_width=8;meta.motion_height=6;
    check(aliased.record(alias.get(),meta)&&aliased.record(alias.get(),meta),"Capture through opaque command-list facade");
    cheeky::foveated_dlss::FgColorProvenance alias_color;alias_color.token(depth.Get());
    alias_color.observe(alias.get(),depth.Get(),aliased,909);
    list->Close();queue->ExecuteCommandLists(1,lists);
    aliased.submitted(queue.Get(),list.Get());alias_color.submitted(queue.Get(),list.Get(),aliased);wait();
    check(alias_color.token(depth.Get())==2,"Native submission must commit guides/color recorded through an opaque alias");
    CheekyFgPair alias_pair;alias_pair.capture_token=2;alias_pair.render_frame=909;
    check(aliased.acquire(alias_pair),"Opaque alias must preserve the exact native frame's guide lease");
    check(alias_pair.eyes[0].depth->GetDesc().Width==7&&alias_pair.eyes[0].depth->GetDesc().Height==9&&
        alias_pair.eyes[0].motion->GetDesc().Width==8&&alias_pair.eyes[0].motion->GetDesc().Height==6&&
        alias_pair.eyes[0].depth_base_x==0&&alias_pair.eyes[0].motion_base_y==0,
        "Snapshots retain only each active guide rectangle and rebase offsets without changing the logical frame");release(alias_pair);
    check(alias.references==0,"Alias guide/color command references retire after native enqueue");
    aliased.retire();const auto guide_gpu=aliased.capture_gpu();
    check(guide_gpu.samples==1&&guide_gpu.mean>0,"Both completed eye snapshots must contribute one measured stereo guide-copy sample");
    std::cout<<"Guide alignment: reused source pixels, copy provenance, delayed consumption, bounded/reset recovery and reversed-Z head translation passed\n";
}
