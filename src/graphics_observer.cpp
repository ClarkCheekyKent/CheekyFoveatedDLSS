#include "graphics_observer.hpp"
#include "gaze_foveation.hpp"
#include "runtime.hpp"
#include "eye_calibration_d3d12.hpp"
#include "dlss_nr_lifetime.hpp"
#include "d3d12_ngx_dispatch.hpp"
#include "d3d12_native.hpp"
#include <MinHook.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <new>

namespace cheeky::foveated_dlss {
namespace {
using Microsoft::WRL::ComPtr;
std::mutex install_mutex;
std::mutex probe_mutex;
std::atomic<bool> ready{};
std::atomic<std::uint64_t> submissions{}, copies{}, resets{}, destroyed{};
thread_local bool observing{};
struct ObservationScope {
    bool outer{!observing};
    ObservationScope() {
        observing = true;
    }
    ~ObservationScope() {
        if (outer)
            observing = false;
    }
};
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*,
                                            ID3D12PipelineState*);
using CopyFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using CopyTextureFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*,
                                               UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*,
                                               const D3D12_BOX*);
using ResolveFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT, ID3D12Resource*,
                                           UINT, DXGI_FORMAT);
// A continuation belongs to the hooked entry point, never the object's current
// vtable. Opaque proxies can forward through a second, native hook family.
constexpr std::size_t max_method_hooks = 40;
struct MethodHook { void* target{}; void* handler{}; bool enabled{}; };
std::array<MethodHook, max_method_hooks> method_hooks{};
std::array<std::atomic<void*>, max_method_hooks> continuations{};
std::size_t method_count{}; // Protected by install_mutex; callbacks use atomics.
template<auto Handler, class Signature> struct MethodEntry;
template<auto Handler, class Result, class... Args>
struct MethodEntry<Handler, Result(STDMETHODCALLTYPE*)(Args...)> {
    using Function = Result(STDMETHODCALLTYPE*)(Args...);
    template<std::size_t Index> static Result STDMETHODCALLTYPE call(Args... args) {
        const auto next = reinterpret_cast<Function>(continuations[Index].load(std::memory_order_acquire));
        return Handler(next, args...);
    }
    template<std::size_t... Index> static auto entries(std::index_sequence<Index...>) {
        return std::array<Function, sizeof...(Index)>{&call<Index>...};
    }
};
template<class Signature, auto Handler> bool install_method(void* target) {
    const auto handler = reinterpret_cast<void*>(Handler);
    std::size_t index{};
    for (; index < method_count; ++index) {
        if (method_hooks[index].target != target) continue;
        if (method_hooks[index].handler != handler) return false;
        if (method_hooks[index].enabled) return true;
        break;
    }
    if (index == method_count) {
        if (method_count == max_method_hooks) return false;
        static const auto entries = MethodEntry<Handler, Signature>::entries(std::make_index_sequence<max_method_hooks>{});
        void* trampoline{};
        if (MH_CreateHook(target, reinterpret_cast<void*>(entries[index]), &trampoline) != MH_OK) return false;
        // Publish before enabling; game threads may enter immediately. Retain
        // partial installations on failure and retry them without allocating
        // another slot or disturbing an already-working observer family.
        continuations[index].store(trampoline, std::memory_order_release);
        method_hooks[index] = {target, handler, false};
        ++method_count;
    }
    const auto result = MH_EnableHook(target);
    if (result != MH_OK && result != MH_ERROR_ENABLED) return false;
    method_hooks[index].enabled = true;
    return true;
}
// ID3D12CommandQueue / ID3D12GraphicsCommandList COM ABI indices.
constexpr unsigned execute_slot = 10, reset_slot = 10, copy_texture_slot = 16, copy_slot = 17,
                   resolve_slot = 19;
void* method(void* object, unsigned index) {
    return (*static_cast<void***>(object))[index];
}
constexpr GUID observer_lifetime_key{
    0x11cd13ab, 0x47e8, 0x41cc, {0x9d, 0x3f, 0x53, 0x58, 0x9e, 0x2c, 0x17, 0x95}};
