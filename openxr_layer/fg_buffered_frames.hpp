#pragma once
#include "fg_transport.hpp"
#include "fg_frame_backend.hpp"

namespace cheeky::fg {
// Owned frame records. Pacing uses their identities, poses, timestamps and
// policy. Images and guide payloads are opaque here: only the backend that
// produced them dereferences graphics resources or GPU completion tokens.
struct BufferedAuxiliary {XrSwapchain source{};Image image;};
struct BufferedNative {
    unsigned multiplier{2};
    bool automatic_multiplier{};
    std::uint64_t id{},ready{},epoch{};
    unsigned direct_output_mode{};bool latency_diagnostics{};
    TimingClock::time_point captured_at{},handed_off_at{};
    std::array<Image,2> color;
    std::array<ColorReady,2> color_ready;
    ColorReady final_color_ready; // GPU end of the combined native-eye crop/assembly batch.
    std::array<XrCompositionLayerProjectionView,2> views;
    XrSpace space{};XrEnvironmentBlendMode blend{};
    FrameLayers layers;
    std::vector<BufferedAuxiliary> auxiliary;
    FramePolicy policy;
    std::shared_ptr<FrameGuides> guides; // Backend payload; set only while guides_valid.
    void clear_guides() noexcept {guides.reset();}
    bool guides_valid{};
    CheekyFgState guide_state{CheekyFgState::waiting_for_guides};
    unsigned source_depth{},source_motion{};
    std::uint64_t guide_token{};
    std::uint64_t source_configuration{}; // Survives release of guide textures.
    std::uint64_t render_frame{};
};
struct BufferedPair {
    struct Phase {
        std::array<Image,2> images;
        std::shared_ptr<FrameCompletion> completion;
        TimingClock::time_point submitted_at{};
    };
    struct Progress {
        // Every access is protected by BufferedPresenter::mx. A published
        // phase's images and fence are immutable and retain their own leases.
        std::array<Phase,3> phases;
        bool complete{},failed{};
    };
    std::shared_ptr<Progress> progress;
    // First output retains its existing name for the direct-output 2x path.
    std::array<std::array<Image,2>,2> extra_generated;
    unsigned generated_count{1},generated_index{};
    float fraction() const noexcept {return float(generated_index+1)/float(generated_count+1);}
    std::uint64_t native_id{},current_id{},ready{};
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
}
