// The frame owner must preserve native cadence independently of GPU inference.
// A backend fixture isolates that contract from NGX/model availability.
struct NativeNrBackendFixture final : cheeky::fg::FrameBackend {
    cheeky::fg::BufferedPresenter& owner;
    unsigned processed{},generated{},published{};bool fail{};
    explicit NativeNrBackendFixture(cheeky::fg::BufferedPresenter& p):owner(p){}
    bool ready(std::uint64_t) const override{return true;}
    cheeky::fg::CaptureOutcome capture(const std::shared_ptr<cheeky::fg::BufferedNative>& job,
        const XrCompositionLayerProjection* projection,bool,const CheekyFgPair*,std::uint64_t(*)(unsigned)) override {
        for(unsigned eye=0;eye<2;++eye){job->color[eye]=std::make_shared<cheeky::fg::TransportImage>();job->views[eye]=projection->views[eye];}
        return cheeky::fg::CaptureOutcome::captured;
    }
    cheeky::fg::GenerationOutcome generate(const std::shared_ptr<cheeky::fg::BufferedNative>&,
        const std::shared_ptr<cheeky::fg::BufferedNative>&,cheeky::fg::BufferedPair&,bool&,std::uint64_t&,
        std::shared_ptr<cheeky::fg::BufferedDirectRequest>&) override {++generated;return {};}
    bool process_native(const std::shared_ptr<cheeky::fg::BufferedNative>&,
        const std::shared_ptr<cheeky::fg::BufferedNative>& job,cheeky::fg::BufferedPair&) override {
        ++processed;job->universal_nr=!fail;return !fail;
    }
    bool publish(const cheeky::fg::BufferedPair&,const std::array<cheeky::fg::Image,2>&,
        std::array<XrCompositionLayerProjectionView,2>& views,cheeky::fg::FrameLayers&,bool generated_frame) override {
        check(!generated_frame,"Native NR must never publish an interpolated image");++published;
        for(auto& v:views)v.subImage.swapchain=h<XrSwapchain>(950);return true;
    }
    void retire() override{}void finish_generation() override{}
    void retire_guides(cheeky::fg::BufferedNative&) override{}void discard_captures(bool) override{}
    void reset_generation() override{}void reset_statistics(bool) override{}
    cheeky::fg::GenerationStatus generation_status() const override{return {};}
    void snapshot(CheekyFgPerformance&,CheekyFgOverhead&,CheekyFgExperimentDiagnostics&) const override{}
    XrResult end_frame(const XrFrameEndInfo* end) override{return owner.xr.end_frame(owner.session,end);}
    bool service_direct_output(double,cheeky::fg::BufferedPair&) override{return false;}
    bool supports_direct_output() const override{return false;}
};
void native_nr_validation() {
    using namespace cheeky::fg;
    static unsigned waits,begins,ends,native_outputs,original_outputs;
    waits=begins=ends=native_outputs=original_outputs=0;
    BufferedPresenter p;p.native_nr_only=true;p.session=h<XrSession>(2);
    auto backend=std::make_unique<NativeNrBackendFixture>(p);auto* fixture=backend.get();p.backend=std::move(backend);
    p.xr.wait_frame=[](XrSession,const XrFrameWaitInfo*,XrFrameState* out)->XrResult{
        out->predictedDisplayTime=XrTime(++waits)*100;out->predictedDisplayPeriod=100;out->shouldRender=XR_TRUE;return XR_SUCCESS;};
    p.xr.begin_frame=[](XrSession,const XrFrameBeginInfo*)->XrResult{++begins;return XR_SUCCESS;};
    p.xr.end_frame=[](XrSession,const XrFrameEndInfo* end)->XrResult{
        ++ends;check(end->displayTime==XrTime(ends)*100,"Native NR preserves the application's display time");
        check(end->layerCount==2&&end->layers[1]->type==XR_TYPE_COMPOSITION_LAYER_QUAD,"Native NR preserves auxiliary layers");
        const auto* scene=reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[0]);
        if(scene->views[0].subImage.swapchain==h<XrSwapchain>(950))++native_outputs;else ++original_outputs;
        return XR_SUCCESS;};
    std::array<XrCompositionLayerProjectionView,2> views{};
    for(auto& v:views){v.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;v.pose.orientation.w=1;
        v.fov={-.7f,.7f,.7f,-.7f};v.subImage={h<XrSwapchain>(900),{{0,0},{64,64}},0};}
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space=h<XrSpace>(1);projection.views=views.data();projection.viewCount=2;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};quad.space=projection.space;
    const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
    for(unsigned frame=0;frame<3;++frame){
        XrFrameState state{XR_TYPE_FRAME_STATE};check(p.wait_frame(nullptr,&state)==XR_SUCCESS&&state.predictedDisplayPeriod==100,
            "Native NR must not multiply the runtime cadence");
        check(p.begin_frame(nullptr)==XR_SUCCESS,"Native NR forwards BeginFrame");
        fixture->fail=frame==2;
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=state.predictedDisplayTime;end.layerCount=2;end.layers=layers;
        check(p.end_frame(&end,{},&projection)==XR_SUCCESS,"Native NR processes or safely forwards this same frame");
    }
    check(waits==3&&begins==3&&ends==3&&native_outputs==2&&original_outputs==1,
        "Native NR adds no private runtime frames and inference failure submits the original pair exactly once");
    check(fixture->processed==3&&fixture->generated==0&&fixture->published==2&&!p.started&&
        !p.presentation_thread.joinable()&&!p.generation_thread.joinable()&&!p.release_thread.joinable(),
        "Native NR requires neither FG features nor pacing/generation threads");
    check(views[0].subImage.swapchain==h<XrSwapchain>(900),"Native publication leaves application layer storage unchanged");
    // Exercise the real EndFrame interceptor: selection failures must reach the
    // native presenter without being flattened into a generic projection error.
    reset();swapchains.clear();const auto session=h<XrSession>(2);
    auto native=std::make_shared<BufferedPresenter>();native->native_nr_only=true;native->session=session;
    auto rejected_backend=std::make_unique<NativeNrBackendFixture>(*native);auto* rejected_fixture=rejected_backend.get();
    native->backend=std::move(rejected_backend);
    static unsigned color_submissions;color_submissions=0;
    native->xr.end_frame=[](XrSession,const XrFrameEndInfo* original)->XrResult{
        check(original->layerCount==2&&original->layers[0]->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            "Rejected native frame retains the original layers");
        if(original->layers[0]->next){
            const auto* color=static_cast<const XrCompositionLayerColorScaleBiasKHR*>(original->layers[0]->next);
            // The earlier unknown-extension rejection is still an original passthrough.
            if(color->type==XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR){
                check(color->colorScale.r==.5f&&color->colorBias.b==.25f&&!color->next,
                    "Native NR runtime submission must preserve scene color scale/bias");++color_submissions;}
        }
        return XR_SUCCESS;};
    instances.at(instance).dispatch.end_frame=native->xr.end_frame;
    SessionState owner{};owner.instance=instance;owner.session=session;owner.running=true;
    owner.view_configuration=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;owner.fg=native;
    sessions.emplace(session,std::move(owner));
    XrFrameEndInfo rejected{XR_TYPE_FRAME_END_INFO};rejected.layerCount=2;rejected.layers=layers;
    check(cheeky_xrEndFrame(session,&rejected)==XR_SUCCESS&&native->last_submission_diagnostic.eye==0&&
        native->last_submission_diagnostic.swapchain==h<XrSwapchain>(900)&&
        std::strstr(native->last_submission_diagnostic.reason,"not tracked"),"Untracked swapchain reason reaches native NR");
    SwapchainState chain;chain.session=session;swapchains.emplace(h<XrSwapchain>(900),std::move(chain));
    check(cheeky_xrEndFrame(session,&rejected)==XR_SUCCESS&&
        std::strstr(native->last_submission_diagnostic.reason,"no released image"),"Unreleased swapchain is a separate rejection");
    swapchains.at(h<XrSwapchain>(900)).has_released=true;
    XrBaseInStructure unsupported{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};projection.next=&unsupported;
    check(cheeky_xrEndFrame(session,&rejected)==XR_SUCCESS&&native->last_submission_diagnostic.layer==0&&
        native->last_submission_diagnostic.structure_type==unsupported.type&&
        std::strstr(native->last_submission_diagnostic.reason,"layer has an unsupported extension"),
        "Layer preparation reports the rejected extension after swapchain validation");projection.next=nullptr;
    check(rejected_fixture->processed==0,"Rejected frames do not attempt inference");
    native->capture_overflow=true;
    check(cheeky_xrEndFrame(session,&rejected)==XR_SUCCESS&&
        std::strstr(native->last_submission_diagnostic.reason,"capture overflow"),"Capture overflow has its own NR rejection");
    check(cheeky_xrEndFrame(session,&rejected)==XR_SUCCESS&&!native->last_submission_diagnostic.reason&&
        rejected_fixture->processed==1,"A later compatible frame processes and clears the rejection");
    XrCompositionLayerColorScaleBiasKHR color{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
    color.colorScale={.5f,1,1,1};color.colorBias={0,0,.25f,0};projection.next=&color;
    check(cheeky_xrEndFrame(session,&rejected)==XR_SUCCESS&&!native->last_submission_diagnostic.reason&&
        rejected_fixture->processed==2&&color_submissions==1&&projection.next==&color,
        "Color scale/bias must reach native NR processing and the final runtime submission");
    rejected_fixture->fail=true;
    check(cheeky_xrEndFrame(session,&rejected)==XR_SUCCESS&&color_submissions==2&&projection.next==&color,
        "Inference fallback preserves the application's original color metadata");projection.next=nullptr;
    sessions.clear();swapchains.clear();
    std::cout<<"PASS native NR cadence, auxiliary layers, immutable submission and inference fallback without frame generation\n";
}