constexpr GUID observer_probe_key{
    0x8a682e9a, 0x73c8, 0x4460, {0x88, 0x56, 0xd2, 0xe1, 0xa4, 0xeb, 0x5d, 0x6c}};
// Device-owned private data avoids stale pointer keys and retaining devices
// indefinitely. Hooks stay resident; a successful factory probe remains valid.
// Failures retry after a short cooldown, or immediately when hooks become ready.
class ObserverProbe final : public IUnknown {
    std::atomic<ULONG> references{1};
public:
    struct Factory {
        std::array<void*, 3> methods{};
        ULONGLONG attempted_at{}, used_at{};
        bool attempted{}, succeeded{}, hooks_ready{};
    };
    // Wrappers can share private data with the native device while exposing
    // different queue/list factories. Never transfer a successful probe across
    // those families. Bound both storage and churn if many wrappers appear.
    std::array<Factory, 8> factories{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references;
        if (!left) delete this;
        return left;
    }
};

class Lifetime final : public IUnknown {
    std::atomic<ULONG> references{1};
    std::uint64_t identity;
    bool list;

  public:
    Lifetime(std::uint64_t id, bool is_list) : identity(id), list(is_list) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown))
            return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references;
        if (!left) {
            // Runtime DLL is pinned, so this callback remains executable even
            // after the UEVR adapter has been unloaded.
            if (list)
                reset_gaze_copies(identity);
            else {
                afw_forget_depth_resource(identity);
                forget_gaze_resource(identity);
            }
            ++destroyed;
            delete this;
        }
        return left;
    }
};
std::uint64_t track(ID3D12Object* object, bool list = false) {
    if (!object)
        return 0;
    ComPtr<IUnknown> identity;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))))
        return 0;
    const auto id =
        list ? reinterpret_cast<std::uint64_t>(object) : reinterpret_cast<std::uint64_t>(identity.Get());
    IUnknown* existing{};
    UINT size = sizeof(existing);
    if (SUCCEEDED(object->GetPrivateData(observer_lifetime_key, &size, &existing)) && existing) {
        existing->Release();
        return id;
    }
    auto* sentinel = new (std::nothrow) Lifetime(id, list);
    if (!sentinel)
        return 0;
    const auto hr = object->SetPrivateDataInterface(observer_lifetime_key, sentinel);
    sentinel->Release();
    return SUCCEEDED(hr) ? id : 0;
}
bool region(ID3D12Resource* resource, UINT subresource, const D3D12_BOX* box, GazeCopyRegion& out) {
    if (!resource || subresource != 0)
        return false;
    const auto d = resource->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width > UINT32_MAX)
        return false;
    out = {track(resource), 0, 0, 0, static_cast<UINT>(d.Width), d.Height};
    if (!out.resource)
        return false;
    if (box) {
        if (box->front != 0 || box->back != 1 || box->left >= box->right || box->top >= box->bottom ||
            box->right > out.width || box->bottom > out.height)
            return false;
        out.x = box->left;
        out.y = box->top;
        out.width = box->right - box->left;
        out.height = box->bottom - box->top;
    }
    return out.width && out.height;
}
void record(ID3D12GraphicsCommandList* list, const GazeCopyEdge& edge) {
    if (edge.source.width != edge.destination.width || edge.source.height != edge.destination.height)
        return;
    const auto id = track(list, true);
    if (id) {
        record_gaze_copy(id, edge);
        ++copies;
    }
}
void execute(ExecuteFn real_execute, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    ObservationScope scope;
    if (calibration12_internal_work() || !scope.outer || !ready || !lists) {
        real_execute(queue, count, lists);
        return;
    }
    {
        std::lock_guard execution_lock(calibration12_execution_mutex());
        real_execute(queue, count, lists);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<ID3D12GraphicsCommandList> graphics;
            if (lists[i] && SUCCEEDED(lists[i]->QueryInterface(IID_PPV_ARGS(&graphics)))) {
                nr_recording_submitted(queue, graphics.Get());
                calibration12_submitted(queue, graphics.Get());
            }
        }
    }
    // This is AFTER execution is enqueued, unlike the ReShade pre-submit event.
    // Never infer the executing queue from the desktop presentation queue.
    try {
        for (UINT i = 0; i < count; ++i) {
            if (!lists[i])
                continue;
            ComPtr<ID3D12GraphicsCommandList> graphics;
            if (SUCCEEDED(lists[i]->QueryInterface(IID_PPV_ARGS(&graphics)))) {
                submit_gaze_copies(reinterpret_cast<std::uint64_t>(graphics.Get()));
                note_d3d12_command_list_submission(queue, graphics.Get());
            }
        }
        ++submissions;
        // Collect only uses already associated with their actual executing
        // queue. The legacy present-queue fallback could otherwise signal
        // timings recorded on a different, not-yet-submitted command list.
        note_d3d12_present(nullptr);
    } catch (...) {
        log_warning("Native D3D12 submission observation failed");
    }
}
HRESULT reset(ResetFn real_reset, ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator,
                                ID3D12PipelineState* state) {
    ObservationScope scope;
    if (calibration12_internal_work() || !scope.outer || !ready)
        return real_reset(list, allocator, state);
    HRESULT hr;
    {
        std::lock_guard execution_lock(calibration12_execution_mutex());
        hr = real_reset(list, allocator, state);
        if (SUCCEEDED(hr)) {
            nr_recording_reset(list, hr);
            calibration12_retired(list);
        }
    }
    if (SUCCEEDED(hr)) {
        note_d3d12_command_list_reset(list);
        reset_gaze_copies(reinterpret_cast<std::uint64_t>(list));
        ++resets;
    }
    return hr;
}
void copy_resource(CopyFn real_copy, ID3D12GraphicsCommandList* list, ID3D12Resource* destination,
                                     ID3D12Resource* source) {
    ObservationScope scope;
    real_copy(list, destination, source);
    if (scope.outer && ready && afw_pending_depth_copy(list, source))
        afw_observe_depth_copy(list, source, track(destination));
    if (!scope.outer || !ready || !uses_coordinated_center(current_settings()))
        return;
    try {
        GazeCopyEdge edge;
        if (region(source, 0, nullptr, edge.source) && region(destination, 0, nullptr, edge.destination))
            record(list, edge);
    } catch (...) {
    }
}
void copy_texture(CopyTextureFn real_copy_texture, ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dst,
                                    UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src,
                                    const D3D12_BOX* box) {
    ObservationScope scope;
    real_copy_texture(list, dst, x, y, z, src, box);
    if (scope.outer && ready && dst && src && !x && !y && !z &&
            src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
            !src->SubresourceIndex && !dst->SubresourceIndex && afw_pending_depth_copy(list, src->pResource)) {
        const auto desc = src->pResource->GetDesc();
        if (!box || (!box->left && !box->top && !box->front && box->right == desc.Width && box->bottom == desc.Height && box->back == 1))
            afw_observe_depth_copy(list, src->pResource, track(dst->pResource));
    }
    if (!scope.outer || !ready || !dst || !src || z != 0 || !uses_coordinated_center(current_settings()))
        return;
    if (src->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ||
        dst->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
        return;
    try {
        GazeCopyEdge edge;
        if (!region(src->pResource, src->SubresourceIndex, box, edge.source) ||
            UINT64(x) + edge.source.width > UINT32_MAX || UINT64(y) + edge.source.height > UINT32_MAX)
            return;
        const D3D12_BOX target{x, y, 0, x + edge.source.width, y + edge.source.height, 1};
        if (region(dst->pResource, dst->SubresourceIndex, &target, edge.destination))
            record(list, edge);
    } catch (...) {
    }
}
void resolve(ResolveFn real_resolve, ID3D12GraphicsCommandList* list, ID3D12Resource* dst, UINT dst_sub,
                               ID3D12Resource* src, UINT src_sub, DXGI_FORMAT format) {
    ObservationScope scope;
    real_resolve(list, dst, dst_sub, src, src_sub, format);
    if (!scope.outer || !ready || !uses_coordinated_center(current_settings()))
        return;
    try {
        GazeCopyEdge edge;
        if (region(src, src_sub, nullptr, edge.source) && region(dst, dst_sub, nullptr, edge.destination))
            record(list, edge);
    } catch (...) {
    }
}
} // namespace
std::uint64_t observe_native_resource(ID3D12Resource* resource) noexcept {
    try { return track(resource); } catch (...) { return 0; }
}
bool initialize_native_observer(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
    const auto native_device = native_d3d12_interface(device);
    const auto native_queue = native_d3d12_interface(queue);
    device = native_device.Get(); queue = native_queue.Get();
    if (!device || !queue)
        return false;
    std::lock_guard lock(install_mutex);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list))))
        return false;
    list->Close();
    const std::array<void*, 5> targets{method(queue, execute_slot), method(list.Get(), reset_slot),
                                       method(list.Get(), copy_slot), method(list.Get(), copy_texture_slot),
                                       method(list.Get(), resolve_slot)};
    // ReShade can detach its UI while game COM lifetime callbacks remain.
    HMODULE resident{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&execute), &resident))
        return false;
    const auto mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    const auto before = method_count;
    if (!install_method<ExecuteFn, &execute>(targets[0]) ||
        !install_method<ResetFn, &reset>(targets[1]) ||
        !install_method<CopyFn, &copy_resource>(targets[2]) ||
        !install_method<CopyTextureFn, &copy_texture>(targets[3]) ||
        !install_method<ResolveFn, &resolve>(targets[4])) {
        log_error("Could not install all D3D12 observer methods for this object family");
        return false;
    }
    ready = true;
    if (before != method_count)
        trace_event("D3D12 observer family ready methods=%zu execute=%p reset=%p copy=%p texture=%p resolve=%p",
            method_count, targets[0], targets[1], targets[2], targets[3], targets[4]);
    return true;
}
bool ensure_native_observer(ID3D12GraphicsCommandList* list) noexcept {
    const auto native_list = native_d3d12_interface(list);
    list = native_list.Get();
    ComPtr<ID3D12Device> device;
    if (!list || FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
    device = native_d3d12_interface(device.Get());
    if (!device) return false;
    std::lock_guard lock(probe_mutex);
    ComPtr<ObserverProbe> probe;
    IUnknown* stored{};
    UINT bytes = sizeof(stored);
    if (SUCCEEDED(device->GetPrivateData(observer_probe_key, &bytes, &stored)) && stored) {
        probe.Attach(static_cast<ObserverProbe*>(stored));
    } else {
        // Even a failed probe leaves a COM callback on the device.
        HMODULE resident{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&ensure_native_observer), &resident)) return false;
        probe.Attach(new (std::nothrow) ObserverProbe);
        if (!probe || FAILED(device->SetPrivateDataInterface(observer_probe_key, probe.Get()))) return false;
        // A wrapper that discards private data cannot provide a stable cache.
        bytes = sizeof(stored);
        if (FAILED(device->GetPrivateData(observer_probe_key, &bytes, &stored)) || !stored) return false;
        const bool matches = stored == probe.Get();
        stored->Release();
        if (!matches) return false;
    }
    const auto now = GetTickCount64();
    const std::array<void*, 3> factories{method(device.Get(), 8), method(device.Get(), 9), method(device.Get(), 12)};
    ObserverProbe::Factory* cached{};
    ObserverProbe::Factory* oldest = &probe->factories.front();
    for (auto& entry : probe->factories) {
        if (entry.methods == factories) { cached = &entry; break; }
        if (!cached && !entry.attempted) cached = &entry;
        if (entry.used_at < oldest->used_at) oldest = &entry;
    }
    if (!cached) {
        if (now - oldest->used_at < 1000U) return false;
        cached = oldest;
        *cached = {};
    }
    cached->methods = factories;
    cached->used_at = now;
    if (cached->succeeded) return true;
    if (cached->attempted && cached->hooks_ready == ready.load() && now - cached->attempted_at < 1000U)
        return false;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};
    cached->attempted = true;
    cached->attempted_at = now;
    cached->succeeded = SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) &&
        initialize_native_observer(device.Get(), queue.Get());
    cached->hooks_ready = ready.load();
    return cached->succeeded;
}
NativeObserverStatus native_observer_status() noexcept {
    return {ready.load(), submissions.load(), copies.load(), resets.load(), destroyed.load()};
}
} // namespace cheeky::foveated_dlss
