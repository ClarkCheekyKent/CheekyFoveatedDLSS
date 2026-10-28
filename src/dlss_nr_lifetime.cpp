#include "dlss_nr_lifetime.hpp"
#include "eye_calibration_d3d12.hpp"
#include "graphics_observer.hpp"
#include "d3d12_native.hpp"
#include "runtime.hpp"
#include <atomic>
#include <vector>
#include <mutex>
#include <wrl/client.h>

namespace cheeky::foveated_dlss {
using Microsoft::WRL::ComPtr;
struct NrAllocatorEpoch {
    std::atomic<bool> retired{};
    std::atomic<std::uint64_t> nr_recordings{};
};
struct NrRecording {
    struct Point {
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence;
        std::uint64_t value{};
        bool needs_signal{};
    };
    bool retired{};
    std::shared_ptr<NrAllocatorEpoch> allocator_epoch;
    std::vector<Point> points;
    std::uint64_t created{}, released{};
};
namespace {
constexpr GUID recording_key{0x24c7a6bb, 0xe7d4, 0x44d2, {0xb9, 0x9d, 0x36, 0xb2, 0x41, 0xc7, 0x8f, 0x71}};
constexpr GUID allocator_key{0xf7406731, 0xdb57, 0x498b, {0xa2,0x7c,0x62,0x85,0x16,0x3e,0x8f,0x4a}};
class AllocatorIdentity final : public IUnknown {
    std::atomic<ULONG> refs{1};
public:
    std::mutex mutex;
    std::shared_ptr<NrAllocatorEpoch> epoch;
    ~AllocatorIdentity() { if (epoch) epoch->retired.store(true); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --refs; if (!left) delete this; return left;
    }
};
ComPtr<AllocatorIdentity> allocator_identity(ID3D12CommandAllocator* allocator, bool create) {
    auto native = native_d3d12_interface(allocator);
    // Several closed lists may share one allocator. Never replace a live token
    // when their first metadata bindings arrive on different threads.
    static std::mutex creation_mutex;
    std::unique_lock creation_lock(creation_mutex, std::defer_lock);
    if (create) creation_lock.lock();
    ComPtr<AllocatorIdentity> result;
    IUnknown* stored{}; UINT bytes = sizeof(stored);
    if (native && SUCCEEDED(native->GetPrivateData(allocator_key, &bytes, &stored)) && stored)
        result.Attach(static_cast<AllocatorIdentity*>(stored));
    if (!result && native && create) {
        result.Attach(new AllocatorIdentity);
        if (FAILED(native->SetPrivateDataInterface(allocator_key, result.Get()))) result.Reset();
    }
    return result;
}
class RecordingIdentity final : public IUnknown {
    std::atomic<ULONG> refs{1};
public:
    std::shared_ptr<NrRecording> current;
    std::shared_ptr<NrAllocatorEpoch> allocator_epoch;
    std::atomic<bool> tracked{};
    std::atomic<void*> allocator_reset_target{};
    void* observed_allocator_reset{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --refs;
        if (!left) {
            // No owner callbacks or owner locks here. COM releases during
            // collection can reenter this notification under the execution lock.
            if (current) {
                std::lock_guard execution_lock(calibration12_execution_mutex());
                current->retired = true;
            }
            delete this;
        }
        return left;
    }
};
ComPtr<RecordingIdentity> identity(ID3D12Object* list) {
    const auto native = native_d3d12_interface(list);
    list = native.Get();
    ComPtr<RecordingIdentity> result;
    IUnknown* tag{};
    UINT bytes = sizeof(tag);
    if (list && SUCCEEDED(list->GetPrivateData(recording_key, &bytes, &tag)) && tag)
        result.Attach(static_cast<RecordingIdentity*>(tag));
    return result;
}
HRESULT signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, std::uint64_t value) {
    return queue->Signal(fence, value);
}
void signal_point(NrRecording& recording, NrRecording::Point& point, NrSignal submit_signal) {
    if (!point.fence) {
        ComPtr<ID3D12Device> device;
        if (FAILED(point.queue->GetDevice(IID_PPV_ARGS(&device))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&point.fence)))) return;
        ++recording.created;
    }
    if (point.needs_signal && SUCCEEDED(submit_signal(point.queue.Get(), point.fence.Get(), point.value)))
        point.needs_signal = false;
}
void collect_recording(NrRecording& recording, NrSignal submit_signal) {
    for (auto it = recording.points.begin(); it != recording.points.end();) {
        signal_point(recording, *it, submit_signal);
        if (it->fence && !it->needs_signal && it->fence->GetCompletedValue() >= it->value) {
            // Finished executions can drain even while replay keeps the
            // recording alive. A later execution starts its own fence point.
            it = recording.points.erase(it);
            ++recording.released;
        } else ++it;
    }
}
}
bool ensure_dlss_nr_recording(ID3D12GraphicsCommandList* list) noexcept {
    const auto native = native_d3d12_interface(list);
    list = native.Get();
    if (!list) return false;
    std::lock_guard execution_lock(calibration12_execution_mutex());
    auto tag = identity(list);
    if (!tag || !tag->tracked.load() ||
            tag->observed_allocator_reset != tag->allocator_reset_target.load()) {
        // Probe through this list's device, even if a different device already
        // installed hooks. Unsupported observation/private data means fallback.
        if (!ensure_native_observer(list)) return false;
        if (!tag) {
            tag.Attach(new RecordingIdentity);
            if (FAILED(list->SetPrivateDataInterface(recording_key, tag.Get()))) return false;
        }
        const auto stored = identity(list);
        if (stored.Get() != tag.Get()) return false;
        tag->observed_allocator_reset = tag->allocator_reset_target.load();
    }
    if (tag->allocator_epoch && tag->allocator_epoch->retired.load()) return false;
    if (!tag->current) {
        tag->current = std::make_shared<NrRecording>();
        tag->current->allocator_epoch = tag->allocator_epoch;
        if (tag->allocator_epoch) ++tag->allocator_epoch->nr_recordings;
    }
    tag->tracked.store(true);
    return true;
}
bool NrLifetime::record(ID3D12GraphicsCommandList* list) noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    if (!ensure_dlss_nr_recording(list)) return false;
    const auto recording = identity(list)->current;
    for (const auto& use : uses_) if (use == recording) return true;
    uses_.push_back(recording);
    return true;
}
bool NrLifetime::contains(ID3D12GraphicsCommandList* list) const noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    const auto tag = identity(list);
    if (!tag || !tag->current) return false;
    for (const auto& use : uses_) if (use == tag->current) return true;
    return false;
}
void nr_recording_submitted(ID3D12CommandQueue* queue, ID3D12Object* list, NrSignal submit_signal) noexcept {
    if (!queue || !list) return;
    std::lock_guard execution_lock(calibration12_execution_mutex());
    const auto tag = identity(list);
    if (!tag || !tag->current) return;
    auto& recording = *tag->current;
    // Never share fence values between independent queues: completing one
    // queue must not satisfy a blocked execution on another queue.
    auto found = recording.points.end();
    for (auto it = recording.points.begin(); it != recording.points.end(); ++it)
        if (it->queue.Get() == queue) { found = it; break; }
    if (found == recording.points.end()) {
        recording.points.push_back({});
        found = std::prev(recording.points.end());
        found->queue = queue;
    }
    ++found->value;
    found->needs_signal = true;
    signal_point(recording, *found, submit_signal ? submit_signal : signal);
}
bool nr_recording_tagged(ID3D12Object* list) noexcept {
    const auto tag = list ? identity(list) : ComPtr<RecordingIdentity>{};
    return tag && tag->tracked.load();
}
void nr_recording_reset(ID3D12Object* list, HRESULT result) noexcept {
    if (FAILED(result)) return;
    std::lock_guard execution_lock(calibration12_execution_mutex());
    const auto tag = identity(list);
    if (tag && tag->current) {
        tag->current->retired = true;
        tag->current.reset();
    }
}
// Binding metadata never retains the allocator or takes the graphics execution
// mutex. Unrelated OpenXR/driver list Reset/Create calls must remain unblocked.
void nr_recording_bind_allocator(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator) noexcept {
    const auto native = native_d3d12_interface(list);
    if (!native || !allocator) return;
    auto allocation = allocator_identity(allocator, true);
    if (!allocation) return;
    auto tag = identity(native.Get());
    if (!tag) {
        tag.Attach(new RecordingIdentity);
        if (FAILED(native->SetPrivateDataInterface(recording_key, tag.Get()))) return;
    }
    std::lock_guard lock(allocation->mutex);
    if (!allocation->epoch) allocation->epoch = std::make_shared<NrAllocatorEpoch>();
    tag->allocator_epoch = allocation->epoch;
    const auto native_allocator = native_d3d12_interface(allocator);
    tag->allocator_reset_target.store(native_allocator
        ? (*reinterpret_cast<void***>(native_allocator.Get()))[8] : nullptr);
}
void* nr_recording_allocator_reset_target(ID3D12GraphicsCommandList* list) noexcept {
    const auto tag = identity(list);
    return tag ? tag->allocator_reset_target.load() : nullptr;
}
void nr_allocator_reset(ID3D12CommandAllocator* allocator, HRESULT result) noexcept {
    if (FAILED(result)) return;
    auto allocation = allocator_identity(allocator, false);
    if (!allocation) return;
    std::lock_guard lock(allocation->mutex);
    if (allocation->epoch) {
        allocation->epoch->retired.store(true);
        if (const auto records = allocation->epoch->nr_recordings.load()) {
            static std::atomic<std::uint64_t> retirements{};
            const auto count = ++retirements;
            if (count <= 4 || count % 300 == 0)
                trace_event("DLSS-NR allocator storage retired recordings=%llu generations=%llu", records, count);
        }
    }
    allocation->epoch.reset();
}
void NrLifetime::collect(NrSignal submit_signal) noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    for (auto it = uses_.begin(); it != uses_.end();) {
        auto& recording = **it;
        collect_recording(recording, submit_signal ? submit_signal : signal);
        const bool retired = recording.retired ||
            (recording.allocator_epoch && recording.allocator_epoch->retired.load());
        if (retired && recording.points.empty()) {
            created_ += recording.created;
            released_ += recording.released;
            it = uses_.erase(it);
        } else ++it;
    }
}
std::uint64_t NrLifetime::fences_created() const noexcept {
    auto total = created_;
    for (const auto& use : uses_) total += use->created;
    return total;
}
NrLifetimeStatus NrLifetime::status() const noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    NrLifetimeStatus result{};
    for (const auto& recording : uses_) {
        result.pending_queues += recording->points.size();
        if (!recording->allocator_epoch) ++result.allocator_unknown;
        if (recording->points.empty()) {
            if (recording->created) ++result.completed_recordings;
            else ++result.unsubmitted_recordings;
        }
    }
    return result;
}
std::uint64_t NrLifetime::fences_released() const noexcept {
    auto total = released_;
    for (const auto& use : uses_) total += use->released;
    return total;
}
} // namespace cheeky::foveated_dlss
