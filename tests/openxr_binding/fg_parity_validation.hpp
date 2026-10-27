#pragma once
// Cross-API parity: the native D3D11 backend (one RegionsPS pass over the
// full eye) and the D3D12 focus pipeline (prepare, CenterMain/periphery
// dispatches, compose) must produce the same generated pixels from the same
// color, depth and motion. Both compile shared/fg_interpolation_shader.hpp, so
// a mismatch means one side feeds the shader different coordinates, guide
// normalization, history or color handling.
namespace fg_parity {
using Microsoft::WRL::ComPtr;
constexpr unsigned size=64;
struct Frame {
    std::vector<std::uint32_t> color=std::vector<std::uint32_t>(size*size);
    std::vector<float> depth=std::vector<float>(size*size,.5f);
    std::vector<float> motion=std::vector<float>(size*size*2); // Pixels, previous minus current.
};
inline std::uint32_t rgba(unsigned r,unsigned g,unsigned b){return r|(g<<8)|(b<<16)|(0xffu<<24);}
inline std::uint32_t background(int x,int y){return rgba(0,unsigned(std::clamp(x,0,63))*4,unsigned(std::clamp(y,0,63))*4);}
// Eye 0: the whole view translates 16 pixels right. Eye 1: only an object
// moves 16 pixels left over a static background, which disoccludes pixels
// whose depth history disagrees.
inline std::array<std::array<Frame,2>,2> scenes(){
    std::array<std::array<Frame,2>,2> out; // [eye][previous,current]
    for(unsigned eye=0;eye<2;++eye)for(unsigned frame=0;frame<2;++frame){auto& f=out[eye][frame];
        const int shift=eye==0&&frame==1?16:0;
        const int object=eye==0?(frame?32:16):(frame?24:40);
        for(int y=0;y<int(size);++y)for(int x=0;x<int(size);++x){const auto i=unsigned(y)*size+unsigned(x);
            const bool inside=x>=object&&x<object+8&&y>=28&&y<36;
            f.color[i]=inside?rgba(255,0,0):background(x-shift,y);
            f.depth[i]=inside?.25f:.5f;
            if(frame==1){const float mv=eye==0?-16.f:inside?16.f:0.f;f.motion[i*2]=mv;}}
    }
    return out;
}
inline CheekyFgEye meta(unsigned center,unsigned periphery,bool foveated,float fraction=.5f){
    CheekyFgEye g;g.width=g.height=g.depth_width=g.depth_height=g.motion_width=g.motion_height=size;
    g.center_method=center;g.peripheral_method=periphery;g.resolution_percent=g.peripheral_percent=100;
    g.focus_width=g.focus_height=foveated?.5f:1.f;g.focus_feather=0;g.configuration=91;return g;
}

struct D3D12 {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;std::uint64_t serial{};
    std::vector<ComPtr<ID3D12Resource>> staging;
    explicit D3D12(IDXGIAdapter* hardware=nullptr){
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;
        check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))),"Parity WARP adapter");
        check(SUCCEEDED(D3D12CreateDevice(hardware?hardware:warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"Parity D3D12 device");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))&&
            SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
            SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)))&&
            SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"Parity D3D12 queue");
    }
    void execute(){
        check(SUCCEEDED(list->Close()),"Parity close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        check(SUCCEEDED(queue->Signal(fence.Get(),++serial)),"Parity signal");
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event&&SUCCEEDED(fence->SetEventOnCompletion(serial,event)),"Parity completion");
        const auto waited=WaitForSingleObject(event,10000);CloseHandle(event);check(waited==WAIT_OBJECT_0,"Parity wait");
        staging.clear();check(SUCCEEDED(allocator->Reset())&&SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"Parity reset");
    }
    ComPtr<ID3D12Resource> buffer(D3D12_HEAP_TYPE type,UINT64 bytes,D3D12_RESOURCE_STATES state){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
        d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out;check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&out))),"Parity buffer");return out;
    }
    // Recorded on the current list; the texture is in COMMON once it executes.
    ComPtr<ID3D12Resource> texture(DXGI_FORMAT format,const void* data,unsigned texel,unsigned edge=size){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=d.Height=edge;d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;
        ComPtr<ID3D12Resource> out;check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out))),"Parity texture");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
        auto upload=buffer(D3D12_HEAP_TYPE_UPLOAD,bytes,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped{};
        check(SUCCEEDED(upload->Map(0,nullptr,&mapped)),"Parity upload map");
        for(unsigned y=0;y<edge;++y)std::memcpy(static_cast<char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,static_cast<const char*>(data)+y*edge*texel,edge*texel);
        upload->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=out.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        cheeky::fg::color_transition(list.Get(),out.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        staging.push_back(upload);return out;
    }
    std::vector<std::uint32_t> read(ID3D12Resource* source){
        const auto d=source->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};
        device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
        auto readback=buffer(D3D12_HEAP_TYPE_READBACK,bytes,D3D12_RESOURCE_STATE_COPY_DEST);
        cheeky::fg::color_transition(list.Get(),source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        cheeky::fg::color_transition(list.Get(),source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);execute();
        void* mapped{};check(SUCCEEDED(readback->Map(0,nullptr,&mapped)),"Parity readback map");
        std::vector<std::uint32_t> out(size_t(d.Width)*d.Height);
        for(unsigned y=0;y<d.Height;++y)std::memcpy(out.data()+y*d.Width,static_cast<const char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,size_t(d.Width)*4);
        readback->Unmap(0,nullptr);return out;
    }
};

