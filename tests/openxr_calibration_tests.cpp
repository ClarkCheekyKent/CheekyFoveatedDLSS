#include "eye_calibration.hpp"
#include "eye_calibration_d3d12.hpp"
#include "settings.hpp"
#include "../openxr_layer/eye_calibration.hpp"
#include <wrl/client.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <iostream>
#include <vector>
#include <stdexcept>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include "../third_party/openxr/include/openxr/openxr.h"
#include "../third_party/openxr/include/openxr/openxr_platform.h"
#include "../third_party/openxr/include/openxr/openxr_loader_negotiation.h"
#include "../openxr_layer/projection_selection.hpp"
#include "cheeky_gaze_abi.h"

namespace {
using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
void check(HRESULT hr) {
    require(SUCCEEDED(hr), "OpenXR/D3D12 test GPU operation failed");
}
void cleanup() {
    eye_calibration_stop();
    unregister_stereo_view(9101);
    unregister_stereo_view(9102);
}
void roles() {
    cleanup();
    eye_calibration_reset_stats();
    eye_calibration_enable(true);
    register_stereo_view(9101);
    register_stereo_view(9102);
    Settings settings{};
    (void)settings_for_view(settings, 9101);
    (void)settings_for_view(settings, 9102);
}
unsigned captures{}, accepted{};
std::array<unsigned, 2> labels{};
// Run the actual layer DLL against a tiny loader/runtime, without a headset.
struct XRLayer {
    inline static std::uintptr_t next_chain{};
    inline static void* image{};
    inline static unsigned graphics{};
    inline static XrResult wait_result{XR_SUCCESS}, release_result{XR_SUCCESS}, end_result{XR_SUCCESS};
    HMODULE module{};
    PFN_xrGetInstanceProcAddr get{};
    XrInstance instance{};
    XrSession session{};
    XrSwapchain chain{};
    XrSwapchain dummy_chain{};
    unsigned projection_mode{}; // 0: normal, 1/2: dummy first/last, 3: ambiguous, 4: dummy only
    unsigned image_width{}, image_height{};
    static XrResult XRAPI_CALL create(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo*,
                                      XrInstance* out) {
        *out = reinterpret_cast<XrInstance>(1);
        return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL next(XrInstance, const char* name, PFN_xrVoidFunction* out) {
        *out = nullptr;
#define XR_FAKE(n, fn)                                                                                       \
    if (!strcmp(name, n)) {                                                                                  \
        *out = reinterpret_cast<PFN_xrVoidFunction>(+fn);                                                    \
        return XR_SUCCESS;                                                                                   \
    }
        XR_FAKE("xrDestroyInstance", [](XrInstance) { return XR_SUCCESS; });
        XR_FAKE("xrCreateSession", [](XrInstance, const XrSessionCreateInfo*, XrSession* s) {
            *s = reinterpret_cast<XrSession>(2);
            return XR_SUCCESS;
        });
        XR_FAKE("xrDestroySession", [](XrSession) { return XR_SUCCESS; });
        XR_FAKE("xrBeginSession", [](XrSession, const XrSessionBeginInfo*) { return XR_SUCCESS; });
        XR_FAKE("xrCreateSwapchain", [](XrSession, const XrSwapchainCreateInfo*, XrSwapchain* s) {
            *s = reinterpret_cast<XrSwapchain>(++next_chain);
            return XR_SUCCESS;
        });
        XR_FAKE("xrDestroySwapchain", [](XrSwapchain) { return XR_SUCCESS; });
        XR_FAKE("xrEnumerateSwapchainImages", [](XrSwapchain, std::uint32_t capacity, std::uint32_t* count,
                                                 XrSwapchainImageBaseHeader* images) {
            *count = 1;
            if (capacity && images) {
                if (graphics == 11)
                    reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)->texture =
                        static_cast<ID3D11Texture2D*>(image);
                else
                    reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)->texture =
                        static_cast<ID3D12Resource*>(image);
            }
            return XR_SUCCESS;
        });
        XR_FAKE("xrAcquireSwapchainImage",
                [](XrSwapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t* index) {
                    *index = 0;
                    return XR_SUCCESS;
                });
        XR_FAKE("xrWaitSwapchainImage",
                [](XrSwapchain, const XrSwapchainImageWaitInfo*) { return wait_result; });
        XR_FAKE("xrReleaseSwapchainImage",
                [](XrSwapchain, const XrSwapchainImageReleaseInfo*) { return release_result; });
        XR_FAKE("xrBeginFrame", [](XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; });
        XR_FAKE("xrEndFrame", [](XrSession, const XrFrameEndInfo*) { return end_result; });
#undef XR_FAKE
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    template <class T> T fn(const char* name) {
        PFN_xrVoidFunction out{};
        require(get(instance, name, &out) == XR_SUCCESS && out, "Layer dispatch missing");
        return reinterpret_cast<T>(out);
    }
    XRLayer(void* texture, unsigned api, void* binding, unsigned width = 128, unsigned slices = 2,
             unsigned height = 128) : image_width(width), image_height(height) {
        image = texture;
        graphics = api;
        next_chain = 76;
        wait_result = release_result = end_result = XR_SUCCESS;
        module = LoadLibraryW(L"CheekyOpenXRLayer.dll");
        require(module != nullptr, "Layer DLL unavailable");
        auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
            GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
        require(negotiate != nullptr, "Layer negotiation export missing");
        XrNegotiateLoaderInfo loader{};
        loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
        loader.structSize = sizeof(loader);
        loader.minInterfaceVersion = loader.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
        loader.maxApiVersion = XR_CURRENT_API_VERSION;
        XrNegotiateApiLayerRequest request{};
        request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
        request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
        request.structSize = sizeof(request);
        require(negotiate(&loader, "XR_APILAYER_CHEEKY_foveated_dlss", &request) == XR_SUCCESS,
                "Layer negotiation failed");
        get = request.getInstanceProcAddr;
        XrApiLayerNextInfo next_info{};
        next_info.nextGetInstanceProcAddr = next;
        next_info.nextCreateApiLayerInstance = create;
        XrApiLayerCreateInfo layer_info{};
        layer_info.nextInfo = &next_info;
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        require(request.createApiLayerInstance(&info, &layer_info, &instance) == XR_SUCCESS,
                "Layer instance failed");
        XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
        session_info.next = binding;
        session_info.systemId = 1;
        require(fn<PFN_xrCreateSession>("xrCreateSession")(instance, &session_info, &session) == XR_SUCCESS,
                "Layer session failed");
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        require(fn<PFN_xrBeginSession>("xrBeginSession")(session, &begin) == XR_SUCCESS,
                "BeginSession failed");
        XrSwapchainCreateInfo chain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        chain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        chain_info.width = width;
        chain_info.height = height;
        chain_info.arraySize = slices;
        chain_info.mipCount = chain_info.faceCount = chain_info.sampleCount = 1;
        require(fn<PFN_xrCreateSwapchain>("xrCreateSwapchain")(session, &chain_info, &chain) == XR_SUCCESS,
                "Layer swapchain failed");
        XrSwapchainImageD3D11KHR image11{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        XrSwapchainImageD3D12KHR image12{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        std::uint32_t count{};
        require(fn<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages")(
                    chain, 1, &count,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(
                        api == 11 ? static_cast<void*>(&image11) : static_cast<void*>(&image12))) ==
                    XR_SUCCESS,
                "Layer image enumeration failed");
        chain_info.width = chain_info.height = 4;
        chain_info.arraySize = 1;
        require(fn<PFN_xrCreateSwapchain>("xrCreateSwapchain")(session, &chain_info, &dummy_chain) == XR_SUCCESS,
                "Dummy swapchain failed");
    }
    void begin() {
        XrFrameBeginInfo info{XR_TYPE_FRAME_BEGIN_INFO};
        require(fn<PFN_xrBeginFrame>("xrBeginFrame")(session, &info) == XR_SUCCESS, "BeginFrame failed");
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        std::uint32_t index{};
        require(fn<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage")(chain, &acquire, &index) ==
                    XR_SUCCESS,
                "Acquire failed");
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        require(fn<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage")(chain, &wait) == wait_result,
                "Wait result was not forwarded");
    }
    void release() {
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        require(fn<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage")(chain, &info) == release_result,
                "Release result was not forwarded");
    }
    void end(bool swap, bool array = true) {
        std::array<XrCompositionLayerProjectionView, 2> views{};
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto& v = views[eye];
            v.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            v.subImage.swapchain = chain;
            v.subImage.imageArrayIndex = array ? eye : 0;
            const auto eye_width = static_cast<int>(array ? image_width : image_width / 2);
            v.subImage.imageRect = {{array ? 0 : static_cast<int>(eye) * eye_width, 0},
                                   {eye_width, static_cast<int>(image_height)}};
        }
        if (swap)
            std::swap(views[0], views[1]);
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.viewCount = 2;
        projection.views = views.data();
        auto dummy_views = views;
        for (unsigned eye = 0; eye < 2; ++eye) {
            dummy_views[eye].subImage = {dummy_chain, {{static_cast<int>(eye * 2), 0}, {2, 4}}, 0};
        }
        auto dummy = projection;
        dummy.views = dummy_views.data();
        dummy.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        const auto* real_header = reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection);
        const auto* dummy_header = reinterpret_cast<XrCompositionLayerBaseHeader*>(&dummy);
        const XrCompositionLayerBaseHeader* layers[]{real_header, dummy_header};
        if (projection_mode == 1) std::swap(layers[0], layers[1]);
        if (projection_mode == 3) layers[1] = real_header;
        if (projection_mode == 4) layers[0] = dummy_header;
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
        info.layerCount = projection_mode >= 1 && projection_mode <= 3 ? 2 : 1;
        info.layers = layers;
        require(fn<PFN_xrEndFrame>("xrEndFrame")(session, &info) == end_result,
                "EndFrame result was not forwarded");
        if (XR_SUCCEEDED(end_result)) {
            auto get_snapshot = reinterpret_cast<CheekyOpenXRGetGazeSnapshotFn>(
                GetProcAddress(module, "CheekyOpenXR_GetGazeSnapshot"));
            CheekyGazeSnapshotV1 snapshot{};
            require(get_snapshot && get_snapshot(CHEEKY_GAZE_ABI_VERSION, &snapshot, sizeof(snapshot)),
                    "Layer snapshot unavailable");
            require(snapshot.views[0].image_rect_width == (projection_mode >= 3 ? 0U :
                        (array ? image_width : image_width / 2)),
                    "Gaze must select the real projection or clear rejected geometry");
            require(bool(snapshot.status_flags & CHEEKY_GAZE_STATUS_AMBIGUOUS_RESOURCE) ==
                        (projection_mode == 3 || (array && projection_mode < 3)),
                    "Ambiguous projection status must not persist into a valid frame");
        }
    }
    ~XRLayer() {
        if (session) {
            fn<PFN_xrDestroySwapchain>("xrDestroySwapchain")(chain);
            fn<PFN_xrDestroySwapchain>("xrDestroySwapchain")(dummy_chain);
            fn<PFN_xrDestroySession>("xrDestroySession")(session);
        }
        if (instance)
            fn<PFN_xrDestroyInstance>("xrDestroyInstance")(instance);
        if (module)
            FreeLibrary(module);
    }
};
void layer_policy() {
    using namespace cheeky::openxr_calibration;
    CheekyEyeCalibrationBridgeV1 api;
    api.begin = [](std::uint64_t, std::uint32_t) noexcept { return true; };
    api.capture = [](std::uint64_t, void*, void*, std::uint32_t, std::uint32_t eye, std::uint32_t, float,
                     float, float, float) noexcept -> std::uint64_t {
        ++captures;
        return eye + 1;
    };
    api.result = [](std::uint64_t ticket, int result, std::uint32_t eye) noexcept {
        if (!result && ticket && ticket <= 2) {
            ++accepted;
            labels[ticket - 1] = eye;
        }
    };
    api.destroy = [](std::uint64_t) noexcept {};
    const std::array<Region, 2> normal{{{10, 0, 0, 0, 128, 128}, {20, 1, 0, 0, 128, 128}}};
    for (unsigned variant = 0; variant < 6; ++variant) {
        Frame frame;
        frame.history = normal;
        captures = accepted = 0;
        labels = {};
        frame.begin(&api, 42, 12);
        frame.before_release(&api, 10, 1, &frame, nullptr, 128, 128);
        frame.after_release(10, 1, variant != 1);
        frame.before_release(&api, 20, 2, &frame, nullptr, 128, 128);
        frame.after_release(20, 2, true);
        auto actual = normal;
        std::array<std::uint32_t, 2> indices{1, 2};
        if (variant == 3)
            actual[0].width = 120;
        if (variant == 4)
            indices[0] = 3;
        if (variant == 5) {
            std::swap(actual[0], actual[1]);
            std::swap(indices[0], indices[1]);
        }
        frame.end(&api, actual, indices, variant != 2);
        require(captures == 2, "OpenXR must capture both patches before release");
        require(accepted == (variant == 0 || variant == 5 ? 2U : 0U),
                "OpenXR must reject the entire uncertain pair");
        if (variant == 5)
            require(labels[0] == 1 && labels[1] == 0, "EndFrame must relabel swapped subimages");
    }
}
void projection_policy() {
    std::array<XrCompositionLayerProjectionView, 2> real_views{}, dummy_views{};
    for (unsigned eye = 0; eye < 2; ++eye) {
        real_views[eye].subImage = {reinterpret_cast<XrSwapchain>(1),
            {{static_cast<int>(eye * 3768), 0}, {3768, 3532}}, 0};
        dummy_views[eye].subImage = {reinterpret_cast<XrSwapchain>(2),
            {{static_cast<int>(eye * 2), 0}, {2, 4}}, 0};
    }
    XrCompositionLayerProjection real{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    real.viewCount = 2;
    real.views = real_views.data();
    auto dummy = real;
    dummy.views = dummy_views.data();
    dummy.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    auto is_dummy = [](XrSwapchain s) { return s == reinterpret_cast<XrSwapchain>(2); };
    const auto* r = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&real);
    const auto* d = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&dummy);
    const XrCompositionLayerBaseHeader* layers[]{d, r};
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.layers = layers;
    info.layerCount = 2;
    for (unsigned order = 0; order < 2; ++order) {
        require(cheeky::openxr::select_projection(&info, is_dummy).projection == &real,
                "Dummy ordering must not hide the game projection");
        std::swap(layers[0], layers[1]);
    }
    dummy.layerFlags = 0;
    require(cheeky::openxr::select_projection(&info, is_dummy).ambiguous,
            "Tiny opaque projections must not be silently discarded");
    dummy.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    require(cheeky::openxr::select_projection(&info, [](XrSwapchain) { return false; }).ambiguous,
            "A tiny crop of a large or unknown swapchain is not a dummy");
    dummy.viewCount = 1;
    require(cheeky::openxr::select_projection(&info, is_dummy).unsupported &&
                !cheeky::openxr::select_projection(&info, is_dummy).projection,
            "Unsupported projections must not select a guessed scene");
    dummy.viewCount = 2;
    dummy.views = nullptr;
    require(!cheeky::openxr::select_projection(&info, is_dummy).projection,
            "Missing projection views must be rejected");
    dummy.views = dummy_views.data();
    info.layerCount = 1;
    require(!cheeky::openxr::select_projection(&info, is_dummy).projection,
            "A dummy-only frame must not become the game scene");
    layers[0] = r;
    require(cheeky::openxr::select_projection(&info, is_dummy).projection == &real,
            "Normal stereo must remain supported");
    info.layerCount = 0;
    require(!cheeky::openxr::select_projection(&info, is_dummy).projection,
            "An empty frame must clear scene selection");
}
void openxr11() {
    using namespace cheeky::openxr_calibration;
    roles();
    const auto* api = bridge();
    require(api != nullptr, "OpenXR bridge export unavailable");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                            nullptr, &context));
    D3D11_TEXTURE2D_DESC desc{128, 128, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              0,   0,   0};
    ComPtr<ID3D11Texture2D> a, b, array;
    check(device->CreateTexture2D(&desc, nullptr, &a));
    check(device->CreateTexture2D(&desc, nullptr, &b));
    desc.ArraySize = 2;
    check(device->CreateTexture2D(&desc, nullptr, &array));
    std::vector<unsigned char> black(128 * 128 * 4);
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device.Get();
    XRLayer layer(array.Get(), 11, &binding);
    auto render = [&](bool swap) {
        layer.begin();
        context->UpdateSubresource(a.Get(), 0, nullptr, black.data(), 512, 0);
        context->UpdateSubresource(b.Get(), 0, nullptr, black.data(), 512, 0);
        eye_calibration_stamp(context.Get(), a.Get(), 9101, 0, 0, 128, 128);
        eye_calibration_stamp(context.Get(), b.Get(), 9102, 0, 0, 128, 128);
        context->CopySubresourceRegion(array.Get(), 0, 0, 0, 0, a.Get(), 0, nullptr);
        context->CopySubresourceRegion(array.Get(), 1, 0, 0, 0, b.Get(), 0, nullptr);
        layer.release();
        // The runtime may reuse images immediately after release. EndFrame
        // classification must use our already-recorded small readbacks.
        context->UpdateSubresource(array.Get(), 0, nullptr, black.data(), 512, 0);
        context->UpdateSubresource(array.Get(), 1, nullptr, black.data(), 512, 0);
        layer.end(swap);
        context->Flush();
        Sleep(2);
        eye_calibration_tick();
    };
    for (unsigned mode : {3U, 4U}) {
        layer.projection_mode = mode;
        for (unsigned i = 0; i < 16; ++i) render(false);
    }
    require(!eye_calibration_stats().valid && !stereo_eye_assignment(9101).calibrated,
            "Ambiguous and dummy-only frames must never calibrate");
    for (unsigned i = 0; i < 16; ++i) {
        layer.projection_mode = i % 3;
        render(false);
    }
    require(stereo_eye_assignment(9101).calibrated && stereo_eye_assignment(9101).calibration_session != 0,
            "OpenXR D3D11 array mapping must retain session provenance");
    for (unsigned i = 0; i < 16; ++i)
        render(true);
    require(eye_calibration_stats().corrections == 1 && stereo_eye_assignment(9101).eye_index == 1,
            "OpenXR D3D11 changed projection labels must correct once");
    eye_calibration_enable(false);
    layer.begin();
    layer.release();
    layer.end(false);
    context->Flush();
    const auto deadline = GetTickCount64() + 5000;
    while (eye_calibration_stats().in_flight && GetTickCount64() < deadline) {
        Sleep(1);
        eye_calibration_tick();
    }
    require(!eye_calibration_stats().in_flight, "OpenXR D3D11 readbacks did not drain");
    cleanup();
}
struct GPU12 {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue, submit_queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value{};
    explicit GPU12(bool hardware = false) {
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> adapter;
        if (!hardware)
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&submit_queue)));
        check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        check(list->Close());
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }
    void wait(ID3D12CommandQueue* q) {
        check(q->Signal(fence.Get(), ++value));
        const auto deadline = GetTickCount64() + 10000;
        while (fence->GetCompletedValue() < value && GetTickCount64() < deadline)
            Sleep(1);
        require(fence->GetCompletedValue() >= value, "Test GPU fence timed out");
        check(device->GetDeviceRemovedReason());
    }
    void begin() {
        check(allocator->Reset());
        check(list->Reset(allocator.Get(), nullptr));
        calibration12_retired(list.Get());
    }
    void execute() {
        check(list->Close());
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        calibration12_submitted(queue.Get(), list.Get());
    }
    void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &b);
    }
    ComPtr<ID3D12Resource> texture(unsigned width, unsigned slices, D3D12_RESOURCE_STATES state,
                                  DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, unsigned height = 128) {
        D3D12_HEAP_PROPERTIES h{};
        h.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = static_cast<UINT16>(slices);
        d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> r;
        check(
            device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
        return r;
    }
    void copy(ID3D12Resource* dst, unsigned slice, unsigned x, ID3D12Resource* src) {
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = dst;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.SubresourceIndex = slice;
        s.pResource = src;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&d, x, 0, 0, &s, nullptr);
    }
};
// A shader conversion/resize does not appear in the resource-copy graph.
// Follow the markers through that path, including swapped array destinations.
struct ScaledSubmission12 {
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    UINT increment{};
    ScaledSubmission12(GPU12& gpu, ID3D12Resource* a, ID3D12Resource* b, ID3D12Resource* target) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        increment = gpu.device->GetDescriptorHandleIncrementSize(hd.Type);
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        for (auto* source : {a, b}) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = source->GetDesc().Format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            gpu.device->CreateShaderResourceView(source, &srv, cpu);
            cpu.ptr += increment;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = target->GetDesc().Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Texture2DArray.ArraySize = 2;
        gpu.device->CreateUnorderedAccessView(target, nullptr, &uav, cpu);
        const D3D12_DESCRIPTOR_RANGE ranges[] = {
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
        D3D12_ROOT_PARAMETER parameters[3]{};
        for (unsigned i = 0; i < 2; ++i) {
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].DescriptorTable = {1, &ranges[i]};
        }
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants = {0, 0, 1};
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 3;
        desc.pParameters = parameters;
        ComPtr<ID3DBlob> signature, errors, shader;
        check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors));
        check(gpu.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                              IID_PPV_ARGS(&root)));
        constexpr char source[] = R"(
