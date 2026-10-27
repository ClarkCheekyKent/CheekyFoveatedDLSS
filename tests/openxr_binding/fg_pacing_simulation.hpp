#pragma once
// GPU pacing simulation: the real BufferedPresenter + native D3D11 backend on
// the real adapter, a DCS-style game (simulation thread waits for frame N+1
// while the render thread owns frame N) doing real pixel-shader work, and a
// Virtual Desktop-like runtime. The runtime's xrWaitFrame blocks until the
// previous frame ended AND its GPU work (including a compositor copy) has
// completed, then returns at the next vsync of a clock whose true period may
// differ from the reported predictedDisplayPeriod.
//
//   CheekyOpenXRBindingTests.exe --fg-pacing-sim [seconds] [csv]
//
// Each pacing clock mode runs the same load profile. Output: per-window CSV
// (optional) and a summary of shown native/generated FPS, vsyncs the runtime
// had to skip, frames the game rendered that never reached the runtime, and
// the GPU time the game got.
#include <d3dcompiler.h>
#include <cstdio>
namespace pacing_sim {
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
inline double ms_since(Clock::time_point a,Clock::time_point b=Clock::now()){return std::chrono::duration<double,std::milli>(b-a).count();}

struct Config {
    const char* name{"steady"};
    double refresh_hz{90},true_hz{90}; // Reported vs actual vsync rate.
    double game_gpu_ms{14},spike_gpu_ms{26}; // Baseline and spike GPU cost per native frame.
    double spike_every_s{6},spike_for_s{1.5};
    double game_cpu_ms{4};
    double compositor_gpu_ms{1}; // VD's own per-submission GPU work (encode/warp).
    unsigned eye{1024};
    double seconds{12};
    bool block_on_gpu{true};
};

struct Gpu {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11Buffer> constants;
    double iterations_per_ms{};
    static ComPtr<ID3DBlob> compile(const char* source,const char* entry,const char* profile){
        ComPtr<ID3DBlob> code,error;
        check(SUCCEEDED(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error)),"Compile pacing load shader");
        return code;
    }
    void initialize(){
        check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"Hardware D3D11 device for pacing simulation");
        static const char* source=R"(
cbuffer Load:register(b0){uint iterations;float seed;float2 pad;};
float4 vs(uint id:SV_VertexID):SV_Position{float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);}
float4 ps(float4 p:SV_Position):SV_Target{float v=seed+p.x*.0013+p.y*.0007;
  [loop]for(uint i=0;i<iterations;++i)v=frac(sin(v*12.9898+i*.001)*43758.5453);
  return float4(v,frac(v*7.),frac(p.x/64.),1);})";
        auto vcode=compile(source,"vs","vs_5_0"),pcode=compile(source,"ps","ps_5_0");
        check(SUCCEEDED(device->CreateVertexShader(vcode->GetBufferPointer(),vcode->GetBufferSize(),nullptr,&vs))&&
            SUCCEEDED(device->CreatePixelShader(pcode->GetBufferPointer(),pcode->GetBufferSize(),nullptr,&ps)),"Pacing load shaders");
        D3D11_BUFFER_DESC b{};b.ByteWidth=16;b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check(SUCCEEDED(device->CreateBuffer(&b,nullptr,&constants)),"Pacing load constants");
    }
    // Records one full-screen pass; the caller owns timing queries.
    void load(ID3D11RenderTargetView* target,unsigned size,unsigned iterations,float seed){
        const std::uint32_t data[4]{iterations,0,0,0};std::uint32_t words[4];std::memcpy(words,data,16);
        std::memcpy(&words[1],&seed,4);context->UpdateSubresource(constants.Get(),0,nullptr,words,0,0);
        const D3D11_VIEWPORT viewport{0,0,float(size),float(size),0,1};
        context->OMSetRenderTargets(1,&target,nullptr);context->RSSetViewports(1,&viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->IASetInputLayout(nullptr);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        auto* cb=constants.Get();context->PSSetConstantBuffers(0,1,&cb);context->Draw(3,0);
        context->OMSetRenderTargets(0,nullptr,nullptr);
    }
    // GPU milliseconds for one recorded pass, measured with timestamps.
    double time(ID3D11RenderTargetView* target,unsigned size,unsigned iterations){
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT};ComPtr<ID3D11Query> disjoint,begin,end;
        device->CreateQuery(&q,&disjoint);q.Query=D3D11_QUERY_TIMESTAMP;device->CreateQuery(&q,&begin);device->CreateQuery(&q,&end);
        context->Begin(disjoint.Get());context->End(begin.Get());load(target,size,iterations,.5f);context->End(end.Get());context->End(disjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};std::uint64_t a{},b{};
        while(context->GetData(disjoint.Get(),&d,sizeof(d),0)!=S_OK)Sleep(0);
        while(context->GetData(begin.Get(),&a,sizeof(a),0)!=S_OK)Sleep(0);
        while(context->GetData(end.Get(),&b,sizeof(b),0)!=S_OK)Sleep(0);
        return d.Disjoint||!d.Frequency?0.:double(b-a)*1000./double(d.Frequency);
    }
    void calibrate(ID3D11RenderTargetView* target,unsigned size){
        time(target,size,64); // Warm up.
        unsigned n=256;double t{};
        for(;n<(1u<<22);n*=2){t=time(target,size,n);if(t>8)break;}
        double best=1e9;for(unsigned k=0;k<3;++k)best=std::min(best,time(target,size,n));
        iterations_per_ms=double(n)/best;
    }
};