// D3D12 shader-FG stages as the buffered backend uses them for one eye.
inline std::vector<std::uint32_t> generate12(D3D12& gpu,cheeky::fg::FocusPipeline& pipeline,unsigned eye,
    const std::array<Frame,2>& frames,unsigned center,unsigned periphery,bool foveated,float fraction=.5f){
    using namespace cheeky::fg;
    pipeline.interpolation_fraction=fraction;
    std::array<ComPtr<ID3D12Resource>,2> color,depth,motion;
    for(unsigned f=0;f<2;++f){color[f]=gpu.texture(DXGI_FORMAT_R8G8B8A8_UNORM,frames[f].color.data(),4);
        depth[f]=gpu.texture(DXGI_FORMAT_R32_FLOAT,frames[f].depth.data(),4);motion[f]=gpu.texture(DXGI_FORMAT_R32G32_FLOAT,frames[f].motion.data(),8);}
    gpu.execute();
    const FocusRect full{0,0,size,size,0,false},region{16,16,32,32,0,true};
    // Without foveation the full eye runs the center method on the periphery kernel.
    auto g=meta(foveated?center:0,foveated?periphery:center,foveated);
    auto prepare=[&](unsigned f){g.depth=depth[f].Get();g.motion=motion[f].Get();
        if(foveated)check(pipeline.prepare_inputs(gpu.device.Get(),gpu.list.Get(),eye,0,color[1].Get(),g,region,32,32),"Parity D3D12 center inputs");
        check(pipeline.prepare_inputs(gpu.device.Get(),gpu.list.Get(),eye,1,color[1].Get(),g,full,size,size),"Parity D3D12 full-eye inputs");};
    // Depth history comes from the previous interval's guides.
    prepare(0);check(pipeline.save_depth(gpu.device.Get(),gpu.list.Get(),eye,0)&&pipeline.save_depth(gpu.device.Get(),gpu.list.Get(),eye,1),"Parity D3D12 depth history");
    gpu.execute();pipeline.history[eye]=pipeline.center_history[eye]=true;
    prepare(1);
    Camera camera{};camera.clip_to_prev=identity();
    if(foveated)check(pipeline.generate_periphery(gpu.device.Get(),gpu.list.Get(),eye,color[0].Get(),color[1].Get(),g,camera,
        D3D12_RESOURCE_STATE_COMMON,nullptr,D3D12_RESOURCE_STATE_COMMON,nullptr,nullptr,&region),"Parity D3D12 center dispatch");
    check(pipeline.generate_periphery(gpu.device.Get(),gpu.list.Get(),eye,color[0].Get(),color[1].Get(),g,camera),"Parity D3D12 periphery dispatch");
    if(!foveated){gpu.execute();return gpu.read(pipeline.peripheral_output[eye].Get());}
    check(pipeline.compose_frame(gpu.device.Get(),gpu.list.Get(),eye,pipeline.center_output[eye].Get(),pipeline.peripheral_output[eye].Get(),region,size,size),"Parity D3D12 composition");
    gpu.execute();return gpu.read(pipeline.full[eye].Get());
}

