#pragma once
#include <d3d12.h>
#include <array>
#include <memory>
#include <mutex>
#include "eye_calibration_capture.hpp"
#include "eye_calibration_search.hpp"
#include "eye_calibration_tracking.hpp"

namespace cheeky::foveated_dlss {
struct Calibration12Frame;
struct Calibration12Failure {
    // Static stage names; HRESULT is S_OK for a logical rejection.
    const char* stage{"none"};
    HRESULT result{S_OK};
};
struct Calibration12Readback {
    bool ready{}, reusable{}, valid{}, timing_valid{};
    // Source before/after, then eight submitted scores per placement:
    // A/B per eye, followed by vertically flipped A/B per eye.
    std::array<float, calibration_patch_count> scores{};
    std::array<CalibrationSearchResult, calibration_patch_count> tracked{};
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
                         Calibration12StampMode mode = Calibration12StampMode::source_proof,
                         std::span<const CalibrationMarkerPoint> extra_markers = {}) noexcept;
bool calibration12_capture(Calibration12Frame&, ID3D12CommandQueue*, ID3D12Resource*, unsigned eye,
                           unsigned slice, D3D12_RESOURCE_STATES state, std::span<const D3D12_BOX> boxes,
                           std::uint64_t& allocations, Calibration12Failure* failure = nullptr,
                           const CalibrationImageRequestPtr& support = {},
                           const CalibrationImageInfo& support_info = {},
                           std::span<const std::uint32_t> codes = {},
                           std::span<const unsigned> mirrors = {},
                           const CalibrationSearchPtr& search = {},
                           std::span<const CalibrationTrackingPatch> tracking = {}) noexcept;
// Mixed API frames have only source proof in DX12; submitted patches arrive
// independently from DX11 and must not be treated as missing DX12 readbacks.
Calibration12Readback calibration12_poll(Calibration12Frame&, bool source_only = false, unsigned source_mask = 3) noexcept;
void calibration12_submitted(ID3D12CommandQueue*, ID3D12GraphicsCommandList*) noexcept;
void calibration12_retired(ID3D12GraphicsCommandList*) noexcept;
bool calibration12_internal_work() noexcept;
// Serialize Execute/Reset with readback retirement, including forwarding wrappers.
std::recursive_mutex& calibration12_execution_mutex() noexcept;
} // namespace cheeky::foveated_dlss
