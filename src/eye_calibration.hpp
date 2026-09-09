#pragma once
#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>
#include <string>
namespace cheeky::foveated_dlss {
enum class EyeCalibrationBackend { none, openvr, openxr };
// Asynchronous D3D11/D3D12 calibration for OpenVR and OpenXR. D3D11 work
// stays on the immediate context's owning thread; UI only reads snapshots.
struct EyeCalibrationStats {
    bool enabled{};
    EyeCalibrationBackend backend{};
    bool runtime_active{};
    unsigned graphics_api{};
    std::uint64_t frames{}, captures{}, completed{}, valid{}, skipped{}, allocations{}, mismatches{},
        gpu_samples{};
    unsigned in_flight{};
    double cpu_us_per_frame{}, max_cpu_call_us{}, gpu_us{}, max_gpu_us{}, latency_frames{};
    std::uint64_t left_view{}, right_view{};
    std::uint64_t corrections{}, applied{};
    bool correction_active{};
    bool openvr_active{}, unsupported_submission{};
    std::uint64_t unsupported_submissions{};
};
const char* eye_calibration_status(const EyeCalibrationStats&) noexcept;
const char* eye_calibration_backend_name(EyeCalibrationBackend) noexcept;
std::string eye_calibration_json();
// Session control. Disabling invalidates the pair and outstanding results.
void eye_calibration_enable(bool) noexcept;
// Loader-lock safe: stop new captures/publications when the host detaches.
// Resource draining remains on the render thread; enable() starts a new epoch.
void eye_calibration_suspend() noexcept;
bool eye_calibration_enabled() noexcept;
EyeCalibrationStats eye_calibration_stats() noexcept;
void eye_calibration_reset_stats() noexcept;
bool eye_calibration_frame(EyeCalibrationBackend backend = EyeCalibrationBackend::openvr,
                           std::uint64_t session_generation = 0, unsigned graphics_api = 0) noexcept;
void eye_calibration_tick() noexcept;
void eye_calibration_stop() noexcept;
void eye_calibration_stamp(ID3D11DeviceContext*, ID3D11Resource*, std::uint64_t, unsigned, unsigned, unsigned,
                           unsigned) noexcept;
void eye_calibration_stamp12(ID3D12GraphicsCommandList*, ID3D12Resource*, std::uint64_t, unsigned, unsigned,
                             unsigned, unsigned,
                             D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS) noexcept;
std::uint64_t eye_calibration_submit(ID3D11Texture2D*, unsigned, float, float, float, float,
                                     unsigned array_slice = 0,
                                     EyeCalibrationBackend backend = EyeCalibrationBackend::openvr,
                                     std::uint64_t session_generation = 0) noexcept;
void eye_calibration_destroy_session(std::uint64_t session_generation) noexcept;
std::uint64_t eye_calibration_submit12(ID3D12Resource*, ID3D12CommandQueue*, unsigned eye, float, float,
                                       float, float, unsigned slice = 0,
                                       EyeCalibrationBackend backend = EyeCalibrationBackend::openvr,
                                       std::uint64_t session_generation = 0) noexcept;
void eye_calibration_result(std::uint64_t, int, unsigned physical_eye = ~0U) noexcept;
void eye_calibration_unsupported_submit() noexcept;
} // namespace cheeky::foveated_dlss
