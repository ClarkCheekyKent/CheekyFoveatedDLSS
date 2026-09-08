#include "graphics_observer.hpp"
#include "gaze_foveation.hpp"
#include "runtime.hpp"
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
std::atomic<bool> ready{};
std::atomic<std::uint64_t> submissions{}, copies{}, resets{}, destroyed{};
thread_local bool observing{};
struct ObservationScope { bool outer{!observing}; ObservationScope() { observing = true; } ~ObservationScope() { if (outer) observing = false; } };
using ExecuteFn = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ResetFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using CopyFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using CopyTextureFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using ResolveFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT);
ExecuteFn real_execute{};
ResetFn real_reset{};
CopyFn real_copy{};
CopyTextureFn real_copy_texture{};
ResolveFn real_resolve{};
std::array<void*, 5> installed_targets{};
// ID3D12CommandQueue / ID3D12GraphicsCommandList COM ABI indices.
constexpr unsigned execute_slot = 10, reset_slot = 10, copy_texture_slot = 16, copy_slot = 17, resolve_slot = 19;
void* method(void* object, unsigned index) { return (*static_cast<void***>(object))[index]; }
constexpr GUID observer_lifetime_key{0x11cd13ab, 0x47e8, 0x41cc, {0x9d,0x3f,0x53,0x58,0x9e,0x2c,0x17,0x95}};

