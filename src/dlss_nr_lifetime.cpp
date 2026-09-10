#include "dlss_nr_lifetime.hpp"
#include "d3d12_submission_identity.hpp"

namespace cheeky::foveated_dlss {
HRESULT NrLifetime::signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, std::uint64_t value) {
    return queue->Signal(fence, value);
}
bool NrLifetime::record(ID3D12GraphicsCommandList* list) noexcept {
    if (!list) return false;
    for (const auto& use : uses_) if (use.list.Get() == list) return true;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Use use;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&use.fence)))) return false;
    use.list = list;
    use.identity = d3d12_submission_identity(list, true);
    uses_.push_back(std::move(use));
    ++created_;
    return true;
}
void NrLifetime::submitted(ID3D12CommandQueue* queue, ID3D12Object* list, bool already_submitted) noexcept {
    if (!queue || !list) return;
    const auto identity = d3d12_submission_identity(list);
    for (auto& use : uses_) {
        if (!use.list || (use.list.Get() != list && (!identity || use.identity != identity))) continue;
        use.queue = queue;
        use.list.Reset();
        if (already_submitted && SUCCEEDED(signal(queue, use.fence.Get(), 1U))) use.queue.Reset();
    }
}
void NrLifetime::signal_pending(Signal submit_signal) noexcept {
    for (auto& use : uses_)
        if (use.queue && SUCCEEDED(submit_signal(use.queue.Get(), use.fence.Get(), 1U))) use.queue.Reset();
}
void NrLifetime::collect() noexcept {
    for (auto it = uses_.begin(); it != uses_.end();) {
        if (!it->list && !it->queue && it->fence->GetCompletedValue() >= 1U) {
            it = uses_.erase(it);
            ++released_;
        } else ++it;
    }
}
} // namespace cheeky::foveated_dlss
