#pragma once
#include <d3d12.h>
#include <array>
#include <memory>
#include <mutex>

namespace cheeky::foveated_dlss {
struct Calibration12Frame;
struct Calibration12Failure {
    // Static stage names; HRESULT is S_OK for a logical rejection.
    const char* stage{"none"};
    HRESULT result{S_OK};
};
struct Calibration12Readback {
    bool ready{}, reusable{}, valid{}, timing_valid{};
    std::array<float, 8> scores{};
    double gpu_us{};
    std::uint64_t allocations{};
    Calibration12Failure failure{};
};
std::shared_ptr<Calibration12Frame> calibration12_create(ID3D12Device*);
bool calibration12_begin(Calibration12Frame&) noexcept;
bool calibration12_stamp(Calibration12Frame&, ID3D12GraphicsCommandList*, ID3D12Resource*, unsigned candidate,
                         unsigned x, unsigned y, D3D12_RESOURCE_STATES, std::uint64_t& allocations,
                         Calibration12Failure* failure = nullptr) noexcept;
bool calibration12_capture(Calibration12Frame&, ID3D12CommandQueue*, ID3D12Resource*, unsigned eye,
                           unsigned slice, D3D12_RESOURCE_STATES state, const std::array<D3D12_BOX, 2>& boxes,
                           std::uint64_t& allocations, Calibration12Failure* failure = nullptr) noexcept;
Calibration12Readback calibration12_poll(Calibration12Frame&) noexcept;
void calibration12_submitted(ID3D12CommandQueue*, ID3D12GraphicsCommandList*) noexcept;
void calibration12_retired(ID3D12GraphicsCommandList*) noexcept;
bool calibration12_internal_work() noexcept;
// Serialize Execute/Reset with readback retirement, including forwarding wrappers.
std::recursive_mutex& calibration12_execution_mutex() noexcept;
} // namespace cheeky::foveated_dlss
