void color_scale_bias_validation() {
    using cheeky::fg::FrameLayers;
    FrameLayers queued;
    {
        XrCompositionLayerColorScaleBiasKHR color{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
        color.colorScale={.5f,.75f,1.25f,.8f};color.colorBias={.1f,.2f,.3f,.4f};
        XrCompositionLayerColorScaleBiasKHR auxiliary_color=color;
        auxiliary_color.colorScale.r=.25f;auxiliary_color.colorBias.a=.6f;
        XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for(auto& view:views)view.subImage.swapchain=h<XrSwapchain>(900);
        XrCompositionLayerProjection scene{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        scene.next=&color;scene.viewCount=2;scene.views=views;scene.space=h<XrSpace>(1);
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};quad.next=&auxiliary_color;quad.space=scene.space;
        const XrCompositionLayerBaseHeader* headers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&scene),
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=2;end.layers=headers;
        cheeky::openxr::SubmissionDiagnostic diagnostic;FrameLayers captured;
        check(captured.prepare(&end,&scene,&diagnostic)&&!diagnostic.reason,
            "Scene and auxiliary color scale/bias extensions must be accepted");
        queued=captured;
        // Simulate the app reusing its stack storage before a queued FG frame submits.
        color.colorScale={};color.colorBias={};auxiliary_color.colorScale={};auxiliary_color.colorBias={};
        check(scene.next==&color&&quad.next==&auxiliary_color,"Capture must leave the application's next chains intact");
        XrBaseInStructure unknown{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};color.next=&unknown;
        check(!captured.prepare(&end,&scene,&diagnostic)&&diagnostic.layer==0&&diagnostic.structure_type==unknown.type&&
            std::strstr(diagnostic.reason,"unsupported extension")&&captured.layers.empty(),
            "Unknown tail after a supported extension must reject the whole frame with the actual tail type");
        color.next=&auxiliary_color;
        check(!captured.prepare(&end,&scene,&diagnostic)&&std::strstr(diagnostic.reason,"repeats"),
            "Duplicate color scale/bias nodes must not silently overwrite one another");
        check(color.next==&auxiliary_color,"Rejected chains remain untouched");
    }
    auto moved=std::move(queued);auto copied=moved;moved.clear();
    const auto verify=[](const XrCompositionLayerBaseHeader* header,bool auxiliary){
        const auto* color=static_cast<const XrCompositionLayerColorScaleBiasKHR*>(header->next);
        check(color&&color->type==XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR&&!color->next,
            "Submitted color metadata must use an owned, terminated extension node");
        check(color->colorScale.r==(auxiliary?.25f:.5f)&&color->colorScale.g==.75f&&color->colorScale.b==1.25f&&color->colorScale.a==.8f&&
            color->colorBias.r==.1f&&color->colorBias.g==.2f&&color->colorBias.b==.3f&&color->colorBias.a==(auxiliary?.6f:.4f),
            "Every scale and bias component must survive queued frame copies/moves and upstream storage reuse");
    };
    auto original=copied.original();verify(original[0],false);verify(original[1],true);
    const auto* original_scene=reinterpret_cast<const XrCompositionLayerProjection*>(original[0]);
    check(original_scene->views[0].subImage.swapchain==h<XrSwapchain>(900),"Queued original retains source images");
    XrCompositionLayerProjectionView output_views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for(auto& view:output_views)view.subImage.swapchain=h<XrSwapchain>(950);
    XrCompositionLayerProjection replacement{XR_TYPE_COMPOSITION_LAYER_PROJECTION};replacement.viewCount=2;replacement.views=output_views;
    for(unsigned generated=0;generated<2;++generated){
        auto output=copied.replace_scene(replacement);verify(output[0],false);verify(output[1],true);
        const auto* output_scene=reinterpret_cast<const XrCompositionLayerProjection*>(output[0]);
        check(output_scene->views==output_views&&output_scene->views[0].subImage.swapchain==h<XrSwapchain>(950)&&!replacement.next,
            "Native/generated replacement keeps private eye images and attaches metadata without mutating the caller");
    }
    original=copied.original();verify(original[0],false);verify(original[1],true);
    check(reinterpret_cast<const XrCompositionLayerProjection*>(original[0])->views[0].subImage.swapchain==h<XrSwapchain>(900),
        "Original fallback images must survive replacement submissions");
    std::cout<<"PASS owned color scale/bias on scene and auxiliary layers, queued copies/moves, replacement and original fallback\n";
}