// Virtual Desktop-like runtime with a real-time vsync clock.
struct Runtime {
    static inline Gpu* gpu{};
    static inline std::mutex m;static inline std::condition_variable changed;
    struct Chain {ComPtr<ID3D11Texture2D> texture;bool acquired{},waited{};};
    static inline std::unordered_map<XrSwapchain,Chain> chains;
    static inline std::uint64_t next_chain{20000},begun{},ended{};
    static inline Clock::time_point origin;
    static inline double true_period_ms{},reported_period_ms{},compositor_ms{};
    static inline std::int64_t last_vsync{-1};
    static inline ComPtr<ID3D11Query> last_end_query;
    static inline ComPtr<ID3D11Texture2D> compositor_target;static inline ComPtr<ID3D11RenderTargetView> compositor_rtv;
    static inline std::atomic<std::uint64_t> waits{},ends{},scene_ends{},skipped_vsyncs{},late_ends{},errors{};
    static inline double gpu_wait_ms_total{};
    static inline std::int64_t pending_vsync{-1};
    // Compositor: latches each submission at its vsync. GPU work that is not
    // complete by then is a late (juddering) frame.
    static inline bool block_on_gpu{true};
    static inline std::deque<std::pair<ComPtr<ID3D11Query>,Clock::time_point>> inflight;
    static inline std::uint64_t submitted{},completed{};
    static inline std::thread compositor;static inline std::atomic<bool> running{};
    static void compositor_loop(){
        while(running){
            ComPtr<ID3D11Query> query;Clock::time_point deadline;
            {std::lock_guard lock(m);if(!inflight.empty()){query=inflight.front().first;deadline=inflight.front().second;}}
            if(!query){std::this_thread::sleep_for(std::chrono::microseconds(200));continue;}
            if(gpu->context->GetData(query.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_FALSE){std::this_thread::sleep_for(std::chrono::microseconds(200));continue;}
            const auto now=Clock::now();
            {std::lock_guard lock(m);inflight.pop_front();++completed;if(now>deadline)++late_ends;}
            changed.notify_all();
        }
    }
    static std::int64_t vsync_index(Clock::time_point t){return std::int64_t(std::floor(ms_since(origin,t)/true_period_ms));}
    static Clock::time_point vsync_time(std::int64_t k){return origin+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double,std::milli>(k*true_period_ms));}
    static XrResult XRAPI_CALL create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* out){
        D3D11_TEXTURE2D_DESC d{};d.Width=info->width;d.Height=info->height;d.MipLevels=1;d.ArraySize=info->arraySize;d.SampleDesc.Count=1;
        const auto typed=DXGI_FORMAT(info->format);d.Format=cheeky::fg::srgb_color(typed)?cheeky::fg::color_family(typed):typed;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        Chain chain;if(FAILED(gpu->device->CreateTexture2D(&d,nullptr,&chain.texture)))return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        std::lock_guard lock(m);*out=reinterpret_cast<XrSwapchain>(++next_chain);chains[*out]=std::move(chain);return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL destroy(XrSwapchain chain){std::lock_guard lock(m);chains.erase(chain);return XR_SUCCESS;}
    static XrResult XRAPI_CALL enumerate(XrSwapchain chain,unsigned capacity,unsigned* count,XrSwapchainImageBaseHeader* images){
        std::lock_guard lock(m);*count=1;if(capacity){if(!chains.contains(chain))return XR_ERROR_HANDLE_INVALID;
            reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[0].texture=chains.at(chain).texture.Get();}return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL acquire(XrSwapchain chain,const XrSwapchainImageAcquireInfo*,unsigned* index){
        std::lock_guard lock(m);auto& c=chains.at(chain);if(c.acquired)++errors;c.acquired=true;c.waited=false;*index=0;return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL wait_image(XrSwapchain chain,const XrSwapchainImageWaitInfo*){
        std::lock_guard lock(m);auto& c=chains.at(chain);if(!c.acquired)++errors;c.waited=true;return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL release(XrSwapchain chain,const XrSwapchainImageReleaseInfo*){
        std::lock_guard lock(m);auto& c=chains.at(chain);if(!c.acquired||!c.waited)++errors;c.acquired=c.waited=false;return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL wait_frame(XrSession,const XrFrameWaitInfo*,XrFrameState* state){
        const auto gpu_wait=Clock::now();
        {std::unique_lock lock(m);
            if(!changed.wait_for(lock,std::chrono::seconds(2),[]{return ended==begun;}))return XR_ERROR_RUNTIME_FAILURE;
            // Assumption A (VD-like): the next frame is not handed out before
            // the GPU finished the previous submission. Assumption B: it is,
            // and a submission still running at its vsync is shown late.
            if(block_on_gpu&&!changed.wait_for(lock,std::chrono::seconds(2),[]{return completed==submitted;}))return XR_ERROR_RUNTIME_FAILURE;}
        const auto now=Clock::now();
        std::int64_t k=vsync_index(now)+1;
        std::unique_lock lock(m);gpu_wait_ms_total+=ms_since(gpu_wait,now);
        if(last_vsync>=0&&k>last_vsync+1)skipped_vsyncs+=std::uint64_t(k-last_vsync-1);
        if(k<=last_vsync)k=last_vsync+1;
        last_vsync=k;pending_vsync=k;
        lock.unlock();
        std::this_thread::sleep_until(vsync_time(k)-std::chrono::microseconds(1500)); // Wake ~1.5 ms before vsync k.
        ++waits;state->shouldRender=XR_TRUE;
        state->predictedDisplayPeriod=XrDuration(reported_period_ms*1e6);
        state->predictedDisplayTime=XrTime((k+2)*true_period_ms*1e6); // Two-frame pipeline like VD streaming.
        return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL begin(XrSession,const XrFrameBeginInfo*){
        std::lock_guard lock(m);if(begun!=ended)++errors;++begun;return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL end(XrSession,const XrFrameEndInfo* info){
        ID3D11Texture2D* source{};
        {std::lock_guard lock(m);if(begun!=ended+1)++errors;
            if(info->layerCount){const auto* scene=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
                auto it=chains.find(scene->views[0].subImage.swapchain);if(it!=chains.end())source=it->second.texture.Get();++scene_ends;}}
        // Compositor GPU work for this submission, then an event the next
        // xrWaitFrame waits for. Real VD uses the game's immediate context.
        ComPtr<ID3D11Query> query;D3D11_QUERY_DESC q{D3D11_QUERY_EVENT};gpu->device->CreateQuery(&q,&query);
        if(source&&compositor_ms>0)gpu->load(compositor_rtv.Get(),512,unsigned(compositor_ms*gpu->iterations_per_ms*4),.25f); // 512^2 is 1/4 of the calibration area.
        gpu->context->End(query.Get());gpu->context->Flush();
        {std::lock_guard lock(m);++ended;++ends;last_end_query=query;++submitted;inflight.emplace_back(query,vsync_time(pending_vsync+1));}
        changed.notify_all();return XR_SUCCESS;
    }
    static cheeky::fg::XrFns functions(){
        cheeky::fg::XrFns fn;fn.create=create;fn.destroy=destroy;fn.enumerate=enumerate;fn.acquire=acquire;fn.wait_image=wait_image;
        fn.release=release;fn.wait_frame=wait_frame;fn.begin_frame=begin;fn.end_frame=end;return fn;
    }
    static void reset(const Config& c){
        std::lock_guard lock(m);chains.clear();begun=ended=0;last_vsync=pending_vsync=-1;last_end_query.Reset();
        true_period_ms=1000./c.true_hz;reported_period_ms=1000./c.refresh_hz;compositor_ms=c.compositor_gpu_ms;origin=Clock::now();
        waits=ends=scene_ends=skipped_vsyncs=late_ends=errors=0;gpu_wait_ms_total=0;
        inflight.clear();submitted=completed=0;block_on_gpu=c.block_on_gpu;
    }
    static void start(){running=true;compositor=std::thread(compositor_loop);}
    static void stop(){running=false;if(compositor.joinable())compositor.join();}
};

struct Window {double t{},game_fps{},native_fps{},generated_fps{},skipped_per_s{},late_per_s{},game_gpu_ms_per_s{},credit_wait_ms{},clock_ms{},presented_ms{};};
struct Summary {
    const char* mode{};double native_fps{},generated_fps{},game_fps{},dropped_per_s{},skipped_per_s{},late_per_s{},gpu_share{},worst_native_fps{1e9};
    double spike_native_fps{},recovery_native_fps{};unsigned errors{};
};

inline Summary run(Gpu& gpu,const Config& c,unsigned mode,unsigned release,std::FILE* csv){
    using namespace cheeky::fg;
    Runtime::gpu=&gpu;Runtime::reset(c);Runtime::start();
    shader11_fixture::device=gpu.device;shader11_fixture::context=gpu.context;
    {D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=512;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        Runtime::compositor_target.Reset();Runtime::compositor_rtv.Reset();
        check(SUCCEEDED(gpu.device->CreateTexture2D(&d,nullptr,&Runtime::compositor_target))&&
            SUCCEEDED(gpu.device->CreateRenderTargetView(Runtime::compositor_target.Get(),nullptr,&Runtime::compositor_rtv)),"Compositor target");}
    auto presenter=std::make_unique<BufferedPresenter>();auto& p=*presenter;
    p.trace=[](const char*) noexcept {};
    std::vector<XrSwapchain> retired;p.retire_chain=[&](XrSwapchain chain){retired.push_back(chain);};
    check(p.setup11(reinterpret_cast<XrSession>(1),Runtime::functions(),gpu.device.Get(),7,&cheeky::quad_capture::d3d11_dispatch_mutex),"Pacing simulation presenter");
    p.pacing_clock_mode=mode;p.native11_release_point=release;
    const unsigned size=c.eye;const auto format=DXGI_FORMAT_R8G8B8A8_UNORM;
    auto make=[&](DXGI_FORMAT f,unsigned w,unsigned h,ComPtr<ID3D11RenderTargetView>* rtv){
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=f;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> t;
        check(SUCCEEDED(gpu.device->CreateTexture2D(&d,nullptr,&t))&&(!rtv||SUCCEEDED(gpu.device->CreateRenderTargetView(t.Get(),nullptr,rtv->GetAddressOf()))),"Pacing simulation texture");
        return t;};
    std::array<ComPtr<ID3D11RenderTargetView>,2> color_rtv;ComPtr<ID3D11RenderTargetView> depth_rtv,motion_rtv;
    std::array<ComPtr<ID3D11Texture2D>,2> color{make(format,size,size,&color_rtv[0]),make(format,size,size,&color_rtv[1])};
    auto depth=make(DXGI_FORMAT_R32_FLOAT,size,size,&depth_rtv);auto motion=make(DXGI_FORMAT_R32G32_FLOAT,size,size,&motion_rtv);
    {const float d[]{.5f,0,0,0},z[]{0,0,0,0};gpu.context->ClearRenderTargetView(depth_rtv.Get(),d);gpu.context->ClearRenderTargetView(motion_rtv.Get(),z);}
    const std::array<XrSwapchain,2> chains{reinterpret_cast<XrSwapchain>(100),reinterpret_cast<XrSwapchain>(101)};
    auto views=[&]{std::array<XrCompositionLayerProjectionView,2> v;
        for(unsigned eye=0;eye<2;++eye){v[eye]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v[eye].pose.orientation.w=1;
            v[eye].fov={-.7f,.7f,.7f,-.7f};v[eye].subImage={chains[eye],{{0,0},{int(size),int(size)}},0};}return v;}();
    auto source=[&](unsigned eye){CheekyFgShaderSource11V1 s;s.eye=eye;s.session_generation=7;s.mapping_revision=3;s.generation=1;
        s.context=gpu.context.Get();s.depth=depth.Get();s.motion=motion.Get();auto& g=s.meta;g.feature=200+eye;
        g.width=g.height=g.depth_width=g.depth_height=g.motion_width=g.motion_height=size;g.configuration=55;
        g.focus_width=g.focus_height=.5f;g.center_method=2;g.peripheral_method=cheeky_fg_previous;g.resolution_percent=g.peripheral_percent=100;g.reset=0;return s;};

    // Game GPU time per native frame, measured with timestamp queries that are
    // read back asynchronously so the game never waits for its own GPU work.
    struct Stamp {ComPtr<ID3D11Query> disjoint,begin,end;};
    std::deque<Stamp> stamps;std::mutex stamp_mutex;double game_gpu_ms_acc{};
    auto collect=[&](bool all){
        while(!stamps.empty()){auto& s=stamps.front();D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};std::uint64_t a{},b{};
            const UINT flags=all?0:D3D11_ASYNC_GETDATA_DONOTFLUSH;
            if(gpu.context->GetData(s.disjoint.Get(),&d,sizeof(d),flags)!=S_OK)break;
            if(gpu.context->GetData(s.begin.Get(),&a,sizeof(a),flags)!=S_OK||gpu.context->GetData(s.end.Get(),&b,sizeof(b),flags)!=S_OK)break;
            if(!d.Disjoint&&d.Frequency&&b>a){std::lock_guard lock(stamp_mutex);game_gpu_ms_acc+=double(b-a)*1000./double(d.Frequency);}
            stamps.pop_front();}};

    std::mutex handoff;std::condition_variable ready;std::deque<XrTime> waited;std::atomic<bool> done{},simulation_done{};
    std::atomic<unsigned> app_errors{},rendered{};
    const auto start=Clock::now();
    auto gpu_ms_now=[&]{const double t=ms_since(start)/1000.;
        const double phase=std::fmod(t,c.spike_every_s);return phase>=c.spike_every_s-c.spike_for_s?c.spike_gpu_ms:c.game_gpu_ms;};
    auto in_spike=[&](double t){return std::fmod(t,c.spike_every_s)>=c.spike_every_s-c.spike_for_s;};
    std::thread render([&]{
        for(unsigned n=0;;++n){
            XrTime time{};
            {std::unique_lock lock(handoff);ready.wait(lock,[&]{return !waited.empty()||simulation_done;});if(waited.empty())return;time=waited.front();waited.pop_front();}
            if(XR_FAILED(p.begin_frame(nullptr))){++app_errors;continue;}
            Stamp s;D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT};gpu.device->CreateQuery(&q,&s.disjoint);
            q.Query=D3D11_QUERY_TIMESTAMP;gpu.device->CreateQuery(&q,&s.begin);gpu.device->CreateQuery(&q,&s.end);
            gpu.context->Begin(s.disjoint.Get());gpu.context->End(s.begin.Get());
            // Calibrated at 1024x1024; split the frame's GPU time across both eyes.
            const double area=1024.*1024./(double(size)*size);
            for(unsigned eye=0;eye<2;++eye)gpu.load(color_rtv[eye].Get(),size,unsigned(gpu_ms_now()*gpu.iterations_per_ms*area*.5),float(n%97)/97.f+eye);
            gpu.context->End(s.end.Get());gpu.context->End(s.disjoint.Get());
            stamps.push_back(std::move(s));collect(false);
            for(unsigned eye=0;eye<2;++eye)if(!p.capture11(chains[eye],0,color[eye].Get(),size,size,1,format))++app_errors;
            for(unsigned eye=0;eye<2;++eye)p.native11->observe(source(eye));
            auto v=views;
            XrCompositionLayerProjection scene{XR_TYPE_COMPOSITION_LAYER_PROJECTION};scene.space=reinterpret_cast<XrSpace>(1);scene.views=v.data();scene.viewCount=2;
            const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&scene);XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime=time;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&layer;
            gpu.context->Flush();
            if(XR_FAILED(p.end_frame(&end,{},&scene)))++app_errors;
            ++rendered;
        }
    });
    std::thread simulation([&]{
        while(!done){
            XrFrameState state{XR_TYPE_FRAME_STATE};
            if(XR_FAILED(p.wait_frame(nullptr,&state))){if(!done)++app_errors;break;}
            std::this_thread::sleep_for(std::chrono::microseconds(unsigned(c.game_cpu_ms*1000)));
            {std::lock_guard lock(handoff);waited.push_back(state.predictedDisplayTime);}ready.notify_all();
        }
        {std::lock_guard lock(handoff);simulation_done=true;}ready.notify_all();
    });
    // Sample windows from the presenter's own counters.
    static const char* names[2][3]{{"independent","follow","follow+vsync"},{"indep/rel-gen","follow/rel-gen","follow+vsync/gen"}};
    Summary out;out.mode=names[release?1:0][std::min(mode,2u)];
    std::vector<Window> windows;
    std::uint64_t last_native{},last_generated{},last_game{},last_skipped{},last_late{};double last_gpu{},last_wait_total{};
    auto counters=[&]{std::lock_guard lock(p.mx);return std::tuple{p.distinct_native,p.generated_count,p.native_count,p.pacing_wait.mean(),p.native_clock_ms,p.presented_native_ms};};
    const double window_s=.5;
    for(double t=window_s;t<=c.seconds+1e-9;t+=window_s){
        std::this_thread::sleep_until(start+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(t)));
        const auto [native,generated,game,wait_ms,clock_ms,presented_ms]=counters();
        double gpu_ms;{std::lock_guard lock(stamp_mutex);gpu_ms=game_gpu_ms_acc;}
        Window w;w.t=t;w.native_fps=double(native-last_native)/window_s;w.generated_fps=double(generated-last_generated)/window_s;
        w.game_fps=double(game-last_game)/window_s;w.skipped_per_s=double(Runtime::skipped_vsyncs-last_skipped)/window_s;
        w.late_per_s=double(Runtime::late_ends-last_late)/window_s;w.game_gpu_ms_per_s=(gpu_ms-last_gpu)/window_s;
        w.credit_wait_ms=wait_ms;w.clock_ms=clock_ms;w.presented_ms=presented_ms;
        last_native=native;last_generated=generated;last_game=game;last_skipped=Runtime::skipped_vsyncs;last_late=Runtime::late_ends;last_gpu=gpu_ms;
        windows.push_back(w);
        if(csv)std::fprintf(csv,"%s/%s,%s,%.1f,%u,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f\n",c.name,c.block_on_gpu?"A":"B",out.mode,t,unsigned(in_spike(t-window_s/2)),w.game_fps,w.native_fps,w.generated_fps,
            w.skipped_per_s,w.late_per_s,w.game_gpu_ms_per_s/10.,w.credit_wait_ms,w.clock_ms,w.presented_ms);
    }
    done=true;
    const auto generated=p.generated_count;
    auto disable=std::async(std::launch::async,[&]{p.finish_application_frame();p.invalidate_temporal();});
    if(disable.wait_for(std::chrono::seconds(10))!=std::future_status::ready){
        std::size_t queued{};{std::lock_guard lock(handoff);queued=waited.size();}
        std::cerr<<"pacing simulation disable hung: waited="<<p.waited<<" begun="<<p.begun<<" ended="<<p.ended<<" stopping="<<p.stopping
            <<" credits="<<p.credits<<" pending="<<p.pending.size()<<" inflight="<<p.inflight_id<<" submitted_native="<<p.submitted_native_id
            <<" awaiting_generated="<<p.awaiting_generated_submission<<" raw="<<(p.raw!=nullptr)<<" queued_app_frames="<<queued<<" rendered="<<rendered
            <<" runtime begun/ended="<<Runtime::begun<<"/"<<Runtime::ended<<" gpu submitted/completed="<<Runtime::submitted<<"/"<<Runtime::completed<<"\n";
        if(std::getenv("CHEEKY_SIM_HOLD")){std::cerr<<"holding for debugger pid="<<GetCurrentProcessId()<<std::endl;Sleep(120000);}
        std::_Exit(3);}
    {std::lock_guard lock(handoff);}ready.notify_all();
    p.reset();render.join();simulation.join();collect(true);Runtime::stop();
    for(auto chain:retired)Runtime::destroy(chain);
    // Skip the first 2 s (history warm-up) in the summary.
    unsigned n{},spike_n{},recovery_n{};
    for(const auto& w:windows){if(w.t<=2)continue;++n;
        out.native_fps+=w.native_fps;out.generated_fps+=w.generated_fps;out.game_fps+=w.game_fps;
        out.skipped_per_s+=w.skipped_per_s;out.late_per_s+=w.late_per_s;out.gpu_share+=w.game_gpu_ms_per_s/10.;
        out.worst_native_fps=std::min(out.worst_native_fps,w.native_fps);
        const double mid=w.t-window_s/2;
        if(in_spike(mid)){out.spike_native_fps+=w.native_fps;++spike_n;}
        else if(std::fmod(mid,c.spike_every_s)<1.){out.recovery_native_fps+=w.native_fps;++recovery_n;}}
    if(n){out.native_fps/=n;out.generated_fps/=n;out.game_fps/=n;out.skipped_per_s/=n;out.late_per_s/=n;out.gpu_share/=n;}
    if(spike_n)out.spike_native_fps/=spike_n;if(recovery_n)out.recovery_native_fps/=recovery_n;
    out.dropped_per_s=std::max(0.,out.game_fps-out.native_fps);
    out.errors=unsigned(Runtime::errors+app_errors);
    (void)generated;
    return out;
}

inline void fg_pacing_simulation(double seconds,const char* csv_path,const char* only=nullptr){
    Gpu gpu;gpu.initialize();
    {D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=1024;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> t;ComPtr<ID3D11RenderTargetView> rtv;
        check(SUCCEEDED(gpu.device->CreateTexture2D(&d,nullptr,&t))&&SUCCEEDED(gpu.device->CreateRenderTargetView(t.Get(),nullptr,&rtv)),"Calibration target");
        gpu.calibrate(rtv.Get(),1024);}
    std::printf("Calibrated load: %.0f iterations per GPU ms at 1024x1024\n",gpu.iterations_per_ms);
    std::FILE* csv=csv_path?std::fopen(csv_path,"w"):nullptr;
    if(csv)std::fprintf(csv,"scenario,mode,t,spike,game_fps,native_fps,generated_fps,skipped_vsyncs_per_s,late_ends_per_s,game_gpu_pct,credit_wait_ms,clock_ms,presented_native_ms\n");
    // 90 Hz headset, 45 native: budget 22.2 ms per native frame.
    std::vector<Config> scenarios;
    {Config c;c.name="light";c.game_gpu_ms=c.spike_gpu_ms=3;c.seconds=seconds;scenarios.push_back(c);}
    {Config c;c.name="medium";c.game_gpu_ms=c.spike_gpu_ms=8;c.seconds=seconds;scenarios.push_back(c);}
    {Config c;c.name="spikes";c.seconds=seconds;scenarios.push_back(c);}
    {Config c;c.name="heavy";c.game_gpu_ms=19;c.spike_gpu_ms=19;c.seconds=seconds;scenarios.push_back(c);}
    {Config c;c.name="refresh-off";c.true_hz=89.9;c.game_gpu_ms=16;c.spike_gpu_ms=16;c.seconds=seconds;scenarios.push_back(c);}
    std::printf("%-14s %-17s %7s %7s %7s %8s %7s %7s %7s %7s %7s %6s\n","scenario","clock","game","native","gen","dropped","skip/s","late/s","gpu%","worst","spike","errors");
    const std::array<std::pair<unsigned,unsigned>,5> modes{{{0,0},{1,0},{2,0},{0,3},{1,3}}};
    for(auto c:scenarios)for(bool block:{true,false})for(auto [mode,release]:modes){
        if(only&&*only&&std::strcmp(only,c.name))continue;
        c.block_on_gpu=block;const std::string label=std::string(c.name)+(block?"/A":"/B");
        const auto s=run(gpu,c,mode,release,csv);
        std::printf("%-14s %-17s %7.1f %7.1f %7.1f %8.1f %7.1f %7.1f %7.1f %7.1f %7.1f %6u\n",label.c_str(),s.mode,s.game_fps,s.native_fps,s.generated_fps,
            s.dropped_per_s,s.skipped_per_s,s.late_per_s,s.gpu_share,s.worst_native_fps,s.spike_native_fps,s.errors);
        std::fflush(stdout);
    }
    if(csv)std::fclose(csv);
}
}
