#pragma once

#include <d3d12.h>
#include <cstdint>
#include <deque>
#include <wrl/client.h>

namespace cheeky::foveated_dlss {
// Internal NR use bookkeeping. The owner serializes access and retains its
// resources until empty(); collection is independent of owner retirement.
class NrLifetime {
public:
    using Signal = HRESULT (*)(ID3D12CommandQueue*, ID3D12Fence*, std::uint64_t);
    bool record(ID3D12GraphicsCommandList*) noexcept;
    void submitted(ID3D12CommandQueue*, ID3D12Object*, bool already_submitted) noexcept;
    void signal_pending(Signal = signal) noexcept;
    void collect() noexcept;
    [[nodiscard]] bool empty() const noexcept { return uses_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return uses_.size(); }
    [[nodiscard]] std::uint64_t fences_created() const noexcept { return created_; }
    [[nodiscard]] std::uint64_t fences_released() const noexcept { return released_; }
private:
    static HRESULT signal(ID3D12CommandQueue*, ID3D12Fence*, std::uint64_t);
    struct Use {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        std::uint64_t identity{};
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    };
    std::deque<Use> uses_;
    std::uint64_t created_{}, released_{};
};
} // namespace cheeky::foveated_dlss
