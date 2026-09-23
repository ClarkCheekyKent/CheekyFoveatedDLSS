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
#include <thread>
#include <future>
#include <condition_variable>
#include <d3d11_4.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include "../third_party/openxr/include/openxr/openxr.h"
#include "../third_party/openxr/include/openxr/openxr_platform.h"
#include "../third_party/openxr/include/openxr/openxr_loader_negotiation.h"
#include "../openxr_layer/projection_selection.hpp"
#include "cheeky_gaze_abi.h"
#include "timing_list_alias.hpp"

namespace {
using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
class XRThread {
    std::mutex mutex;
    std::condition_variable changed;
    std::packaged_task<void()> work;
    bool stopping{};
    std::thread thread{[this] {
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait(lock, [&] { return stopping || work.valid(); });
            if (stopping) return;
            auto call = std::move(work);
            lock.unlock(); call(); lock.lock();
        }
    }};
public:
    template<class F> void invoke(F call) {
        std::packaged_task<void()> task(std::move(call));
        auto result = task.get_future();
        { std::lock_guard lock(mutex); work = std::move(task); }
        changed.notify_one(); result.get();
    }
    ~XRThread() {
        { std::lock_guard lock(mutex); stopping = true; }
        changed.notify_one(); thread.join();
    }
};
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
void openxr11_pipeline(bool hardware, bool separate_device = false, bool support_images = false) {
    roles();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    D3D11_TEXTURE2D_DESC desc{128, 128, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT,
        {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
    ComPtr<ID3D11Texture2D> a, b, array, stale, xr_array, transfer;
    check(device->CreateTexture2D(&desc, nullptr, &a));
    check(device->CreateTexture2D(&desc, nullptr, &b));
    desc.ArraySize = 2;
    check(device->CreateTexture2D(&desc, nullptr, &array));
    check(device->CreateTexture2D(&desc, nullptr, &stale));
    ComPtr<ID3D11Device> xr_device = device;
    ComPtr<ID3D11DeviceContext> xr_context = context;
    xr_array = array;
    if (separate_device) {
        check(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &xr_device, nullptr, &xr_context));
        check(xr_device->CreateTexture2D(&desc, nullptr, &xr_array));
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc, nullptr, &transfer));
    }
    std::vector<unsigned> black(128 * 128);
    context->UpdateSubresource(b.Get(), 0, nullptr, black.data(), 512, 0);
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = xr_device.Get();
    XRLayer layer(xr_array.Get(), 11, &binding);
    XRThread worker;
    const auto xr = [&](auto call) { worker.invoke(call); };
    const auto render = [&](bool swap, bool replay, bool mono, unsigned alternating = 0) {
        xr([&] { layer.begin(); });
        if (alternating != 2) {
            context->UpdateSubresource(a.Get(), 0, nullptr, black.data(), 512, 0);
            eye_calibration_stamp(context.Get(), a.Get(), 9101, 0, 0, 128, 128);
        }
        if (replay) context->CopyResource(array.Get(), stale.Get());
        else {
            context->CopySubresourceRegion(array.Get(), 0, 0, 0, 0, a.Get(), 0, nullptr);
            context->CopySubresourceRegion(array.Get(), 1, 0, 0, 0, mono ? a.Get() : b.Get(), 0, nullptr);
            context->CopyResource(stale.Get(), array.Get());
        }
        if (separate_device) {
            // Test-only transport models host composition onto a second
            // device. Calibration itself never copies between devices.
            context->CopyResource(transfer.Get(), array.Get());
            for (unsigned eye = 0; eye < 2; ++eye) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                check(context->Map(transfer.Get(), eye, D3D11_MAP_READ, 0, &mapped));
                xr([&] { xr_context->UpdateSubresource(xr_array.Get(), eye, nullptr,
                    mapped.pData, mapped.RowPitch, 0); });
                context->Unmap(transfer.Get(), eye);
            }
        }
        xr([&] {
            layer.release();
            // Runtime can overwrite released images before EndFrame.
            xr_context->UpdateSubresource(xr_array.Get(), 0, nullptr, black.data(), 512, 0);
            xr_context->UpdateSubresource(xr_array.Get(), 1, nullptr, black.data(), 512, 0);
            if (separate_device) xr_context->Flush();
        });
        if (!mono && alternating != 1) {
            context->UpdateSubresource(b.Get(), 0, nullptr, black.data(), 512, 0);
            eye_calibration_stamp(context.Get(), b.Get(), 9102, 0, 0, 128, 128);
        }
        xr([&] { layer.end(swap); });
        context->Flush(); // GPU progress is test-only.
        Sleep(2);
        eye_calibration_tick();
    };
    auto support = support_images ? request_calibration_images(true) : CalibrationImageRequestPtr{};
    // Matches the BG3 trace: A, XR release on another thread, B, XR end.
    for (unsigned i = 0; i < 100; ++i) render(false, false, false);
    const auto warm = eye_calibration_stats();
    require(warm.applied > 0 && stereo_eye_assignment(9101).calibrated &&
        stereo_eye_assignment(9101).eye_index == 0,
        "Cross-thread AER pipeline must map using both pre-release eye copies");
    if (support) {
        const auto report = collect_calibration_images(support);
        require(report.files.size() == 5 && report.files[0].contents == report.files[2].contents &&
                    report.files[1].contents == report.files[3].contents,
                "AER support images must retain both source eyes and capture submitted pixels before XR reuses them");
        require(report.diagnostics.find("\"array_slice\":1") != std::string::npos,
                "AER diagnostics must identify each submitted array slice");
    }
    ComPtr<ID3D11Multithread> protection;
    check(context.As(&protection));
    require(protection->GetMultithreadProtected(), "Cross-thread copies require D3D context protection");
    // One eye render per interval also works; changing labels must still correct.
    for (unsigned i = 0; i < 100; ++i) render(true, false, false, 1 + i % 2);
    require(eye_calibration_stats().valid > warm.valid && stereo_eye_assignment(9101).eye_index == 1,
        "Alternating single-eye renders must calibrate across XR intervals");
    require(eye_calibration_stats().allocations == warm.allocations,
        "Continuous source stamping must reuse warmed GPU resources");
    // Drain before measuring rejection of the frozen previous submission.
    eye_calibration_enable(false);
    render(true, true, false);
    for (unsigned i = 0; i < 20; ++i) {
        Sleep(2); xr([] { eye_calibration_tick(); }); eye_calibration_tick();
    }
    eye_calibration_reset_stats();
    eye_calibration_enable(true);
    for (unsigned i = 0; i < 100; ++i) render(true, true, false);
    require(!eye_calibration_stats().valid && !stereo_eye_assignment(9101).calibrated,
        "Markers from a previous calibration epoch must not validate a new capture");
    for (unsigned i = 0; i < 100; ++i) render(false, false, true);
    require(eye_calibration_stats().valid && eye_calibration_stats().in_flight < 8 &&
        stereo_eye_assignment(9101).shared_source && !stereo_eye_assignment(9102).calibrated,
        "Mono must map its one source to both eyes without inventing a second source or exhausting the ring");
    eye_calibration_enable(false); render(false, false, true);
    for (unsigned i = 0; i < 20; ++i) {
        Sleep(2); xr([] { eye_calibration_tick(); }); eye_calibration_tick();
    }
    eye_calibration_reset_stats(); eye_calibration_enable(true);
    XRLayer::release_result = XR_ERROR_RUNTIME_FAILURE;
    for (unsigned i = 0; i < 40; ++i) render(false, false, false);
    require(!eye_calibration_stats().valid, "Failed releases must reject pipelined captures");
    XRLayer::release_result = XR_SUCCESS;
    for (unsigned i = 0; i < 40; ++i) render(false, false, false);
    require(stereo_eye_assignment(9101).calibrated, "Capture must recover after failed releases");
    eye_calibration_enable(false);
    render(false, false, false);
    context->Flush();
    xr([&] { xr_context->Flush(); });
    const auto deadline = GetTickCount64() + 5000;
    while (eye_calibration_stats().in_flight && GetTickCount64() < deadline) {
        Sleep(1); xr([] { eye_calibration_tick(); }); eye_calibration_tick();
    }
    require(!eye_calibration_stats().in_flight, "Pipelined captures must drain after disable");
    cleanup();
    std::cout << "OpenXR D3D11 " << (hardware ? "hardware" : "WARP")
        << (separate_device ? " two devices" : " one device")
        << ": cross-thread AER, alternating eyes, stale markers and failed releases passed\n";
}
void openxr11_singlethreaded() {
    roles();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    D3D11_TEXTURE2D_DESC desc{128, 128, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
        {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
    ComPtr<ID3D11Texture2D> a, b;
    check(device->CreateTexture2D(&desc, nullptr, &a));
    check(device->CreateTexture2D(&desc, nullptr, &b));
    std::thread begin([] { eye_calibration_frame(EyeCalibrationBackend::openxr, 951, 11); });
    begin.join();
    eye_calibration_stamp(context.Get(), a.Get(), 9101, 0, 0, 128, 128);
    eye_calibration_stamp(context.Get(), b.Get(), 9102, 0, 0, 128, 128);
    std::uint64_t ticket{1};
    std::thread submit([&] {
        ticket = eye_calibration_submit(a.Get(), 0, 0, 0, 1, 1, 0, EyeCalibrationBackend::openxr, 951);
        eye_calibration_enable(false);
        eye_calibration_frame(EyeCalibrationBackend::openxr, 951, 11);
    });
    submit.join();
    require(!ticket, "SINGLETHREADED device must reject foreign-thread submission without D3D calls");
    eye_calibration_tick();
    context->Flush();
    const auto deadline = GetTickCount64() + 5000;
    while (eye_calibration_stats().in_flight && GetTickCount64() < deadline) {
        Sleep(1); eye_calibration_tick();
    }
    require(!eye_calibration_stats().in_flight && !stereo_eye_assignment(9101).calibrated,
        "Unsupported threading must drain without publishing a pair");
    cleanup();
}
void openxr11_context_diagnostics(bool separate_device) {
    roles();
    XRThread worker;
    ComPtr<ID3D11Device> source_device, submitted_device;
    ComPtr<ID3D11DeviceContext> source_context, submitted_context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &source_device, nullptr, &source_context));
    if (separate_device) {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &submitted_device, nullptr, &submitted_context));
    } else {
        submitted_device = source_device;
        submitted_context = source_context;
    }
    D3D11_TEXTURE2D_DESC desc{128, 128, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
        {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
    ComPtr<ID3D11Texture2D> a, b, submitted;
    check(source_device->CreateTexture2D(&desc, nullptr, &a));
    check(source_device->CreateTexture2D(&desc, nullptr, &b));
    check(submitted_device->CreateTexture2D(&desc, nullptr, &submitted));
    worker.invoke([] { eye_calibration_frame(EyeCalibrationBackend::openxr, 952, 11); });
    eye_calibration_stamp(source_context.Get(), a.Get(), 9101, 0, 0, 128, 128);
    eye_calibration_stamp(source_context.Get(), b.Get(), 9102, 0, 0, 128, 128);
    ComPtr<ID3D11Multithread> protection;
    check(source_context.As(&protection));
    require(protection->GetMultithreadProtected(), "Source context protection was enabled");
    if (!separate_device) protection->SetMultithreadProtected(FALSE);
    std::uint64_t ticket{1};
    worker.invoke([&] {
        ticket = eye_calibration_submit(submitted.Get(), 0, 0, 0, 1, 1, 0, EyeCalibrationBackend::openxr, 952);
        eye_calibration_enable(false);
        eye_calibration_frame(EyeCalibrationBackend::openxr, 952, 11);
    });
    require(bool(ticket) == separate_device, "Separate-device capture is supported; lost source protection is rejected");
    const auto json = eye_calibration_json();
    require(json.find(separate_device ? "\"rejection\":\"none\"" :
        "\"rejection\":\"submission_context_unprotected\"") != std::string::npos,
        "Diagnostics must distinguish a second context from protection turned off on the same context");
    require(json.find(separate_device ? "\"source_protected_when_stamped\":true,\"submitted_protected\":true" :
        "\"source_protected_when_stamped\":true,\"submitted_protected\":false") != std::string::npos,
        "Report must distinguish cached source protection from observed submission protection");
    ComPtr<IUnknown> identity;
    check(submitted_device.As(&identity));
    require(json.find("\"submitted_device\":\"" +
        std::to_string(reinterpret_cast<std::uintptr_t>(identity.Get())) + "\"") != std::string::npos,
        "Report must identify the actual submission device using COM identity");
    eye_calibration_tick();
    source_context->Flush();
    worker.invoke([&] { submitted_context->Flush(); });
    const auto deadline = GetTickCount64() + 5000;
    while (eye_calibration_stats().in_flight && GetTickCount64() < deadline) {
        Sleep(1); worker.invoke([] { eye_calibration_tick(); }); eye_calibration_tick();
    }
    require(!eye_calibration_stats().in_flight && !stereo_eye_assignment(9101).calibrated,
        "Rejected context must drain without an eye assignment");
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
// Model the older R.E.A.L. VR device address returned by GetDevice. Its COM
// identity is native, but its ID3D12Device pointer differs from texture.GetDevice.
struct CalibrationDeviceAlias {
    void** vtable;
    std::array<void*, 44> methods;
    ID3D12Device* target;
    bool distinct{};
    bool shared_private_data{};
    explicit CalibrationDeviceAlias(ID3D12Device* device) : vtable(methods.data()), target(device) {
        methods.fill(reinterpret_cast<void*>(&TimingListAlias::unexpected));
        methods[0] = reinterpret_cast<void*>(&query); methods[1] = reinterpret_cast<void*>(&addref);
        methods[2] = reinterpret_cast<void*>(&release); methods[37] = reinterpret_cast<void*>(&removed);
        methods[3] = reinterpret_cast<void*>(&get_private); methods[5] = reinterpret_cast<void*>(&set_interface);
    }
    static HRESULT STDMETHODCALLTYPE query(CalibrationDeviceAlias* s, REFIID id, void** out) {
        if (s->distinct) {
            if (!out) return E_POINTER;
            *out = nullptr;
            if (id != __uuidof(IUnknown) && id != __uuidof(ID3D12Device)) return E_NOINTERFACE;
            *out = s; addref(s); return S_OK;
        }
        return s->target->QueryInterface(id, out);
    }
    static ULONG STDMETHODCALLTYPE addref(CalibrationDeviceAlias* s) { return s->target->AddRef(); }
    static ULONG STDMETHODCALLTYPE release(CalibrationDeviceAlias* s) { return s->target->Release(); }
    static HRESULT STDMETHODCALLTYPE removed(CalibrationDeviceAlias* s) { return s->target->GetDeviceRemovedReason(); }
    static HRESULT STDMETHODCALLTYPE get_private(CalibrationDeviceAlias* s, REFGUID key, UINT* size, void* data) {
        return s->shared_private_data ? s->target->GetPrivateData(key, size, data) : DXGI_ERROR_NOT_FOUND;
    }
    static HRESULT STDMETHODCALLTYPE set_interface(CalibrationDeviceAlias* s, REFGUID key, const IUnknown* value) {
        return s->shared_private_data ? s->target->SetPrivateDataInterface(key, value) : E_FAIL;
    }
};
struct CalibrationListAlias : TimingListAlias {
    CalibrationDeviceAlias device;
    CalibrationListAlias(ID3D12GraphicsCommandList* list, ID3D12Device* d) : TimingListAlias(list), device(d) {
        methods[7] = reinterpret_cast<void*>(&get_device);
        methods[8] = reinterpret_cast<void*>(&get_type);
        methods[16] = reinterpret_cast<void*>(&copy_texture);
        methods[26] = reinterpret_cast<void*>(&barriers);
    }
    static HRESULT STDMETHODCALLTYPE get_device(CalibrationListAlias* s, REFIID id, void** out) {
        if (id != __uuidof(ID3D12Device)) return s->target->GetDevice(id, out);
        *out = &s->device; CalibrationDeviceAlias::addref(&s->device); return S_OK;
    }
    static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE get_type(CalibrationListAlias* s) { return s->target->GetType(); }
    static void STDMETHODCALLTYPE copy_texture(CalibrationListAlias* s, const D3D12_TEXTURE_COPY_LOCATION* d,
        UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* r, const D3D12_BOX* b) {
        s->target->CopyTextureRegion(d, x, y, z, r, b);
    }
    static void STDMETHODCALLTYPE barriers(CalibrationListAlias* s, UINT n, const D3D12_RESOURCE_BARRIER* b) {
        s->target->ResourceBarrier(n, b);
    }
};
void calibration_device_identity12() {
    GPU12 gpu;
    auto texture = gpu.texture(128, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto frame = calibration12_create(gpu.device.Get());
    require(calibration12_begin(*frame), "Device identity test frame initialization");
    CalibrationListAlias alias(gpu.list.Get(), gpu.device.Get());
    std::uint64_t allocations{};
    Calibration12Failure failure;
    gpu.begin();
    const bool stamp_accepted = calibration12_stamp(*frame, alias.get(), texture.Get(), 0, 12, 12,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, allocations, &failure);
    gpu.execute(); gpu.wait(gpu.queue.Get()); calibration12_retired(gpu.list.Get());
    require(stamp_accepted, "Wrapped source and native texture with the same COM device must not fail stamp_device_mismatch");
    require(calibration12_poll(*frame).ready, "Native submission/reset must retire a wrapped calibration recording");
    require(calibration12_begin(*frame), "Wrapped calibration resources must be reusable after retirement");
    alias.device.distinct = true;
    alias.device.shared_private_data = true;
    gpu.begin();
    const auto opaque_accepted = calibration12_stamp(*frame, alias.get(), texture.Get(), 0, 12, 12,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, allocations, &failure);
    gpu.execute(); gpu.wait(gpu.queue.Get()); calibration12_retired(gpu.list.Get());
    require(opaque_accepted, "Opaque wrapper identity must be verified through shared device-private data");
    require(calibration12_begin(*frame), "Opaque source must retire through the native queue");
    alias.device.shared_private_data = false;
    gpu.begin();
    require(!calibration12_stamp(*frame, alias.get(), texture.Get(), 0, 12, 12,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, allocations, &failure) &&
        std::string(failure.stage) == "stamp_device_mismatch", "Different device identities must remain rejected");
    check(gpu.list->Close());
    std::cout << "PASS calibration device identity: aliases accepted, distinct devices rejected\n";
}
void refresh_recording_lifetime12() {
    GPU12 gpu;
    auto texture = gpu.texture(128, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto frame = calibration12_create(gpu.device.Get());
    require(calibration12_begin(*frame), "Fresh refresh-lifetime frame");
    std::uint64_t allocations{};
    gpu.begin();
    require(calibration12_stamp(*frame, gpu.list.Get(), texture.Get(), 0, 12, 12,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, allocations), "Initial refresh-lifetime stamp");
    gpu.execute(); gpu.wait(gpu.queue.Get()); calibration12_retired(gpu.list.Get());
    const auto first = calibration12_poll(*frame, true);
    gpu.begin();
    require(calibration12_stamp(*frame, gpu.list.Get(), texture.Get(), 0, 12, 12,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, allocations, nullptr, {}, {}, 0, Calibration12StampMode::refresh), "Marker refresh on a new recording");
    Calibration12Failure failure;
    require(!calibration12_stamp(*frame, gpu.list.Get(), texture.Get(), 0, 12, 12,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, allocations, &failure, {}, {}, 42, Calibration12StampMode::refresh) &&
        std::string(failure.stage) == "refresh_source_changed", "An in-flight upload must not change marker epochs");
    ComPtr<ID3D12Fence> gate;
    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    check(gpu.queue->Wait(gate.Get(), 1));
    gpu.execute(); calibration12_retired(gpu.list.Get());
    const auto blocked = calibration12_poll(*frame, true);
    const auto reused_early = calibration12_begin(*frame);
    check(gate->Signal(1)); // Always release the test queue before any assertion.
    gpu.wait(gpu.queue.Get());
    require(!blocked.ready && !reused_early, "Pending marker refresh must retain the upload and prevent slot reuse");
    const auto after = calibration12_poll(*frame, true);
    require(after.ready && after.reusable && after.scores[0] == first.scores[0] && after.scores[1] == first.scores[1],
        "Refresh must retain the original before/after marker proof");
    require(calibration12_begin(*frame), "Refresh resources must drain after its fence completes");
    std::cout << "PASS DX12 marker refresh lifetime: pending fence, immutable upload and original proof\n";
}
void continuous_marker_lifetime12() {
    GPU12 gpu;
    auto texture = gpu.texture(128, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto frame = calibration12_create(gpu.device.Get());
    std::uint64_t allocations{};
    require(calibration12_begin(*frame), "Fresh marker-only frame");
    gpu.begin();
    require(calibration12_stamp(*frame, gpu.list.Get(), texture.Get(), 0, 12, 12,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, allocations, nullptr, {}, {}, 12345,
        Calibration12StampMode::marker_only), "Marker-only stamp");
    require(allocations == 1, "Marker-only work must allocate just an upload, with no readback or timestamp objects");
    gpu.execute(); gpu.wait(gpu.queue.Get());
    require(!calibration12_begin(*frame), "Replayable marker-only recording must retain its upload");
    ComPtr<ID3D12Fence> gate;
    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    check(gpu.submit_queue->Wait(gate.Get(), 1));
    ID3D12CommandList* lists[]{gpu.list.Get()};
    gpu.submit_queue->ExecuteCommandLists(1, lists);
    calibration12_submitted(gpu.submit_queue.Get(), gpu.list.Get());
    calibration12_retired(gpu.list.Get());
    const bool reused = calibration12_begin(*frame);
    check(gate->Signal(1)); // Release the owned test queue before asserting.
    gpu.wait(gpu.submit_queue.Get());
    require(!reused && calibration12_begin(*frame, &allocations),
        "Marker-only upload must survive retirement until every queue finishes");
    const auto warmed = allocations;
    gpu.begin();
    require(calibration12_stamp(*frame, gpu.list.Get(), texture.Get(), 0, 12, 12,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, allocations, nullptr, {}, {}, 12345,
        Calibration12StampMode::marker_only) && allocations == warmed, "Marker-only upload must be reusable without allocations");
    check(gpu.list->Close()); gpu.begin();
    require(calibration12_begin(*frame), "Unsubmitted marker-only recording must drain after Reset");
    check(gpu.list->Close());

    roles();
    // Move outside the sampled window, then keep every upload recording live.
    for (unsigned i = 0; i < 6; ++i) eye_calibration_frame(EyeCalibrationBackend::openxr, 993, 11);
    gpu.begin();
    for (unsigned i = 0; i < 64; ++i)
        eye_calibration_stamp12(gpu.list.Get(), texture.Get(), 9101, 0, 0, 128, 128);
    const auto saturated = eye_calibration_stats();
    require(saturated.d3d12_continuous_stamps == 16 && saturated.d3d12_continuous_skipped == 48 &&
        saturated.allocations == 16, "A full marker pool must skip without blocking, growing, or overwriting live uploads");
    gpu.execute(); gpu.wait(gpu.queue.Get()); gpu.begin();
    eye_calibration_stamp12(gpu.list.Get(), texture.Get(), 9101, 0, 0, 128, 128);
    require(eye_calibration_stats().d3d12_continuous_stamps == 17, "Continuous stamps must resume after pool retirement");
    const auto enabled_stamps = eye_calibration_stats().d3d12_continuous_stamps;
    eye_calibration_enable(false);
    eye_calibration_stamp12(gpu.list.Get(), texture.Get(), 9101, 0, 0, 128, 128);
    require(eye_calibration_stats().d3d12_continuous_stamps == enabled_stamps, "Disabling calibration must stop continuous stamps");
    gpu.execute(); gpu.wait(gpu.queue.Get()); calibration12_retired(gpu.list.Get());
    cleanup();
    std::cout << "PASS continuous DX12 markers: no readbacks, queue/recording lifetime, bounded pool, disable\n";
}
void mixed_api12to11(bool wrapped, bool flipped, bool cropped = false, bool delayed = false, bool mono = false) {
    roles();
    if (mono) unregister_stereo_view(9102);
    unsigned mono_source = mono ? 0U : 2U;
    bool missing_eye{};
    GPU12 gpu;
    CalibrationListAlias alias(gpu.list.Get(), gpu.device.Get());
    if (wrapped) { alias.device.distinct = true; alias.device.shared_private_data = true; }
    constexpr auto format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        gpu.texture(128, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, format),
        gpu.texture(128, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, format)};
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer_desc.Width = 128 * 128 * 8;
    buffer_desc.Height = buffer_desc.SampleDesc.Count = 1;
    buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> black, transfer;
    check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&black)));
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&transfer)));
    void* bytes{}; check(black->Map(0, nullptr, &bytes)); memset(bytes, 0, SIZE_T(buffer_desc.Width)); black->Unmap(0, nullptr);
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device11, nullptr, &context11));
    constexpr unsigned target_size = 192;
    D3D11_TEXTURE2D_DESC desc{target_size, target_size, 1, 2, DXGI_FORMAT_R8G8B8A8_UNORM,
        {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
    ComPtr<ID3D11Texture2D> submitted;
    check(device11->CreateTexture2D(&desc, nullptr, &submitted));
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; binding.device = device11.Get();
    XRLayer layer(submitted.Get(), 11, &binding, target_size, 2, target_size);
    XRThread xr;
    std::array<std::vector<unsigned char>, 2> transported;
    for (auto& pixels : transported) pixels.resize(target_size * target_size * 4);
    const std::vector<unsigned char> blank(target_size * target_size * 4);
    const auto render_eye = [&](unsigned eye) {
        gpu.begin();
        auto* source = sources[eye].Get();
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{}, readback{};
        src.pResource = black.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {format, 128, 128, 1, 1024};
        dst.pResource = source; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        gpu.barrier(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        gpu.barrier(source, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        eye_calibration_stamp12(wrapped ? alias.get() : gpu.list.Get(), source, 9101 + eye, 0, 0, 128, 128);
        gpu.barrier(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        readback = src; readback.pResource = transfer.Get();
        gpu.list->CopyTextureRegion(&readback, 0, 0, 0, &dst, nullptr);
        gpu.barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.execute(); gpu.wait(gpu.queue.Get()); calibration12_retired(gpu.list.Get());
        // Test-only host transport. Production calibration never moves textures
        // between devices or waits for a queue. Model resize/flip/crop by the host.
        check(transfer->Map(0, nullptr, &bytes));
        for (unsigned y = 0; y < target_size; ++y)
            for (unsigned x = 0; x < target_size; ++x) {
                const unsigned sx = cropped ? 52 + x * 24 / target_size : x * 128 / target_size;
                const unsigned sy = flipped ? 127 - y * 128 / target_size : y * 128 / target_size;
                const auto p = calibration_decode(static_cast<const unsigned char*>(bytes) + sy * 1024 + sx * 8, format);
                auto* out = transported[eye].data() + (y * target_size + x) * 4;
                out[0] = static_cast<unsigned char>(p.r * 255); out[1] = static_cast<unsigned char>(p.g * 255);
                out[2] = static_cast<unsigned char>(p.b * 255); out[3] = 255;
            }
        transfer->Unmap(0, nullptr);
    };
    CalibrationImageRequestPtr support;
    std::vector<std::array<std::vector<unsigned char>, 2>> history;
    const auto frame = [&](bool swap, bool replay, bool alternating, unsigned n) {
        xr.invoke([&] { layer.begin(); });
        if (mono_source < 2) render_eye(mono_source);
        else {
            if (!alternating || !(n & 1)) render_eye(0);
            if (alternating && (n & 1)) render_eye(1);
            if (delayed && !alternating) render_eye(1);
        }
        if (delayed) history.push_back(transported);
        xr.invoke([&] {
            if (!replay) for (unsigned eye = 0; eye < 2; ++eye) {
                const unsigned source = mono_source < 2 ? mono_source : eye;
                const auto& pixels = missing_eye && eye == 1 ? blank :
                    delayed ? (history.size() > 3 ? history.front()[source] : blank) : transported[source];
                context11->UpdateSubresource(submitted.Get(), eye, nullptr, pixels.data(), target_size * 4, 0);
            }
            layer.release();
            if (!replay) for (unsigned eye = 0; eye < 2; ++eye)
                context11->UpdateSubresource(submitted.Get(), eye, nullptr, blank.data(), target_size * 4, 0);
            context11->Flush();
        });
        if (delayed && history.size() > 3) history.erase(history.begin());
        if (mono_source == 2 && !alternating && !delayed) render_eye(1);
        xr.invoke([&] { layer.end(swap); eye_calibration_tick(); });
        Sleep(2); eye_calibration_tick();
    };
    if (mono) {
        for (unsigned i = 0; i < 60; ++i) frame(false, false, false, i);
        require(stereo_eye_assignment(9101).calibrated && eye_calibration_stats().left_view == 9101 &&
            eye_calibration_stats().right_view == 9101,
            "Cold mono must calibrate one source observed in both submitted eyes without prior stereo");
        support = request_calibration_images(true);
        for (unsigned i = 0; i < 20; ++i) frame(false, false, false, i);
        const auto report = collect_calibration_images(support);
        require(report.files.size() == 4 && report.diagnostics.find("\"status\":\"complete\"") != std::string::npos &&
            report.diagnostics.find("not_applicable_single_source") != std::string::npos,
            "Mono support report must contain one source and two submitted images without a missing-source timeout");
        register_stereo_view(9102); mono_source = 2;
        for (unsigned i = 0; i < 80; ++i) frame(false, false, false, i);
        require(eye_calibration_stats().left_view == 9101 && eye_calibration_stats().right_view == 9102,
            "Mono-to-stereo must replace the shared mapping with distinct sources");
        mono_source = 1;
        for (unsigned i = 0; i < 80; ++i) frame(false, false, false, i);
        require(eye_calibration_stats().left_view == 9102 && eye_calibration_stats().right_view == 9102,
            "Stereo-to-mono must calibrate candidate B even while candidate A remains registered");
        eye_calibration_enable(false); frame(false, false, false, 0);
        eye_calibration_enable(true); missing_eye = true;
        for (unsigned i = 0; i < 60; ++i) frame(false, false, false, i);
        require(!stereo_eye_assignment(9102).calibrated,
            "One submitted eye containing a marker is insufficient proof of mono");
        missing_eye = false;
        for (unsigned i = 0; i < 60; ++i) frame(false, false, false, i);
        require(stereo_eye_assignment(9102).calibrated, "Mono must recover with fresh evidence in both eyes");
        unregister_stereo_view(9102); register_stereo_view(9102);
        require(!stereo_eye_assignment(9102).calibrated, "Recreated mono source must invalidate its calibration");
        eye_calibration_enable(false); frame(false, false, false, 0);
        for (unsigned i = 0; i < 20; ++i) { xr.invoke([] { eye_calibration_tick(); }); eye_calibration_tick(); Sleep(1); }
        cleanup();
        std::cout << "PASS mono OpenXR: " << (wrapped ? "wrapped/flipped" : "native")
            << ", cold start, three images, stereo transitions, missing-eye rejection and handle lifetime\n";
        return;
    }
    if (delayed) {
        // A host may submit an image rendered several intervals ago, including
        // intervals between calibration samples. Re-rendering clears old stamps.
        for (unsigned i = 0; i < 40; ++i) frame(false, false, false, i);
        require(stereo_eye_assignment(9101).calibrated,
            "Delayed DX12-to-DX11 submissions from unsampled renders must establish eye mapping");
        const auto capture_count = eye_calibration_stats().captures;
        for (unsigned i = 0; i < 20; ++i) {
            frame(false, false, false, i);
            for (const auto& pixels : transported) {
                bool marked{};
                for (std::size_t p = 0; p < pixels.size(); p += 4) marked |= pixels[p] != 0;
                require(marked, "Every DX12 source render must be stamped between readback samples");
            }
        }
        require(eye_calibration_stats().captures - capture_count == 2,
            "Continuous DX12 stamping must retain one-in-ten readback cadence");
    }
    support = request_calibration_images(true);
    for (unsigned i = 0; i < 100; ++i) frame(false, false, false, i);
    const auto report = collect_calibration_images(support);
    require(report.files.size() == 5, "DX12-to-DX11 support ZIP must contain both stamped and both submitted images");
    require(report.diagnostics.find("\"graphics_api\":12") != std::string::npos &&
        report.diagnostics.find("\"graphics_api\":11") != std::string::npos,
        "Mixed support images must identify their source and submission APIs");
    require(eye_calibration_stats().source_graphics_api == 12 && eye_calibration_stats().submission_graphics_api == 11,
        "Calibration diagnostics must retain both API identities rather than whichever handler ran last");
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto& source = report.files[eye].contents;
        const auto& destination = report.files[eye + 2].contents;
        require(source.size() < 1000000 && destination.size() < 1000000, "Mixed support previews must stay below 1 MB each");
        require(std::any_of(source.begin() + 54, source.end(), [](char value) { return value != 0; }),
            "Mixed source preview must contain the actual stamped pattern");
        for (unsigned y = 0; y < target_size; ++y)
            for (unsigned x = 0; x < target_size; ++x) {
                const unsigned sx = cropped ? 52 + x * 24 / target_size : x * 128 / target_size;
                const unsigned sy = flipped ? 127 - y * 128 / target_size : y * 128 / target_size;
                require(destination.compare(54 + ((target_size - 1 - y) * target_size + x) * 3, 3,
                    source, 54 + ((127 - sy) * 128 + sx) * 3, 3) == 0,
                    "Mixed submitted preview must match the host's resize/flip/crop before released images are overwritten");
            }
    }
    if (cropped) {
        require(!eye_calibration_stats().valid && !stereo_eye_assignment(9101).calibrated,
            "Cropped-away markers must remain unmapped even though support images are captured");
    } else {
        require(stereo_eye_assignment(9101).calibrated && stereo_eye_assignment(9101).eye_index == 0 &&
            stereo_eye_assignment(9101).vertical_flip == flipped, "Mixed API pipeline must map both eyes and orientation");
        for (unsigned i = 0; i < 100; ++i) frame(true, false, true, i);
        require(stereo_eye_assignment(9101).eye_index == 1 && eye_calibration_stats().corrections == 1,
            "Mixed API alternating-eye rendering must follow changed projection eye labels");
        const auto warmed = eye_calibration_stats().allocations;
        for (unsigned i = 0; i < 100; ++i) frame(true, false, true, i);
        require(eye_calibration_stats().allocations == warmed, "Mixed API readback and marker allocations must settle after warm-up");
    }
    eye_calibration_enable(false); frame(false, false, false, 0);
    for (unsigned i = 0; i < 20; ++i) { xr.invoke([] { eye_calibration_tick(); }); eye_calibration_tick(); Sleep(1); }
    require(!eye_calibration_stats().in_flight, "Mixed API queries and command recordings must drain after disabling");
    if (!cropped) {
        // Replay known readable submitted pixels from the *old* marker epoch.
        // New source proof alone must not authenticate an old headset image.
        eye_calibration_reset_stats();
        xr.invoke([&] {
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto& bitmap = report.files[eye + 2].contents;
                std::vector<unsigned char> stale(target_size * target_size * 4);
                for (unsigned y = 0; y < target_size; ++y) for (unsigned x = 0; x < target_size; ++x) {
                    const auto* pixel = bitmap.data() + 54 + ((target_size - 1 - y) * target_size + x) * 3;
                    auto* out = stale.data() + (y * target_size + x) * 4;
                    out[0] = pixel[2]; out[1] = pixel[1]; out[2] = pixel[0]; out[3] = 255;
                }
                context11->UpdateSubresource(submitted.Get(), eye, nullptr, stale.data(), target_size * 4, 0);
            }
        });
        eye_calibration_enable(true);
        for (unsigned i = 0; i < 80; ++i) frame(false, true, false, i);
        require(!eye_calibration_stats().valid && !stereo_eye_assignment(9101).calibrated,
            "Mixed API stale markers from before disable/re-enable must not publish eye assignments");
        for (unsigned i = 0; i < 60; ++i) frame(false, false, false, i);
        require(stereo_eye_assignment(9101).calibrated, "Mixed API path must recover when fresh submitted pixels resume");
        eye_calibration_enable(false); frame(false, false, false, 0);
        for (unsigned i = 0; i < 20; ++i) { xr.invoke([] { eye_calibration_tick(); }); eye_calibration_tick(); Sleep(1); }
    }
    const auto root = std::filesystem::temp_directory_path() / "Cheeky-mixed-calibration-tests" / std::to_string(GetCurrentProcessId());
    std::filesystem::create_directories(root);
    write_support_zip(root / (delayed ? (wrapped ? "mixed-delayed-wrapped.zip" : "mixed-delayed.zip") :
        cropped ? "mixed-cropped.zip" : wrapped ? "mixed-wrapped-flipped.zip" : "mixed.zip"), report.files);
    cleanup();
    std::cout << "PASS DX12-to-DX11 OpenXR: " << (wrapped ? "wrapped source" : "native source")
        << (flipped ? ", flipped" : "") << (delayed ? ", delayed unsampled renders" : "")
        << (cropped ? ", cropped markers rejected" : ", eye mapping") << ", four support images\n";
}
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
    if (options & 16) {
        uint w, h; a.GetDimensions(w, h);
        float side = min(w, h) * ((options & 64) ? .75 : 1.0);
        float2 start = (float2(w, h) - side) * .5;
        if (options & 32) start.x = w - side;
        float2 uv = (float2(id.xy) + .5) / 192;
        if (options & 2) uv.y = 1 - uv.y;
        p = uint2(start + uv * side);
    } else if (options & 2) p.y = 127 - p.y;
    if (options & 256) {
        float2 uv = (float2(id.xy) + .5) / 192;
        if (options & 2) uv.y = 1 - uv.y;
        float2 start = id.z ? float2(34.3, 21.7) : float2(41.6, 24.9);
        float2 extent = id.z ? float2(330, 250) : float2(316.9, 245.4);
        p = uint2(start + uv * extent);
    }
    if (options & 4) p.y = min(p.y, 127 - p.y);
    if (options & 8) p = uint2(clamp(int2(p) + int2(8, 8), int2(0, 0), int2(127, 127)));
    float4 color = ((id.z ^ (options & 1)) == 0) ? a.Load(int3(p, 0)) : b.Load(int3(p, 0));
    if (options & 8) color.rgb = float3(.75, .65, .8) + color.rgb * float3(.12, .2, .15);
    target[id] = (options & 128) ? float4(.25, .25, .25, 1) : color;
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
                  bool shifted = false, unsigned crop_options = 0) {
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
            (swapped ? 1U : 0U) | (flipped ? 2U : 0U) | (ambiguous ? 4U : 0U) | (shifted ? 8U : 0U) | crop_options, 0);
        gpu.list->Dispatch(24, 24, 2);
        gpu.barrier(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, state);
        for (auto* r : {a, b})
            gpu.barrier(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
};
void crop_calibration12(bool mixed) {
    roles();
    GPU12 gpu;
    constexpr unsigned width = 400, height = 300, pitch = 1792, target_size = 192;
    constexpr auto state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    auto a = gpu.texture(width, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, DXGI_FORMAT_R8G8B8A8_UNORM, height);
    auto b = gpu.texture(width, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, DXGI_FORMAT_R8G8B8A8_UNORM, height);
    auto target = gpu.texture(target_size, 2, state, DXGI_FORMAT_R8G8B8A8_UNORM, target_size);
    ScaledSubmission12 shader(gpu, a.Get(), b.Get(), target.Get());
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = pitch * height;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> background, readback;
    check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&background)));
    void* data{}; check(background->Map(0, nullptr, &data)); memset(data, 64, SIZE_T(bd.Width)); background->Unmap(0, nullptr);
    heap.Type = D3D12_HEAP_TYPE_READBACK; bd.Width = target_size * target_size * 4 * 2;
    check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    ComPtr<ID3D11Device> device11; ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D11Texture2D> target11;
    if (mixed) {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device11, nullptr, &context11));
        D3D11_TEXTURE2D_DESC td{target_size, target_size, 1, 2, DXGI_FORMAT_R8G8B8A8_UNORM,
            {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
        check(device11->CreateTexture2D(&td, nullptr, &target11));
    }
    const auto attempt = [&](unsigned options, bool flip = false) {
        auto support = request_calibration_images(true);
        for (unsigned n = 0; !eye_calibration_frame(EyeCalibrationBackend::openxr, 901, mixed ? 11 : 12) && n < 20; ++n) {}
        gpu.begin();
        unsigned c{};
        for (auto* source : {a.Get(), b.Get()}) {
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = source; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = background.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, pitch};
            gpu.barrier(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            gpu.barrier(source, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            eye_calibration_stamp12(gpu.list.Get(), source, 9101 + c++, 0, 0, width, height);
        }
        shader.record(gpu, a.Get(), b.Get(), target.Get(), state, true, flip, false, false, options);
        if (mixed) {
            gpu.barrier(target.Get(), state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (unsigned eye = 0; eye < 2; ++eye) {
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                src.pResource = target.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = eye;
                dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset = eye * target_size * target_size * 4;
                dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, target_size, target_size, 1, target_size * 4};
                gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            gpu.barrier(target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        }
        gpu.execute(); gpu.wait(gpu.queue.Get()); calibration12_retired(gpu.list.Get());
        if (mixed) {
            check(readback->Map(0, nullptr, &data));
            for (unsigned eye = 0; eye < 2; ++eye)
                context11->UpdateSubresource(target11.Get(), eye, nullptr,
                    static_cast<unsigned char*>(data) + eye * target_size * target_size * 4, target_size * 4, 0);
            readback->Unmap(0, nullptr);
        }
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto ticket = mixed ? eye_calibration_submit(target11.Get(), eye, 0, 0, 1, 1, eye, EyeCalibrationBackend::openxr, 901) :
                eye_calibration_submit12(target.Get(), gpu.submit_queue.Get(), eye, 0, 0, 1, 1, eye, EyeCalibrationBackend::openxr, 901);
            require(ticket != 0, "Crop capture must record before submission"); eye_calibration_result(ticket, 0);
        }
        eye_calibration_frame(EyeCalibrationBackend::openxr, 901, mixed ? 11 : 12);
        gpu.wait(gpu.submit_queue.Get()); if (mixed) context11->Flush();
        const auto deadline = GetTickCount64() + 5000;
        while (eye_calibration_stats().in_flight && GetTickCount64() < deadline) { Sleep(1); eye_calibration_tick(); }
        require(!eye_calibration_stats().in_flight, "Crop frame must safely retire all GPU readbacks");
        require(collect_calibration_images(support).files.size() == 5, "Crop capture must retain both APIs' image evidence");
        return support;
    };
    auto acquire = [&](auto render) {
        const auto before = eye_calibration_stats().valid;
        CalibrationImageRequestPtr support;
        for (unsigned i = 0; i < 40; ++i) {
            support = render();
            require(support->images[0].info.markers.count <= 2, "Only a corner pair may be stamped during acquisition");
            if (eye_calibration_stats().valid > before) return support;
            Sleep(100);
        }
        throw std::runtime_error("Sequential DX12 corner acquisition did not converge");
    };
    auto first = acquire([&] { return attempt(16); });
    require(eye_calibration_stats().valid && first->images[0].info.markers.count > 1,
        "DX12 inner markers must survive shader crop and resize");
    auto locked = attempt(16);
    require(locked->images[0].info.markers.count == 1 && locked->images[2].info.sample_count == 4 &&
        eye_calibration_stats().left_view == 9102, "DX12 must lock to one authenticated location and swapped eye identity");
    const auto before = eye_calibration_stats().valid;
    const auto moved = attempt(16 | 32);
    require(eye_calibration_stats().valid == before + 1 && moved->images[0].info.markers.count == 1,
        "Local tracking must follow the displaced edge crop without reopening acquisition");
    auto edge = acquire([&] { return attempt(16 | 32); });
    require(eye_calibration_stats().valid > before && edge->images[0].info.markers.count <= 2, "DX12 must reacquire from a local corner pair");
    acquire([&] { return attempt(16 | 64, true); });
    auto zoom = attempt(16 | 64, true);
    require(eye_calibration_stats().vertical_flip && zoom->images[0].info.markers.count == 1,
        "DX12 crop, resize and flip must keep the authenticated placement");
    const auto visible = eye_calibration_stats().valid;
    attempt(16 | 64 | 128, true);
    require(eye_calibration_stats().valid == visible, "DX12 obscured codes must not authenticate any hypothesis");
    attempt(16 | 64, true);
    require(eye_calibration_stats().valid == visible + 1, "DX12 marker loss must reopen search and recover");
    const auto before_wide = eye_calibration_stats().valid;
    acquire([&] { return attempt(256); });
    require(eye_calibration_stats().valid > before_wide && eye_calibration_json().find("\"per_eye\":true") != std::string::npos,
        "Wide acquisition must decode arbitrary per-eye shader crops across both D3D paths");
    const auto before_tracking = eye_calibration_stats().valid;
    for (unsigned i = 0; i < 3; ++i) {
        auto tracked = attempt(256);
        require(tracked->images[0].info.markers.count == 1 && tracked->images[2].info.sample_count == 4,
            "D3D12 acquired transform must reduce to tiny tracking patches");
    }
    require(eye_calibration_stats().valid == before_tracking + 3,
        "D3D12 and mixed-API acquisition must remain locked on fresh captures");
    cleanup();
    // Registry collection is deliberately lazy; release our retired support
    // allocations before the next test exercises the process-wide budget.
    const auto swept = calibration12_create(gpu.device.Get());
    require(calibration_image_bytes.load() == 0, "Retired crop support readbacks must release their memory budget");
    std::cout << "PASS " << (mixed ? "DX12-to-DX11" : "DX12") << " shader crop/resize: inner codes, one-location lock, changed crop, flip, loss/recovery\n";
}
void recording_lifetime12() {
    GPU12 gpu;
    auto texture = gpu.texture(128, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto frame = calibration12_create(gpu.device.Get());
    std::uint64_t allocations{};
    require(calibration12_begin(*frame), "Fresh recording slot must be reusable");
    const auto support = request_calibration_images(true);
    CalibrationImageInfo image;
    image.width = image.height = 128; image.format = DXGI_FORMAT_R8G8B8A8_UNORM; image.graphics_api = 12;
    gpu.begin();
    require(calibration12_stamp(*frame, gpu.list.Get(), texture.Get(), 0, 12, 12,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, allocations, nullptr, support, image),
            "Lifetime test stamp failed");
    gpu.execute();
    gpu.wait(gpu.queue.Get());
    require(!calibration12_poll(*frame).ready && !calibration12_begin(*frame),
            "Completed but still resubmittable recordings must not be mapped or reused");
    require(support->images[0].bitmap.empty() && calibration_image_bytes.load() > 0,
            "Support image must wait for command-list retirement before mapping");
    support->deadline = std::chrono::steady_clock::now(); // Deterministic timeout while the list remains replayable.
    const auto timed_out = collect_calibration_images(support);
    require(timed_out.files.size() == 1 && timed_out.diagnostics.find("gpu_readback_timeout") != std::string::npos &&
                calibration_image_bytes.load() > 0,
            "Support ZIP timeout must not release resources referenced by a replayable GPU recording");
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
    require(calibration_image_bytes.load() == 0 && support->images[0].bitmap.empty(),
            "Timed-out support images must drain only after retirement and ignore late results");
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
           bool late_begin = false, bool fail_end = false, bool support_images = false) {
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
    auto support = support_images ? request_calibration_images(true) : CalibrationImageRequestPtr{};
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
    if (support) {
        const auto report = collect_calibration_images(support);
        require(report.files.size() == 5, "DX12 support capture must retain both sources and both submitted slices");
        require(report.diagnostics.find("\"graphics_api\":12") != std::string::npos &&
            report.diagnostics.find("\"original_width\":" + std::to_string(allocation_width)) != std::string::npos &&
            report.diagnostics.find("\"original_height\":" + std::to_string(target_height)) != std::string::npos,
            "DX12 capture must report original source/submitted dimensions");
        if (array) require(report.diagnostics.find("\"array_slice\":1") != std::string::npos,
            "DX12 support captures must identify the right array slice");
        for (unsigned i = 0; i < 4; ++i) {
            const auto& bitmap = report.files[i].contents;
            require(bitmap.size() < 1000000 && bitmap.starts_with("BM"), "DX12 image must be a bounded bitmap");
            if (i < 2) require(std::any_of(bitmap.begin() + 54, bitmap.end(), [](char pixel) { return pixel != 0; }),
                "DX12 source image must show the marker after stamping");
        }
        if (array) {
            const auto source_stride = (allocation_width * 3 + 3) & ~3U;
            const auto target_stride = (target_width * 3 + 3) & ~3U;
            for (unsigned eye = 0; eye < 2; ++eye)
                for (unsigned y = 0; y < target_height; ++y)
                    for (unsigned x = 0; x < target_width; ++x) {
                        const auto sx = x * 128 / target_width;
                        const auto sy = flipped ? 127 - y * 128 / target_height : y * 128 / target_height;
                        require(report.files[2 + eye].contents.compare(
                            54 + (target_height - 1 - y) * target_stride + x * 3, 3,
                            report.files[1 - eye].contents, 54 + (127 - sy) * source_stride + sx * 3, 3) == 0,
                            "Submitted image must capture the correct swapped array slice, scaling and vertical orientation");
                    }
        }
        const auto root = std::filesystem::temp_directory_path() / "Cheeky-stereo-support-tests" /
            std::to_string(GetCurrentProcessId());
        std::filesystem::create_directories(root);
        write_support_zip(root / (converted ? "dx12-converted.zip" : "dx12-array.zip"), report.files);
    }
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
// Real GPU stamps/readbacks: after acquisition, change-only mode must leave
// freshly rendered source pixels untouched on every supported submission route.
void retained_calibration(bool source12, bool submit11, EyeCalibrationBackend backend) {
    roles();
    auto settings = configured_settings();
    settings.eye_calibration_continuous = false;
    update_settings(settings);
    GPU12 gpu;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context));
    constexpr unsigned size = 512;
    const std::vector<unsigned> background(size * size, 0xff404040);
    std::array<ComPtr<ID3D11Texture2D>, 2> textures11;
    D3D11_TEXTURE2D_DESC desc{size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
        {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
    for (auto& texture : textures11) check(device->CreateTexture2D(&desc, nullptr, &texture));
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging11;
    check(device->CreateTexture2D(&desc, nullptr, &staging11));
    std::array<ComPtr<ID3D12Resource>, 2> textures12;
    for (auto& texture : textures12)
        texture = gpu.texture(size, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, DXGI_FORMAT_R8G8B8A8_UNORM, size);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = size * size * 4;
    buffer.Height = buffer.SampleDesc.Count = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload, readback;
    check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    void* data{}; check(upload->Map(0, nullptr, &data));
    memcpy(data, background.data(), background.size() * 4); upload->Unmap(0, nullptr);
    std::array<std::uint64_t, 2> views{9101, 9102};
    std::uint64_t session = backend == EyeCalibrationBackend::openxr ? 9901 : 0;
    unsigned source_width = size;
    float right_bound = 1;
    bool swapped = false;
    const auto native_state = backend == EyeCalibrationBackend::openxr ?
        D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    auto frame = [&](bool expect_clean = false) {
        eye_calibration_frame(backend, session, submit11 ? 11 : 12);
        for (unsigned c = 0; c < 2; ++c) {
            if (source12) {
                gpu.begin();
                D3D12_TEXTURE_COPY_LOCATION texture{}, linear{};
                texture.pResource = textures12[c].Get(); texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                linear.pResource = upload.Get(); linear.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                linear.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, size, size, 1, size * 4};
                gpu.barrier(textures12[c].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                gpu.list->CopyTextureRegion(&texture, 0, 0, 0, &linear, nullptr);
                gpu.barrier(textures12[c].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                eye_calibration_stamp12(gpu.list.Get(), textures12[c].Get(), views[c], 0, 0, source_width, size);
                gpu.barrier(textures12[c].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                linear.pResource = readback.Get();
                gpu.list->CopyTextureRegion(&linear, 0, 0, 0, &texture, nullptr);
                gpu.barrier(textures12[c].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, native_state);
                gpu.execute(); gpu.wait(gpu.queue.Get());
                check(readback->Map(0, nullptr, &data));
                if (expect_clean) require(memcmp(data, background.data(), background.size() * 4) == 0,
                    "Retained calibration must stop all DX12 marker stamping");
                if (submit11) context->UpdateSubresource(textures11[c].Get(), 0, nullptr, data, size * 4, 0);
                readback->Unmap(0, nullptr);
            } else {
                context->UpdateSubresource(textures11[c].Get(), 0, nullptr, background.data(), size * 4, 0);
                eye_calibration_stamp(context.Get(), textures11[c].Get(), views[c], 0, 0, source_width, size);
                if (expect_clean) {
                    context->CopyResource(staging11.Get(), textures11[c].Get());
                    D3D11_MAPPED_SUBRESOURCE mapped{}; check(context->Map(staging11.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                    for (unsigned y = 0; y < size; ++y)
                        require(memcmp(static_cast<char*>(mapped.pData) + y * mapped.RowPitch,
                            background.data() + y * size, size * 4) == 0,
                            "Retained calibration must stop all DX11 marker stamping");
                    context->Unmap(staging11.Get(), 0);
                }
            }
        }
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto c = swapped ? 1 - eye : eye;
            const auto ticket = submit11 ? eye_calibration_submit(textures11[c].Get(), eye, 0, 0, right_bound, 1, 0, backend, session) :
                eye_calibration_submit12(textures12[c].Get(), gpu.queue.Get(), eye, 0, 0, right_bound, 1, 0, backend, session);
            if (expect_clean) require(ticket == 0, "Retained calibration must not enqueue readbacks");
            if (ticket) eye_calibration_result(ticket, 0);
        }
        context->Flush();
        if (source12) {
            gpu.wait(gpu.queue.Get()); gpu.begin();
            for (auto& texture : textures12) gpu.barrier(texture.Get(), native_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gpu.execute(); gpu.wait(gpu.queue.Get());
        }
        Sleep(2); eye_calibration_tick();
    };
    auto acquire = [&] {
        const auto deadline = GetTickCount64() + 10000;
        do { frame(); } while ((!eye_calibration_stats().crop_mapping_active || eye_calibration_stats().acquisition_confirmations < 4) && GetTickCount64() < deadline);
        require(eye_calibration_stats().crop_mapping_active, "Change-only calibration must acquire and publish a crop");
        for (unsigned i = 0; i < 12; ++i) frame(true);
    };
    acquire();
    const auto stable = eye_calibration_stats();
    Sleep(2600);
    swapped = true;
    for (unsigned i = 0; i < 25; ++i) frame(true);
    require(eye_calibration_stats().captures == stable.captures && eye_calibration_stats().applied == stable.applied &&
        eye_calibration_stats().crop_mapping_active && stereo_eye_assignment(views[0]).eye_index == 0,
        "Change-only mode must retain its crop past expiry and deliberately ignore same-view eye swaps");
    eye_calibration_reset_stats(); frame(true);
    require(eye_calibration_stats().captures == 0, "Counter reset must preserve retained calibration");
    eye_calibration_recalibrate();
    require(!stereo_eye_assignment(views[0]).calibrated, "Manual recalibration must invalidate the old pair");
    acquire();
    require(stereo_eye_assignment(views[0]).eye_index == 1, "Manual recalibration must learn the swapped eyes");
    source_width -= 16; frame();
    require(!eye_calibration_stats().crop_mapping_active, "Source dimension change must invalidate retained alignment");
    acquire();
    right_bound = .95F; frame();
    require(!eye_calibration_stats().crop_mapping_active, "Submission bounds change must restart without marker validation");
    acquire();
    unregister_stereo_view(views[0]); register_stereo_view(views[0]); frame();
    require(!eye_calibration_stats().crop_mapping_active, "Recreated view at the same address must invalidate retained alignment");
    acquire();
    register_stereo_view(9103); views[0] = 9103; frame();
    require(!eye_calibration_stats().crop_mapping_active, "New view identity must restart calibration");
    acquire();
    if (session) {
        eye_calibration_destroy_session(session); ++session; frame();
        require(!eye_calibration_stats().crop_mapping_active, "New VR session must invalidate retained alignment");
        acquire();
    }
    settings.eye_calibration_continuous = true; update_settings(settings);
    const auto captures_before = eye_calibration_stats().captures;
    for (unsigned i = 0; i < 25; ++i) frame();
    require(eye_calibration_stats().captures > captures_before, "Continuous validation must resume when selected");
    settings.eye_calibration_continuous = false; update_settings(settings);
    const auto before_switch = eye_calibration_stats();
    for (unsigned i = 0; i < 25; ++i) frame(true);
    require(eye_calibration_stats().crop_mapping_active &&
        eye_calibration_stats().captures == before_switch.captures &&
        eye_calibration_stats().recalibration_requests == before_switch.recalibration_requests,
        "Switching off validation must drain outstanding work without reopening acquisition");
    settings.eye_calibration_continuous = true; update_settings(settings);
    cleanup(); unregister_stereo_view(9103);
    std::cout << "PASS retained calibration " << (source12 ? "DX12" : "DX11") << " -> " <<
        (submit11 ? "DX11 " : "DX12 ") << eye_calibration_backend_name(backend) << '\n';
}
} // namespace
int run_retained_calibration_tests() {
    try {
        for (const auto backend : {EyeCalibrationBackend::openvr, EyeCalibrationBackend::openxr}) {
            retained_calibration(false, true, backend);
            retained_calibration(true, false, backend);
        }
        retained_calibration(true, true, EyeCalibrationBackend::openxr);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Retained calibration: " << e.what() << '\n'; cleanup();
        auto settings = configured_settings(); settings.eye_calibration_continuous = true; update_settings(settings);
        unregister_stereo_view(9103); return 1;
    }
}
int run_crop_calibration12_tests() {
    try { crop_calibration12(false); crop_calibration12(true); return 0; }
    catch (const std::exception& e) { std::cerr << "Crop calibration DX12: " << e.what() << '\n'; cleanup(); return 1; }
}
int run_mixed_api_calibration_tests() {
    int failures{};
    try { continuous_marker_lifetime12(); }
    catch (const std::exception& e) { std::cerr << "FAIL continuous marker lifetime: " << e.what() << '\n'; ++failures; cleanup(); }
    try { calibration_device_identity12(); }
    catch (const std::exception& e) { std::cerr << "FAIL device identity: " << e.what() << '\n'; ++failures; }
    try { refresh_recording_lifetime12(); }
    catch (const std::exception& e) { std::cerr << "FAIL marker refresh lifetime: " << e.what() << '\n'; ++failures; }
    for (unsigned variant = 0; variant < 7; ++variant) {
        try { mixed_api12to11(variant == 1 || variant == 4 || variant == 6, variant == 1 || variant == 6,
            variant == 2, variant >= 3, variant >= 5); }
        catch (const std::exception& e) { std::cerr << "FAIL mixed API " << variant << ": " << e.what() << '\n'; ++failures; cleanup(); }
    }
    return failures;
}
int run_stereo_support12_tests() {
    try {
        openxr11_pipeline(false, false, true);
        openxr11_pipeline(false, true, true);
        recording_lifetime12();
        run12(EyeCalibrationBackend::openxr, true, false, DXGI_FORMAT_R8G8B8A8_UNORM,
            false, false, false, false, false, false, true);
        run12(EyeCalibrationBackend::openxr, true, false, DXGI_FORMAT_R11G11B10_FLOAT,
            true, true, false, false, false, false, true);
        require(calibration_image_bytes.load() == 0, "DX12 support readbacks must retire after GPU completion");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "OpenXR stereo support: " << e.what() << '\n'; cleanup(); return 1;
    }
}
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
        openxr11_pipeline(false);
        openxr11_pipeline(true);
        openxr11_pipeline(false, true);
        openxr11_pipeline(true, true);
        openxr11_singlethreaded();
        openxr11_context_diagnostics(false);
        openxr11_context_diagnostics(true);
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
