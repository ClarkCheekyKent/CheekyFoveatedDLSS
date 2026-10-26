#pragma once

CheekyOpenXRMenuCursorInput test_cursor_input;
bool test_cursor_enabled{};
unsigned test_cursor_creates{};
DXGI_FORMAT test_cursor_format{DXGI_FORMAT_R8G8B8A8_UNORM};

void menu_cursor_validation(DXGI_FORMAT format) {
    using Microsoft::WRL::ComPtr;using Runtime=buffered_probe::Runtime;
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))&&
        SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))&&
        SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"Live cursor WARP device");
    Runtime::device=device.Get();Runtime::chains.clear();Runtime::direct_states.clear();Runtime::image_releases=0;
    SessionState state;state.session=h<XrSession>(9100);state.graphics_api=12;
    state.graphics_device=device.Get();state.graphics_queue=queue.Get();state.max_layers=16;
    Dispatch d;const auto functions=Runtime::functions();
    test_cursor_format=format;
    d.enumerate_swapchain_formats=[](XrSession,std::uint32_t capacity,std::uint32_t* count,std::int64_t* values)->XrResult {
        *count=1;if(capacity)values[0]=test_cursor_format;return XR_SUCCESS;
    };
    d.create_swapchain=[](XrSession session,const XrSwapchainCreateInfo* create,XrSwapchain* out)->XrResult {
        check((create->createFlags&XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT)!=0&&create->width==32&&create->height==32,
            "The live cursor uses a small static image, not a full menu redraw");
        check(create->format==test_cursor_format,"Use the runtime's advertised cursor format, including sRGB-only runtimes");
        ++test_cursor_creates;return Runtime::create(session,create,out);
    };
    d.enumerate_swapchain_images=functions.enumerate;d.acquire_swapchain_image=functions.acquire;
    d.wait_swapchain_image=functions.wait_image;d.release_swapchain_image=functions.release;d.destroy_swapchain=functions.destroy;
    ComPtr<ID3D12Fence> gate,done;
    check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)))&&
        SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done))),"Live cursor test fences");
    struct Retire {
        ID3D12CommandQueue* queue;ID3D12Fence* gate;ID3D12Fence* done;
        ~Retire(){gate->Signal(1);queue->Signal(done,1);HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(event){done->SetEventOnCompletion(1,event);WaitForSingleObject(event,5000);CloseHandle(event);}}
    } retire{queue.Get(),gate.Get(),done.Get()};
    check(SUCCEEDED(queue->Wait(gate.Get(),1)),"Hold the software GPU before the cursor upload");
    test_cursor_creates=0;
    check(initialize_menu_cursor(state,d)&&state.cursor_ready&&state.cursor_menu.fence->GetCompletedValue()==0&&Runtime::image_releases==1,
        "Cursor publication queues its upload without a CPU GPU-completion wait");
    PublishedMenu menu;menu.swapchain=h<XrSwapchain>(9199);menu.max_layers=16;menu.strips.resize(1);
    menu.panel_pose.orientation.w=1;menu.panel_size={1.5f,1.f};menu.panel_width=800;menu.display_width=800;menu.display_height=600;
    MenuCursorHost hooks;
    hooks.input=[](CheekyOpenXRMenuCursorInput* out){*out=test_cursor_input;return true;};
    hooks.notify=[](bool enabled){test_cursor_enabled=enabled;};
    test_cursor_input={};test_cursor_input.controllers=0;test_cursor_input.valid=1;test_cursor_input.u=.2f;test_cursor_input.v=.3f;
    XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};frame.displayTime=100;
    update_menu_cursor_locked(state,d,&frame,menu,&hooks);
    check(test_cursor_enabled&&menu.cursor_visible,"The independent cursor is visible over a cached menu");
    const float first=menu.cursor_layer.pose.position.x;
    test_cursor_input.u=.8f;frame.displayTime=200;
    update_menu_cursor_locked(state,d,&frame,menu,&hooks);
    check(menu.cursor_layer.pose.position.x>first&&menu.swapchain==h<XrSwapchain>(9199)&&
        test_cursor_creates==1&&Runtime::image_releases==1&&state.cursor_menu.fence_value==1,
        "Successive headset ticks move the cursor without refreshing background pixels or uploading again");
    test_cursor_input.u=.4f;const auto previous=menu.cursor_layer.pose.position.x;
    update_menu_desktop_cursor(&frame,menu,&hooks);
    check(menu.cursor_visible&&menu.cursor_layer.pose.position.x<previous&&test_cursor_enabled&&test_cursor_creates==1&&
        Runtime::image_releases==1&&state.cursor_menu.fence_value==1,
        "A busy background handoff still permits live desktop motion from the published cursor without GPU work");
    for(bool cylinder:{false,true})for(float u:{.1f,.5f,.9f})for(float v:{.2f,.8f}){
        menu.cylinder=cylinder;
        const auto quad=position_menu_cursor(menu,XR_NULL_HANDLE,{u,v,false,true});
        const auto normal=rotate_menu_vector(quad.pose.orientation,{0,0,1});
        const XrVector3f origin{quad.pose.position.x+normal.x*.25f,quad.pose.position.y+normal.y*.25f,quad.pose.position.z+normal.z*.25f};
        float hit_u{},hit_v{};
        check(cheeky::xr_menu::hit(origin,{-normal.x,-normal.y,-normal.z},menu.panel_size.width,menu.panel_size.height,
            menu.panel_width,1,cylinder,hit_u,hit_v)&&std::abs(hit_u-u)<.0001f&&std::abs(hit_v-v)<.0001f,
            "Live cursor placement matches flat and curved panel hit testing");
    }
    menu.max_layers=1;update_menu_cursor_locked(state,d,&frame,menu,&hooks);
    check(!test_cursor_enabled&&!menu.cursor_visible,"No spare layer restores the baked-cursor fallback");
    gate->Signal(1);queue->Signal(done.Get(),1);HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    check(event&&SUCCEEDED(done->SetEventOnCompletion(1,event))&&WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"Retire the one-time cursor upload");CloseHandle(event);
    destroy_menu(state.cursor_menu,d);Runtime::chains.clear();Runtime::device=nullptr;
    std::cout<<"PASS live cursor publication, per-tick pose, cached background and curved/flat hit alignment\n";
}
