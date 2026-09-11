#pragma once

#include <d3d12.h>
#include <cstdint>
#include <deque>
#include <memory>

namespace cheeky::foveated_dlss {
struct NrRecording;
using NrSignal = HRESULT (*)(ID3D12CommandQueue*, ID3D12Fence*, std::uint64_t);
// All operations use calibration12_execution_mutex(), before any owner mutex.
// A forwarding object's private data identifies the same recording generation.
bool ensure_dlss_nr_recording(ID3D12GraphicsCommandList*) noexcept;
void nr_recording_submitted(ID3D12CommandQueue*, ID3D12Object*, NrSignal = nullptr) noexcept;
void nr_recording_reset(ID3D12Object*, HRESULT reset_result) noexcept;

// Resources belong to recordings, not their first execution. A use drains only
// after successful Reset/destruction and completion on every executing queue.
class NrLifetime {
public:
    bool record(ID3D12GraphicsCommandList*) noexcept;
    void collect(NrSignal = nullptr) noexcept;
    [[nodiscard]] bool empty() const noexcept { return uses_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return uses_.size(); }
    [[nodiscard]] std::uint64_t fences_created() const noexcept;
    [[nodiscard]] std::uint64_t fences_released() const noexcept;
private:
    std::deque<std::shared_ptr<NrRecording>> uses_;
    std::uint64_t created_{}, released_{};
};
} // namespace cheeky::foveated_dlss