class Lifetime final : public IUnknown {
    std::atomic<ULONG> references{1};
    std::uint64_t identity;
    bool list;
public:
    Lifetime(std::uint64_t id, bool is_list) : identity(id), list(is_list) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references;
        if (!left) {
            // Runtime DLL is pinned, so this callback remains executable even
            // after the UEVR adapter has been unloaded.
            if (list) reset_gaze_copies(identity); else forget_gaze_resource(identity);
            ++destroyed; delete this;
        }
        return left;
    }
};
std::uint64_t track(ID3D12Object* object, bool list = false) {
    if (!object) return 0;
    ComPtr<IUnknown> identity;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) return 0;
    const auto id = list ? reinterpret_cast<std::uint64_t>(object) : reinterpret_cast<std::uint64_t>(identity.Get());
    IUnknown* existing{}; UINT size = sizeof(existing);
    if (SUCCEEDED(object->GetPrivateData(observer_lifetime_key, &size, &existing)) && existing) {
        existing->Release(); return id;
    }
    auto* sentinel = new (std::nothrow) Lifetime(id, list);
    if (!sentinel) return 0;
    const auto hr = object->SetPrivateDataInterface(observer_lifetime_key, sentinel);
    sentinel->Release();
    return SUCCEEDED(hr) ? id : 0;
}
bool region(ID3D12Resource* resource, UINT subresource, const D3D12_BOX* box, GazeCopyRegion& out) {
    if (!resource || subresource != 0) return false;
    const auto d = resource->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width > UINT32_MAX) return false;
    out = {track(resource), 0, 0, 0, static_cast<UINT>(d.Width), d.Height};
    if (!out.resource) return false;
    if (box) {
        if (box->front != 0 || box->back != 1 || box->left >= box->right || box->top >= box->bottom ||
            box->right > out.width || box->bottom > out.height) return false;
        out.x = box->left; out.y = box->top;
        out.width = box->right - box->left; out.height = box->bottom - box->top;
    }
    return out.width && out.height;
}
void record(ID3D12GraphicsCommandList* list, const GazeCopyEdge& edge) {
    if (edge.source.width != edge.destination.width || edge.source.height != edge.destination.height) return;
    const auto id = track(list, true);
    if (id) { record_gaze_copy(id, edge); ++copies; }
}
void STDMETHODCALLTYPE execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    ObservationScope scope;
    real_execute(queue, count, lists);
    if (!scope.outer || !ready || !lists) return;
    // This is AFTER execution is enqueued, unlike the ReShade pre-submit event.
    // Never infer the executing queue from the desktop presentation queue.
    try {
        for (UINT i = 0; i < count; ++i) {
            if (!lists[i]) continue;
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
    } catch (...) { log_warning("Native D3D12 submission observation failed"); }
}
HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* state) {
    const auto hr = real_reset(list, allocator, state);
    if (SUCCEEDED(hr)) { reset_gaze_copies(reinterpret_cast<std::uint64_t>(list)); ++resets; }
    return hr;
}
void STDMETHODCALLTYPE copy_resource(ID3D12GraphicsCommandList* list, ID3D12Resource* destination, ID3D12Resource* source) {
    ObservationScope scope;
    real_copy(list, destination, source);
    if (!scope.outer || !ready || !uses_coordinated_center(current_settings())) return;
    try {
        GazeCopyEdge edge;
        if (region(source, 0, nullptr, edge.source) && region(destination, 0, nullptr, edge.destination)) record(list, edge);
    } catch (...) {}
}
void STDMETHODCALLTYPE copy_texture(ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    ObservationScope scope;
    real_copy_texture(list, dst, x, y, z, src, box);
    if (!scope.outer || !ready || !dst || !src || z != 0 || !uses_coordinated_center(current_settings())) return;
    if (src->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX || dst->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) return;
    try {
        GazeCopyEdge edge;
        if (!region(src->pResource, src->SubresourceIndex, box, edge.source) ||
            UINT64(x) + edge.source.width > UINT32_MAX || UINT64(y) + edge.source.height > UINT32_MAX) return;
        const D3D12_BOX target{x, y, 0, x + edge.source.width, y + edge.source.height, 1};
        if (region(dst->pResource, dst->SubresourceIndex, &target, edge.destination)) record(list, edge);
    } catch (...) {}
}
void STDMETHODCALLTYPE resolve(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, UINT dst_sub,
    ID3D12Resource* src, UINT src_sub, DXGI_FORMAT format) {
    ObservationScope scope;
    real_resolve(list, dst, dst_sub, src, src_sub, format);
    if (!scope.outer || !ready || !uses_coordinated_center(current_settings())) return;
    try {
        GazeCopyEdge edge;
        if (region(src, src_sub, nullptr, edge.source) && region(dst, dst_sub, nullptr, edge.destination)) record(list, edge);
    } catch (...) {}
}
}
bool initialize_native_observer(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
    if (!device || !queue) return false;
    std::lock_guard lock(install_mutex);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) return false;
    list->Close();
    const std::array<void*, 5> targets{method(queue, execute_slot), method(list.Get(), reset_slot), method(list.Get(), copy_slot),
        method(list.Get(), copy_texture_slot), method(list.Get(), resolve_slot)};
    if (ready) return targets == installed_targets;
    const auto mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) return false;
    const std::array<void*, 5> detours{reinterpret_cast<void*>(&execute), reinterpret_cast<void*>(&reset), reinterpret_cast<void*>(&copy_resource),
        reinterpret_cast<void*>(&copy_texture), reinterpret_cast<void*>(&resolve)};
    const std::array<void**, 5> originals{reinterpret_cast<void**>(&real_execute), reinterpret_cast<void**>(&real_reset), reinterpret_cast<void**>(&real_copy),
        reinterpret_cast<void**>(&real_copy_texture), reinterpret_cast<void**>(&real_resolve)};
    std::size_t created{};
    for (; created < targets.size(); ++created) {
        if (MH_CreateHook(targets[created], detours[created], originals[created]) != MH_OK) break;
    }
    if (created != targets.size()) {
        while (created) MH_RemoveHook(targets[--created]);
        log_error("Could not create native D3D12 observer hooks"); return false;
    }
    for (auto* target : targets) MH_QueueEnableHook(target);
    if (MH_ApplyQueued() != MH_OK) {
        for (auto* target : targets) { MH_DisableHook(target); MH_RemoveHook(target); }
        log_error("Could not enable native D3D12 observer hooks"); return false;
    }
    installed_targets = targets; ready = true;
    log_info("Native D3D12 submission/copy observer ready");
    return true;
}
NativeObserverStatus native_observer_status() noexcept {
    return {ready.load(), submissions.load(), copies.load(), resets.load(), destroyed.load()};
}
}
