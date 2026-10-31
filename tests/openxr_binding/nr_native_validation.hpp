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
    std::cout<<"PASS native NR cadence, auxiliary layers, immutable submission and inference fallback without frame generation\n";
}
