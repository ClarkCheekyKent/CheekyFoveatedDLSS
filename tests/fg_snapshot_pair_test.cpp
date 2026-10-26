#include "../src/fg_snapshot.hpp"
#include <dxgi1_4.h>
#include <iostream>
#include <stdexcept>
#include <cmath>

static void require(bool result,const char* message){if(!result)throw std::runtime_error(message);}
int main(){
    using Microsoft::WRL::ComPtr;
    using cheeky::foveated_dlss::FgSnapshots;
    try{
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
        require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))&&
            SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"WARP device");
        D3D12_COMMAND_QUEUE_DESC q{};ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        require(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))&&
            SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
            SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)))&&
            SUCCEEDED(list->Close()),"WARP command context");
        ComPtr<ID3D12Fence> done;require(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done))),"Readback fence");
        std::uint64_t serial{};
        auto wait=[&]{
            require(SUCCEEDED(queue->Signal(done.Get(),++serial)),"Signal test fence");
            HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"Test event");
            const bool complete=SUCCEEDED(done->SetEventOnCompletion(serial,event))&&WaitForSingleObject(event,5000)==WAIT_OBJECT_0;
            CloseHandle(event);require(complete,"Retire test commands");
        };
        ComPtr<ID3D12DescriptorHeap> rtvs;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
        require(SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs))),"Source RTV heap");
        const auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);
        auto texture=[&](DXGI_FORMAT format,unsigned index){
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
            d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=16;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
            d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;ComPtr<ID3D12Resource> result;
            require(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&result))),"Guide source texture");
            auto view=rtvs->GetCPUDescriptorHandleForHeapStart();view.ptr+=index*stride;device->CreateRenderTargetView(result.Get(),nullptr,view);return result;
        };
        auto depth=texture(DXGI_FORMAT_R32_FLOAT,0),motion=texture(DXGI_FORMAT_R32G32_FLOAT,1);
        auto transition=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
        };
        FgSnapshots shots;require(shots.initialize(queue.Get()),"Snapshot producer");
        auto start=[&]{require(SUCCEEDED(allocator->Reset())&&SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"Reset test commands");shots.reset(list.Get());};
        auto eye=[&](unsigned physical,float value){
            unsigned index{};for(auto* source:{depth.Get(),motion.Get()}){
                transition(source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
                auto view=rtvs->GetCPUDescriptorHandleForHeapStart();view.ptr+=index++*stride;const float color[]{value,value,0,0};list->ClearRenderTargetView(view,color,0,nullptr);
                transition(source,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            CheekyFgEye meta;meta.depth=depth.Get();meta.motion=motion.Get();meta.width=meta.height=16;
            meta.depth_width=meta.depth_height=meta.motion_width=meta.motion_height=16;meta.configuration=7;meta.feature=100+physical;
            require(shots.record(list.Get(),meta,{physical,42}),"Record real immutable guide pixels");
        };
        auto pair=[&]{start();eye(0,.25f);eye(1,.75f);};
        auto submit=[&]{require(SUCCEEDED(list->Close()),"Close snapshot commands");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);shots.submitted(queue.Get(),list.Get());wait();};
        auto acquire=[&](std::uint64_t token){CheekyFgPair result;result.capture_token=token;require(shots.acquire(result),"Acquire exact color-bound pair");return result;};
        auto release=[](CheekyFgPair& p){for(auto& e:p.eyes)for(auto* r:{e.depth,e.motion,e.peripheral_depth,e.peripheral_motion})if(r)r->Release();p.producer_queue->Release();p.lease->Release();};

        pair();submit();const auto b=shots.capture_token();require(b==2,"Pin B at color capture");
        pair(); // C is CPU-recorded but not submitted: the reported failure.
        require(shots.token(b)==b&&shots.last_match.future==2&&shots.last_match.pending==0,"Future pending C must not invalidate B");
        CheekyFgCaptureCropsV2 crops;crops.token=b;
        require(shots.peek_current_crops(crops,7)&&crops.token==b&&crops.valid_mask==3,"Compact color keeps B's crop with C pending");
        auto retained=acquire(b);require(!shots.token(),"C cannot be consumed before its GPU submission");
        submit();const auto c=shots.token();require(c==4,"C remains available after B is consumed");auto next=acquire(c);release(next);

        pair();submit();const auto d=shots.capture_token();pair();submit();
        require(!shots.token(),"Multiple submitted pairs without an explicit color anchor remain ambiguous");
        require(shots.capture_token(d)==d&&shots.token(d)==6,"Batched submission of E cannot change D's color anchor");
        next=acquire(d);release(next);require(shots.token()==8,"Submitting D leaves E untouched");next=acquire(8);release(next);

        pair();submit();const auto skipped=shots.capture_token();pair();
        shots.discard_interval(skipped);submit();require(shots.token()==12,"Skipping a color-bound pair preserves the next pending frame");
        shots.discard_interval(skipped);shots.discard_interval(9999);
        require(shots.token()==12,"Stale or unknown color tokens cannot discard newer guides");next=acquire(12);release(next);
        pair();submit();pair();shots.discard_interval();submit();
        require(shots.token()==16,"An unanchored skipped handoff retires only its submitted prefix");next=acquire(16);release(next);

        start();eye(0,.25f);submit();require(!shots.capture_token(),"A single submitted eye cannot anchor a stereo pair");
        start();eye(1,.75f);submit();require(shots.capture_token()==18,"Separate eye submissions can complete the same pair");next=acquire(18);release(next);
        start();eye(0,.25f);eye(0,.75f);submit();require(!shots.token(),"Duplicate physical eyes still fail closed");shots.discard_interval();

        // B has remained leased through every source overwrite above.
        const auto desc=retained.eyes[0].depth->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;require(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))),"Snapshot pixel readback");
        start();transition(retained.eyes[0].depth,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=retained.eyes[0].depth;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        transition(retained.eyes[0].depth,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);submit();
        void* data{};require(SUCCEEDED(readback->Map(0,nullptr,&data)),"Map retained guide pixels");
        const bool unchanged=std::abs(*reinterpret_cast<float*>(static_cast<char*>(data)+footprint.Offset)-.25f)<.0001f;
        readback->Unmap(0,nullptr);release(retained);require(unchanged,"Existing lease preserves B's actual pixels without another guide copy");
        std::cout<<"PASS color-bound FG guides: future pending/submitted pairs, compact crops, skips, stale tokens, split eyes and retained GPU pixels\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
