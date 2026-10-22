// Included after the layer's state and dispatch declarations.
bool quad_capture_executable(const wchar_t* name) noexcept {
    wchar_t path[32768]{};
    const auto length=GetModuleFileNameW(nullptr,path,static_cast<DWORD>(std::size(path)));
    if(!length||length>=std::size(path))return false;
    const auto* file=std::wcsrchr(path,L'\\');
    return _wcsicmp(file?file+1:path,name)==0;
}
bool msfs_quad_version_bridge_host() noexcept {return quad_capture_executable(L"FlightSimulator2024.exe");}
bool dcs_quad_capture_host() noexcept {return quad_capture_executable(L"DCS.exe");}

XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrLocateSpaces(XrSession session,const XrSpacesLocateInfo* info,XrSpaceLocations* output) {
    Dispatch dispatch;bool bridge{};
    {std::lock_guard lock(state_mutex);auto* instance=find_instance_for_session_locked(session);if(!instance)return XR_ERROR_HANDLE_INVALID;
        dispatch=instance->dispatch;bridge=instance->msfs_version_bridge;}
    if(dispatch.locate_spaces&&!bridge)return dispatch.locate_spaces(session,info,output);
    if(!bridge||!dispatch.locate_space)return XR_ERROR_FUNCTION_UNSUPPORTED;
    if(!info||!output||info->type!=XR_TYPE_SPACES_LOCATE_INFO||output->type!=XR_TYPE_SPACE_LOCATIONS||
       info->next||!info->spaceCount||!info->spaces||output->locationCount!=info->spaceCount||!output->locations)return XR_ERROR_VALIDATION_FAILURE;
    XrSpaceVelocities* velocities{};
    for(auto* next=static_cast<XrBaseOutStructure*>(output->next);next;next=next->next){
        if(next->type!=XR_TYPE_SPACE_VELOCITIES||velocities)return XR_ERROR_VALIDATION_FAILURE;
        velocities=reinterpret_cast<XrSpaceVelocities*>(next);
        if(velocities->velocityCount!=info->spaceCount||!velocities->velocities)return XR_ERROR_VALIDATION_FAILURE;
    }
    // Keep caller arrays unchanged if a later space fails. Every location uses
    // the original base space and time, including optional velocity queries.
    std::vector<XrSpaceLocationData> locations(info->spaceCount);
    std::vector<XrSpaceVelocityData> speeds(velocities?info->spaceCount:0);
    XrResult result=XR_SUCCESS;
    for(uint32_t i=0;i<info->spaceCount;++i){
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if(velocities)location.next=&velocity;
        const auto located=dispatch.locate_space(info->spaces[i],info->baseSpace,info->time,&location);
        if(XR_FAILED(located))return located;
        if(located!=XR_SUCCESS)result=located;
        locations[i]={location.locationFlags,location.pose};
        if(velocities)speeds[i]={velocity.velocityFlags,velocity.linearVelocity,velocity.angularVelocity};
    }
    std::copy(locations.begin(),locations.end(),output->locations);
    if(velocities)std::copy(speeds.begin(),speeds.end(),velocities->velocities);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEnumerateViewConfigurations(XrInstance instance,XrSystemId system,
    uint32_t capacity,uint32_t* count,XrViewConfigurationType* output) {
    Dispatch dispatch;bool enabled{},prefer_quad{};
    const auto finish=[&](XrResult result){
        const bool count_valid=count&&(XR_SUCCEEDED(result)||result==XR_ERROR_SIZE_INSUFFICIENT);
        log_startup("QUAD-CAPTURE enumerate_configurations instance=%p system=%llu capacity=%u result=%d count_valid=%u count=%u provider=%u prefer_quad=%u\n",
            instance,static_cast<unsigned long long>(system),capacity,int(result),unsigned(count_valid),count_valid?*count:0,unsigned(enabled),unsigned(prefer_quad));
        if(XR_SUCCEEDED(result)&&capacity&&count&&output)for(uint32_t i=0;i<(std::min)(capacity,*count);++i)
            log_startup("QUAD-CAPTURE offered configuration=%d preference=%u instance=%p\n",int(output[i]),i,instance);
        return result;
    };
    {std::lock_guard lock(state_mutex);auto it=instances.find(instance);if(it==instances.end())return finish(XR_ERROR_HANDLE_INVALID);
        dispatch=it->second.dispatch;enabled=it->second.quad_capture_enabled;prefer_quad=it->second.quad_capture_prefer_quad;}
    if(!dispatch.enumerate_view_configurations)return finish(XR_ERROR_FUNCTION_UNSUPPORTED);
    if(!enabled)return finish(dispatch.enumerate_view_configurations(instance,system,capacity,count,output));
    uint32_t n{};auto result=dispatch.enumerate_view_configurations(instance,system,0,&n,nullptr);if(XR_FAILED(result))return finish(result);
    std::vector<XrViewConfigurationType> values(n);
    if(n){result=dispatch.enumerate_view_configurations(instance,system,n,&n,values.data());if(XR_FAILED(result))return finish(result);
        if(n>values.size())return finish(XR_ERROR_RUNTIME_FAILURE);values.resize(n);}
    if(std::find(values.begin(),values.end(),XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)!=values.end()&&
       std::find(values.begin(),values.end(),cheeky::quad_capture::configuration)==values.end())values.push_back(cheeky::quad_capture::configuration);
    if(prefer_quad){const auto quad=std::find(values.begin(),values.end(),cheeky::quad_capture::configuration);
        if(quad!=values.end())std::rotate(values.begin(),quad,quad+1);}
    return finish(cheeky::quad_capture::enumerate(values,capacity,count,output));
}
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEnumerateViewConfigurationViews(XrInstance instance,XrSystemId system,
    XrViewConfigurationType type,uint32_t capacity,uint32_t* count,XrViewConfigurationView* output) {
    Dispatch dispatch;bool enabled{};
    const auto finish=[&](XrResult result){
        const bool count_valid=count&&(XR_SUCCEEDED(result)||result==XR_ERROR_SIZE_INSUFFICIENT);
        log_startup("QUAD-CAPTURE enumerate_views instance=%p system=%llu configuration=%d capacity=%u result=%d count_valid=%u count=%u provider=%u\n",
            instance,static_cast<unsigned long long>(system),int(type),capacity,int(result),unsigned(count_valid),count_valid?*count:0,unsigned(enabled));
        if(XR_SUCCEEDED(result)&&capacity&&count&&output)for(uint32_t i=0;i<(std::min)(capacity,*count);++i)
            log_startup("QUAD-CAPTURE view_description configuration=%d view=%u recommended=%ux%u maximum=%ux%u samples=%u/%u\n",
                int(type),i,output[i].recommendedImageRectWidth,output[i].recommendedImageRectHeight,output[i].maxImageRectWidth,output[i].maxImageRectHeight,
                output[i].recommendedSwapchainSampleCount,output[i].maxSwapchainSampleCount);
        return result;
    };
    {std::lock_guard lock(state_mutex);auto it=instances.find(instance);if(it==instances.end())return finish(XR_ERROR_HANDLE_INVALID);
        dispatch=it->second.dispatch;enabled=it->second.quad_capture_enabled;}
    if(!dispatch.enumerate_view_configuration_views)return finish(XR_ERROR_FUNCTION_UNSUPPORTED);
    if(!enabled||!cheeky::quad_capture::is_quad(type))return finish(dispatch.enumerate_view_configuration_views(instance,system,type,capacity,count,output));
    if(!count||(capacity&&!output))return finish(XR_ERROR_VALIDATION_FAILURE);
    *count=4;if(!capacity)return finish(XR_SUCCESS);if(capacity<4)return finish(XR_ERROR_SIZE_INSUFFICIENT);
    std::array<XrViewConfigurationView,2> stereo{{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}}};uint32_t n{};
    const auto result=dispatch.enumerate_view_configuration_views(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&n,stereo.data());
    if(XR_FAILED(result))return finish(result);if(n!=2)return finish(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);
    for(unsigned i=0;i<4;++i){
        if(output[i].type!=XR_TYPE_VIEW_CONFIGURATION_VIEW)return finish(XR_ERROR_VALIDATION_FAILURE);
        // No quad-view configuration extension is advertised by this fixed provider.
        if(output[i].next)return finish(XR_ERROR_VALIDATION_FAILURE);
        output[i]=stereo[i%2];output[i].recommendedImageRectWidth=(std::max)(1u,output[i].recommendedImageRectWidth/2);
        output[i].recommendedImageRectHeight=(std::max)(1u,output[i].recommendedImageRectHeight/2);
        output[i].recommendedSwapchainSampleCount=output[i].maxSwapchainSampleCount=1;
        log_startup("QUAD-CAPTURE recommended view=%u role=%s eye=%u size=%ux%u\n",i,i<2?"peripheral":"focus",i%2,
            output[i].recommendedImageRectWidth,output[i].recommendedImageRectHeight);
    }
    return finish(result);
}
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetViewConfigurationProperties(XrInstance instance,XrSystemId system,
    XrViewConfigurationType type,XrViewConfigurationProperties* properties) {
    Dispatch dispatch;bool enabled{};
    const auto finish=[&](XrResult result){
        log_startup("QUAD-CAPTURE configuration_properties instance=%p system=%llu configuration=%d result=%d provider=%u fov_mutable=%u\n",
            instance,static_cast<unsigned long long>(system),int(type),int(result),unsigned(enabled),unsigned(XR_SUCCEEDED(result)&&properties?properties->fovMutable:0));
        return result;
    };
    {std::lock_guard lock(state_mutex);auto it=instances.find(instance);if(it==instances.end())return finish(XR_ERROR_HANDLE_INVALID);
        dispatch=it->second.dispatch;enabled=it->second.quad_capture_enabled;}
    if(!dispatch.get_view_configuration_properties)return finish(XR_ERROR_FUNCTION_UNSUPPORTED);
    const bool quad=enabled&&cheeky::quad_capture::is_quad(type);
    const auto result=dispatch.get_view_configuration_properties(instance,system,quad?XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO:type,properties);
    if(XR_SUCCEEDED(result)&&quad&&properties)properties->viewConfigurationType=type;
    return finish(result);
}
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrEnumerateEnvironmentBlendModes(XrInstance instance,XrSystemId system,
    XrViewConfigurationType type,uint32_t capacity,uint32_t* count,XrEnvironmentBlendMode* modes) {
    Dispatch dispatch;bool enabled{};
    const auto finish=[&](XrResult result){
        const bool count_valid=count&&(XR_SUCCEEDED(result)||result==XR_ERROR_SIZE_INSUFFICIENT);
        log_startup("QUAD-CAPTURE blend_modes instance=%p system=%llu configuration=%d capacity=%u result=%d count_valid=%u count=%u provider=%u\n",
            instance,static_cast<unsigned long long>(system),int(type),capacity,int(result),unsigned(count_valid),count_valid?*count:0,unsigned(enabled));
        return result;
    };
    {std::lock_guard lock(state_mutex);auto it=instances.find(instance);if(it==instances.end())return finish(XR_ERROR_HANDLE_INVALID);
        dispatch=it->second.dispatch;enabled=it->second.quad_capture_enabled;}
    if(!dispatch.enumerate_environment_blend_modes)return finish(XR_ERROR_FUNCTION_UNSUPPORTED);
    const auto result=dispatch.enumerate_environment_blend_modes(instance,system,
        enabled&&cheeky::quad_capture::is_quad(type)?XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO:type,capacity,count,modes);
    return finish(result);
}
XRAPI_ATTR XrResult XRAPI_CALL cheeky_xrGetVisibilityMaskKHR(XrSession session,XrViewConfigurationType type,
    uint32_t view,XrVisibilityMaskTypeKHR mask_type,XrVisibilityMaskKHR* mask) {
    Dispatch dispatch;bool quad{};
    {std::lock_guard lock(state_mutex);auto* instance=find_instance_for_session_locked(session);if(!instance)return XR_ERROR_HANDLE_INVALID;
        dispatch=instance->dispatch;quad=instance->quad_capture_enabled&&cheeky::quad_capture::is_quad(type);}
    if(quad){
        if(!mask||mask->type!=XR_TYPE_VISIBILITY_MASK_KHR)return XR_ERROR_VALIDATION_FAILURE;
        if(view>=4)return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
        // Empty visibility meshes are allowed. A stereo hidden-area mask must
        // never be reused in an inset projection with a different FOV.
        mask->vertexCountOutput=mask->indexCountOutput=0;return XR_SUCCESS;
    }
    return dispatch.get_visibility_mask?dispatch.get_visibility_mask(session,type,view,mask_type,mask):XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult quad_capture_locate(XrSession session,const XrViewLocateInfo* info,XrViewState* state,
    uint32_t capacity,uint32_t* count,XrView* views,PFN_xrLocateViews next) {
    if(!info||!state||!count||(capacity&&!views))return XR_ERROR_VALIDATION_FAILURE;
    *count=4;if(!capacity)return XR_SUCCESS;if(capacity<4)return XR_ERROR_SIZE_INSUFFICIENT;
    for(unsigned i=0;i<4;++i)if(views[i].type!=XR_TYPE_VIEW)return XR_ERROR_VALIDATION_FAILURE;
    auto forwarded=*info;forwarded.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::array<XrView,2> stereo{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};uint32_t n{};
    const auto result=next(session,&forwarded,state,2,&n,stereo.data());if(XR_FAILED(result))return result;
    if(n!=2)return XR_ERROR_RUNTIME_FAILURE;
    for(unsigned i=0;i<4;++i){views[i].pose=stereo[i%2].pose;views[i].fov=i<2?stereo[i].fov:cheeky::quad_capture::inset_fov(stereo[i%2].fov);}
    bool diagnostic{};
    {std::lock_guard lock(state_mutex);auto it=sessions.find(session);if(it!=sessions.end()){
        diagnostic=++it->second.quad_capture_locates<=4;
        if(it->second.quad_capture_correct_focus_fov){
            // DCS has submitted outer FOVs for its focus views. Preserve the
            // exact located FOVs by display time, not just the latest query.
            auto& history=it->second.quad_capture_fovs;
            history[info->displayTime]={views[0].fov,views[1].fov,views[2].fov,views[3].fov};
            while(history.size()>32)history.erase(history.begin());
        }
    }}
    if(diagnostic)for(unsigned i=0;i<4;++i)log_startup("QUAD-CAPTURE locate view=%u role=%s eye=%u fov=%.6f,%.6f,%.6f,%.6f time=%lld\n",
        i,i<2?"peripheral":"focus",i%2,views[i].fov.angleLeft,views[i].fov.angleRight,views[i].fov.angleUp,views[i].fov.angleDown,static_cast<long long>(info->displayTime));
    return result;
}

XrResult quad_capture_end(XrSession session,const XrFrameEndInfo* info,const Dispatch& dispatch,
    const std::shared_ptr<cheeky::quad_capture::Compositor>& compositor,
    const std::shared_ptr<cheeky::fg::Presenter>& active_presenter={}) {
    const auto presenter=std::dynamic_pointer_cast<cheeky::fg::BufferedPresenter>(active_presenter);
    const auto native11=presenter&&presenter->native11?presenter:nullptr;
    struct FinishInterval{cheeky::quad_capture::QuadFg11* guides;~FinishInterval(){if(guides)guides->finish();}};
    FinishInterval interval{compositor->d3d11?compositor->d3d11->fg_guides.get():nullptr};
    if(!info||(info->layerCount&&!info->layers))return XR_ERROR_VALIDATION_FAILURE;
    const XrCompositionLayerProjection* scene{};unsigned scene_index{};
    for(unsigned i=0;i<info->layerCount;++i){
        if(!info->layers[i])return XR_ERROR_VALIDATION_FAILURE;
        if(info->layers[i]->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION)continue;
        if(scene)return XR_ERROR_LAYER_INVALID;scene=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);scene_index=i;
    }
    if(!scene)return presenter?presenter->end_quad_frame(info,nullptr,{}):dispatch.end_frame(session,info);
    if(scene->viewCount!=4||!scene->views)return XR_ERROR_LAYER_INVALID;
    std::array<XrCompositionLayerProjectionView,4> corrected;
    std::copy_n(scene->views,4,corrected.begin());
    auto projection=*scene;projection.views=corrected.data();
    std::array<cheeky::quad_capture::Input,4> inputs{};bool diagnostic{};
    {std::lock_guard lock(state_mutex);auto owner=sessions.find(session);if(owner==sessions.end())return XR_ERROR_HANDLE_INVALID;
        diagnostic=++owner->second.quad_capture_frames<=8;
        if(owner->second.quad_capture_correct_focus_fov){
            auto& history=owner->second.quad_capture_fovs;const auto found=history.find(info->displayTime);
            if(found!=history.end()){
                corrected[2].fov=found->second[2];corrected[3].fov=found->second[3];
                // Keep the matching sample for multiple submissions at the same time.
                history.erase(history.begin(),found);
            }else{
                // This provider uses a fixed mathematical inset, so it can be
                // reconstructed from the submitted outer FOV without borrowing
                // another frame's sample. No synthetic eye tracking is involved.
                for(unsigned eye=0;eye<2;++eye)corrected[eye+2].fov=cheeky::quad_capture::inset_fov(corrected[eye].fov);
                if(diagnostic)log_startup("QUAD-CAPTURE focus FOV fixed fallback time=%lld\n",static_cast<long long>(info->displayTime));
            }
        }
        for(unsigned i=0;i<4;++i){
            const auto& image=scene->views[i].subImage;auto chain=swapchains.find(image.swapchain);
            if(chain==swapchains.end()||chain->second.session!=session||!chain->second.has_released||
               chain->second.released_index>=chain->second.calibration_images.size())return XR_ERROR_LAYER_INVALID;
            auto* resource=chain->second.calibration_images[chain->second.released_index];
            inputs[i].image=image;inputs[i].format=static_cast<DXGI_FORMAT>(chain->second.create_info.format);
            if(compositor->d3d11)compositor->d3d11->source_chains[i]=image.swapchain;
            if(owner->second.graphics_api==11)inputs[i].texture11=static_cast<ID3D11Texture2D*>(resource);
            else inputs[i].texture=static_cast<ID3D12Resource*>(resource);
            if(diagnostic)log_startup("QUAD-CAPTURE submit input_views=4 view=%u role=%s eye=%u chain=%p resource=%p released=%u slice=%u rect=%d,%d,%d,%d format=%lld\n",
                i,i<2?"peripheral":"focus",i%2,image.swapchain,resource,chain->second.released_index,image.imageArrayIndex,
                image.imageRect.offset.x,image.imageRect.offset.y,image.imageRect.extent.width,image.imageRect.extent.height,static_cast<long long>(chain->second.create_info.format));
        }
    }
    // The fixed provider promises matching poses for each context/inset pair.
    for(unsigned eye=0;eye<2;++eye){
        const auto& a=scene->views[eye].pose;const auto& b=scene->views[eye+2].pose;
        if(std::memcmp(&a,&b,sizeof(a))!=0)return XR_ERROR_POSE_INVALID;
    }
    std::array<XrCompositionLayerProjectionView,2> views{};
    uint64_t mapping_ticket{};
    if(compositor->d3d11&&compositor->d3d11->mapping){
        std::array<cheeky::quad_capture::RoleMapping11::Input,4> targets{};
        for(unsigned i=0;i<4;++i){const auto& input=inputs[i];const auto& rect=input.image.imageRect;
            targets[i]={input.texture11,input.image.imageArrayIndex,unsigned(rect.offset.x),unsigned(rect.offset.y),
                unsigned(rect.extent.width),unsigned(rect.extent.height),canonical_resource_identity(input.texture11)};}
        mapping_ticket=compositor->d3d11->mapping->capture(targets,info->displayTime);
    }
    const auto complete_mapping=[&](bool success){if(compositor->d3d11&&compositor->d3d11->mapping)
        compositor->d3d11->mapping->complete(mapping_ticket,success);};
    auto replacement=*scene;replacement.viewCount=2;replacement.views=views.data();
    std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers,info->layers+info->layerCount);
    layers[scene_index]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&replacement);
    auto forwarded=*info;forwarded.layers=layers.data();
    XrResult result=XR_SUCCESS;
    if(!presenter&&compositor->d3d11&&compositor->d3d11->fg_bridge){
        // Prove the four-view guide/sharing path while the application still
        // owns ordinary runtime frames. No color bridge or pacing takeover.
        auto& eleven=*compositor->d3d11;auto roles=eleven.mapping->snapshot();roles.active=1;
        std::array<unsigned,2> widths{},heights{};std::array<std::array<float,4>,2> insets;
        for(unsigned eye=0;eye<2;++eye){widths[eye]=compositor->recommended[eye].recommendedImageRectWidth;
            heights[eye]=compositor->recommended[eye].recommendedImageRectHeight;
            insets[eye]=cheeky::quad_capture::inset_rect(corrected[eye].fov,corrected[eye+2].fov);}
        const bool was_ready=eleven.fg_guides->ready();
        auto probe=eleven.fg_guides->build(roles,projection,widths,heights,insets,*eleven.fg_bridge);
        if(probe.pair.capture_token&&!was_ready)log_startup("QUAD-FG four-view guide preflight ready; presentation eligible at next frame boundary\n");
    }
    if(native11&&compositor->d3d11){
        // Native D3D11 shader FG: full-eye guide atlases, composed eye colors,
        // then the shared buffered pacing. No D3D12 bridge.
        auto& eleven=*compositor->d3d11;auto roles=eleven.mapping->snapshot();roles.active=1;
        std::array<std::array<float,4>,2> insets;
        for(unsigned eye=0;eye<2;++eye)insets[eye]=cheeky::quad_capture::inset_rect(corrected[eye].fov,corrected[eye+2].fov);
        auto guides=eleven.fg_guides->build_native(roles,projection,insets);
        if(guides.valid)for(unsigned eye=0;eye<2;++eye){
            auto meta=guides.meta[eye];meta.focus_feather=2.f/std::max(1u,std::min(compositor->recommended[eye].recommendedImageRectWidth,
                compositor->recommended[eye].recommendedImageRectHeight));
            native11->native11->adopt_quad_guides(eye,guides.depth[eye].Get(),guides.motion[eye].Get(),
                guides.depth_srv[eye].Get(),guides.motion_srv[eye].Get(),meta);
        }
        std::array<XrSwapchain,4> sources{};for(unsigned i=0;i<4;++i)sources[i]=inputs[i].image.swapchain;
        native11->set_quad_sources11(sources);
        eleven.capture_output=[&](unsigned,XrSwapchain chain,unsigned,ID3D11Texture2D* texture,unsigned,unsigned,DXGI_FORMAT format){
            native11->capture_composed11(chain,texture,format);};
        result=compositor->compose(inputs,projection,views);eleven.capture_output={};
        if(result==XR_SUCCESS)result=native11->end_frame(&forwarded,{},&replacement);
    }else if(presenter&&compositor->d3d11&&presenter->interop11){
        auto& eleven=*compositor->d3d11;auto roles=eleven.mapping->snapshot();roles.active=1;
        std::array<unsigned,2> widths{},heights{};
        for(unsigned eye=0;eye<2;++eye){widths[eye]=compositor->recommended[eye].recommendedImageRectWidth;heights[eye]=compositor->recommended[eye].recommendedImageRectHeight;
            views[eye]=corrected[eye];views[eye].next=nullptr;views[eye].subImage.imageRect={{0,0},{int32_t(widths[eye]),int32_t(heights[eye])}};views[eye].subImage.imageArrayIndex=0;}
        std::array<std::array<float,4>,2> insets;
        for(unsigned eye=0;eye<2;++eye)insets[eye]=cheeky::quad_capture::inset_rect(corrected[eye].fov,corrected[eye+2].fov);
        auto guides=eleven.fg_guides->build(roles,projection,widths,heights,insets,*presenter->interop11);
        // Native color publication does not require an inference lease. Keep
        // the packed raw views even if a guide pool slot is temporarily busy.
        const bool packed=presenter->capture_quad_planes(projection,replacement,insets,guides.pair);
        static thread_local uint64_t fg_submissions{};
        const auto fg_submission=++fg_submissions;
        if(fg_submission<=3||fg_submission%240==0){const auto stats=eleven.fg_guides->stats();
            log_startup("QUAD-FG submit guides=%u packed=%u roles=%u manual=%u observations=%llu snapshots=%llu pairs=%llu capture_stage=%u assembly_stage=%u\n",
                unsigned(guides.pair.capture_token!=0),unsigned(packed),roles.status,roles.manual_mode,stats.observations,stats.snapshots,stats.guide_pairs,stats.capture_stage,stats.assembly_stage);}
        if(packed){
            // The raw center/peripheral planes remain separate in storage.
            // DLSS-G runs on the raw center; final color composition is last.
            if(diagnostic)log_startup("QUAD-FG raw planes before composition four_view_guides=%u token=%llu\n",unsigned(guides.pair.capture_token!=0),guides.pair.capture_token);
            result=presenter->end_quad_frame(&forwarded,&replacement,guides.pair,[&]{return compositor->compose(inputs,projection,views);});
        }else{
            // Missing assignments/guides or capture pressure uses the proven
            // native compositor. A null projection selects coordinated runtime
            // passthrough; no precomposed colors enter the FG upload path.
            result=presenter->end_quad_frame(&forwarded,nullptr,{},[&]{return compositor->compose(inputs,projection,views);});
        }
    }else{
        result=compositor->compose(inputs,projection,views);
        if(result==XR_SUCCESS)result=dispatch.end_frame(session,&forwarded);
    }
    complete_mapping(result==XR_SUCCESS);
    if(diagnostic)log_startup("QUAD-CAPTURE submit output_views=2 result=%d\n",int(result));
    return result;
}

