// Production OpenXR frame lookup and placement, without a headset or DLSS.
void stereo_alignment_validation() {
    using namespace cheeky::fg;
    reset();
    const auto session=h<XrSession>(2),missing=h<XrSession>(99);
    SessionState owner{};owner.instance=instance;owner.session=session;
    sessions.emplace(session,std::move(owner));
    auto& sample=sessions.at(session).located_history[0];
    sample.valid=true;sample.time=100;sample.space=h<XrSpace>(10);
    for(auto& p:sample.poses)p.orientation={0,0,0,1};
    std::array<XrCompositionLayerProjectionView,2> views{};
    for(auto& v:views){v.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;v.pose.orientation={0,0,0,1};
        v.fov={std::atan(-.8f),std::atan(1.2f),std::atan(1.1f),std::atan(-.9f)};
        v.subImage.imageRect={{37,59},{1200,900}};v.subImage.imageArrayIndex=1;}
    views[1].fov.angleLeft=std::atan(-1.2f);views[1].fov.angleRight=std::atan(.8f);
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space=sample.space;projection.viewCount=2;projection.views=views.data();
    const auto close_uv=[](float a,float b){return std::abs(a-b)<1.e-5f;};
    auto aligned=resolve_stereo_alignment(session,100,&projection);
    check(aligned.valid&&close_uv(aligned.u[0],.4f)&&close_uv(aligned.u[1],.6f)&&close_uv(aligned.v[0],.55f),
        "Direct alignment uses each submitted asymmetric FOV, in eye-local UV despite packing");
    check(!resolve_stereo_alignment(session,101,&projection).valid&&
        !resolve_stereo_alignment(missing,100,&projection).valid,"Missing frame/session cannot reuse old alignment");

    // Actual submitted eye rotation/FOV may differ from xrLocateViews.
    views[0].pose.orientation={0,std::sin(.1f),0,std::cos(.1f)};
    views[1].pose.orientation={0,-std::sin(.1f),0,std::cos(.1f)};
    aligned=resolve_stereo_alignment(session,100,&projection);
    check(aligned.valid&&close_uv(aligned.u[0],(.8f+std::tan(.2f))/2)&&close_uv(aligned.u[1],(1.2f-std::tan(.2f))/2),
        "Submitted eye cant is relative to the tracked common forward, not an independent eye center");
    views[0].pose.orientation={0,0,0,1};views[1].pose.orientation={0,0,0,-1};
    sample.poses[0].orientation={0,std::sin(.1f),0,std::cos(.1f)};
    sample.poses[1].orientation={0,-std::sin(.1f),0,std::cos(.1f)};
    const auto canted=resolve_stereo_alignment(session,100,&projection);
    check(canted.valid&&close_uv(canted.u[0],.4f),"Tracked cant and opposite quaternion signs preserve common forward");

    // Resolve the tracked direction in the submitted layer's space at its own time.
    static unsigned space_calls;space_calls=0;
    instances.at(instance).dispatch.locate_space=[](XrSpace from,XrSpace to,XrTime time,XrSpaceLocation* location)->XrResult{
        ++space_calls;check(from==h<XrSpace>(10)&&to==h<XrSpace>(20)&&time==100,"Frame-specific space transform");
        location->locationFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        location->pose.orientation={0,std::sin(.15f),0,std::cos(.15f)};return XR_SUCCESS;};
    projection.space=h<XrSpace>(20);
    for(auto& v:views)v.pose.orientation={0,std::sin(.15f),0,std::cos(.15f)};
    auto in_other_space=resolve_stereo_alignment(session,100,&projection);
    check(in_other_space.valid&&space_calls==1&&close_uv(in_other_space.u[0],.4f)&&close_uv(in_other_space.v[0],.55f),
        "Changing reference space does not change head-relative alignment");
    BufferedNative buffered;buffered.alignment=in_other_space;
    sample.time=200;sample.poses[0].orientation=sample.poses[1].orientation={0,std::sin(.2f),0,std::cos(.2f)};
    check(close_uv(buffered.alignment.u[0],.4f)&&!resolve_stereo_alignment(session,100,&projection).valid,
        "A newer locate cannot mutate the alignment owned by an older buffered frame");
    sample.time=100;
    instances.at(instance).dispatch.locate_space=[](XrSpace,XrSpace,XrTime,XrSpaceLocation*)->XrResult{return XR_ERROR_RUNTIME_FAILURE;};
    check(!resolve_stereo_alignment(session,100,&projection).valid,"Failed space conversion falls back instead of guessing");
    instances.at(instance).dispatch.locate_space=[](XrSpace,XrSpace,XrTime,XrSpaceLocation* location)->XrResult{
        location->locationFlags=0;return XR_SUCCESS;};
    check(!resolve_stereo_alignment(session,100,&projection).valid,"Invalid orientation flags reject space conversion");

    CheekyNrUniversalOptionsV1 nr;nr.automatic_alignment=1;nr.aligned_height_offset=.1f;
    nr.center_u[0]=.23f;nr.center_v[0]=.31f;
    CheekyFgEye guide;guide.focus_width=guide.focus_height=1;
    auto center=universal_eye_center(nr,guide,buffered.alignment,0);
    check(close_uv(center[0],.4f)&&close_uv(center[1],.6f),"Independent NR uses direct alignment plus the existing vertical bias");
    nr.automatic_alignment=0;center=universal_eye_center(nr,guide,buffered.alignment,0);
    check(close_uv(center[0],.23f)&&close_uv(center[1],.31f),"Disabling automatic alignment preserves manual placement");
    nr.automatic_alignment=1;center=universal_eye_center(nr,guide,{},0);
    check(close_uv(center[0],.23f)&&close_uv(center[1],.31f),"Unavailable XR geometry uses manual NR placement");
    nr.center_from_fg=1;guide.focus_width=guide.focus_height=.5f;guide.focus_x=.12f;guide.focus_y=-.08f;
    center=universal_eye_center(nr,guide,buffered.alignment,0);
    check(close_uv(center[0],.62f)&&close_uv(center[1],.42f),"NR retains an explicitly shared FG/gaze center without applying bias twice");

    CheekyFgColorOptionsV1 fg;fg.automatic_alignment_mask=1;fg.aligned_height_offset=.1f;
    fg.eyes[0].focus_x=.15f;fg.eyes[1].focus_x=.19f;fg.eyes[1].focus_y=-.14f;
    align_color_options(fg,buffered.alignment);
    check(close_uv(fg.eyes[0].focus_x,-.1f)&&close_uv(fg.eyes[0].focus_y,.1f)&&
        close_uv(fg.eyes[1].focus_x,.19f)&&close_uv(fg.eyes[1].focus_y,-.14f),
        "FG aligns only the settings fallback, preserving intercepted gaze centers");
    fg.automatic_alignment_mask=0;fg.eyes[0].focus_x=.17f;align_color_options(fg,buffered.alignment);
    check(close_uv(fg.eyes[0].focus_x,.17f),"Independent FG placement remains manual");
    fg.automatic_alignment_mask=3;align_color_options(fg,{});
    check(close_uv(fg.eyes[0].focus_x,.17f),"Missing geometry keeps FG fallback intact");

    CheekyXrBorderOptionsV1 boxes;boxes.enabled=boxes.show_sr=boxes.show_nr=boxes.automatic_alignment=1;
    boxes.width[0]=boxes.height[0]=.4f;boxes.width[1]=boxes.height[1]=.2f;boxes.aligned_height_offset=.1f;
    const auto borders=xr_alignment_borders(boxes,buffered.alignment,0,1000,1000);
    check(borders[0].visible&&borders[1].visible&&close_uv(borders[0].x,.2f)&&close_uv(borders[0].y,.4f)&&
        close_uv(borders[1].x,.3f)&&close_uv(borders[1].y,.5f),
        "Hook-free presentation boxes share the aligned eye center and retain independent SR/NR dimensions");
    boxes.enabled=0;
    const auto hidden=xr_alignment_borders(boxes,buffered.alignment,0,1000,1000);
    check(!hidden[0].visible&&!hidden[1].visible,"Disabling Universal boxes clears both presentation overlays");

    projection.space=sample.space;sample.poses[0].orientation=sample.poses[1].orientation={0,0,0,1};
    views[0].pose.orientation={0,0,0,0};
    check(!resolve_stereo_alignment(session,100,&projection).valid,"Degenerate submitted quaternion is rejected");
    views[0].pose.orientation={0,0,0,1};views[0].fov.angleLeft=views[0].fov.angleRight;
    check(!resolve_stereo_alignment(session,100,&projection).valid,"Degenerate submitted FOV is rejected for the whole pair");
    projection.viewCount=4;
    check(!resolve_stereo_alignment(session,100,&projection).valid,"Quad projections require their own placement path");
    reset();
    std::cout<<"PASS direct OpenXR stereo alignment: asymmetric FOV, cant, submitted geometry, frame/space ownership, NR/FG placement and fallbacks\n";
}
