// Included after pointer sampling and non-blocking menu image helpers.
bool initialize_menu_cursor(SessionState& state, const Dispatch& d) {
    if (state.cursor_ready) return true;
    if (state.cursor_failed || state.graphics_api != 12) return false;
    auto& cursor = state.cursor_menu;
    const auto fail = [&](const char* stage) {
        state.cursor_failed = true; report_menu_diagnostic(stage); return false;
    };
    if (!cursor.swapchain) {
        if (!d.enumerate_swapchain_formats || !d.create_swapchain || !d.enumerate_swapchain_images)
            return fail("OpenXR cursor: runtime functions unavailable; retaining baked cursor");
        std::uint32_t count{};
        if (XR_FAILED(d.enumerate_swapchain_formats(state.session,0,&count,nullptr)) || !count || count>1024)
            return fail("OpenXR cursor: no supported texture format");
        std::vector<std::int64_t> formats(count);
        if (XR_FAILED(d.enumerate_swapchain_formats(state.session,count,&count,formats.data()))) return fail("OpenXR cursor: format enumeration failed");
        const auto found=std::find_if(formats.begin(),formats.end(),[](auto format){
            return format==DXGI_FORMAT_R8G8B8A8_UNORM || format==DXGI_FORMAT_B8G8R8A8_UNORM ||
                format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        });
        if (found==formats.end()) return fail("OpenXR cursor: RGBA8/BGRA8 (linear or sRGB) unavailable; retaining baked cursor");
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.createFlags=XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;create.usageFlags=cheeky::xr_menu::swapchain_usage;
        create.format=*found;create.width=create.height=32;create.arraySize=create.faceCount=create.mipCount=create.sampleCount=1;
        if (XR_FAILED(d.create_swapchain(state.session,&create,&cursor.swapchain))) return fail("OpenXR cursor: static swapchain creation failed");
        cursor.width=cursor.height=32;cursor.format=cursor.target_format=std::uint32_t(*found);
        count=0;
        if (XR_FAILED(d.enumerate_swapchain_images(cursor.swapchain,0,&count,nullptr)) || !count || count>16)
            return fail("OpenXR cursor: image enumeration failed");
        cursor.images12.resize(count);for(auto& image:cursor.images12)image.type=XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        if (XR_FAILED(d.enumerate_swapchain_images(cursor.swapchain,count,&count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(cursor.images12.data())))) return fail("OpenXR cursor: image enumeration failed");
    }
    const auto available=poll_menu_image(cursor,d);
    if (available==XR_TIMEOUT_EXPIRED) return false;
    if (available!=XR_SUCCESS || cursor.acquired_index>=cursor.images12.size()) return fail("OpenXR cursor: image availability failed");
    auto* device=static_cast<ID3D12Device*>(state.graphics_device);
    auto* queue=static_cast<ID3D12CommandQueue*>(state.graphics_queue);
    cursor.allocators.resize(1);
    if (!device || !queue || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&cursor.allocators[0]))) ||
        FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,cursor.allocators[0].Get(),nullptr,IID_PPV_ARGS(&cursor.command_list))) ||
        FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&cursor.fence)))) return fail("OpenXR cursor: upload context creation failed");
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=256*32;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,IID_PPV_ARGS(&state.cursor_upload)))) return fail("OpenXR cursor: upload allocation failed");
    void* memory{};D3D12_RANGE read{0,0};
    if (FAILED(state.cursor_upload->Map(0,&read,&memory))) return fail("OpenXR cursor: upload mapping failed");
    std::memset(memory,0,256*32);
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){
        const float radius=std::hypot(float(x)-15.5f,float(y)-15.5f);
        auto* pixel=static_cast<unsigned char*>(memory)+y*256+x*4;
        if(radius<11){pixel[0]=pixel[1]=pixel[2]=radius<8?255:0;pixel[3]=255;}
    }
    state.cursor_upload->Unmap(0,nullptr);
    auto* image=cursor.images12[cursor.acquired_index].texture;
    cheeky::fg::color_transition(cursor.command_list.Get(),image,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION source{},target{};source.pResource=state.cursor_upload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Footprint={DXGI_FORMAT(cursor.target_format),32,32,1,256};
    target.pResource=image;target.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cursor.command_list->CopyTextureRegion(&target,0,0,0,&source,nullptr);
    cheeky::fg::color_transition(cursor.command_list.Get(),image,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (FAILED(cursor.command_list->Close())) return fail("OpenXR cursor: upload recording failed");
    ID3D12CommandList* lists[]{cursor.command_list.Get()};queue->ExecuteCommandLists(1,lists);
    if (FAILED(queue->Signal(cursor.fence.Get(),1))) return fail("OpenXR cursor: upload signal failed");
    cursor.fence_value=1;
    if (XR_FAILED(release_menu_image(cursor,d))) return fail("OpenXR cursor: upload publication failed");
    state.cursor_ready=true;
    char message[180]{};
    std::snprintf(message,sizeof(message),"OpenXR cursor: static texture queued (format=%u); pose sampled independently each headset frame",cursor.target_format);
    report_menu_diagnostic(message);
    return true;
}

void set_menu_cursor_geometry(PublishedMenu& out, const SessionState::Menu& menu) {
    out.panel_pose=menu.pose;out.panel_size=menu.size;out.panel_width=menu.width;out.panel_strips=menu.strips;
    out.display_width=menu.display_width;out.display_height=menu.display_height;
}

XrCompositionLayerQuad position_menu_cursor(const PublishedMenu& menu, XrSpace space, MenuPointerSample pointer) {
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;quad.space=space;
    quad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;quad.subImage={menu.cursor_swapchain,{{0,0},{32,32}},0};
    const float angle=menu.cylinder?(pointer.u-.5f)*menu.panel_size.width/cheeky::xr_menu::radius:0.f;
    const float sn=std::sin(-angle*.5f),cs=std::cos(-angle*.5f);
    quad.pose=menu.panel_pose;const auto q=menu.panel_pose.orientation;
    quad.pose.orientation={q.x*cs-q.z*sn,q.y*cs+q.w*sn,q.z*cs+q.x*sn,q.w*cs-q.y*sn};
    XrVector3f local{(pointer.u-.5f)*menu.panel_size.width,(.5f-pointer.v)*menu.panel_size.height,.002f};
    if(menu.cylinder){local.x=cheeky::xr_menu::radius*std::sin(angle)-.002f*std::sin(angle);
        local.z=cheeky::xr_menu::radius*(1-std::cos(angle))+.002f*std::cos(angle);}
    const auto offset=rotate_menu_vector(q,local);
    quad.pose.position.x+=offset.x;quad.pose.position.y+=offset.y;quad.pose.position.z+=offset.z;
    quad.size={menu.panel_size.width*24/std::max(menu.display_width,1.f),menu.panel_size.height*24/std::max(menu.display_height,1.f)};
    return quad;
}

struct MenuCursorHost {
    CheekyOpenXRMenuCursorInputFn input{};
    CheekyOpenXRMenuCursorLayerFn notify{};
    CheekyOpenXRMenuPointer pointer{};
};
void update_menu_cursor_locked(SessionState& state, const Dispatch& d, const XrFrameEndInfo* frame, PublishedMenu& menu,
    const MenuCursorHost* hooks=nullptr) {
    const auto host=GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
    const auto get_input=hooks?hooks->input:host?reinterpret_cast<CheekyOpenXRMenuCursorInputFn>(GetProcAddress(host,"CheekyOpenXRMenuGetCursorInput")):nullptr;
    const auto notify=hooks?hooks->notify:host?reinterpret_cast<CheekyOpenXRMenuCursorLayerFn>(GetProcAddress(host,"CheekyOpenXRMenuCursorLayerActive")):nullptr;
    CheekyOpenXRMenuCursorInput input;
    const bool host_live=get_input&&notify&&get_input(&input);
    SessionState::Menu panel;panel.pose=menu.panel_pose;panel.size=menu.panel_size;
    panel.width=menu.panel_width;panel.strips=menu.panel_strips;panel.cylinder=menu.cylinder;
    CheekyOpenXRMenuFrame dimensions;dimensions.display_width=menu.display_width;dimensions.display_height=menu.display_height;
    const auto ray=send_menu_pointer(state,d,frame->displayTime,dimensions,hooks?hooks->pointer:nullptr,&panel);
    MenuPointerSample pointer{input.u,input.v,bool(input.down),bool(input.valid)};
    const bool controller=input.controllers&&cheeky::standalone::controller_pointer_owns(ray.active,state.cursor_controller_drag,
        ray.down&&!state.cursor_controller_drag,GetTickCount64(),input.desktop_activity,bool(input.down));
    if(controller)pointer=ray;
    state.cursor_controller_drag=controller&&ray.down;
    menu.cursor_controller_drag=state.cursor_controller_drag;
    pointer.active=pointer.active&&std::isfinite(pointer.u)&&std::isfinite(pointer.v)&&pointer.u>=0&&pointer.u<=1&&pointer.v>=0&&pointer.v<=1;
    const auto backgrounds=menu.cylinder?1u:unsigned(menu.strips.size());
    const bool enabled=host_live&&frame->layerCount+backgrounds+1<=menu.max_layers&&initialize_menu_cursor(state,d);
    if(notify)notify(enabled);
    menu.cursor_visible=enabled&&pointer.active;
    if(!enabled)return;
    menu.cursor_swapchain=state.cursor_menu.swapchain;
    menu.cursor_layer=position_menu_cursor(menu,state.calibration_local_space,pointer);
}

// Cursor pixels and panel geometry are already published. Desktop motion needs
// neither session mutation nor the menu lock held across app frame submission.
void update_menu_desktop_cursor(const XrFrameEndInfo* frame, PublishedMenu& menu,
    const MenuCursorHost* hooks=nullptr) {
    if(!menu.cursor_swapchain)return;
    const auto host=GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
    const auto input_fn=hooks?hooks->input:host?reinterpret_cast<CheekyOpenXRMenuCursorInputFn>(GetProcAddress(host,"CheekyOpenXRMenuGetCursorInput")):nullptr;
    const auto notify=hooks?hooks->notify:host?reinterpret_cast<CheekyOpenXRMenuCursorLayerFn>(GetProcAddress(host,"CheekyOpenXRMenuCursorLayerActive")):nullptr;
    CheekyOpenXRMenuCursorInput input;
    const auto backgrounds=menu.cylinder?1u:unsigned(menu.strips.size());
    const bool enabled=input_fn&&notify&&input_fn(&input)&&frame->layerCount+backgrounds+1<=menu.max_layers;
    if(notify)notify(enabled);
    if(!enabled){menu.cursor_visible=false;return;}
    const auto now=GetTickCount64();
    const bool desktop=!input.controllers||input.down||
        (input.desktop_activity&&now>=input.desktop_activity&&now-input.desktop_activity<=1500);
    if(!desktop||menu.cursor_controller_drag)return;
    menu.cursor_visible=input.valid&&std::isfinite(input.u)&&std::isfinite(input.v)&&input.u>=0&&input.u<=1&&input.v>=0&&input.v<=1;
    if(menu.cursor_visible)menu.cursor_layer=position_menu_cursor(menu,menu.cursor_layer.space,{input.u,input.v,bool(input.down),true});
}

void update_presenter_menu_cursor(XrSession session, const XrFrameEndInfo* frame, PublishedMenu& menu) {
    std::unique_lock menu_lock(menu_mutex,std::try_to_lock);
    if(!menu_lock.owns_lock()){
        update_menu_desktop_cursor(frame,menu);
        return;
    }
    std::lock_guard state_lock(state_mutex);
    const auto owner=sessions.find(session);if(owner==sessions.end())return;
    const auto instance=instances.find(owner->second.instance);if(instance==instances.end())return;
    update_menu_cursor_locked(owner->second,instance->second.dispatch,frame,menu);
    std::lock_guard published_lock(published_menu_mutex);
    const auto published=published_menus.find(session);
    if(published!=published_menus.end()&&published->second.swapchain==menu.swapchain){
        published->second.cursor_swapchain=menu.cursor_swapchain;published->second.cursor_layer=menu.cursor_layer;
        published->second.cursor_visible=menu.cursor_visible;
        published->second.cursor_controller_drag=menu.cursor_controller_drag;
    }
}
