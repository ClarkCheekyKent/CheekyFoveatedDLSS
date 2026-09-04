#pragma once

#include "settings.hpp"

#include <cstdint>

namespace cheeky::foveated_dlss {

enum class DiagnosticApi : std::uint32_t {
    d3d11,
    d3d12,
};

enum class MotionVectorSpace : std::uint32_t {
    unknown,
    input,
    output,
};

enum class DiagnosticGpuTiming : std::uint32_t {
    transport_total,
    foveated_dlss,
    native_dlss,
    peripheral_dlaa,
    full_dlss_nr,
    foveated_dlss_nr,
    d3d12_full_dlss_nr,
    d3d12_foveated_dlss_nr,
    d3d12_peripheral_dlaa,
    count,
};

enum class DiagnosticState : std::uint32_t {
    waiting,
    disabled,
    invalid_arguments,
    missing_resources,
    unsupported_resources,
    incompatible_contract,
    resource_initialization_failed,
    allocation_failed,
    prepare_rejected,
    streamline_direct_path_suppressed,
    active,
    ngx_evaluation_failed,
};

enum class D3D11ExecutionPath : std::uint32_t {
    waiting,
    dx12_transport,
    dx11_direct,
    game_fallback,
    disabled_passthrough,
};

enum class D3D11TransportStatus : std::uint32_t {
    not_attempted,
    active,
    missing_initialization_data,
    missing_callbacks,
    missing_resources,
    unsupported_resources,
    invalid_dimensions,
    unsupported_motion_vectors,
    device_initialization_failed,
    unsupported_context,
    transport_slot_busy,
    resource_initialization_failed,
    depth_conversion_failed,
    synchronization_failed,
    ngx_evaluation_failed,
    compositing_failed,
};

struct DiagnosticSnapshot {
    bool runtime_loaded{};
    bool streamline_detected{};
    bool hook_discovered{};
    bool direct_detour_installed{};
    std::uint64_t create_calls{};
    std::uint64_t evaluate_calls{};
    std::uint64_t active_calls{};
    std::uint32_t received_input_width{};
    std::uint32_t received_input_height{};
    std::uint32_t received_output_width{};
    std::uint32_t received_output_height{};
    std::uint32_t motion_vector_width{};
    std::uint32_t motion_vector_height{};
    MotionVectorSpace motion_vector_space{MotionVectorSpace::unknown};
    CropGeometry passed_crop{};
    float transport_gpu_ms{};
    float foveated_dlss_gpu_ms{};
    float peripheral_dlaa_gpu_ms{};
    float full_dlss_nr_gpu_ms{};
    float foveated_dlss_nr_gpu_ms{};
    float native_dlss_gpu_ms{};
    float foveated_frame_ms{};
    float native_frame_ms{};
    bool has_private_result{};
    std::uint32_t last_private_result{};
    std::uint32_t last_result{};
    DiagnosticState state{DiagnosticState::waiting};
    D3D11ExecutionPath d3d11_execution_path{D3D11ExecutionPath::waiting};
    D3D11TransportStatus d3d11_transport_status{
        D3D11TransportStatus::not_attempted
    };
};

void diagnostic_note_hook(DiagnosticApi api) noexcept;
void diagnostic_note_runtime_loaded(DiagnosticApi api) noexcept;
void diagnostic_note_streamline_detected() noexcept;
void diagnostic_note_direct_detour(DiagnosticApi api) noexcept;
void diagnostic_note_create(DiagnosticApi api) noexcept;
void diagnostic_note_evaluate(
    DiagnosticApi api,
    std::uint32_t input_width,
    std::uint32_t input_height,
    std::uint32_t output_width,
    std::uint32_t output_height
) noexcept;
void diagnostic_note_motion_vectors(
    DiagnosticApi api,
    std::uint32_t width,
    std::uint32_t height,
    MotionVectorSpace space
) noexcept;
void diagnostic_note_state(DiagnosticApi api, DiagnosticState state) noexcept;
void diagnostic_note_activation(
    DiagnosticApi api,
    const CropGeometry& crop
) noexcept;
void diagnostic_note_crop(
    DiagnosticApi api,
    const CropGeometry& crop
) noexcept;
void diagnostic_note_private_result(
    DiagnosticApi api,
    std::uint32_t result
) noexcept;
void diagnostic_note_result(DiagnosticApi api, std::uint32_t result) noexcept;
void diagnostic_note_transport_gpu_time(float milliseconds) noexcept;
void diagnostic_note_foveated_dlss_gpu_time(float milliseconds) noexcept;
void diagnostic_note_peripheral_dlaa_gpu_time(
    DiagnosticApi api,
    float milliseconds
) noexcept;
void diagnostic_note_dlss_nr_gpu_time(
    DiagnosticApi api,
    float milliseconds,
    bool foveated
) noexcept;
void diagnostic_note_native_dlss_gpu_time(float milliseconds) noexcept;
void diagnostic_note_frame_rate(
    float frames_per_second,
    bool foveated_enabled
) noexcept;
[[nodiscard]] bool diagnostic_should_sample_gpu_time(
    DiagnosticGpuTiming timing
) noexcept;
void diagnostic_note_d3d11_execution_path(D3D11ExecutionPath path) noexcept;
void diagnostic_note_d3d11_transport_status(D3D11TransportStatus status) noexcept;

[[nodiscard]] DiagnosticSnapshot diagnostic_snapshot(
    DiagnosticApi api
) noexcept;
[[nodiscard]] const char* motion_vector_space_name(
    MotionVectorSpace space
) noexcept;
[[nodiscard]] const char* diagnostic_state_name(DiagnosticState state) noexcept;
[[nodiscard]] const char* d3d11_execution_path_name(
    D3D11ExecutionPath path
) noexcept;
[[nodiscard]] const char* d3d11_transport_status_name(
    D3D11TransportStatus status
) noexcept;

}  // namespace cheeky::foveated_dlss
