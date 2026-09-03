#include "diagnostics.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace cheeky::foveated_dlss {
namespace {

struct RuntimeDiagnostics {
    std::atomic<bool> runtime_loaded{};
    std::atomic<bool> hook_discovered{};
    std::atomic<bool> direct_detour_installed{};
    std::atomic<std::uint64_t> create_calls{};
    std::atomic<std::uint64_t> evaluate_calls{};
    std::atomic<std::uint64_t> active_calls{};
    std::atomic<std::uint32_t> received_input_width{};
    std::atomic<std::uint32_t> received_input_height{};
    std::atomic<std::uint32_t> received_output_width{};
    std::atomic<std::uint32_t> received_output_height{};
    std::atomic<std::uint32_t> crop_input_base_x{};
    std::atomic<std::uint32_t> crop_input_base_y{};
    std::atomic<std::uint32_t> crop_input_width{};
    std::atomic<std::uint32_t> crop_input_height{};
    std::atomic<std::uint32_t> crop_output_base_x{};
    std::atomic<std::uint32_t> crop_output_base_y{};
    std::atomic<std::uint32_t> crop_output_width{};
    std::atomic<std::uint32_t> crop_output_height{};
    std::atomic<std::uint32_t> transport_gpu_ms_bits{};
    std::atomic<std::uint32_t> foveated_dlss_gpu_ms_bits{};
    std::atomic<std::uint32_t> full_dlss_nr_gpu_ms_bits{};
    std::atomic<std::uint32_t> foveated_dlss_nr_gpu_ms_bits{};
    std::atomic<std::uint32_t> native_dlss_gpu_ms_bits{};
    std::atomic<std::uint32_t> foveated_frame_ms_bits{};
    std::atomic<std::uint32_t> native_frame_ms_bits{};
    std::atomic<bool> has_private_result{};
    std::atomic<std::uint32_t> last_private_result{};
    std::atomic<std::uint32_t> last_result{};
    std::atomic<DiagnosticState> state{DiagnosticState::waiting};
    std::atomic<D3D11ExecutionPath> d3d11_execution_path{
        D3D11ExecutionPath::waiting
    };
    std::atomic<D3D11TransportStatus> d3d11_transport_status{
        D3D11TransportStatus::not_attempted
    };
};

std::array<RuntimeDiagnostics, 2U> diagnostics{};
std::atomic<bool> streamline_detected{};

constexpr auto timing_sample_interval = std::chrono::milliseconds(125);
constexpr auto timing_publish_interval = std::chrono::milliseconds(250);

struct GpuTimingAccumulator {
    std::atomic<std::int64_t> last_sample_ns{};
    std::mutex mutex;
    std::int64_t window_start_ns{};
    double sum_ms{};
    std::uint32_t sample_count{};
};

std::array<
    GpuTimingAccumulator,
    static_cast<std::size_t>(DiagnosticGpuTiming::count)
> timing_accumulators{};

struct FrameTimingAccumulator {
    std::mutex mutex;
    std::int64_t settle_until_ns{};
    std::int64_t window_start_ns{};
    double sum_ms{};
    std::uint32_t sample_count{};
    bool foveated_enabled{};
    bool mode_initialized{};
};

FrameTimingAccumulator frame_timing{};

[[nodiscard]] RuntimeDiagnostics& for_api(const DiagnosticApi api) noexcept {
    return diagnostics[api == DiagnosticApi::d3d12 ? 1U : 0U];
}

[[nodiscard]] std::int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

[[nodiscard]] std::size_t timing_index(
    const DiagnosticGpuTiming timing
) noexcept {
    return static_cast<std::size_t>(timing);
}

void note_averaged_gpu_time(
    const DiagnosticGpuTiming timing,
    std::atomic<std::uint32_t>& destination,
    const float milliseconds
) noexcept {
    if (!(milliseconds > 0.0F)) return;
    const auto now = steady_now_ns();
    auto& accumulator = timing_accumulators[timing_index(timing)];
    std::lock_guard lock(accumulator.mutex);
    if (accumulator.window_start_ns == 0) {
        accumulator.window_start_ns = now;
    }
    accumulator.sum_ms += milliseconds;
    ++accumulator.sample_count;
    if (now - accumulator.window_start_ns <
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            timing_publish_interval
        ).count()) {
        return;
    }
    const auto average = static_cast<float>(
        accumulator.sum_ms / static_cast<double>(accumulator.sample_count)
    );
    std::uint32_t bits{};
    std::memcpy(&bits, &average, sizeof(bits));
    destination.store(bits, std::memory_order_release);
    accumulator.window_start_ns = now;
    accumulator.sum_ms = 0.0;
    accumulator.sample_count = 0U;
}

