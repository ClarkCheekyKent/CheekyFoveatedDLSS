#pragma once
#include "fg_transport.hpp"
#include "fg_backend11.hpp"

namespace cheeky::fg {
// Owned frame records. Pacing uses their identities, poses and timestamps;
// only the backends dereference graphics resources and GPU completion tokens.
struct BufferedAuxiliary {XrSwapchain source{};Image image;};
struct BufferedNative {
    std::uint64_t id{},ready{},epoch{};
    unsigned capture_mode{},upload_mode{},direct_output_mode{},direct_shading_mode{1};bool latency_diagnostics{};
    TimingClock::time_point captured_at{},handed_off_at{};
    std::array<Image,2> color;
    std::array<ColorReady,2> color_ready;
    ColorReady final_color_ready; // GPU end of the combined native-eye crop/assembly batch.
    std::array<XrCompositionLayerProjectionView,2> views;
    XrSpace space{};XrEnvironmentBlendMode blend{};
    FrameLayers layers;
    std::vector<BufferedAuxiliary> auxiliary;
    CheekyFgPair guides;
    ComPtr<IUnknown> guide_lease;
    std::array<ComPtr<ID3D12Resource>,2> guide_depth,guide_motion;
    std::array<ComPtr<ID3D12Resource>,2> peripheral_depth,peripheral_motion;
    // Only hook-free split-queue captures use this GPU-side dependency.
    // The producer queue signals it after the NGX guide command lists.
    ComPtr<ID3D12Fence> guide_sync_fence;
    std::uint64_t guide_sync_value{};
    void retain_guides(const CheekyFgPair& source) noexcept {
        guides=source;guide_lease=source.lease;
        for(unsigned eye=0;eye<2;++eye){guide_depth[eye]=source.eyes[eye].depth;guide_motion[eye]=source.eyes[eye].motion;
            peripheral_depth[eye]=source.eyes[eye].peripheral_depth;peripheral_motion[eye]=source.eyes[eye].peripheral_motion;}
        // Explicit members own the returned references. The metadata copy
        // must never be passed to the bridge's ReleasePair function.
        guides.lease=nullptr;guides.producer_queue=nullptr;
    }
    void clear_guides() noexcept {guides={};guide_depth={};guide_motion={};peripheral_depth={};peripheral_motion={};
        guide_lease.Reset();guide_sync_fence.Reset();guide_sync_value=0;guides11={};}
    bool guides_valid{};
    CheekyFgState guide_state{CheekyFgState::waiting_for_guides};
    unsigned source_depth{},source_motion{};
    std::uint64_t guide_token{};
    std::uint64_t source_configuration{}; // Survives release of guide textures.
    std::uint64_t render_frame{};
    std::array<std::shared_ptr<Backend11::Guides>,2> guides11; // Native D3D11 backend only.
};
struct BufferedPair {
    std::uint64_t native_id{},current_id{},ready{};
    unsigned upload_mode{};
    TimingClock::time_point native_captured{},current_captured{},completed_at{},handed_off_at{},worker_started_at{},submitted_at{};
    std::array<Image,2> native,generated,current;
    std::array<ColorReady,2> native_color_ready,current_color_ready;
    ColorReady native_final_ready,current_final_ready;
    std::array<XrCompositionLayerProjectionView,2> native_views,generated_views,current_views;
    FrameLayers layers;
    std::vector<BufferedAuxiliary> auxiliary;
    XrSpace space{};XrEnvironmentBlendMode blend{};
    bool interpolated{},direct_xr{};
};
struct BufferedDirectRequest {
    std::uint64_t epoch{},native_id{};
    std::array<Image,2> previous;
    std::array<Worker::OutputCopy,2> targets;
    DXGI_FORMAT format{};unsigned width{},height{};
    bool decided{},allowed{},complete{},success{};
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
