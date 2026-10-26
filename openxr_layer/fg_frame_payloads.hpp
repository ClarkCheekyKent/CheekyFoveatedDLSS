#pragma once
#include "fg_buffered_frames.hpp"
#include "fg_backend11.hpp"

namespace cheeky::fg {
// Backend-specific frame payloads. Only the backend that created a payload
// (and its GPU regression fixtures) look inside it.

// D3D12: the plugin's immutable guide pair, retained through inference.
struct FrameGuides12 final : FrameGuides {
    CheekyFgPair pair;
    ComPtr<IUnknown> lease;
    std::array<ComPtr<ID3D12Resource>,2> depth,motion,peripheral_depth,peripheral_motion;
    // Only hook-free split-queue captures use this GPU-side dependency.
    // The producer queue signals it after the NGX guide command lists.
    ComPtr<ID3D12Fence> sync_fence;
    std::uint64_t sync_value{};
    void retain(const CheekyFgPair& source) noexcept {
        pair=source;lease=source.lease;
        for(unsigned eye=0;eye<2;++eye){depth[eye]=source.eyes[eye].depth;motion[eye]=source.eyes[eye].motion;
            peripheral_depth[eye]=source.eyes[eye].peripheral_depth;peripheral_motion[eye]=source.eyes[eye].peripheral_motion;}
        // Explicit members own the returned references. The metadata copy
        // must never be passed to the bridge's ReleasePair function.
        pair.lease=nullptr;pair.producer_queue=nullptr;
    }
    static FramePolicy policy_of(const CheekyFgPair& source) noexcept {
        const auto& g=source.eyes[0];
        return {source.release_point,source.fg_queue_priority,cheeky_fg_uses_flow(g.center_method,g.peripheral_method,g.focus_width<.999f||g.focus_height<.999f)};
    }
};
inline FrameGuides12* guides12(const BufferedNative& frame) noexcept {return dynamic_cast<FrameGuides12*>(frame.guides.get());}

struct FrameCompletion12 final : FrameCompletion {
    ComPtr<ID3D12Fence> fence;
    std::uint64_t value{};
    FrameCompletion12(ID3D12Fence* f,std::uint64_t v):fence(f),value(v){}
    bool ready() const noexcept override {return fence&&fence->GetCompletedValue()>=value;}
};

// Native D3D11: per-eye snapshots from Backend11 (interpolation also samples
// the previous interval's depth, so these outlive generation).
// The shared immediate context orders generation and publication; this means
// safe to enqueue the consumer, not that the GPU has completed the image.
struct FrameCompletion11 final : FrameCompletion {
    bool ready() const noexcept override {return true;}
};
struct FrameGuides11 final : FrameGuides {
    std::array<std::shared_ptr<Backend11::Guides>,2> eyes;
};
inline FrameGuides11* guides11(const BufferedNative& frame) noexcept {return dynamic_cast<FrameGuides11*>(frame.guides.get());}

struct BufferedDirectRequest {
    std::uint64_t epoch{},native_id{};
    std::array<Image,2> previous;
    std::array<Worker::OutputCopy,2> targets;
    DXGI_FORMAT format{};unsigned width{},height{};
    bool decided{},allowed{},complete{},success{},queued{};
    unsigned deferred_ticks{};
    ComPtr<ID3D12Fence> fence;std::uint64_t serial{};
    BufferedPair result;
};
struct BufferedChain {
    XrSwapchain handle{};std::vector<XrSwapchainImageD3D12KHR> images;D3D12_RESOURCE_DESC desc{};
    std::uint64_t uploaded{}; // Content identity; GPU contexts retain sources only until their upload fence.
    unsigned last_uploaded_index{UINT32_MAX}; // Last image released to OpenXR for this chain.
    bool direct_writable{},linear_color{};
    std::vector<XrSwapchainImageD3D11KHR> images11; // Native D3D11 backend; desc then holds width/height/format.
};
}
