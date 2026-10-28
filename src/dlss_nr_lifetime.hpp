#pragma once

#include <d3d12.h>
#include <cstdint>
#include <deque>
#include <memory>

namespace cheeky::foveated_dlss {
struct NrRecording;
struct NrLifetimeStatus {
    std::size_t pending_queues{};
    std::size_t completed_recordings{};
    std::size_t unsubmitted_recordings{};
    std::size_t allocator_unknown{};
};
using NrSignal = HRESULT (*)(ID3D12CommandQueue*, ID3D12Fence*, std::uint64_t);
// All operations use calibration12_execution_mutex(), before any owner mutex.
// A forwarding object's private data identifies the same recording generation.
bool ensure_dlss_nr_recording(ID3D12GraphicsCommandList*) noexcept;
void nr_recording_submitted(ID3D12CommandQueue*, ID3D12Object*, NrSignal = nullptr) noexcept;
void nr_recording_reset(ID3D12Object*, HRESULT reset_result) noexcept;
// Observe the allocator association on list creation and successful list Reset.
// Allocator Reset invalidates old command storage even if list objects survive.
void nr_recording_bind_allocator(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*) noexcept;
void nr_allocator_reset(ID3D12CommandAllocator*, HRESULT reset_result) noexcept;
[[nodiscard]] void* nr_recording_allocator_reset_target(ID3D12GraphicsCommandList*) noexcept;
// Whether the list has ever carried a DLSS-NR recording. Private data only, no lock.
bool nr_recording_tagged(ID3D12Object*) noexcept;

// Resources belong to recordings, not their first execution. A use drains only
// after list/allocator Reset or destruction and completion on every executing queue.
class NrLifetime {
public:
    bool record(ID3D12GraphicsCommandList*) noexcept;
    [[nodiscard]] bool contains(ID3D12GraphicsCommandList*) const noexcept;
    void collect(NrSignal = nullptr) noexcept;
    [[nodiscard]] bool empty() const noexcept { return uses_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return uses_.size(); }
    // Call collect() first to refresh queue completion before reporting holds.
    [[nodiscard]] NrLifetimeStatus status() const noexcept;
    [[nodiscard]] std::uint64_t fences_created() const noexcept;
    [[nodiscard]] std::uint64_t fences_released() const noexcept;
private:
    std::deque<std::shared_ptr<NrRecording>> uses_;
    std::uint64_t created_{}, released_{};
};
} // namespace cheeky::foveated_dlss
