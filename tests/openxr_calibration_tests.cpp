#include "eye_calibration.hpp"
#include "eye_calibration_d3d12.hpp"
#include "settings.hpp"
#include "../openxr_layer/eye_calibration.hpp"
#include <wrl/client.h>
#include <dxgi1_4.h>
#include <iostream>
#include <vector>
#include <stdexcept>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include "../third_party/openxr/include/openxr/openxr.h"
#include "../third_party/openxr/include/openxr/openxr_platform.h"
#include "../third_party/openxr/include/openxr/openxr_loader_negotiation.h"

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
    inline static void* image{};
    inline static unsigned graphics{};
    inline static XrResult wait_result{XR_SUCCESS}, release_result{XR_SUCCESS}, end_result{XR_SUCCESS};
    HMODULE module{};
    PFN_xrGetInstanceProcAddr get{};
    XrInstance instance{};
    XrSession session{};
    XrSwapchain chain{};
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
            *s = reinterpret_cast<XrSwapchain>(77);
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
    XRLayer(void* texture, unsigned api, void* binding, unsigned width = 128, unsigned slices = 2) {
        image = texture;
        graphics = api;
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
        chain_info.height = 128;
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
            v.subImage.imageRect = {{array ? 0 : static_cast<int>(eye * 128), 0}, {128, 128}};
        }
        if (swap)
            std::swap(views[0], views[1]);
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.viewCount = 2;
        projection.views = views.data();
        const XrCompositionLayerBaseHeader* layers[]{
            reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection)};
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
        info.layerCount = 1;
        info.layers = layers;
        require(fn<PFN_xrEndFrame>("xrEndFrame")(session, &info) == end_result,
                "EndFrame result was not forwarded");
    }
    ~XRLayer() {
        if (session) {
            fn<PFN_xrDestroySwapchain>("xrDestroySwapchain")(chain);
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
    for (unsigned i = 0; i < 16; ++i)
        render(false);
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
    ComPtr<ID3D12Resource> texture(unsigned width, unsigned slices, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES h{};
        h.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = 128;
        d.DepthOrArraySize = static_cast<UINT16>(slices);
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    require(discarded.ready && discarded.reusable && !discarded.valid,
            "Reset without Execute must discard safely");
    check(gpu.list->Close());
    std::cout << "D3D12 recording lifetime: resubmission, independent queue fences, discarded Reset passed\n";
}
void run12(EyeCalibrationBackend backend, bool array, bool hardware = false) {
    roles();
    GPU12 gpu(hardware);
    const auto state = backend == EyeCalibrationBackend::openxr ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const std::uint64_t generation = backend == EyeCalibrationBackend::openxr ? 901 : 0;
    auto a = gpu.texture(128, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
         b = gpu.texture(128, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto target = gpu.texture(array ? 128 : 256, array ? 2 : 1, state);
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = gpu.device.Get();
    binding.queue = gpu.submit_queue.Get();
    std::unique_ptr<XRLayer> layer;
    if (hardware)
        layer = std::make_unique<XRLayer>(target.Get(), 12, &binding, array ? 128 : 256, array ? 2 : 1);
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
    for (unsigned frame = 0; frame < (layer ? 65U : 64U); ++frame) {
        if (layer)
            layer->begin();
        else
            eye_calibration_frame(backend, generation, 12);
        gpu.begin();
        for (auto* r : {a.Get(), b.Get()}) {
            gpu.barrier(r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = r;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = black.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 128, 128, 1, 512};
            gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            gpu.barrier(r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        eye_calibration_stamp12(gpu.list.Get(), a.Get(), 9101, 0, 0, 128, 128);
        eye_calibration_stamp12(gpu.list.Get(), b.Get(), 9102, 0, 0, 128, 128);
        gpu.barrier(a.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.barrier(b.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.barrier(target.Get(), state, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.copy(target.Get(), 0, 0, frame < 32 ? b.Get() : a.Get());
        gpu.copy(target.Get(), array ? 1 : 0, array ? 0 : 128, frame < 32 ? a.Get() : b.Get());
        gpu.barrier(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
        gpu.barrier(a.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.barrier(b.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.execute();
        check(gpu.queue->Signal(gpu.fence.Get(), ++gpu.value));
        check(gpu.submit_queue->Wait(gpu.fence.Get(), gpu.value));
        if (layer) {
            layer->release();
            layer->end(false, array);
        } else
            for (unsigned eye = 0; eye < 2; ++eye) {
                const auto ticket = eye_calibration_submit12(
                    target.Get(), gpu.submit_queue.Get(), eye, array ? 0 : eye * .5F, 0,
                    array ? 1 : (eye + 1) * .5F, 1, array ? eye : 0, backend, generation);
                require(ticket != 0, "D3D12 capture was not accepted");
                eye_calibration_result(ticket, 0);
            }
        gpu.wait(gpu.submit_queue.Get());
        calibration12_retired(gpu.list.Get());
        eye_calibration_tick();
        if (frame == 31) {
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
    require(stats.valid == 64 && stats.corrections == 2 && stereo_eye_assignment(9101).eye_index == 0,
            "D3D12 transition must correct exactly once");
    require(stats.allocations == warm_allocations, "D3D12 allocations must stop after pool warmup");
    require(stats.gpu_samples > 0 && stats.gpu_us > 0, "D3D12 calibration timestamps missing");
    std::cout << "D3D12 " << eye_calibration_backend_name(backend) << (array ? " array" : " packed")
              << (hardware ? " hardware" : " WARP") << ": 64 valid, 2 corrections, stable allocations\n";
    cleanup();
}
} // namespace
int run_openxr_calibration_tests() {
    try {
        layer_policy();
        openxr11();
        recording_lifetime12();
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
