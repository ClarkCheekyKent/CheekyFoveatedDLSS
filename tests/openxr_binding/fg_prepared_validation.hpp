#pragma once
#include "../timing_list_alias.hpp"

void fg_prepared_validation(ID3D12Device* device) {
    using Microsoft::WRL::ComPtr;using namespace cheeky::fg;
    using cheeky::foveated_dlss::FgSnapshots;
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Prepared guide queue");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;UINT64 serial{};
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)))&&
        SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"Prepared guide commands");
    auto execute=[&]{check(SUCCEEDED(list->Close()),"Prepared guide close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        check(SUCCEEDED(queue->Signal(fence.Get(),++serial)),"Prepared guide signal");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        check(event&&SUCCEEDED(fence->SetEventOnCompletion(serial,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"Prepared guide completion");CloseHandle(event);
        check(SUCCEEDED(allocator->Reset())&&SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"Prepared guide reset");};
    auto texture=[&](unsigned w,unsigned h,DXGI_FORMAT format){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> result;check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&result))),"Prepared guide source");return result;
    };
    auto depth=texture(320,256,DXGI_FORMAT_R32_FLOAT),motion=texture(448,320,DXGI_FORMAT_R32G32_FLOAT),color=texture(640,512,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto upload=[&](ID3D12Resource* target,bool vectors,bool zero=false){
        const auto d=target->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;ComPtr<ID3D12Resource> source;
        check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&source))),"Prepared upload buffer");
        void* data{};source->Map(0,nullptr,&data);
        for(unsigned y=0;y<d.Height;++y)for(unsigned x=0;x<d.Width;++x){auto* p=reinterpret_cast<float*>(static_cast<char*>(data)+y*fp.Footprint.RowPitch)+x*(vectors?2:1);
            p[0]=zero?0.f:(vectors?float(x%32)/1024:float(x+y)/1024);if(vectors){p[1]=zero?0.f:float(y%16)/1024;
                if(!zero&&x>=11&&x<111&&y>=9&&y<109)p[0]=std::numeric_limits<float>::quiet_NaN();}}
        source->Unmap(0,nullptr);color_transition(list.Get(),target,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=source.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=fp;
        b.pResource=target;b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
        color_transition(list.Get(),target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);execute();
    };
    auto read=[&](ID3D12Resource* target){
        const auto d=target->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> result;
        check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result))),"Prepared readback");
        color_transition(list.Get(),target,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=target;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        b.pResource=result.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=fp;list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
        color_transition(list.Get(),target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);execute();
        unsigned pixel_bytes=d.Format==DXGI_FORMAT_R32G32_FLOAT?8:d.Format==DXGI_FORMAT_R16_FLOAT?2:4;
        std::vector<unsigned char> out(SIZE_T(d.Width)*d.Height*pixel_bytes);void* data{};result->Map(0,nullptr,&data);
        for(unsigned y=0;y<d.Height;++y)std::memcpy(out.data()+SIZE_T(y)*d.Width*pixel_bytes,static_cast<char*>(data)+y*fp.Footprint.RowPitch,SIZE_T(d.Width)*pixel_bytes);
        result->Unmap(0,nullptr);return out;
    };
    for(unsigned method=0;method<4;++method)for(unsigned bits:{16u,32u}){
        upload(depth.Get(),false);upload(motion.Get(),true);
        CheekyFgEye g;g.depth=depth.Get();g.motion=motion.Get();g.width=640;g.height=512;
        g.depth_base_x=7;g.depth_base_y=5;g.depth_width=256;g.depth_height=192;
        g.motion_base_x=11;g.motion_base_y=9;g.motion_width=384;g.motion_height=256;
        g.focus_width=.55f;g.focus_height=.45f;g.focus_x=.0625f;g.focus_y=-.0625f;g.resolution_percent=75;g.peripheral_percent=50;
        g.peripheral_method=method;g.depth_bits=g.motion_bits=bits;g.motion_scale_x=2;g.motion_scale_y=-4;
        g.jitter_correction_x=.00390625f;g.jitter_correction_y=-.0078125f;g.render_frame=77;
        FocusPipeline reference;reference.color_format=DXGI_FORMAT_R8G8B8A8_UNORM;
        for(unsigned pass=0;pass<2;++pass){const auto grid=guide_grid(g,256,192,384,256,pass);
            FocusRect r{grid.x,grid.y,grid.w,grid.h,0,pass==0};
            check(reference.prepare_inputs(device,list.Get(),0,pass,color.Get(),g,r,grid.color_width,grid.color_height,pass?bits:32,pass?bits:32),"Legacy guide preparation reference");}
        execute();FgSnapshots snapshots;check(snapshots.initialize(queue.Get()),"Prepared snapshot producer");
        g.guide_capture_mode=1;
        color_transition(list.Get(),depth.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        color_transition(list.Get(),motion.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TimingListAlias alias(list.Get());check(snapshots.record(alias.get(),g)&&snapshots.record(alias.get(),g),"Prepare both eyes at capture through opaque facade");
        color_transition(list.Get(),depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        color_transition(list.Get(),motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        check(SUCCEEDED(list->Close()),"Prepared capture close");snapshots.submitted(queue.Get(),list.Get());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        CheekyFgPair pair;pair.capture_token=snapshots.token();pair.render_frame=77;check(snapshots.acquire(pair),"Lease exact prepared stereo pair");
        queue->Signal(fence.Get(),++serial);HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        check(event&&SUCCEEDED(fence->SetEventOnCompletion(serial,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"Prepared capture fence");CloseHandle(event);
        allocator->Reset();list->Reset(allocator.Get(),nullptr);
        check(pair.eyes[0].prepared_guides&&pair.eyes[1].prepared_guides,"Prepared mode must actually replace raw storage");
        // Reuse the engine inputs before reading either prepared representation.
        upload(depth.Get(),false,true);upload(motion.Get(),true,true);
        FocusPipeline consumer;consumer.color_format=DXGI_FORMAT_R8G8B8A8_UNORM;
        for(unsigned pass=0;pass<2;++pass){const auto& meta=pair.eyes[0];const auto grid=guide_grid(meta,256,192,384,256,pass);
            FocusRect r{grid.x,grid.y,grid.w,grid.h,0,pass==0};
            check(consumer.prepare_inputs(device,list.Get(),0,pass,color.Get(),meta,r,grid.color_width,grid.color_height,pass?bits:32,pass?bits:32),"Consume prepared guides without re-normalization");}
        execute();
        for(unsigned pass=0;pass<2;++pass){const auto& actual=consumer.inputs[0][pass];const auto& old=reference.inputs[0][pass];
            check(actual.borrowed_guides,"Consumer marks borrowed capture allocations");
            check(actual.depth.Get()==(pass?pair.eyes[0].peripheral_depth:pair.eyes[0].depth)&&
                actual.motion.Get()==(pass?pair.eyes[0].peripheral_motion:pair.eyes[0].motion),"No second guide allocation/copy at inference");
            for(const auto& resources:{std::pair{actual.depth.Get(),old.depth.Get()},std::pair{actual.motion.Get(),old.motion.Get()}}){
                check(bool(resources.first)==bool(resources.second),"Periphery allocates only required guides");if(!resources.first)continue;
                const auto a=resources.first->GetDesc(),b=resources.second->GetDesc();check(a.Width==b.Width&&a.Height==b.Height&&a.Format==b.Format,"Prepared grid/precision parity");
                check(read(resources.first)==read(resources.second),"Prepared capture exactly matches legacy guide pixels, offsets, jitter and invalid-vector encoding");}
        }
        const auto original_motion=read(pair.eyes[0].motion);
        const auto center_grid=guide_grid(pair.eyes[0],256,192,384,256,0);
        FocusRect center_rect{center_grid.x,center_grid.y,center_grid.w,center_grid.h,0,true};
        check(consumer.prepare_inputs(device,list.Get(),0,0,color.Get(),pair.eyes[0],center_rect,
            center_grid.color_width,center_grid.color_height,32,32,D3D12_RESOURCE_STATE_COMMON,nullptr,.03125f,-.015625f),
            "Moving-crop correction on producer-prepared guides");execute();
        const auto corrected_motion=read(consumer.inputs[0][0].motion.Get());
        check(consumer.inputs[0][0].motion.Get()!=pair.eyes[0].motion&&read(pair.eyes[0].motion)==original_motion,
            "Moving-crop correction must leave the producer's leased vectors immutable");
        for(std::size_t at=0;at<original_motion.size();at+=8){float original[2]{},actual[2]{};
            std::memcpy(original,original_motion.data()+at,8);std::memcpy(actual,corrected_motion.data()+at,8);
            const bool valid=std::isfinite(original[0])&&std::isfinite(original[1])&&std::abs(original[0])<65504.f&&std::abs(original[1])<65504.f;
            if(valid)check(std::abs(actual[0]-original[0]-.03125f)<1.e-6f&&std::abs(actual[1]-original[1]+.015625f)<1.e-6f,
                "Prepared crop vectors retain native motion and add the crop displacement exactly once");
            else check(actual[0]==original[0]&&actual[1]==original[1],"Prepared crop correction preserves invalid-vector sentinels");
        }
        CheekyFgMemory memory;cheeky::fg_memory::Collector collector(device);snapshots.memory(memory,collector);
        check(memory.categories[cheeky::fg_memory::guide_depth].resources>=2&&memory.categories[cheeky::fg_memory::guide_motion].resources>=2,"Capture inventory owns prepared resources");
        for(auto& eye:pair.eyes)for(auto* resource:{eye.depth,eye.motion,eye.peripheral_depth,eye.peripheral_motion})if(resource)resource->Release();
        pair.producer_queue->Release();pair.lease->Release();snapshots.invalidate();snapshots.retire();execute();
        check(alias.references==0,"Prepared capture releases opaque command-list references");
    }
    list->Close();
    std::cout<<"PASS prepared guide capture: exact legacy pixels at 16/32 bits, all periphery methods, source offsets/reuse, jitter, invalid vectors and direct leased consumption\n";
}