[[nodiscard]] float load_gpu_time(
    const std::atomic<std::uint32_t>& source
) noexcept {
    const auto bits = source.load(std::memory_order_acquire);
    float milliseconds{};
    std::memcpy(&milliseconds, &bits, sizeof(milliseconds));
    return milliseconds;
}

}  // namespace

void diagnostic_note_hook(const DiagnosticApi api) noexcept {
    for_api(api).hook_discovered.store(true, std::memory_order_release);
}

void diagnostic_note_runtime_loaded(const DiagnosticApi api) noexcept {
    for_api(api).runtime_loaded.store(true, std::memory_order_release);
}

void diagnostic_note_streamline_detected() noexcept {
    streamline_detected.store(true, std::memory_order_release);
}

void diagnostic_note_direct_detour(const DiagnosticApi api) noexcept {
    auto& data = for_api(api);
    data.hook_discovered.store(true, std::memory_order_release);
    data.direct_detour_installed.store(true, std::memory_order_release);
}

void diagnostic_note_create(const DiagnosticApi api) noexcept {
    for_api(api).create_calls.fetch_add(1U, std::memory_order_relaxed);
}

void diagnostic_note_evaluate(
    const DiagnosticApi api,
    const std::uint32_t input_width,
    const std::uint32_t input_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height
) noexcept {
    auto& data = for_api(api);
    data.evaluate_calls.fetch_add(1U, std::memory_order_relaxed);
    data.received_input_width.store(input_width, std::memory_order_release);
    data.received_input_height.store(input_height, std::memory_order_release);
    data.received_output_width.store(output_width, std::memory_order_release);
    data.received_output_height.store(output_height, std::memory_order_release);
}

void diagnostic_note_state(
    const DiagnosticApi api,
    const DiagnosticState state
) noexcept {
    for_api(api).state.store(state, std::memory_order_release);
}

void diagnostic_note_activation(
    const DiagnosticApi api,
    const CropGeometry& crop
) noexcept {
    auto& data = for_api(api);
    data.active_calls.fetch_add(1U, std::memory_order_relaxed);
    diagnostic_note_crop(api, crop);
    data.state.store(DiagnosticState::active, std::memory_order_release);
}

void diagnostic_note_crop(
    const DiagnosticApi api,
    const CropGeometry& crop
) noexcept {
    auto& data = for_api(api);
    data.crop_input_base_x.store(crop.input_base_x, std::memory_order_release);
    data.crop_input_base_y.store(crop.input_base_y, std::memory_order_release);
    data.crop_input_width.store(crop.input_width, std::memory_order_release);
    data.crop_input_height.store(crop.input_height, std::memory_order_release);
    data.crop_output_base_x.store(crop.output_base_x, std::memory_order_release);
    data.crop_output_base_y.store(crop.output_base_y, std::memory_order_release);
    data.crop_output_width.store(crop.output_width, std::memory_order_release);
    data.crop_output_height.store(crop.output_height, std::memory_order_release);
}

void diagnostic_note_private_result(
    const DiagnosticApi api,
    const std::uint32_t result
) noexcept {
    auto& data = for_api(api);
    data.last_private_result.store(result, std::memory_order_release);
    data.has_private_result.store(true, std::memory_order_release);
}

void diagnostic_note_result(
    const DiagnosticApi api,
    const std::uint32_t result
) noexcept {
    for_api(api).last_result.store(result, std::memory_order_release);
}

void diagnostic_note_transport_gpu_time(const float milliseconds) noexcept {
    note_averaged_gpu_time(
        DiagnosticGpuTiming::transport_total,
        for_api(DiagnosticApi::d3d11).transport_gpu_ms_bits,
        milliseconds
    );
}

void diagnostic_note_foveated_dlss_gpu_time(
    const float milliseconds
) noexcept {
    note_averaged_gpu_time(
        DiagnosticGpuTiming::foveated_dlss,
        for_api(DiagnosticApi::d3d11).foveated_dlss_gpu_ms_bits,
        milliseconds
    );
}

void diagnostic_note_dlss_nr_gpu_time(
    const DiagnosticApi api,
    const float milliseconds,
    const bool foveated
) noexcept {
    note_averaged_gpu_time(
        api == DiagnosticApi::d3d12
            ? (foveated
                ? DiagnosticGpuTiming::d3d12_foveated_dlss_nr
                : DiagnosticGpuTiming::d3d12_full_dlss_nr)
            : (foveated
                ? DiagnosticGpuTiming::foveated_dlss_nr
                : DiagnosticGpuTiming::full_dlss_nr),
        foveated
            ? for_api(api).foveated_dlss_nr_gpu_ms_bits
            : for_api(api).full_dlss_nr_gpu_ms_bits,
        milliseconds
    );
}

