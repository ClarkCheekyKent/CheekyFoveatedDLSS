#pragma once
#include <openxr/openxr.h>
#include <array>
#include <cstdint>
#include <memory>
#include "fg_auto.hpp"

struct CheekyFgPair;
struct CheekyFgPerformance;
struct CheekyFgOverhead;
struct CheekyFgExperimentDiagnostics;
namespace cheeky::fg {
struct BufferedNative;
struct BufferedPair;
struct BufferedDirectRequest;
struct TransportImage;
struct FrameLayers;
using Image=std::shared_ptr<TransportImage>;

// Scheduling policy carried by a frame. The backend fills it at capture from
// whatever source supplied the guides; pacing reads only these values.
struct FramePolicy {
    std::uint32_t release_point{};  // 0 submission, 1 preparation, 2 completion, 3 generated XR, 4 submission + 4ms.
    std::uint32_t queue_priority{2}; // 1 separate HIGH (optical flow only); 2 shared game queue.
    bool optical_flow{}; // Flow is shared by all phases: use its amortized generation cost for Auto.
};
// Backend-owned per-frame guide state (textures, leases, GPU dependencies).
// Pacing only holds and drops it; each backend defines its own subtype.
struct FrameGuides {virtual ~FrameGuides()=default;};
struct FrameCompletion {
    virtual ~FrameCompletion()=default;
    virtual bool ready() const noexcept=0; // Poll only; never wait or call OpenXR.
    // True only after this phase's output commands and fence signal have been
    // submitted on this exact queue. Later work on it needs no CPU-ready poll.
    virtual bool ordered_on(const void* /*queue*/) const noexcept {return false;}
};

enum class CaptureOutcome { captured, unavailable, busy };
struct GenerationOutcome {
    bool success{},attempted{},history_reset{};
};
struct GenerationStatus {
    bool failed{},capability_ready{};
    std::uint64_t serial{};
    const char* diagnostic{};
};

// The pacing threads exchange owned frame handles and submission milestones.
// Implementations retain their graphics resources until their own GPU work has
// retired. In particular, ready() must never wait for the GPU or enter OpenXR.
// Capture runs under capture_mutex; publish runs on the presentation thread;
// generate runs on the generation thread. The host owns all runtime frame calls.
struct FrameBackend {
    virtual ~FrameBackend()=default;
    virtual bool ready(std::uint64_t ticket) const=0;
    virtual void begin_application_frame(bool) {}
    virtual ApplicationTiming application_timing() const {return {};}
    virtual unsigned maximum_multiplier(const BufferedNative&) const {return 4;}
    virtual CaptureOutcome capture(const std::shared_ptr<BufferedNative>& frame,
        const XrCompositionLayerProjection* projection,bool inference_slot,
        const CheekyFgPair* external,std::uint64_t (*complete_hookless)(unsigned))=0;
    virtual GenerationOutcome generate(const std::shared_ptr<BufferedNative>& previous,
        const std::shared_ptr<BufferedNative>& current,BufferedPair& result,
        bool& reset_history,std::uint64_t& logged_samples,
        std::shared_ptr<BufferedDirectRequest>& direct)=0;
    virtual bool publish(const BufferedPair& pair,const std::array<Image,2>& images,
        std::array<XrCompositionLayerProjectionView,2>& views,FrameLayers& layers,bool generated)=0;
    virtual void retire()=0;
    virtual void finish_generation()=0;
    virtual void retire_guides(BufferedNative& frame)=0;
    // Drops backend capture state for released images (capture_mutex held).
    // session: the session is stopping, so per-swapchain plans go too.
    virtual void discard_captures(bool session)=0;
    virtual void reset_generation()=0;
    virtual void reset_statistics(bool reset_gpu)=0;
    virtual GenerationStatus generation_status() const=0;
    virtual void snapshot(CheekyFgPerformance& performance,CheekyFgOverhead& overhead,
        CheekyFgExperimentDiagnostics& experiments) const=0;
    virtual XrResult end_frame(const XrFrameEndInfo* end)=0;
    virtual bool service_direct_output(double position,BufferedPair& result)=0;
    virtual bool supports_direct_output() const=0;
};
}
