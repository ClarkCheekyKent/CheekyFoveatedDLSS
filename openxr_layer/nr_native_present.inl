// Native-cadence Universal NR. Reuses capture, NR and upload resources, but
// forwards the application's runtime frame calls without FG pacing threads.
namespace cheeky::fg {
inline XrResult BufferedPresenter::end_native_nr_frame(const XrFrameEndInfo* end,
    const XrCompositionLayerProjection* projection) noexcept {
    if(!end)return XR_ERROR_VALIDATION_FAILURE;
    bool stopped{};
    {std::lock_guard lock(mx);stopped=stopping;if(!stopped)native_nr_ending=true;}
    if(stopped)return runtime_end_frame(end);
    struct EndLease {BufferedPresenter& owner;~EndLease(){std::lock_guard lock(owner.mx);
        owner.native_nr_ending=false;owner.changed.notify_all();}} end_lease{*this};
    const auto submit_original=[&]{const auto result=runtime_end_frame(end);
        {std::lock_guard lock(mx);++ended;++native_count;changed.notify_all();}return result;};
    std::shared_ptr<Native> job;StereoAlignment alignment;bool prepared{};
    // Space retention and projection resolution enter the layer's state lock.
    // Complete them before taking the capture lock, as in the FG frame owner.
    try {
        job=std::make_shared<Native>();
        prepared=projection&&job->layers.prepare(end,projection)&&job->layers.retain_spaces(session,xr.retain_space);
        if(prepared&&xr.stereo_alignment)alignment=xr.stereo_alignment(session,end->displayTime,projection);
    }catch(...){return submit_original();}
    std::unique_lock capture_lock(capture_mutex);
    // The preceding frame's guide/input leases may retire only after its NR
    // commands complete. No game/render fence is signalled from this thread.
    backend->finish_generation();native_nr_inflight.reset();
    const auto cleanup=[&]{
        captured.clear();backend->discard_captures(false);
        capture_tokens.clear();capture_frames.clear();capture_crop_tokens.clear();capture_times.clear();capture_ready.clear();
        color_stamp_frame=color_stamp_token=0;capture_overflow=false;
    };
    const auto passthrough_native=[&](const char* reason){
        native_nr_previous.reset();cleanup();
        CheekyNrUniversalStatusV1 status;status.fallback=1;status.reset=1;status.reason=reason;
        UniversalNr12::report(status,{});
        capture_lock.unlock();
        return submit_original();
    };
    try {
        job->id=++next_id;job->epoch=epoch;
        // Unknown layer chains remain on the application's original path.
        if(!prepared||capture_overflow)
            return passthrough_native("Universal DLSS 5 requires a supported stereo OpenXR projection");
        job->alignment=alignment;
        if(backend->capture(job,projection,true,nullptr,nullptr)!=CaptureOutcome::captured)
            return passthrough_native("Universal DLSS 5 native eye capture unavailable");
        job->space=job->layers.scene_space();job->blend=end->environmentBlendMode;
        const auto runtime=GetModuleHandleW(L"CheekyFoveatedDLSSRuntime.dll");
        const auto borders_query=runtime?reinterpret_cast<CheekyXrGetBorderOptionsV1Fn>(
            GetProcAddress(runtime,"CheekyOpenXR_GetBorderOptionsV1")):nullptr;
        CheekyXrBorderOptionsV1 borders;const bool show_borders=query_border_options(borders_query,borders)&&borders.enabled;
        for(unsigned eye=0;eye<2;++eye){const auto& sub=projection->views[eye].subImage;
            job->color[eye]->alignment_borders=show_borders?xr_alignment_borders(borders,job->alignment,eye,
                unsigned(sub.imageRect.extent.width),unsigned(sub.imageRect.extent.height)):std::array<BorderRect,2>{};}
        Pair pair;native_nr_inflight=job;
        if(!backend->process_native(native_nr_previous,job,pair))
            return passthrough_native("Universal DLSS 5 native processing unavailable; using original submission");
        // NR records on the same queue as uploads; capture -> NR -> upload is
        // ordered on the GPU without an extra runtime WaitFrame or BeginFrame.
        pair.native=pair.current=job->color;pair.native_views=pair.current_views=job->views;
        pair.native_id=pair.current_id=job->id;pair.space=job->space;pair.blend=job->blend;
        pair.layers=job->layers;pair.auxiliary=job->auxiliary;
        auto views=job->views;auto layers=job->layers;
        if(!backend->publish(pair,job->color,views,layers,false))
            return passthrough_native("Universal DLSS 5 native upload unavailable; using original submission");
        XrCompositionLayerProjection replacement{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        replacement.space=pair.space;replacement.layerFlags=layers.scene_flags;replacement.viewCount=2;replacement.views=views.data();
        auto headers=layers.replace_scene(replacement);auto submitted=*end;
        submitted.layerCount=unsigned(headers.size());submitted.layers=headers.data();
        native_nr_previous=job;cleanup();capture_lock.unlock();
        const auto result=runtime_end_frame(&submitted);
        if(XR_FAILED(result))native_nr_previous.reset();
        {std::lock_guard lock(mx);++ended;++native_count;++frames;++ticks;changed.notify_all();}
        return result;
    }catch(...){
        // No modified application images: a failure can still submit the
        // original layer list once, on its original runtime frame.
        if(!capture_lock.owns_lock())return XR_ERROR_RUNTIME_FAILURE;
        return passthrough_native("Universal DLSS 5 native processing exception");
    }
}
} // namespace cheeky::fg