void diagnostic_note_native_dlss_gpu_time(
    const float milliseconds
) noexcept {
    note_averaged_gpu_time(
        DiagnosticGpuTiming::native_dlss,
        for_api(DiagnosticApi::d3d11).native_dlss_gpu_ms_bits,
        milliseconds
    );
}

void diagnostic_note_frame_rate(
    const float frames_per_second,
    const bool foveated_enabled
) noexcept {
    if (!(frames_per_second >= 1.0F && frames_per_second <= 1000.0F)) {
        return;
    }
    const auto now = steady_now_ns();
    constexpr auto settle_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::seconds(1)
        ).count();
    constexpr auto publish_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            timing_publish_interval
        ).count();
    std::lock_guard lock(frame_timing.mutex);
    if (!frame_timing.mode_initialized ||
        frame_timing.foveated_enabled != foveated_enabled) {
        frame_timing.mode_initialized = true;
        frame_timing.foveated_enabled = foveated_enabled;
        frame_timing.settle_until_ns = now + settle_ns;
        frame_timing.window_start_ns = 0;
        frame_timing.sum_ms = 0.0;
        frame_timing.sample_count = 0U;
        return;
    }
    if (now < frame_timing.settle_until_ns) return;
    const auto frame_ms = 1000.0 / static_cast<double>(frames_per_second);
    if (frame_timing.window_start_ns == 0) {
        frame_timing.window_start_ns = now;
    }
    frame_timing.sum_ms += frame_ms;
    ++frame_timing.sample_count;
    if (now - frame_timing.window_start_ns < publish_ns) return;

    const auto average = static_cast<float>(
        frame_timing.sum_ms / static_cast<double>(frame_timing.sample_count)
    );
    std::uint32_t bits{};
    std::memcpy(&bits, &average, sizeof(bits));
    auto& destination = foveated_enabled
        ? for_api(DiagnosticApi::d3d11).foveated_frame_ms_bits
        : for_api(DiagnosticApi::d3d11).native_frame_ms_bits;
    destination.store(bits, std::memory_order_release);
    frame_timing.window_start_ns = now;
    frame_timing.sum_ms = 0.0;
    frame_timing.sample_count = 0U;
}