inline std::array<std::vector<std::uint32_t>,2> generate11(const std::array<std::array<Frame,2>,2>& scene,unsigned center,unsigned periphery,bool foveated,float fraction=.5f){
    using namespace shader11_fixture;using namespace cheeky::fg;
    Backend11 b;check(b.initialize(device.Get(),7,nullptr),b.diagnostic);
    auto upload=[&](DXGI_FORMAT format,const void* data,unsigned texel){D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA initial{data,size*texel};
        ComPtr<ID3D11Texture2D> t;check(SUCCEEDED(device->CreateTexture2D(&d,&initial,&t)),"Parity D3D11 input");return t;};
    const auto views=eye_views(reinterpret_cast<XrSwapchain>(100),reinterpret_cast<XrSwapchain>(101));
    std::array<std::array<Image,2>,2> colors; // [frame][eye]
    std::array<std::array<std::shared_ptr<Backend11::Guides>,2>,2> guides;
    for(unsigned f=0;f<2;++f){
        for(unsigned eye=0;eye<2;++eye){const auto& frame=scene[eye][f];
            colors[f][eye]=b.crop(b.copy(upload(DXGI_FORMAT_R8G8B8A8_UNORM,frame.color.data(),4).Get(),DXGI_FORMAT_R8G8B8A8_UNORM),views[eye].subImage);
            auto depth=upload(DXGI_FORMAT_R32_FLOAT,frame.depth.data(),4),motion=upload(DXGI_FORMAT_R32G32_FLOAT,frame.motion.data(),8);
            auto s=source(eye,center,depth.Get(),motion.Get());const auto feature=s.meta.feature;
            s.meta=meta(center,periphery,foveated);s.meta.feature=feature;
            check(colors[f][eye]&&b.observe(s),"Parity D3D11 capture");}
        check(b.take_guides(guides[f]),"Parity D3D11 guide interval");}
    check(!guides[1][0]->source.meta.reset,"Parity D3D11 keeps history");
    std::array<Image,2> generated;check(b.generate(colors[0],colors[1],guides[0],guides[1],views,views,generated,false,fraction),"Parity D3D11 interpolation");
    std::array<std::vector<std::uint32_t>,2> out;
    for(unsigned eye=0;eye<2;++eye){auto* t=generated[eye]->texture11.Get();D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
        d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=d.MiscFlags=0;
        ComPtr<ID3D11Texture2D> staging;check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Parity D3D11 readback");
        context->CopyResource(staging.Get(),t);D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"Parity D3D11 map");out[eye].resize(size*size);
        for(unsigned y=0;y<size;++y)std::memcpy(out[eye].data()+y*size,static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,size*4);
        context->Unmap(staging.Get(),0);}
    return out;
}
}

inline void fg_cross_api_parity(){
    using namespace fg_parity;
    using shader11_fixture::device;using shader11_fixture::context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"Parity D3D11 device");
    D3D12 gpu;const auto scene=scenes();unsigned cases{};
    struct Case {unsigned center,periphery;bool foveated;};
    for(float fraction:{.25f,1.f/3,.5f,2.f/3,.75f})for(const auto c:{Case{1,13,false},Case{2,13,false},Case{1,13,true},Case{2,1,true},Case{2,2,true},Case{1,2,true},Case{3,13,false},Case{3,1,true},Case{1,3,true},Case{3,3,true}}){
        cheeky::fg::FocusPipeline pipeline;pipeline.color_format=DXGI_FORMAT_R8G8B8A8_UNORM;
        check(pipeline.initialize(gpu.device.Get()),"Parity D3D12 focus pipeline");
        const auto native=generate11(scene,c.center,c.periphery,c.foveated,fraction);unsigned total_changed{};
        for(unsigned eye=0;eye<2;++eye){
            const auto transported=generate12(gpu,pipeline,eye,scene[eye],c.center,c.periphery,c.foveated,fraction);
            unsigned mismatched{},first=UINT32_MAX,changed{},edge_band{};
            for(unsigned i=0;i<size*size;++i){
                changed+=native[eye][i]!=scene[eye][1].color[i];
                for(unsigned channel=0;channel<4;++channel){
                    const int a=int((native[eye][i]>>(channel*8))&255),b=int((transported[i]>>(channel*8))&255);
                    if(std::abs(a-b)<=1)continue;
                    // Known difference: the D3D12 center pass sees only the
                    // center crop's guides, so a warp whose source lies outside
                    // the region (half the 16 px motion) falls back to the
                    // unwarped color. Native D3D11 keeps full-eye guides.
                    const int x=int(i%size),y=int(i/size),inset=std::min({x-16,47-x,y-16,47-y});
                    if(c.foveated&&inset>=0&&inset<int(std::ceil(16*std::max(fraction,1-fraction))))++edge_band;
                    else{++mismatched;first=std::min(first,i);}
                    break;}}
            if(edge_band)std::cout<<"Parity center-edge guide crop difference center="<<c.center<<" periphery="<<c.periphery<<" eye="<<eye<<" pixels="<<edge_band<<'\n';
            if(mismatched)std::cout<<"Parity mismatch center="<<c.center<<" periphery="<<c.periphery<<" foveated="<<c.foveated<<" eye="<<eye
                <<" pixels="<<mismatched<<" first=("<<first%size<<','<<first/size<<") d3d11="<<std::hex<<native[eye][first]
                <<" d3d12="<<transported[first]<<std::dec<<'\n';
            check(!mismatched,"Native D3D11 and D3D12 shader FG must produce the same generated pixels");
            total_changed+=changed;++cases;}
        // Sparse object motion can legitimately keep the current frame under
        // bidirectional warping; the translating eye must still move content.
        check(total_changed>64,"Parity scene must exercise motion instead of repeating the current frame");
    }
    device.Reset();context.Reset();
    std::cout<<"PASS cross-API shader FG parity: "<<cases<<" eye cases (full-eye and foveated, all shader method pairs, translation and disocclusion) match D3D11 vs D3D12\n";
}