// Optional, versioned role bridge. Retain compositor ownership, then drop the
// session lock before GPU work or mapper locks. No consumer callbacks occur.
std::shared_ptr<cheeky::quad_capture::Compositor> quad_role_owner(uint64_t* generation=nullptr,ID3D11Device* device=nullptr) {
    std::lock_guard lock(state_mutex);std::shared_ptr<cheeky::quad_capture::Compositor> result;
    for(const auto& [handle,owner]:sessions)if(owner.running&&owner.quad_capture&&owner.graphics_api==11&&
        (!device||owner.graphics_device==device)){
        if(result)return {};result=owner.quad_capture;if(generation)*generation=owner.generation;}
    return result;
}
extern "C" __declspec(dllexport) uint32_t __cdecl CheekyQuad_GetViewsV1(CheekyQuadViewsV1* output) noexcept {
    if(!output||output->version!=CHEEKY_QUAD_VIEWS_VERSION||output->size!=sizeof(*output))return 0;
    try {uint64_t generation{};const auto owner=quad_role_owner(&generation);
        CheekyQuadViewsV1 value{};value.version=CHEEKY_QUAD_VIEWS_VERSION;value.size=sizeof(value);
        if(owner&&owner->d3d11&&owner->d3d11->mapping){value=owner->d3d11->mapping->snapshot();value.active=1;value.graphics_api=11;value.session_generation=generation;}
        *output=value;return 1;
    }catch(...){return 0;}
}
extern "C" __declspec(dllexport) void* __cdecl CheekyQuad_AcquireD3D11V1(void* context) noexcept {
    if(!context)return nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device;static_cast<ID3D11DeviceContext*>(context)->GetDevice(&device);
    // Keep the existing ABI name, but protect stereo FG too (ACC has no quad
    // compositor). Never wait for the dispatch gate while holding state_mutex.
    bool active{};
    {std::lock_guard lock(state_mutex);
        for(const auto& [handle,owner]:sessions)if(owner.running&&owner.graphics_api==11&&
            owner.graphics_device==device.Get()&&(owner.quad_capture||owner.fg)){active=true;break;}}
    if(!active)return nullptr;
    auto& dispatch=cheeky::quad_capture::d3d11_dispatch_mutex;dispatch.lock();return &dispatch;
}
extern "C" __declspec(dllexport) void __cdecl CheekyQuad_ReleaseD3D11V1(void* ticket) noexcept {
    auto& dispatch=cheeky::quad_capture::d3d11_dispatch_mutex;if(ticket==&dispatch)dispatch.unlock();
}
extern "C" __declspec(dllexport) uint32_t __cdecl CheekyQuad_ObserveD3D11V1(const CheekyQuadSourceD3D11V1* input) noexcept {
    if(!input||input->version!=CHEEKY_QUAD_VIEWS_VERSION||input->size!=sizeof(*input)||!input->context||!input->texture)return 0;
    try {Microsoft::WRL::ComPtr<ID3D11Device> device;static_cast<ID3D11DeviceContext*>(input->context)->GetDevice(&device);
        const auto owner=quad_role_owner(nullptr,device.Get());if(!owner||!owner->d3d11||!owner->d3d11->mapping)return 0;
        owner->d3d11->mapping->observe(*input);return 1;
    }catch(...){return 0;}
}
extern "C" __declspec(dllexport) void __cdecl CheekyQuad_ConfigureV1(uint32_t manual,uint32_t numbers) noexcept {
    try {const auto owner=quad_role_owner();if(owner&&owner->d3d11&&owner->d3d11->mapping)owner->d3d11->mapping->configure(manual!=0,numbers!=0);}catch(...){}
}
extern "C" __declspec(dllexport) void __cdecl CheekyQuad_ForgetSourceV1(uint64_t id,uint64_t generation) noexcept {
    try {const auto owner=quad_role_owner();if(owner&&owner->d3d11&&owner->d3d11->mapping)owner->d3d11->mapping->forget(id,generation);}catch(...){}
}
extern "C" __declspec(dllexport) void __cdecl CheekyQuad_RecalibrateV1() noexcept {
    try {const auto owner=quad_role_owner();if(owner&&owner->d3d11&&owner->d3d11->mapping)owner->d3d11->mapping->reset();}catch(...){}
}
extern "C" __declspec(dllexport) uint32_t __cdecl CheekyQuad_AssignV1(uint64_t session,uint32_t number,uint32_t role,uint64_t id,uint64_t generation) noexcept {
    try {uint64_t current{};const auto owner=quad_role_owner(&current);
        return owner&&current==session&&owner->d3d11&&owner->d3d11->mapping&&owner->d3d11->mapping->assign(number,role,id,generation);
    }catch(...){return 0;}
}
extern "C" __declspec(dllexport) uint32_t __cdecl CheekyQuad_SetSavedLayoutV1(const CheekyQuadSavedLayoutV1* layout) noexcept {
    if(!layout||layout->version!=1||layout->size!=sizeof(*layout))return 0;
    try{const auto owner=quad_role_owner();return owner&&owner->d3d11&&owner->d3d11->mapping&&owner->d3d11->mapping->saved_layout(*layout);}catch(...){return 0;}
}
extern "C" __declspec(dllexport) bool __cdecl CheekyQuadFG_ObserveD3D11V1(const CheekyQuadFgSourceV1* input) noexcept {
    if(!input||input->abi!=1||input->size!=sizeof(*input)||!input->context)return false;
    try{Microsoft::WRL::ComPtr<ID3D11Device> device;static_cast<ID3D11DeviceContext*>(input->context)->GetDevice(&device);
        const auto owner=quad_role_owner(nullptr,device.Get());return owner&&owner->d3d11&&owner->d3d11->fg_guides&&owner->d3d11->fg_guides->observe(*input);
    }catch(...){return false;}
}
extern "C" __declspec(dllexport) bool __cdecl CheekyQuadFG_GetStatsV1(CheekyQuadFgStatsV1* out) noexcept {
    if(!out||out->abi!=1||out->size!=sizeof(*out))return false;
    try{const auto owner=quad_role_owner();if(!owner||!owner->d3d11||!owner->d3d11->fg_guides)return false;
        *out=owner->d3d11->fg_guides->stats();return true;}catch(...){return false;}
}
