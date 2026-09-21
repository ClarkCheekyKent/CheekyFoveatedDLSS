#pragma once
#include <d3d12.h>
#include <array>
#include <memory>
#include <mutex>
#include "eye_calibration_capture.hpp"

namespace cheeky::foveated_dlss {
struct Calibration12Frame;
struct Calibration12Failure {
    // Static stage names; HRESULT is S_OK for a logical rejection.
    const char* stage{"none"};
    HRESULT result{S_OK};
};
struct Calibration12Readback {
    bool ready{}, reusable{}, valid{}, timing_valid{};
    // Source before/after, submitted A/B per eye, then vertically flipped A/B per eye.
    std::array<float, 12> scores{};
    double gpu_us{};
    std::uint64_t allocations{};
    Calibration12Failure failure{};
};
std::shared_ptr<Calibration12Frame> calibration12_create(ID3D12Device*);
bool calibration12_begin(Calibration12Frame&, std::uint64_t* allocations = nullptr) noexcept;
enum class Calibration12StampMode { source_proof, refresh, marker_only };
bool calibration12_stamp(Calibration12Frame&, ID3D12GraphicsCommandList*, ID3D12Resource*, unsigned candidate,
                         unsigned x, unsigned y, D3D12_RESOURCE_STATES, std::uint64_t& allocations,
                         Calibration12Failure* failure = nullptr,
                         const CalibrationImageRequestPtr& support = {},
                         const CalibrationImageInfo& support_info = {},
                         std::uint32_t marker_code = 0,
                         Calibration12StampMode mode = Calibration12StampMode::source_proof) noexcept;
bool calibration12_capture(Calibration12Frame&, ID3D12CommandQueue*, ID3D12Resource*, unsigned eye,
                           unsigned slice, D3D12_RESOURCE_STATES state, const std::array<D3D12_BOX, 4>& boxes,
                           std::uint64_t& allocations, Calibration12Failure* failure = nullptr,
                           const CalibrationImageRequestPtr& support = {},
                           const CalibrationImageInfo& support_info = {}) noexcept;
// Mixed API frames have only source proof in DX12; submitted patches arrive
// independently from DX11 and must not be treated as missing DX12 readbacks.
Calibration12Readback calibration12_poll(Calibration12Frame&, bool source_only = false) noexcept;
void calibration12_submitted(ID3D12CommandQueue*, ID3D12GraphicsCommandList*) noexcept;
void calibration12_retired(ID3D12GraphicsCommandList*) noexcept;
bool calibration12_internal_work() noexcept;
// Serialize Execute/Reset with readback retirement, including forwarding wrappers.
std::recursive_mutex& calibration12_execution_mutex() noexcept;
} // namespace cheeky::foveated_dlss
