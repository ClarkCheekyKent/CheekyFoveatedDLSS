#include "dlss_nr_lifetime.hpp"
#include "eye_calibration_d3d12.hpp"
#include "graphics_observer.hpp"
#include <atomic>
#include <vector>
#include <wrl/client.h>

namespace cheeky::foveated_dlss {
using Microsoft::WRL::ComPtr;
struct NrRecording {
    struct Point {
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence;
        std::uint64_t value{};
        bool needs_signal{};
    };
    bool retired{};
    std::vector<Point> points;
    std::uint64_t created{}, released{};
};
namespace {
constexpr GUID recording_key{0x24c7a6bb, 0xe7d4, 0x44d2, {0xb9, 0x9d, 0x36, 0xb2, 0x41, 0xc7, 0x8f, 0x71}};
class RecordingIdentity final : public IUnknown {
    std::atomic<ULONG> refs{1};
public:
    std::shared_ptr<NrRecording> current;
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
            std::lock_guard execution_lock(calibration12_execution_mutex());
            if (current) current->retired = true;
            delete this;
        }
        return left;
    }
};
ComPtr<RecordingIdentity> identity(ID3D12Object* list) {
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
    if (!list) return false;
    std::lock_guard execution_lock(calibration12_execution_mutex());
    auto tag = identity(list);
    if (!tag) {
        // Probe through this list's device, even if a different device already
        // installed hooks. Unsupported observation/private data means fallback.
        if (!ensure_native_observer(list)) return false;
        tag.Attach(new RecordingIdentity);
        if (FAILED(list->SetPrivateDataInterface(recording_key, tag.Get()))) return false;
        const auto stored = identity(list);
        if (stored.Get() != tag.Get()) return false;
    }
    if (!tag->current) tag->current = std::make_shared<NrRecording>();
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
void nr_recording_reset(ID3D12Object* list, HRESULT result) noexcept {
    if (FAILED(result)) return;
    std::lock_guard execution_lock(calibration12_execution_mutex());
    const auto tag = identity(list);
    if (tag && tag->current) {
        tag->current->retired = true;
        tag->current.reset();
    }
}
void NrLifetime::collect(NrSignal submit_signal) noexcept {
    std::lock_guard execution_lock(calibration12_execution_mutex());
    for (auto it = uses_.begin(); it != uses_.end();) {
        auto& recording = **it;
        collect_recording(recording, submit_signal ? submit_signal : signal);
        if (recording.retired && recording.points.empty()) {
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
std::uint64_t NrLifetime::fences_released() const noexcept {
    auto total = released_;
    for (const auto& use : uses_) total += use->released;
    return total;
}
} // namespace cheeky::foveated_dlss
