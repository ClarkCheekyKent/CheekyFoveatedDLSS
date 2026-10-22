#pragma once
#include "fg_buffered_frames.hpp"
#include <atomic>

namespace cheeky::fg {
// Graphics storage retained under its existing names for the layer's capture
// hooks and GPU regression fixtures. No scheduling decisions live here.
struct BufferedGpuResources {
    using Auxiliary=BufferedAuxiliary;
    using Native=BufferedNative;
    using Pair=BufferedPair;
    using DirectRequest=BufferedDirectRequest;
    using Chain=BufferedChain;
    struct Slice {XrRect2Di rect{};unsigned layer{};};
    struct CapturePlan {D3D12_RESOURCE_DESC desc{};std::vector<Slice> slices;std::vector<unsigned> physical_eyes;bool full_required{};};
    struct DirectCapture {std::vector<std::pair<Slice,Image>> eyes;};
    static bool same_slice(const Slice& a,const Slice& b) noexcept {
        return a.layer==b.layer&&a.rect.offset.x==b.rect.offset.x&&a.rect.offset.y==b.rect.offset.y&&
            a.rect.extent.width==b.rect.extent.width&&a.rect.extent.height==b.rect.extent.height;
    }

    std::shared_ptr<DirectRequest> direct_request; // Handshake guarded by mx; only presenter calls XR.
    CheekyFgDirectOutput direct_stats;
    CheekyFgDirectOutputDetails direct_details;
    CheekyFgDirectShading shading_stats;
    CheekyFgDirectOutputDetails logged_direct_details;
    TimingAverage direct_wait;
    std::atomic<unsigned> direct_output_mode{};
    std::atomic<unsigned> direct_shading_mode{1};
    std::atomic<std::uint64_t> direct_blocked_target{};
    static std::uint64_t direct_target_key(unsigned w,unsigned h,DXGI_FORMAT format) noexcept {
        return (std::uint64_t(w)<<32)|(std::uint64_t(h)<<8)|unsigned(format);
    }
    CheekyFgDirectOutputFn direct_output_query{};
    CheekyFgDirectShadingFn direct_shading_query{};

    std::shared_ptr<Backend11> native11;
    std::array<XrSwapchain,4> quad_sources11{}; // Raw quad planes (capture_mutex); only composed eyes are captured.
    Transport producer,output,uploads;
    // Default: dedicated HIGH-priority inference queue. Lua can also select a
    // separate NORMAL queue or share the application's original queue. The
    // OpenXR binding, guide capture and uploads always retain the original queue.
    ComPtr<ID3D12CommandQueue> generation_queue;
    ComPtr<ID3D12CommandQueue> fg_high_queue,fg_normal_queue;
    // Single monotonically increasing fence shared across hook-free frames.
    // It bridges a verified NGX execution queue to the OpenXR/FG queues
    // without depending on UEVR's private renderer/vtable layout.
    ComPtr<ID3D12Fence> guide_sync_fence;
    std::uint64_t guide_sync_serial{};
    bool guide_sync_reported{};
    std::unordered_map<XrSwapchain,Image> captured;
    std::unordered_map<XrSwapchain,DirectCapture> direct_captures;
    std::unordered_map<XrSwapchain,CapturePlan> capture_plans;
    std::unordered_map<XrSwapchain,D3D12_RESOURCE_DESC> captured_descs;
    std::mutex composed11_mutex; // Leaf lock.
    std::unordered_map<XrSwapchain,std::pair<Image,TimingClock::time_point>> composed11;
};
}
