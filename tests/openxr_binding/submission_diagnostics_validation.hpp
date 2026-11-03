void submission_diagnostics_validation() {
    using cheeky::openxr::SubmissionDiagnostic;
    XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection scene{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    scene.space=h<XrSpace>(10);scene.viewCount=2;scene.views=views;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};quad.space=h<XrSpace>(11);
    const XrCompositionLayerBaseHeader* headers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&scene),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=2;end.layers=headers;
    cheeky::fg::FrameLayers layers;SubmissionDiagnostic diagnostic;
    XrBaseInStructure extension{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
    end.next=&extension;
    check(!layers.prepare(&end,&scene,&diagnostic)&&diagnostic.layer==-1&&
        diagnostic.structure_type==extension.type&&std::strstr(diagnostic.reason,"frame has an unsupported extension"),
        "Frame extension rejection identifies its structure type");end.next=nullptr;
    quad.next=&extension;
    check(!layers.prepare(&end,&scene,&diagnostic)&&diagnostic.layer==1&&diagnostic.structure_type==extension.type&&layers.layers.empty(),
        "Layer extension rejection identifies the layer and leaves no partial copy");quad.next=nullptr;
    quad.type=XR_TYPE_UNKNOWN;
    check(!layers.prepare(&end,&scene,&diagnostic)&&diagnostic.layer==1&&std::strstr(diagnostic.reason,"layer type"),
        "Unsupported auxiliary layer reports its type");quad.type=XR_TYPE_COMPOSITION_LAYER_QUAD;
    XrCompositionLayerProjection auxiliary=scene;auxiliary.next=nullptr;
    headers[1]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&auxiliary);
    views[1].next=&extension;
    check(!layers.prepare(&end,&scene,&diagnostic)&&diagnostic.layer==1&&diagnostic.eye==1&&diagnostic.structure_type==extension.type,
        "Auxiliary view extension identifies the layer, eye and type");views[1].next=nullptr;headers[1]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    check(layers.prepare(&end,&scene,&diagnostic)&&!diagnostic.reason,"Valid preparation clears stale failure details");
    const auto retain=[](XrSession,XrSpace source,XrSpace& target)->std::shared_ptr<void>{
        if(source==h<XrSpace>(11))return {};target=source;return std::make_shared<int>(1);};
    check(!layers.retain_spaces(h<XrSession>(2),retain,&diagnostic)&&diagnostic.layer==1&&
        diagnostic.space==quad.space&&std::strstr(diagnostic.reason,"space could not be retained")&&layers.layers.empty(),
        "Space retention reports the failing auxiliary space and releases partial leases");
    // Scene depth remains intentionally accepted and stripped from the private color submission.
    views[0].next=&extension;
    check(layers.prepare(&end,&scene,&diagnostic)&&!diagnostic.reason&&!layers.layers[0].views[0].next&&views[0].next==&extension,
        "Diagnostics preserve scene depth handling and original application storage");views[0].next=nullptr;

    const auto not_dummy=[](XrSwapchain){return false;};
    auto selected=cheeky::openxr::select_projection(&end,not_dummy);
    check(selected.projection==&scene&&!selected.diagnostic.reason,"Stereo projection selection stays compatible");
    scene.viewCount=4;selected=cheeky::openxr::select_projection(&end,not_dummy);
    check(!selected.projection&&selected.unsupported&&selected.diagnostic.view_count==4&&selected.diagnostic.layer==0,
        "Unsupported view layout is distinct from a missing projection");scene.viewCount=2;
    headers[1]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&auxiliary);
    selected=cheeky::openxr::select_projection(&end,not_dummy);
    check(!selected.projection&&selected.ambiguous&&std::strstr(selected.diagnostic.reason,"multiple scene"),
        "Multiple scenes report ambiguity");headers[1]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    end.layerCount=0;selected=cheeky::openxr::select_projection(&end,not_dummy);
    check(!selected.projection&&selected.diagnostic.reason,"Empty submission reports missing stereo scene");end.layerCount=2;

    static std::vector<std::string> logs;logs.clear();
    cheeky::fg::BufferedPresenter p;p.trace=[](const char* message) noexcept {logs.emplace_back(message);};
    diagnostic={};diagnostic.reason="OpenXR layer space could not be retained";
    diagnostic.layer=1;diagnostic.space=quad.space;
    p.note_submission_diagnostic(diagnostic);p.note_submission_diagnostic(diagnostic);
    check(logs.size()==1&&logs[0].find("layer=1")!=std::string::npos&&logs[0].find("space=")!=std::string::npos,
        "Repeated rejection logs once with supporting metadata");
    diagnostic.space=h<XrSpace>(99);p.note_submission_diagnostic(diagnostic);
    check(logs.size()==1,"Handle churn must not spam the log");
    diagnostic.reason="OpenXR eye swapchain is not tracked";p.note_submission_diagnostic(diagnostic);
    p.note_submission_diagnostic({});p.note_submission_diagnostic({});
    check(logs.size()==3&&logs.back().find("recovered")!=std::string::npos,"Reason changes and recovery log once");
    std::cout<<"PASS OpenXR rejection reasons, extension types, retained spaces and transition-only logging\n";
}