Texture2D<float4> a : register(t0);
Texture2D<float4> b : register(t1);
RWTexture2DArray<float4> target : register(u0);
cbuffer Options : register(b0) { uint options; }
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint2 p = id.xy * 128 / 192;
    if (options & 2) p.y = 127 - p.y;
    if (options & 4) p.y = min(p.y, 127 - p.y);
    if (options & 8) p = uint2(clamp(int2(p) + int2(8, 8), int2(0, 0), int2(127, 127)));
    float4 color = ((id.z ^ (options & 1)) == 0) ? a.Load(int3(p, 0)) : b.Load(int3(p, 0));
    if (options & 8) color.rgb = float3(.75, .65, .8) + color.rgb * float3(.12, .2, .15);
    target[id] = color;
})";
        check(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                          &shader, &errors));
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
        ps.pRootSignature = root.Get();
        ps.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        check(gpu.device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipeline)));
    }
    void record(GPU12& gpu, ID3D12Resource* a, ID3D12Resource* b, ID3D12Resource* target,
                  D3D12_RESOURCE_STATES state, bool swapped, bool flipped = false, bool ambiguous = false,
                  bool shifted = false) {
        for (auto* r : {a, b})
            gpu.barrier(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gpu.barrier(target, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[]{heap.Get()};
        gpu.list->SetDescriptorHeaps(1, heaps);
        gpu.list->SetComputeRootSignature(root.Get());
        gpu.list->SetPipelineState(pipeline.Get());
        auto handle = heap->GetGPUDescriptorHandleForHeapStart();
        gpu.list->SetComputeRootDescriptorTable(0, handle);
        handle.ptr += 2ULL * increment;
        gpu.list->SetComputeRootDescriptorTable(1, handle);
        gpu.list->SetComputeRoot32BitConstant(2,
            (swapped ? 1U : 0U) | (flipped ? 2U : 0U) | (ambiguous ? 4U : 0U) | (shifted ? 8U : 0U), 0);
        gpu.list->Dispatch(24, 24, 2);
        gpu.barrier(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, state);
        for (auto* r : {a, b})
            gpu.barrier(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
};
void recording_lifetime12() {
    GPU12 gpu;
    auto texture = gpu.texture(128, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto frame = calibration12_create(gpu.device.Get());
    std::uint64_t allocations{};
    require(calibration12_begin(*frame), "Fresh recording slot must be reusable");
    gpu.begin();
    require(calibration12_stamp(*frame, gpu.list.Get(), texture.Get(), 0, 12, 12,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, allocations),
            "Lifetime test stamp failed");
    gpu.execute();
    gpu.wait(gpu.queue.Get());
    require(!calibration12_poll(*frame).ready && !calibration12_begin(*frame),
            "Completed but still resubmittable recordings must not be mapped or reused");
    ComPtr<ID3D12Fence> gate;
    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    check(gpu.submit_queue->Wait(gate.Get(), 1));
    ID3D12CommandList* lists[]{gpu.list.Get()};
    gpu.submit_queue->ExecuteCommandLists(1, lists);
    calibration12_submitted(gpu.submit_queue.Get(), gpu.list.Get());
    calibration12_retired(gpu.list.Get());
    const auto blocked = calibration12_poll(*frame);
    // Always unblock the test queue, including on assertion failure.
    check(gate->Signal(1));
    gpu.wait(gpu.submit_queue.Get());
    require(!blocked.ready && !blocked.reusable,
            "A completed first queue must not retire a pending second queue");
    require(calibration12_poll(*frame).reusable,
            "Both queue timelines must drain after recording retirement");
    require(calibration12_begin(*frame), "Retired recording slot must be reusable");
    gpu.begin();
    require(calibration12_stamp(*frame, gpu.list.Get(), texture.Get(), 0, 12, 12,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, allocations),
            "Discarded test stamp failed");
    check(gpu.list->Close());
    gpu.begin();
    const auto discarded = calibration12_poll(*frame);
    require(discarded.ready && discarded.reusable && !discarded.valid &&
                std::string(discarded.failure.stage) == "recording_retired_without_observed_submit",
            "Reset without Execute must discard safely");
    check(gpu.list->Close());
    std::cout << "D3D12 recording lifetime: resubmission, independent queue fences, discarded Reset passed\n";
}
void failure_diagnostics12() {
    roles();
    GPU12 gpu;
    // A single-channel texture cannot carry the two chromatic markers.
    // Retain rejection and report its stage instead of inventing an eye role.
    auto source = gpu.texture(128, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, DXGI_FORMAT_R32_FLOAT);
    auto target = gpu.texture(128, 2, D3D12_RESOURCE_STATE_RENDER_TARGET, DXGI_FORMAT_R32_FLOAT);
    eye_calibration_frame(EyeCalibrationBackend::openxr, 902, 12);
    gpu.begin();
    eye_calibration_stamp12(gpu.list.Get(), source.Get(), 9101, 0, 0, 128, 128);
    eye_calibration_stamp12(gpu.list.Get(), source.Get(), 9102, 0, 0, 128, 128);
    gpu.execute();
    for (unsigned eye = 0; eye < 2; ++eye)
        require(eye_calibration_submit12(target.Get(), gpu.queue.Get(), eye, 0, 0, 1, 1, eye,
                                          EyeCalibrationBackend::openxr, 902) == 0,
                "Unsupported marker format must remain rejected");
    gpu.wait(gpu.queue.Get());
    calibration12_retired(gpu.list.Get());
    eye_calibration_frame(EyeCalibrationBackend::openxr, 902, 12);
    const auto stats = eye_calibration_stats();
    const auto json = eye_calibration_json();
    require(stats.valid == 0 && stats.d3d12_stamp_failures == 2 && stats.d3d12_capture_failures == 2 &&
                stats.d3d12_readback_failures == 1 && !stereo_eye_assignment(9101).calibrated &&
                json.find("\"source_formats\":[41,41]") != std::string::npos &&
                json.find("\"submitted_formats\":[41,41]") != std::string::npos &&
                json.find("stamp_texture_format_or_layout") != std::string::npos &&
                json.find("capture_texture_format_or_layout") != std::string::npos,
            "Rejected captures must report format/stage and never establish a mapping");
    eye_calibration_reset_stats();
    require(eye_calibration_stats().d3d12_stamp_failures == 0 &&
                std::string(eye_calibration_stats().d3d12_last_stamp_failure) == "none",
            "Resetting diagnostic counters must also reset D3D12 failure details");
    cleanup();
}
void run12(EyeCalibrationBackend backend, bool array, bool hardware = false,
           DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, bool converted = false,
           bool flipped = false, bool ambiguous = false, bool shifted = false,
           bool late_begin = false, bool fail_end = false) {
    roles();
    GPU12 gpu(hardware);
    const auto state = backend == EyeCalibrationBackend::openxr ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const std::uint64_t generation = backend == EyeCalibrationBackend::openxr ? 901 : 0;
    // Unity can allocate a larger texture than the logical DLSS output view.
    const auto allocation_width = converted ? 160U : 128U;
    auto a = gpu.texture(allocation_width, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, format),
         b = gpu.texture(allocation_width, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, format);
    const auto target_width = converted ? 192U : array ? 128U : 256U;
    const auto target_height = converted ? 192U : 128U;
    const auto target_format = converted ? DXGI_FORMAT_R16G16B16A16_FLOAT : format;
    auto target = gpu.texture(target_width, array ? 2 : 1, state, target_format, target_height);
    std::unique_ptr<ScaledSubmission12> scaling;
    if (converted) scaling = std::make_unique<ScaledSubmission12>(gpu, a.Get(), b.Get(), target.Get());
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = gpu.device.Get();
    binding.queue = gpu.submit_queue.Get();
    std::unique_ptr<XRLayer> layer;
    if (hardware || converted || late_begin)
        layer = std::make_unique<XRLayer>(target.Get(), 12, &binding, target_width, array ? 2 : 1, target_height);
    D3D12_HEAP_PROPERTIES h{};
    h.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 128 * 128 * 4;
    d.Height = 1;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> black;
    check(gpu.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&black)));
    void* mapped{};
    check(black->Map(0, nullptr, &mapped));
    memset(mapped, 0, 128 * 128 * 4);
    black->Unmap(0, nullptr);
    std::uint64_t warm_allocations{};
    for (unsigned frame = 0; frame < (layer ? 641U : 640U); ++frame) {
        bool sampling{};
        if (layer) {
            if (!late_begin) layer->begin();
        } else
            sampling = eye_calibration_frame(backend, generation, 12);
        gpu.begin();
        for (auto* r : {a.Get(), b.Get()}) {
            gpu.barrier(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = r;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = black.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = {format, 128, 128, 1, 512};
            gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            gpu.barrier(r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        eye_calibration_stamp12(gpu.list.Get(), a.Get(), 9101, 0, 0, 128, 128);
        eye_calibration_stamp12(gpu.list.Get(), b.Get(), 9102, 0, 0, 128, 128);
        if (scaling) {
            scaling->record(gpu, a.Get(), b.Get(), target.Get(), state, frame < 320, flipped, ambiguous, shifted);
        } else {
            gpu.barrier(a.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gpu.barrier(b.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gpu.barrier(target.Get(), state, D3D12_RESOURCE_STATE_COPY_DEST);
            gpu.copy(target.Get(), 0, 0, frame < 320 ? b.Get() : a.Get());
            gpu.copy(target.Get(), array ? 1 : 0, array ? 0 : 128, frame < 320 ? a.Get() : b.Get());
            gpu.barrier(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
            gpu.barrier(a.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gpu.barrier(b.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        gpu.execute();
        check(gpu.queue->Signal(gpu.fence.Get(), ++gpu.value));
        check(gpu.submit_queue->Wait(gpu.fence.Get(), gpu.value));
        if (layer) {
            // Reproduce Mortal Shell 2: both DLSS evaluations have already
            // happened when the host opens the OpenXR frame for submission.
            if (late_begin) layer->begin();
            layer->release();
            layer->projection_mode = frame % 3;
            XRLayer::end_result = fail_end && frame == 11 ? XR_ERROR_TIME_INVALID : XR_SUCCESS;
            layer->end(false, array);
        } else
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto ticket = eye_calibration_submit12(
                    target.Get(), gpu.submit_queue.Get(), eye, array ? 0 : eye * .5F, 0,
                    array ? 1 : (eye + 1) * .5F, 1, array ? eye : 0, backend, generation);
                require((ticket != 0) == sampling, "D3D12 capture must follow the frame sampling cadence");
                eye_calibration_result(ticket, 0);
            }
        gpu.wait(gpu.submit_queue.Get());
        calibration12_retired(gpu.list.Get());
        eye_calibration_tick();
        if (frame == 319 && !ambiguous) {
            require(eye_calibration_stats().corrections == 1 && stereo_eye_assignment(9101).eye_index == 1,
                    "D3D12 initial eye swap was not corrected once");
            warm_allocations = eye_calibration_stats().allocations;
        }
    }
    if (layer)
        layer->begin();
    else
        eye_calibration_frame(backend, generation, 12);
    const auto stats = eye_calibration_stats();
    if (ambiguous) {
        require(stats.valid == 0 && !stereo_eye_assignment(9101).calibrated &&
                    stats.rejection_counts[7] >= 63,
                "Conflicting orientation evidence must reject every frame and leave gaze unmapped");
        cleanup();
        std::cout << "D3D12 conflicting vertical orientations rejected\n";
        return;
    }
    require(stats.valid == (fail_end ? 63U : 64U) && stats.corrections == 2 && stereo_eye_assignment(9101).eye_index == 0,
            "D3D12 transition must correct exactly once");
    if (fail_end) require(stats.rejection_counts[3] >= 1,
                         "Failed EndFrame must reject its capture and allow later samples to recover");
    require(stereo_eye_assignment(9101).vertical_flip == flipped &&
                stereo_eye_assignment(9102).vertical_flip == flipped,
            "Calibration must retain the verified image orientation for gaze projection");
    require(stats.allocations == warm_allocations, "D3D12 allocations must stop after pool warmup");
    require(stats.gpu_samples > 0 && stats.gpu_us > 0, "D3D12 calibration timestamps missing");
    require(stats.d3d12_source_formats[0] == unsigned(format) &&
                stats.d3d12_source_formats[1] == unsigned(format) &&
                stats.d3d12_submitted_formats[0] == unsigned(target_format) &&
                stats.d3d12_submitted_formats[1] == unsigned(target_format) &&
                !stats.d3d12_stamp_failures && !stats.d3d12_capture_failures,
            "Calibration diagnostics must identify both source/submission formats without failures");
    std::cout << "D3D12 " << eye_calibration_backend_name(backend) << (array ? " array" : " packed")
              << (hardware ? " hardware" : " WARP") << " format=" << unsigned(format)
              << (converted ? " -> FP16 at 1.5x via shader and layer DLL" : "")
              << (late_begin ? " late BeginFrame" : "")
              << ": " << stats.valid << " valid, 2 corrections, stable allocations\n";
    cleanup();
}
} // namespace
int run_openxr_calibration_format_tests() {
    try {
        for (bool flipped : {false, true})
            run12(EyeCalibrationBackend::openxr, true, false, DXGI_FORMAT_R11G11B10_FLOAT,
                  true, flipped, false, true);
        failure_diagnostics12();
        run12(EyeCalibrationBackend::openxr, true, false, DXGI_FORMAT_R11G11B10_FLOAT, true, true);
        run12(EyeCalibrationBackend::openxr, true, false, DXGI_FORMAT_R11G11B10_FLOAT, true, false, true);
        run12(EyeCalibrationBackend::openxr, true, true, DXGI_FORMAT_R11G11B10_FLOAT, true, true);
        run12(EyeCalibrationBackend::openxr, true, false, DXGI_FORMAT_R11G11B10_FLOAT);
        run12(EyeCalibrationBackend::openxr, true, false, DXGI_FORMAT_R11G11B10_FLOAT, true);
        run12(EyeCalibrationBackend::openxr, true, true, DXGI_FORMAT_R11G11B10_FLOAT, true);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "HDR calibration: " << e.what() << '\n' << eye_calibration_json() << '\n';
        cleanup();
        return 1;
    }
}
int run_openxr_calibration_tests() {
    try {
        layer_policy();
        projection_policy();
        openxr11();
        recording_lifetime12();
        for (bool array : {false, true})
            run12(EyeCalibrationBackend::openxr, array, false, DXGI_FORMAT_R8G8B8A8_UNORM,
                  false, false, false, false, true);
        run12(EyeCalibrationBackend::openxr, false, false, DXGI_FORMAT_R8G8B8A8_UNORM,
              false, false, false, false, true, true);
        for (auto backend : {EyeCalibrationBackend::openvr, EyeCalibrationBackend::openxr})
            for (bool array : {false, true})
                run12(backend, array);
        run12(EyeCalibrationBackend::openxr, true, true);
        return 0;
    } catch (const std::exception& e) {
        cleanup();
        std::cerr << "OpenXR/D3D12 calibration: " << e.what() << '\n';
        return 1;
    }
}