bool diagnostic_should_sample_gpu_time(
    const DiagnosticGpuTiming timing
) noexcept {
    const auto now = steady_now_ns();
    auto& last = timing_accumulators[timing_index(timing)].last_sample_ns;
    auto expected = last.load(std::memory_order_acquire);
    const auto interval_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            timing_sample_interval
        ).count();
    while (expected == 0 || now - expected >= interval_ns) {
        if (last.compare_exchange_weak(
                expected, now,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void diagnostic_note_d3d11_execution_path(
    const D3D11ExecutionPath path
) noexcept {
    for_api(DiagnosticApi::d3d11).d3d11_execution_path.store(
        path, std::memory_order_release
    );
}

void diagnostic_note_d3d11_transport_status(
    const D3D11TransportStatus status
) noexcept {
    for_api(DiagnosticApi::d3d11).d3d11_transport_status.store(
        status, std::memory_order_release
    );
}

DiagnosticSnapshot diagnostic_snapshot(const DiagnosticApi api) noexcept {
    const auto& data = for_api(api);
    const auto transport_gpu_ms = load_gpu_time(data.transport_gpu_ms_bits);
    const auto foveated_dlss_gpu_ms = load_gpu_time(
        data.foveated_dlss_gpu_ms_bits
    );
    const auto full_dlss_nr_gpu_ms = load_gpu_time(
        data.full_dlss_nr_gpu_ms_bits
    );
    const auto foveated_dlss_nr_gpu_ms = load_gpu_time(
        data.foveated_dlss_nr_gpu_ms_bits
    );
    const auto native_dlss_gpu_ms = load_gpu_time(
        data.native_dlss_gpu_ms_bits
    );
    const auto foveated_frame_ms = load_gpu_time(
        data.foveated_frame_ms_bits
    );
    const auto native_frame_ms = load_gpu_time(data.native_frame_ms_bits);
    return {
        data.runtime_loaded.load(std::memory_order_acquire),
        streamline_detected.load(std::memory_order_acquire),
        data.hook_discovered.load(std::memory_order_acquire),
        data.direct_detour_installed.load(std::memory_order_acquire),
        data.create_calls.load(std::memory_order_relaxed),
        data.evaluate_calls.load(std::memory_order_relaxed),
        data.active_calls.load(std::memory_order_relaxed),
        data.received_input_width.load(std::memory_order_acquire),
        data.received_input_height.load(std::memory_order_acquire),
        data.received_output_width.load(std::memory_order_acquire),
        data.received_output_height.load(std::memory_order_acquire),
        {
            data.crop_input_base_x.load(std::memory_order_acquire),
            data.crop_input_base_y.load(std::memory_order_acquire),
            data.crop_input_width.load(std::memory_order_acquire),
            data.crop_input_height.load(std::memory_order_acquire),
            data.crop_output_base_x.load(std::memory_order_acquire),
            data.crop_output_base_y.load(std::memory_order_acquire),
            data.crop_output_width.load(std::memory_order_acquire),
            data.crop_output_height.load(std::memory_order_acquire),
        },
        transport_gpu_ms,
        foveated_dlss_gpu_ms,
        full_dlss_nr_gpu_ms,
        foveated_dlss_nr_gpu_ms,
        native_dlss_gpu_ms,
        foveated_frame_ms,
        native_frame_ms,
        data.has_private_result.load(std::memory_order_acquire),
        data.last_private_result.load(std::memory_order_acquire),
        data.last_result.load(std::memory_order_acquire),
        data.state.load(std::memory_order_acquire),
        data.d3d11_execution_path.load(std::memory_order_acquire),
        data.d3d11_transport_status.load(std::memory_order_acquire),
    };
}

const char* diagnostic_state_name(const DiagnosticState state) noexcept {
    switch (state) {
    case DiagnosticState::waiting: return "Waiting for a DLSS call";
    case DiagnosticState::disabled: return "Disabled in the add-on";
    case DiagnosticState::invalid_arguments: return "Missing command context or NGX parameters";
    case DiagnosticState::missing_resources: return "Missing DLSS color/output resources";
    case DiagnosticState::unsupported_resources: return "Unsupported texture or resource flags";
    case DiagnosticState::incompatible_contract: return "Invalid dimensions or unsupported DLSS contract";
    case DiagnosticState::resource_initialization_failed: return "Composite resource initialization failed";
    case DiagnosticState::allocation_failed: return "Evaluation state allocation failed";
    case DiagnosticState::prepare_rejected: return "Foveated preparation was rejected";
    case DiagnosticState::streamline_direct_path_suppressed: return "Direct NGX path suppressed because Streamline is active";
    case DiagnosticState::active: return "Active";
    case DiagnosticState::ngx_evaluation_failed: return "NGX evaluation returned failure";
    }
    return "Unknown";
}

const char* d3d11_execution_path_name(
    const D3D11ExecutionPath path
) noexcept {
    switch (path) {
    case D3D11ExecutionPath::waiting: return "Waiting for a DX11 DLSS call";
    case D3D11ExecutionPath::dx12_transport: return "DX12 Transport";
    case D3D11ExecutionPath::dx11_direct: return "DX11 Direct";
    case D3D11ExecutionPath::game_fallback:
        return "Game's original DX11 DLSS (foveated path failed)";
    case D3D11ExecutionPath::disabled_passthrough:
        return "Game's original DX11 DLSS (add-on disabled)";
    }
    return "Unknown";
}

const char* d3d11_transport_status_name(
    const D3D11TransportStatus status
) noexcept {
    switch (status) {
    case D3D11TransportStatus::not_attempted: return "Not attempted";
    case D3D11TransportStatus::active: return "Active";
    case D3D11TransportStatus::missing_initialization_data:
        return "D3D11 NGX initialized before its settings were captured";
    case D3D11TransportStatus::missing_callbacks:
        return "Missing required DX12 NGX callbacks";
    case D3D11TransportStatus::missing_resources:
        return "Game did not provide all required DLSS resources";
    case D3D11TransportStatus::unsupported_resources:
        return "Unsupported DX11 resource type or sample count";
    case D3D11TransportStatus::invalid_dimensions:
        return "Invalid crop dimensions or resource bounds";
    case D3D11TransportStatus::unsupported_motion_vectors:
        return "Motion-vector crop is outside the supplied texture";
    case D3D11TransportStatus::device_initialization_failed:
        return "Could not initialize the shared DX11/DX12 device";
    case D3D11TransportStatus::unsupported_context:
        return "DX11 context does not support shared fences";
    case D3D11TransportStatus::transport_slot_busy:
        return "All transport work for this slot is still in flight";
    case D3D11TransportStatus::resource_initialization_failed:
        return "Could not create shared transport resources";
    case D3D11TransportStatus::depth_conversion_failed:
        return "Could not convert the DX11 depth crop";
    case D3D11TransportStatus::synchronization_failed:
        return "DX11/DX12 synchronization or command submission failed";
    case D3D11TransportStatus::ngx_evaluation_failed:
        return "DX12 NGX evaluation failed";
    case D3D11TransportStatus::compositing_failed:
        return "DX11 compositing of the transported crop failed";
    }
    return "Unknown";
}

}  // namespace cheeky::foveated_dlss
