#pragma once
#include "../shared/cheeky_fg_bridge.hpp"
#include "fg_matrices.hpp"
#include "fg_focus.hpp"
#include "fg_timing.hpp"
#include "fg_latency.hpp"
#include "fg_ngx_fallback.hpp"
#include <Windows.h>
#include <d3d12.h>
#include "../third_party/ngx-sdk/include/nvsdk_ngx.h"
#include "../third_party/ngx-sdk/include/nvsdk_ngx_defs_dlssg.h"
#include <wrl/client.h>
#include <array>

namespace cheeky::fg {
using Microsoft::WRL::ComPtr;
using NgxParameters=NVSDK_NGX_Parameter;
struct Worker {
    struct Feature {NVSDK_NGX_Handle* handle{}; NgxParameters* params{};ComPtr<ID3D12Resource> output,disable,readback;
        ID3D12Resource* target{};unsigned width{},height{},eye{},offset_x{},offset_y{};bool direct{},direct_rejected{},xr_direct_rejected{};
        Matrix view_to_clip,clip_to_camera,clip_to_prev,prev_clip_to_clip,lens;
        FocusRect evaluated_crop{}; // Coordinates of the most recent submitted NGX evaluation.
        std::uint64_t evaluated_frame{};
        bool evaluated_crop_valid{},borrowed_handle{};
    };
    struct OutputCopy {ID3D12Resource* texture{};D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};CompactLayout layout;bool final_xr{};};
    using Init=decltype(&NVSDK_NGX_D3D12_Init_with_ProjectID);
    using Caps=decltype(&NVSDK_NGX_D3D12_GetCapabilityParameters);
    Init init{}; Caps capabilities{};
    using Allocate=decltype(&NVSDK_NGX_D3D12_AllocateParameters);
    using Destroy=decltype(&NVSDK_NGX_D3D12_DestroyParameters);
    using Create=decltype(&NVSDK_NGX_D3D12_CreateFeature);
    using Evaluate=decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
    using Release=decltype(&NVSDK_NGX_D3D12_ReleaseFeature);
    Allocate allocate{};Destroy destroy{};Create create{};Evaluate evaluate{};Release release{};
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator,second_allocator,optical_allocator;
    ComPtr<ID3D12GraphicsCommandList> list,second_list,optical_list;
    // Lists for the 2nd/3rd generated frame of a queued interval.
    std::array<ComPtr<ID3D12CommandAllocator>,2> batch_allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>,2> batch_lists;
    // Preparation-gated phases each retain their own second command list too.
    std::array<ComPtr<ID3D12CommandAllocator>,2> batch_second_allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>,2> batch_second_lists;
    // A first phase may split at each of four independent optical consumers.
    // Every allocator remains leased until the native-pair completion fence.
    std::array<ComPtr<ID3D12CommandAllocator>,4> optical_consumer_allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>,4> optical_consumer_lists;
    bool optical_pending[2][2]{};
    ComPtr<ID3D12Fence> fence;HANDLE event{};std::uint64_t serial{},prepared_serial{};
    std::array<Feature,2> features{},peripheral_features{};
    FocusPipeline focus;
    struct NgxPhase {
        // Each index uses the SAME temporal NGX handles, with private parameter
        // storage, matrices, preparation descriptors and output images.
        // The suppression flag belongs to the native pair and is shared.
        std::array<Feature,2> features{},peripheral_features{};
        FocusPipeline focus;
    };
    std::array<NgxPhase,2> ngx_phases;
    NgxFallback ngx_fallback;
    std::array<CompactLayout,2> color_layouts,previous_layouts;
    D3D12_RESOURCE_STATES input_color_state{D3D12_RESOURCE_STATE_COMMON};
    bool temporal_evaluation_completed{}; // Last generate() finished processing both eyes, even if output was suppressed.
    std::uint64_t crop_diagnostic_frame{};
    bool force_fp16{}; // Explicit A/B probe only; preserve SDR precision by default.
    std::uint64_t configuration{};
    unsigned center_width{},center_height{},peripheral_width{},peripheral_height{},peripheral_method{};
    unsigned source_depth_format{},source_motion_format{};
    bool direct_publication{};
    bool direct_periphery{true}; // Test A/B override; custom compact output needs no periphery intermediate.
    void reset_features() noexcept;
    GpuTimings gpu_timings;
    std::array<GpuTimings,3> phase_timings;
    BatchGpuTimings batch_timings;
    void initialize_timings() noexcept {
        gpu_timings.initialize(device.Get(),queue.Get());
        for(auto& t:phase_timings)t.initialize(device.Get(),queue.Get());
        batch_timings.reset();
    }
    LatencyProbe latency;
    TimingAverage completion_wait;
    TimingClock::time_point submitted_at{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};unsigned width{},height{};
    bool configured{},ngx_required{true};
    ComPtr<ID3D12Device> probed_device;
    bool probe_attempted{}, capability_ready{}, failed{};
    unsigned max_generated_frames{1};
    char diagnostic[384]{"NGX capability has not been checked"};
    void (*trace)(const char*) noexcept {};
    bool probe(ID3D12Device*,const wchar_t* model_directory=nullptr) noexcept;
    void report(const char* stage,std::uint32_t code) noexcept;
    ~Worker(){reset();}
    bool wait_idle(const char* site="idle") noexcept;
    bool wait_fence(std::uint64_t value,const char* site="idle") noexcept;
    std::uint64_t long_cpu_waits{}; // CPU fence waits over 5 ms, logged with their call site.
    void reset() noexcept;
    bool initialize(ID3D12Device*,ID3D12CommandQueue*,unsigned,unsigned,DXGI_FORMAT,bool require_ngx=true) noexcept;
    static bool record_output_copies(ID3D12GraphicsCommandList*,const std::array<ID3D12Resource*,2>&,
                                    const std::array<OutputCopy,2>&) noexcept;
    static bool can_batch_ngx(const CheekyFgPair&) noexcept;
    // One indexed output of a real-frame interval. Histories advance on its last output.
    bool generate(const CheekyFgPair&,const std::array<ID3D12Resource*,2>& color,
                  const std::array<ID3D12Resource*,2>& previous,
                  const std::array<Camera,2>& camera,
                  std::array<ID3D12Resource*,2>& output,
                  std::array<OutputCopy,2>* copies=nullptr,
                  void(*on_submitted)(void*) noexcept=nullptr,void* submitted_context=nullptr,
                  void(*on_prepared)(void*) noexcept=nullptr,
                  unsigned frame_count=1,unsigned frame_index=1,
                  void(*on_output_submitted)(void*) noexcept=nullptr,void* output_context=nullptr) noexcept;
};
}
