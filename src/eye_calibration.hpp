#pragma once
#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <array>
namespace cheeky::foveated_dlss {
inline constexpr unsigned eye_calibration_failure_limit = 20;
enum class EyeCalibrationBackend { none, openvr, openxr };
// Asynchronous D3D11/D3D12 calibration for OpenVR and OpenXR. D3D11 work
// uses the render thread except protected native OpenXR pre-release copies;
// UI only reads snapshots.
struct EyeCalibrationStats {
    bool enabled{};
    EyeCalibrationBackend backend{};
    bool runtime_active{};
    unsigned graphics_api{};
    unsigned source_graphics_api{}, submission_graphics_api{};
    std::uint64_t frames{}, captures{}, completed{}, valid{}, skipped{}, allocations{}, mismatches{},
        gpu_samples{};
    unsigned in_flight{};
    double cpu_us_per_frame{}, max_cpu_call_us{}, gpu_us{}, max_gpu_us{}, latency_frames{};
    std::array<double, 2> search_ms{};
    std::array<double,2> capture_total_ms{-1,-1}, capture_setup_ms{-1,-1}, capture_wait_ms{-1,-1},
        capture_map_ms{-1,-1}, capture_copy_ms{-1,-1};
    double max_capture_ms{};
    double peak_capture_setup_ms{}, peak_capture_wait_ms{}, peak_capture_map_ms{}, peak_capture_copy_ms{}, peak_capture_other_ms{};
    std::uint64_t peak_capture_sequence{};
    unsigned peak_capture_eye{}, peak_capture_width{}, peak_capture_height{}, peak_capture_map_polls{};
    std::uint64_t capture_timing_samples{};
    double verification_cpu_ms{}, verification_cpu_us_per_frame{}, verification_last_ms{}, verification_peak_ms{};
    std::uint64_t verification_patch_calls{};
    std::array<std::uint64_t,3> verification_paths{};
    double max_search_ms{};
    std::uint64_t search_timing_samples{};
    std::uint64_t full_calibration_attempts{}, full_calibration_successes{}, recalibration_requests{};
    unsigned verification_failure_streak{};
    const char* full_calibration_reason{"none"};
    const char* gpu_timing_status{"Waiting for a complete GPU timestamp sample"};
    std::uint64_t left_view{}, right_view{};
    std::uint64_t corrections{}, applied{};
    bool correction_active{};
    bool crop_mapping_active{};
    bool vertical_flip{};
    bool openvr_active{}, unsupported_submission{};
    std::uint64_t unsupported_submissions{};
    // A rejected capture can fail multiple checks; counters overlap.
    std::array<std::uint64_t, 8> rejection_counts{};
    std::uint64_t rejected{}, publication_rejected{}, last_rejected_sequence{};
    unsigned last_rejection_mask{}, last_evaluations{}, last_submits{};
    std::array<float, 12> last_rejected_scores{};
    std::array<float,12> last_best_scores{}, last_best_contrasts{}, last_max_contrasts{};
    std::array<unsigned,12> last_best_bits{}, last_score_bits{}, last_search_positions{}, last_score_probes{}, last_low_contrast_positions{};
    const char* last_marker_failure{"none"};
    unsigned last_source_mask{};
    bool last_shared_source_assumed{}, last_motion_unreliable{};
    double last_marker_error_x{}, last_marker_error_y{};
    std::array<std::uint64_t,5> marker_failure_counts{};
    // Captured on the render thread; no repeated logging or GPU waits.
    std::array<unsigned, 2> d3d12_source_formats{}, d3d12_submitted_formats{};
    std::uint64_t d3d12_stamp_failures{}, d3d12_capture_failures{}, d3d12_readback_failures{};
    std::uint64_t d3d12_continuous_stamps{}, d3d12_continuous_skipped{};
    const char* d3d12_last_stamp_failure{"none"};
    const char* d3d12_last_capture_failure{"none"};
    const char* d3d12_last_readback_failure{"none"};
    HRESULT d3d12_stamp_error{S_OK}, d3d12_capture_error{S_OK}, d3d12_readback_error{S_OK};
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
