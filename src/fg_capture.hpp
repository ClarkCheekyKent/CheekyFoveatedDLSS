#pragma once
struct ID3D11DeviceContext;
#include "../shared/cheeky_fg_bridge.hpp"
namespace cheeky::foveated_dlss {
CheekyFgStatus fg_status() noexcept;
CheekyFgDiagnostic fg_diagnostic() noexcept;
bool fg_performance(CheekyFgPerformance&) noexcept;
bool fg_batch_timings(CheekyFgBatchTimingsV1&) noexcept;
bool fg_cadence(CheekyFgCadence&) noexcept;
bool fg_auto_status(CheekyFgAutoStatus&) noexcept;
bool fg_pacing_timeline(CheekyFgPacingTimeline&) noexcept;
bool fg_native_submission_latency(CheekyFgNativeSubmissionLatency&) noexcept;
bool fg_overhead(CheekyFgOverhead&) noexcept;
bool fg_presentation(CheekyFgPresentation&) noexcept;
bool fg_experiment_diagnostics(CheekyFgExperimentDiagnostics&) noexcept;
bool fg_direct_output_diagnostics(CheekyFgDirectOutput&) noexcept;
bool fg_direct_output_details(CheekyFgDirectOutputDetails&) noexcept;
bool fg_memory_diagnostics(CheekyFgMemory&) noexcept;
const char* fg_state_name(CheekyFgState state) noexcept;
}
#include "ngx_abi.hpp"
#include <d3d12.h>
namespace cheeky::foveated_dlss {
void fg_publish_ngx(ID3D12GraphicsCommandList*,const NgxHandle*,const NgxParameters*,NgxResult) noexcept;
void fg_publish_shader11(ID3D11DeviceContext*,const NgxHandle*,const NgxParameters*) noexcept;
void fg_publish_quad11(ID3D11DeviceContext*,const NgxHandle*,const NgxParameters*) noexcept;
void fg_remember_feature(const NgxHandle*,const NgxParameters*) noexcept;
// Only feature=1 (Super Resolution) can provide stereo FG guides.
bool fg_is_registered_sr_feature(const NgxHandle*) noexcept;
void fg_forget_feature(const NgxHandle*) noexcept;
void fg_set_uevr_queue(ID3D12CommandQueue*) noexcept;
// Published by the existing UEVR plugin rendering-method observer; never
// derived from an FG frame number, NGX eval order or a private vtable hook.
void fg_uevr_mode_attached() noexcept;
void fg_uevr_mode_published(std::uint32_t mode) noexcept;
void fg_uevr_mode_detached() noexcept;
void fg_note_submission(ID3D12CommandQueue*,ID3D12GraphicsCommandList*) noexcept;
void fg_retire_snapshots() noexcept;
void fg_note_reset(ID3D12GraphicsCommandList*) noexcept;
bool fg_recording_tagged(ID3D12GraphicsCommandList*) noexcept;
bool fg_has_pending_submissions() noexcept;
}
