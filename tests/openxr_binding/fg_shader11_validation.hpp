#pragma once
#include <condition_variable>
#include <future>
#include <thread>
namespace shader11_fixture {
using Microsoft::WRL::ComPtr;
inline ComPtr<ID3D11Device> device;
inline ComPtr<ID3D11DeviceContext> context;
// Fake OpenXR runtime with D3D11 swapchains. Like Virtual Desktop, xrWaitFrame
// blocks until the previously begun frame has ended. A frame loop that waits
// while holding up its own xrEndFrame therefore stalls here instead of passing.
struct Runtime {
    struct Chain {ComPtr<ID3D11Texture2D> texture;bool acquired{},waited{};};
    static inline std::mutex m;
    static inline std::condition_variable changed;
    static inline std::unordered_map<XrSwapchain,Chain> chains;
    static inline std::uint64_t next_chain{10000},begun{},ended{};
    static inline XrTime clock_time{};
    static inline std::atomic<unsigned> waits{},ends{},scenes{},errors{},stalls{};
    static inline DWORD frame_thread{};
    static void error(){++errors;}
    static XrResult XRAPI_CALL create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* out){
        D3D11_TEXTURE2D_DESC d{};d.Width=info->width;d.Height=info->height;d.MipLevels=1;d.ArraySize=info->arraySize;d.SampleDesc.Count=1;
        const auto typed=DXGI_FORMAT(info->format);d.Format=cheeky::fg::srgb_color(typed)?cheeky::fg::color_family(typed):typed;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        Chain chain;if(FAILED(device->CreateTexture2D(&d,nullptr,&chain.texture)))return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        std::lock_guard lock(m);*out=reinterpret_cast<XrSwapchain>(++next_chain);chains[*out]=std::move(chain);return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL destroy(XrSwapchain chain){std::lock_guard lock(m);chains.erase(chain);return XR_SUCCESS;}
    static XrResult XRAPI_CALL enumerate(XrSwapchain chain,unsigned capacity,unsigned* count,XrSwapchainImageBaseHeader* images){
        std::lock_guard lock(m);*count=1;if(capacity){if(!chains.contains(chain))return XR_ERROR_HANDLE_INVALID;
            reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[0].texture=chains.at(chain).texture.Get();}return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL acquire(XrSwapchain chain,const XrSwapchainImageAcquireInfo*,unsigned* index){
        std::lock_guard lock(m);auto& c=chains.at(chain);if(c.acquired)error();c.acquired=true;c.waited=false;*index=0;return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL wait_image(XrSwapchain chain,const XrSwapchainImageWaitInfo*){
        std::lock_guard lock(m);auto& c=chains.at(chain);if(!c.acquired)error();c.waited=true;return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL release(XrSwapchain chain,const XrSwapchainImageReleaseInfo*){
        std::lock_guard lock(m);auto& c=chains.at(chain);if(!c.acquired||!c.waited)error();c.acquired=c.waited=false;return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL wait_frame(XrSession,const XrFrameWaitInfo*,XrFrameState* state){
        Sleep(4);std::unique_lock lock(m);
        if(!frame_thread)frame_thread=GetCurrentThreadId();else if(frame_thread!=GetCurrentThreadId())error();
        if(!changed.wait_for(lock,std::chrono::seconds(2),[]{return ended==begun;})){++stalls;return XR_ERROR_RUNTIME_FAILURE;}
        ++waits;state->shouldRender=XR_TRUE;state->predictedDisplayTime=clock_time+=5555555;state->predictedDisplayPeriod=5555555;return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL begin(XrSession,const XrFrameBeginInfo*){
        std::lock_guard lock(m);if(frame_thread!=GetCurrentThreadId()||begun!=ended)error();++begun;return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL end(XrSession,const XrFrameEndInfo* info){
        {std::lock_guard lock(m);if(frame_thread!=GetCurrentThreadId()||begun!=ended+1)error();++ended;++ends;
            if(info->layerCount){const auto* scene=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
                if(!chains.contains(scene->views[0].subImage.swapchain)||!chains.contains(scene->views[1].subImage.swapchain))error();else ++scenes;}}
        changed.notify_all();return XR_SUCCESS;
    }
    static cheeky::fg::XrFns functions(){
        cheeky::fg::XrFns fn;fn.create=create;fn.destroy=destroy;fn.enumerate=enumerate;fn.acquire=acquire;fn.wait_image=wait_image;
        fn.release=release;fn.wait_frame=wait_frame;fn.begin_frame=begin;fn.end_frame=end;return fn;
    }
    static void reset(){std::lock_guard lock(m);chains.clear();begun=ended=0;clock_time=0;frame_thread=0;waits=ends=scenes=errors=stalls=0;}
};
inline ComPtr<ID3D11Texture2D> texture(DXGI_FORMAT f,const std::array<float,4>& color,unsigned size=64){
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=f;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11RenderTargetView> rtv;
    check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&t))&&SUCCEEDED(device->CreateRenderTargetView(t.Get(),nullptr,&rtv)),"Native shader input");
    context->ClearRenderTargetView(rtv.Get(),color.data());return t;
}
inline std::array<unsigned char,4> pixel(ID3D11Texture2D* source,unsigned x,unsigned y){
    D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=d.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging;check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Native shader readback");context->CopyResource(staging.Get(),source);
    D3D11_MAPPED_SUBRESOURCE mapped{};check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"Map native shader pixels");
    std::array<unsigned char,4> value;std::memcpy(value.data(),static_cast<const char*>(mapped.pData)+y*mapped.RowPitch+x*4,4);context->Unmap(staging.Get(),0);return value;
}
inline CheekyFgShaderSource11V1 source(unsigned eye,unsigned method,ID3D11Texture2D* depth,ID3D11Texture2D* motion){
    CheekyFgShaderSource11V1 s;s.eye=eye;s.session_generation=7;s.mapping_revision=3;s.generation=1;
    s.context=context.Get();s.depth=depth;s.motion=motion;auto& g=s.meta;g.feature=200+eye;
    g.width=g.height=g.depth_width=g.depth_height=g.motion_width=g.motion_height=64;g.configuration=55;
    g.focus_width=g.focus_height=.5f;g.center_method=method;g.peripheral_method=0;g.resolution_percent=g.peripheral_percent=100;return s;
}
inline std::array<XrCompositionLayerProjectionView,2> eye_views(XrSwapchain left,XrSwapchain right){
    std::array<XrCompositionLayerProjectionView,2> views;
    for(unsigned eye=0;eye<2;++eye){auto& view=views[eye];view={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};view.pose.orientation.w=1;
        view.fov={-.7f,.7f,.7f,-.7f};view.subImage={eye?right:left,{{0,0},{64,64}},0};}
    return views;
}
}
// Backend11 GPU stages: real interpolated pixels for both center methods,
// periphery from the previous frame, BGRA/sRGB render-target output.
inline void fg_backend11_pixels(){
    using namespace shader11_fixture;using namespace cheeky::fg;
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB})for(unsigned method:{1u,2u}){
        Backend11 b;check(b.initialize(device.Get(),7,nullptr),b.diagnostic);
        auto depth=texture(DXGI_FORMAT_R32_FLOAT,{.5f,0,0,0});auto motion=texture(DXGI_FORMAT_R32G32_FLOAT,{0,0,0,0});
        auto red=texture(format,{1,0,0,1});auto blue=texture(format,{0,0,1,1});
        const auto views=eye_views(reinterpret_cast<XrSwapchain>(100),reinterpret_cast<XrSwapchain>(101));
        auto colors=[&](ID3D11Texture2D* t){std::array<Image,2> out;
            for(unsigned eye=0;eye<2;++eye){out[eye]=b.crop(b.copy(t,format),views[eye].subImage);check(bool(out[eye]),"Native D3D11 capture and eye crop");}return out;};
        auto guides=[&](ID3D11Texture2D* m){std::array<std::shared_ptr<Backend11::Guides>,2> out;
            for(unsigned eye=0;eye<2;++eye)check(b.observe(source(eye,method,depth.Get(),m)),"Snapshot native D3D11 eye guides");
            check(b.take_guides(out),"Claim one stereo guide interval");return out;};
        const auto a=colors(red.Get());const auto ga=guides(motion.Get());
        check(ga[0]->source.meta.reset&&ga[1]->source.meta.reset,"First guide interval seeds history");
        const auto c=colors(blue.Get());const auto gc=guides(motion.Get());
        check(!gc[0]->source.meta.reset&&!gc[1]->source.meta.reset,"Consecutive guide interval keeps history");
        std::array<Image,2> generated;
        check(b.generate(a,c,ga,gc,views,views,generated),"Native D3D11 interpolation");
        for(unsigned eye=0;eye<2;++eye){auto* result=generated[eye]->texture11.Get();
            const auto center=pixel(result,32,32),edge=pixel(result,2,2);const unsigned ri=format==DXGI_FORMAT_B8G8R8A8_UNORM?2:0,bi=2-ri;
            check(edge[ri]>250&&edge[bi]<3,"Native D3D11 periphery independently uses previous frame");
            if(method==1)check(center[ri]>250&&center[bi]<3,"Native center simple warp samples previous color");
            else {const unsigned expected=format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?188:128;
                check(std::abs(int(center[ri])-int(expected))<=2&&std::abs(int(center[bi])-int(expected))<=2,"Native center bidirectional interpolation produces real midpoint pixels");}}
        // An ambiguous interval (one eye evaluated twice) claims no guides.
        for(unsigned eye=0;eye<2;++eye)check(b.observe(source(eye,method,depth.Get(),motion.Get())),"Snapshot next interval");
        check(!b.observe(source(0,method,depth.Get(),motion.Get())),"Duplicate eye evaluation is rejected");
        std::array<std::shared_ptr<Backend11::Guides>,2> ambiguous;check(!b.take_guides(ambiguous),"Ambiguous interval yields no guides");
        if(format==DXGI_FORMAT_R8G8B8A8_UNORM){
            auto object=[&](unsigned start){auto t=texture(format,{0,0,0,1});std::vector<unsigned char> pixels(64*64*4);
                for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){pixels[(y*64+x)*4+3]=255;
                    if(x>=start&&x<start+8&&y>=28&&y<36)pixels[(y*64+x)*4]=255;}
                context->UpdateSubresource(t.Get(),0,nullptr,pixels.data(),64*4,0);return t;};
            auto a0=object(16),a1=object(40),b0=object(24),b1=object(32);
            auto left_motion=texture(DXGI_FORMAT_R32G32_FLOAT,{-8,0,0,0}),right_motion=texture(DXGI_FORMAT_R32G32_FLOAT,{8,0,0,0});
            b.clear_guides();
            auto pair=[&](ID3D11Texture2D* left,ID3D11Texture2D* right){std::array<Image,2> out;
                out[0]=b.crop(b.copy(left,format),views[0].subImage);out[1]=b.crop(b.copy(right,format),views[1].subImage);
                for(unsigned eye=0;eye<2;++eye)check(b.observe(source(eye,method,depth.Get(),eye?right_motion.Get():left_motion.Get())),"Moving guides");
                std::array<std::shared_ptr<Backend11::Guides>,2> g;check(b.take_guides(g),"Moving guide interval");return std::pair{out,g};};
            const auto [from,from_guides]=pair(a0.Get(),a1.Get());const auto [to,to_guides]=pair(b0.Get(),b1.Get());
            std::array<Image,2> moved;check(b.generate(from,to,from_guides,to_guides,views,views,moved),"Native moving-object interpolation");
            if(method==2)check(pixel(moved[0]->texture11.Get(),22,32)[0]>250&&pixel(moved[0]->texture11.Get(),19,32)[0]<3&&
                pixel(moved[1]->texture11.Get(),38,32)[0]>250&&pixel(moved[1]->texture11.Get(),44,32)[0]<3,
                "Native center moves objects to the true midpoint with independent opposite eye vectors");
        }
    }
}
// BufferedPresenter + Backend11 driven like DCS: the simulation thread waits
// for frame N+1 while the render thread still owns frame N.
inline void fg_backend11_pipelined(){
    using namespace shader11_fixture;using namespace cheeky::fg;
    Runtime::reset();
    auto presenter=std::make_unique<BufferedPresenter>();auto& p=*presenter;
    p.trace=[](const char*) noexcept {};
    std::vector<XrSwapchain> retired;p.retire_chain=[&](XrSwapchain chain){retired.push_back(chain);};
    check(p.setup11(reinterpret_cast<XrSession>(1),Runtime::functions(),device.Get(),7,&cheeky::quad_capture::d3d11_dispatch_mutex),"Native D3D11 buffered presenter setup");
    check(!p.device&&!p.queue&&!p.interop11,"Native D3D11 backend creates no D3D12 objects");
    const auto format=DXGI_FORMAT_R8G8B8A8_UNORM;
    auto depth=texture(DXGI_FORMAT_R32_FLOAT,{.5f,0,0,0});auto motion=texture(DXGI_FORMAT_R32G32_FLOAT,{0,0,0,0});
    std::array<ComPtr<ID3D11Texture2D>,2> app_colors{texture(format,{1,0,0,1}),texture(format,{0,0,1,1})};
    const auto left=reinterpret_cast<XrSwapchain>(100),right=reinterpret_cast<XrSwapchain>(101);
    constexpr unsigned frames=48;
    std::mutex handoff;std::condition_variable ready;std::deque<XrTime> waited;bool done{};
    std::atomic<unsigned> app_errors{};
    std::thread render([&]{
        for(unsigned n=0;n<frames;++n){
            XrTime time{};
            {std::unique_lock lock(handoff);ready.wait(lock,[&]{return !waited.empty()||done;});if(waited.empty())return;time=waited.front();waited.pop_front();}
            if(XR_FAILED(p.begin_frame(nullptr))){++app_errors;continue;}
            auto* color=app_colors[n%2].Get();
            for(auto chain:{left,right})if(!p.capture11(chain,0,color,64,64,1,format))++app_errors;
            for(unsigned eye=0;eye<2;++eye){auto s=source(eye,2,depth.Get(),motion.Get());s.meta.reset=0;p.native11->observe(s);}
            auto views=eye_views(left,right);
            XrCompositionLayerProjection scene{XR_TYPE_COMPOSITION_LAYER_PROJECTION};scene.space=reinterpret_cast<XrSpace>(1);scene.views=views.data();scene.viewCount=2;
            const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&scene);XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime=time;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&layer;
            Sleep(3); // Render work overlapping the next xrWaitFrame.
            if(XR_FAILED(p.end_frame(&end,{},&scene)))++app_errors;
        }
    });
    for(unsigned n=0;n<frames;++n){
        XrFrameState state{XR_TYPE_FRAME_STATE};
        if(XR_FAILED(p.wait_frame(nullptr,&state))){++app_errors;break;}
        {std::lock_guard lock(handoff);waited.push_back(state.predictedDisplayTime);}ready.notify_all();
    }
    // Disable exactly as xrWaitFrame does, while the render thread may still
    // own the last frame: finish application frames, then drain.
    const auto generated=p.generated_count,native=p.native_count;
    auto disable=std::async(std::launch::async,[&]{p.finish_application_frame();p.invalidate_temporal();});
    const bool disabled=disable.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    if(!disabled){std::cerr<<"backend11 disable hung\n";std::_Exit(3);}
    {std::lock_guard lock(handoff);done=true;}ready.notify_all();render.join();
    p.reset();
    if(Runtime::stalls||Runtime::errors||app_errors||generated<frames/4)
        std::cerr<<"backend11 pipelined stalls="<<Runtime::stalls<<" errors="<<Runtime::errors<<" app_errors="<<app_errors
            <<" native="<<native<<" generated="<<generated<<" runtime_ends="<<Runtime::ends<<" scenes="<<Runtime::scenes<<"\n";
    check(!Runtime::stalls,"Overlapping application frames never stall the runtime xrWaitFrame");
    check(!Runtime::errors,"Presenter alone owns runtime frames and private swapchain image order");
    check(!app_errors,"Application wait/begin/capture/end succeed with overlapping frames");
    check(generated>=frames/4,"Native D3D11 interpolation reaches the headset with overlapping application frames");
    check(retired.size()>=2&&Runtime::chains.size()==retired.size(),"Teardown hands private D3D11 swapchains to the layer instead of destroying them");
    for(auto chain:retired)Runtime::destroy(chain);
    check(Runtime::chains.empty(),"Retired private D3D11 swapchains are destroyed later by the layer");
}
// NGX driver work must share the FG gate in stereo sessions too.
inline void fg_backend11_copy_state(){
    using namespace shader11_fixture;using namespace cheeky::fg;
    Backend11 backend;check(backend.initialize(device.Get(),7,nullptr),backend.diagnostic);
    auto source_color=texture(DXGI_FORMAT_R8G8B8A8_UNORM,{1,0,0,1});
    auto destination=texture(DXGI_FORMAT_R8G8B8A8_UNORM,{0,0,1,1});
    auto game_target=texture(DXGI_FORMAT_R8G8B8A8_UNORM,{0,1,0,1});
    ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11ShaderResourceView> srv;
    check(SUCCEEDED(device->CreateRenderTargetView(game_target.Get(),nullptr,&rtv))&&
        SUCCEEDED(device->CreateShaderResourceView(source_color.Get(),nullptr,&srv)),"Copy-state game views");
    auto* bound_target=rtv.Get();auto* bound_source=srv.Get();
    const D3D11_VIEWPORT viewport{3,7,43,47,0,1};
    context->RSSetViewports(1,&viewport);context->OMSetRenderTargets(1,&bound_target,nullptr);
    context->PSSetShaderResources(2,1,&bound_source);
    auto copy=std::async(std::launch::async,[&]{
        auto full=backend.copy(source_color.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
        XrSwapchainSubImage sub{};sub.imageRect={{4,8},{16,24}};
        auto cropped=backend.crop(full,sub);
        return std::pair{cropped,backend.upload(destination.Get(),full)};
    });
    auto [cropped,uploaded]=copy.get();
    unsigned count=1;D3D11_VIEWPORT after{};ComPtr<ID3D11RenderTargetView> after_target;ComPtr<ID3D11ShaderResourceView> after_source;
    context->RSGetViewports(&count,&after);context->OMGetRenderTargets(1,&after_target,nullptr);context->PSGetShaderResources(2,1,&after_source);
    const bool bindings=count==1&&std::memcmp(&viewport,&after,sizeof(viewport))==0&&after_target==rtv&&after_source==srv;
    context->OMSetRenderTargets(0,nullptr,nullptr);bound_source=nullptr;context->PSSetShaderResources(2,1,&bound_source);context->RSSetViewports(0,nullptr);
    check(cropped&&uploaded&&bindings,"Background capture/crop/upload preserve the game's pipeline bindings");
    check(pixel(cropped->texture11.Get(),8,12)[0]>250&&pixel(destination.Get(),32,32)[0]>250&&
        pixel(game_target.Get(),32,32)[1]>250,"Background copies transfer pixels without touching the game's render target");
}
// NGX driver work must share the FG gate in stereo sessions too.
inline void fg_backend11_dispatch_gate(){
    using namespace shader11_fixture;using namespace cheeky::fg;
    auto presenter=std::make_shared<BufferedPresenter>();
    auto& gate=cheeky::quad_capture::d3d11_dispatch_mutex;
    const auto session=reinterpret_cast<XrSession>(0xf611);
    check(presenter->setup11(session,Runtime::functions(),device.Get(),7,&gate),"Stereo dispatch presenter setup");
    {std::lock_guard lock(state_mutex);auto& owner=sessions[session];
        owner.running=true;owner.graphics_api=11;owner.graphics_device=device.Get();owner.fg=presenter;}
    // The NGX-side export must admit stereo sessions and retain recursion for
    // guide observation within the outer evaluation's dispatch scope.
    auto* ticket=CheekyQuad_AcquireD3D11V1(context.Get());
    auto* nested=CheekyQuad_AcquireD3D11V1(context.Get());
    const bool admitted=ticket==&gate&&nested==ticket;
    CheekyQuad_ReleaseD3D11V1(nested);
    std::promise<void> runtime_started,copy_started;
    auto color=texture(DXGI_FORMAT_R8G8B8A8_UNORM,{1,0,0,1});
    auto runtime=std::async(std::launch::async,[&]{runtime_started.set_value();return presenter->runtime_dispatch([]{return XR_SUCCESS;});});
    auto copy=std::async(std::launch::async,[&]{copy_started.set_value();return presenter->native11->copy(color.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);});
    runtime_started.get_future().wait();copy_started.get_future().wait();
    const bool excluded=runtime.wait_for(std::chrono::milliseconds(25))==std::future_status::timeout&&
        copy.wait_for(std::chrono::milliseconds(25))==std::future_status::timeout;
    CheekyQuad_ReleaseD3D11V1(ticket); // Release before any assertion can throw.
    const bool completed=runtime.get()==XR_SUCCESS&&bool(copy.get());
    // The bridge route shares the same gate even without quad_input.
    auto backend=std::move(presenter->native11);
    presenter->interop11=std::make_shared<D3D11Bridge>();presenter->interop11->dispatch_mutex=&gate;
    const bool bridged=presenter->dispatch_mutex11()==&gate;
    presenter->interop11.reset();presenter->native11=std::move(backend);
    {std::lock_guard lock(state_mutex);sessions.erase(session);}
    auto* inactive_ticket=CheekyQuad_AcquireD3D11V1(context.Get());
    const bool inactive=inactive_ticket==nullptr;CheekyQuad_ReleaseD3D11V1(inactive_ticket);
    check(admitted&&excluded&&completed&&bridged&&inactive,
        "Stereo NGX excludes native GPU/runtime and bridge submissions; inactive sessions remain ungated");
}
// OpenComposite games (ACC) calibrate eyes through OpenVR, so the runtime
// publishes guides with session 0. The layer binds them to its only running
// D3D11 session and refuses when more than one could own them.
inline void fg_backend11_openvr_calibration(){
    using namespace shader11_fixture;using namespace cheeky::fg;
    auto presenter=std::make_shared<BufferedPresenter>();
    const auto session=reinterpret_cast<XrSession>(0xf612),other=reinterpret_cast<XrSession>(0xf613);
    check(presenter->setup11(session,Runtime::functions(),device.Get(),7,nullptr),"OpenVR calibration presenter setup");
    {std::lock_guard lock(state_mutex);auto& owner=sessions[session];
        owner.running=true;owner.graphics_api=11;owner.graphics_device=device.Get();owner.generation=7;owner.fg=presenter;}
    auto depth=texture(DXGI_FORMAT_R32_FLOAT,{.5f,0,0,0});auto motion=texture(DXGI_FORMAT_R32G32_FLOAT,{0,0,0,0});
    auto openvr=[&](unsigned eye){auto s=source(eye,1,depth.Get(),motion.Get());s.session_generation=0;return s;};
    auto left=openvr(0),right=openvr(1);
    const bool bound=CheekyFG_ObserveShaderD3D11V1(&left)&&CheekyFG_ObserveShaderD3D11V1(&right);
    std::array<std::shared_ptr<Backend11::Guides>,2> guides;
    const bool claimed=presenter->native11->take_guides(guides)&&guides[0]->source.session_generation==7;
    {std::lock_guard lock(state_mutex);auto& owner=sessions[other];owner.running=true;owner.graphics_api=11;owner.generation=8;}
    const bool ambiguous=!CheekyFG_ObserveShaderD3D11V1(&left);
    auto stale=source(0,1,depth.Get(),motion.Get());stale.session_generation=6;
    const bool mismatched=!CheekyFG_ObserveShaderD3D11V1(&stale);
    {std::lock_guard lock(state_mutex);sessions.erase(session);sessions.erase(other);}
    check(bound&&claimed&&ambiguous&&mismatched,
        "OpenVR-calibrated guides bind to the single running D3D11 session; ambiguous or stale sessions are rejected");
}
// Exercise the same immediate-context fence submission seen in Virtual
// Desktop's xrEndFrame while Backend11 switches state and records GPU work.
// This uses the real adapter but needs neither a headset nor an OpenXR runtime.
inline void fg_backend11_fence_stress(){
    using namespace shader11_fixture;using namespace cheeky::fg;
    Backend11 backend;check(backend.initialize(device.Get(),7,nullptr),backend.diagnostic);
    ComPtr<ID3D11Device5> device5;ComPtr<ID3D11DeviceContext4> context4;ComPtr<ID3D11Fence> fence;
    check(SUCCEEDED(device.As(&device5))&&SUCCEEDED(context.As(&context4))&&
        SUCCEEDED(device5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"D3D11 fence stress setup");
    auto color=texture(DXGI_FORMAT_R8G8B8A8_UNORM,{1,0,0,1});
    std::atomic<bool> failed{};std::atomic<std::uint64_t> signals{};
    std::jthread runtime([&](std::stop_token stop){
        while(!stop.stop_requested()){
            const auto value=signals.load()+1;
            if(FAILED(context4->Signal(fence.Get(),value))){failed=true;break;}
            context->Flush();signals=value;
            // Bound GPU queue growth while still racing the context-state swaps.
            if((value&63)==0)Sleep(1);
        }
    });
    // Keep the fence thread alive through real guide dispatches, interpolation,
    // readback and resource retirement, not only CopyResource submissions.
    fg_backend11_pixels();
    bool copied=true;
    for(unsigned i=0;i<3000&&copied;++i)copied=bool(backend.copy(color.Get(),DXGI_FORMAT_R8G8B8A8_UNORM));
    runtime.request_stop();runtime.join();backend.flush();
    check(copied&&!failed&&signals>0,"Concurrent native D3D11 copies and runtime fence signals");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(fence->GetCompletedValue()<signals&&std::chrono::steady_clock::now()<deadline)Sleep(1);
    check(SUCCEEDED(device->GetDeviceRemovedReason())&&fence->GetCompletedValue()>=signals,
        "D3D11 driver worker completes every concurrent fence signal");
    std::cout<<"PASS native D3D11 fence stress: 3000 copies plus concurrent shader work, "<<signals.load()<<" signals\n";
}
void fg_shader11_validation(bool hardware=false){
    using namespace shader11_fixture;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"Native shader D3D11 device");
    fg_backend11_copy_state();
    fg_backend11_dispatch_gate();
    fg_backend11_openvr_calibration();
    if(hardware)fg_backend11_fence_stress();
    else fg_backend11_pixels();
    fg_backend11_pipelined();
    {
        cheeky::quad_capture::QuadFg11 quad(device.Get());quad.enable(true);
        auto input=[&](DXGI_FORMAT f,float x){D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=64;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=f;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11RenderTargetView> r;
            check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&t))&&SUCCEEDED(device->CreateRenderTargetView(t.Get(),nullptr,&r)),"Quad native guide input");
            const float value[]{x,0,0,0};context->ClearRenderTargetView(r.Get(),value);return t;};
        auto outer_depth=input(DXGI_FORMAT_R32_FLOAT,.25f),center_depth=input(DXGI_FORMAT_R32_FLOAT,.75f);
        auto outer_motion=input(DXGI_FORMAT_R32G32_FLOAT,0),center_motion=input(DXGI_FORMAT_R32G32_FLOAT,8);
        CheekyQuadViewsV1 roles{};roles.status=2;roles.active=1;roles.revision=3;
        std::array<XrCompositionLayerProjectionView,4> views;
        for(unsigned i=0;i<4;++i){roles.views[i].dlss_view=200+i;roles.views[i].registration_generation=1;
            views[i]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};views[i].subImage.imageRect.extent={64,64};}
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.viewCount=4;projection.views=views.data();
        const std::array<std::array<float,4>,2> insets{{{.25f,.25f,.5f,.5f},{.25f,.25f,.5f,.5f}}};
        auto observe_quad=[&]{for(unsigned i=0;i<4;++i){CheekyQuadFgSourceV1 source;source.context=context.Get();source.generation=1;
            source.depth=i<2?outer_depth.Get():center_depth.Get();source.motion=i<2?outer_motion.Get():center_motion.Get();
            auto& g=source.meta;g.feature=200+i;g.width=g.height=g.depth_width=g.depth_height=g.motion_width=g.motion_height=64;
            g.center_method=2;g.configuration=5;check(quad.observe(source),"Snapshot all four native quad guides");}};
        observe_quad();auto first=quad.build_native(roles,projection,insets);check(first.valid,"Assemble full-eye quad guides without a D3D12 bridge");
        auto value=[&](ID3D11Texture2D* t,unsigned x,unsigned y){D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=0;
            ComPtr<ID3D11Texture2D> staging;check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"Quad native readback");context->CopyResource(staging.Get(),t);
            D3D11_MAPPED_SUBRESOURCE mapped{};check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"Map native quad guide");
            const float result=*reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch+x*(d.Format==DXGI_FORMAT_R32_FLOAT?4:16));
            context->Unmap(staging.Get(),0);return result;};
        check(value(first.depth[0].Get(),64,64)==.75f&&value(first.depth[1].Get(),4,4)==.25f&&value(first.motion[0].Get(),64,64)==.0625f,
            "Native quad atlas retains outer/center depth and normalizes inset motion into full-eye coordinates");
        quad.finish();observe_quad();auto second=quad.build_native(roles,projection,insets);
        check(second.valid&&!second.meta[0].reset&&first.depth[0]!=second.depth[0]&&value(first.depth[0].Get(),64,64)==.75f,
            "Quad native guide history remains immutable while the next interval is assembled");
        quad.finish();observe_quad();roles.views[2].registration_generation=2;
        check(!quad.build_native(roles,projection,insets).valid,"Native quad assembly rejects stale source assignments");
    }
    device.Reset();context.Reset();
    std::cout<<"PASS native D3D11 shader FG: both center methods, independent periphery, real pixels, BGRA/sRGB, stereo history, ambiguity, "
        "buffered pacing with overlapping (DCS-style) application frames, no D3D12\n";
}
