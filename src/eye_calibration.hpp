#pragma once
#include <d3d11.h>
#include <cstdint>
#include <string>
namespace cheeky::foveated_dlss {
// Asynchronous D3D11/OpenVR calibration. GPU work stays on the immediate
// context's owning thread; UI/report callers only read a locked snapshot.
struct EyeCalibrationStats {
    bool enabled{};
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
std::string eye_calibration_json();
// Session control. Disabling invalidates the pair and outstanding results.
void eye_calibration_enable(bool) noexcept;
// Loader-lock safe: stop new captures/publications when the host detaches.
// Resource draining remains on the render thread; enable() starts a new epoch.
void eye_calibration_suspend() noexcept;
bool eye_calibration_enabled() noexcept;
EyeCalibrationStats eye_calibration_stats() noexcept;
void eye_calibration_reset_stats() noexcept;
void eye_calibration_frame() noexcept;
void eye_calibration_tick() noexcept;
void eye_calibration_stop() noexcept;
void eye_calibration_stamp(ID3D11DeviceContext*, ID3D11Resource*, std::uint64_t, unsigned, unsigned, unsigned,
                           unsigned) noexcept;
std::uint64_t eye_calibration_submit(ID3D11Texture2D*, unsigned, float, float, float, float) noexcept;
void eye_calibration_result(std::uint64_t, int) noexcept;
void eye_calibration_unsupported_submit() noexcept;
} // namespace cheeky::foveated_dlss
