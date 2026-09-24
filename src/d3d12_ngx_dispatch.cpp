#include "d3d12_ngx_dispatch.hpp"
#include <atomic>
#include <Windows.h>
#include "afw_compatibility.hpp"
#include "afw_eye_identity.hpp"
#include <mutex>

namespace cheeky::foveated_dlss {
namespace {

thread_local std::uint32_t interception_depth{};
thread_local std::uint32_t afw_private_depth{};
thread_local std::uint32_t afw_protected_private_depth{};
std::atomic<bool> afw_enabled{};
std::atomic<bool> lower_hook_enabled{true};
std::atomic<std::uint64_t> core_calls{}, lower_calls{}, missing_lower_calls{};
std::atomic<std::uint64_t> standalone_lower_calls{}, rejected_core_reentry{};
std::atomic<unsigned> runtime_candidates{};
std::atomic<bool> runtime_selected{};
std::atomic<bool> warp_observer_ready{};
std::atomic<std::uint64_t> warp_calls{}, last_warp_ms{};
std::atomic<bool> warp_metadata_supported{};
std::atomic<std::uint64_t> last_warp_metadata{UINT64_MAX}, source_left_calls{}, source_right_calls{};
std::mutex afw_projection_mutex;
AfwProjectionCache afw_projection_cache;
AfwCoverageStatus afw_coverage;
std::atomic<bool> afw_projection_allowed{};
std::atomic<std::uint64_t> afw_projection_generation{};
thread_local bool* afw_core_lower_seen{};
unsigned host_rendering_mode{UINT32_MAX};
std::uint64_t host_rendering_mode_ms{};
std::mutex depth_eyes_mutex;
AfwDepthEyes depth_eyes;
std::atomic<unsigned> last_evaluation_eye{UINT32_MAX};
std::atomic<std::uint64_t> early_left_calls{}, early_right_calls{}, early_unknown_calls{};
struct CoreDepth {
    ID3D12GraphicsCommandList* list{};
    ID3D12Resource* source{};
    unsigned eye{UINT32_MAX};
    bool ambiguous{};
};
thread_local CoreDepth* current_core_depth{};

struct AfwCoreScope {
    bool seen{};
    bool* previous{afw_core_lower_seen};
    CoreDepth depth{};
    CoreDepth* previous_depth{current_core_depth};
    AfwCoreScope(const D3D12NgxEvaluationCall& call) noexcept {
        ++core_calls; afw_core_lower_seen = &seen;
        depth.list = call.command_list;
        if (call.parameters) call.parameters->Get("Depth", &depth.source);
        current_core_depth = &depth;
    }
    ~AfwCoreScope() {
        if (!seen) ++missing_lower_calls;
        afw_core_lower_seen = previous;
        current_core_depth = previous_depth;
    }
};

}  // namespace

void enable_afw_compatibility() noexcept { afw_enabled.store(true, std::memory_order_release); }
void configure_d3d12_hook_path(bool lower) noexcept { lower_hook_enabled.store(lower, std::memory_order_release); }
bool d3d12_lower_hook_enabled() noexcept { return lower_hook_enabled.load(std::memory_order_acquire); }
bool d3d12_hook_restart_required(bool requested_lower) noexcept {
    return requested_lower != lower_hook_enabled.load(std::memory_order_acquire);
}
bool protected_ngx_core_enabled() noexcept { return d3d12_lower_hook_enabled(); }
bool afw_compatibility_enabled() noexcept { return afw_enabled.load(std::memory_order_acquire); }
void publish_afw_rendering_mode(unsigned mode) noexcept {
    std::lock_guard lock(afw_projection_mutex);
    if (host_rendering_mode != mode) ++afw_projection_generation;
    host_rendering_mode = mode <= 3 ? mode : UINT32_MAX;
    host_rendering_mode_ms = GetTickCount64();
}
bool afw_coverage_enabled() noexcept {
    if (!afw_compatibility_enabled()) return false;
    std::lock_guard lock(afw_projection_mutex);
    const auto now = GetTickCount64();
    return !afw_projection_allowed.load() || now < host_rendering_mode_ms || now - host_rendering_mode_ms > 250 ||
        host_rendering_mode >= 3;
}
AfwCompatibilityStatus afw_compatibility_status() noexcept {
    const auto last_warp = last_warp_ms.load(std::memory_order_acquire);
    const auto now = GetTickCount64();
    const auto metadata = last_warp_metadata.load();
    auto result = AfwCompatibilityStatus{afw_compatibility_enabled(), core_calls.load(), lower_calls.load(),
        missing_lower_calls.load(), standalone_lower_calls.load(), rejected_core_reentry.load(),
        runtime_candidates.load(), runtime_selected.load(), warp_observer_ready.load(),
        warp_calls.load(), last_warp ? (now >= last_warp ? now - last_warp : 0U) : UINT64_MAX,
        warp_metadata_supported.load(), static_cast<unsigned>(metadata >> 32), static_cast<unsigned>(metadata),
        source_left_calls.load(), source_right_calls.load()};
    result.coverage_enabled = afw_coverage_enabled();
    result.last_evaluation_eye = last_evaluation_eye.load();
    result.early_left_calls = early_left_calls.load(); result.early_right_calls = early_right_calls.load();
    result.early_unknown_calls = early_unknown_calls.load();
    std::lock_guard lock(afw_projection_mutex);
    result.rendering_mode_known = afw_projection_allowed.load() && now >= host_rendering_mode_ms &&
        now - host_rendering_mode_ms <= 250 && host_rendering_mode <= 3;
    result.rendering_mode = result.rendering_mode_known ? host_rendering_mode : UINT32_MAX;
    return result;
}
void afw_note_warp_observer(bool ready) noexcept { warp_observer_ready.store(ready); }
void publish_afw_stereo_projection(const float (&matrices)[2][16], unsigned width, unsigned height, bool active) noexcept {
    std::lock_guard lock(afw_projection_mutex);
    const auto before = afw_projection_cache.snapshot(GetTickCount64());
    afw_projection_cache.publish(matrices, width, height, active, GetTickCount64());
    const auto after = afw_projection_cache.snapshot(GetTickCount64());
    if (before.valid != after.valid || before.output_width != after.output_width || before.output_height != after.output_height ||
            (after.valid && (!gaze_projection_matches(before.projections[0], after.projections[0]) ||
                !gaze_projection_matches(before.projections[1], after.projections[1])))) ++afw_projection_generation;
}
void allow_afw_stereo_projection(bool allowed) noexcept {
    if (allowed) {
        std::lock_guard lock(afw_projection_mutex);
        afw_projection_cache = {};
        host_rendering_mode = UINT32_MAX; host_rendering_mode_ms = 0;
        ++afw_projection_generation;
    }
    afw_projection_allowed.store(allowed, std::memory_order_release);
}
AfwStereoProjection afw_stereo_projection() noexcept {
    std::lock_guard lock(afw_projection_mutex);
    auto result = afw_projection_cache.snapshot(GetTickCount64());
    result.generation = afw_projection_generation.load();
    result.valid &= afw_projection_allowed.load(std::memory_order_acquire);
    return result;
}
void note_afw_coverage(const Settings& settings, bool automatic_applied, bool gaze_applied) noexcept {
    std::lock_guard lock(afw_projection_mutex);
    afw_coverage = {true, gaze_applied ? 3U : automatic_applied ? 2U : !settings.afw_automatic_coverage && settings.afw_manual_coverage ? 1U : 0U,
        settings.width, settings.height, settings.x_offset, settings.height_offset, settings.center_supersampling};
}
AfwCoverageStatus afw_coverage_status() noexcept {
    std::lock_guard lock(afw_projection_mutex);
    return afw_coverage;
}
void afw_note_warp_abi(bool supported) noexcept { warp_metadata_supported.store(supported); }
void afw_note_warp_call(unsigned source_eye, unsigned mode) noexcept {
    const bool valid = warp_metadata_supported.load() && source_eye < 2 && mode <= 3;
    last_warp_metadata.store(valid ? (static_cast<std::uint64_t>(source_eye) << 32) | mode : UINT64_MAX);
    if (valid && mode != 0) {
        if (source_eye == 0) ++source_left_calls; else ++source_right_calls;
    }
    ++warp_calls;
    last_warp_ms.store(GetTickCount64(), std::memory_order_release);
}
void afw_note_runtime_discovery(unsigned candidates, bool selected) noexcept {
    runtime_candidates.store(candidates, std::memory_order_relaxed);
    runtime_selected.store(selected, std::memory_order_relaxed);
}
bool afw_claim_lower_evaluation() noexcept {
    if (!protected_ngx_core_enabled()) return true;
    if (!afw_core_lower_seen) {
        ++standalone_lower_calls;
        // A loaded DLL does not require nesting while the host explicitly
        // selects Native Stereo/AFR. Unknown or stale mode stays protected.
        return !afw_coverage_enabled();
    }
    *afw_core_lower_seen = true;
    const auto eye = afw_current_source_eye();
    last_evaluation_eye.store(eye);
    if (eye == 0) ++early_left_calls; else if (eye == 1) ++early_right_calls; else ++early_unknown_calls;
    ++lower_calls;
    return true;
}
void afw_bind_depth_eye(unsigned eye, std::uint64_t resource) noexcept {
    std::lock_guard lock(depth_eyes_mutex);
    depth_eyes.record(eye, resource, GetTickCount64(), afw_projection_generation.load());
}
void afw_forget_depth_resource(std::uint64_t resource) noexcept {
    std::lock_guard lock(depth_eyes_mutex);
    depth_eyes.forget(resource);
}
void afw_observe_depth_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* source, std::uint64_t destination) noexcept {
    if (!afw_pending_depth_copy(list, source)) return;
    std::lock_guard lock(depth_eyes_mutex);
    const auto eye = depth_eyes.lookup(destination, GetTickCount64(), afw_projection_generation.load());
    if (eye > 1) return;
    auto& d = *current_core_depth;
    if (d.eye < 2 && d.eye != eye) d.ambiguous = true;
    d.eye = eye;
}
bool afw_pending_depth_copy(ID3D12GraphicsCommandList* list, ID3D12Resource* source) noexcept {
    return current_core_depth && afw_core_lower_seen && !*afw_core_lower_seen && !afw_private_depth &&
        source && current_core_depth->list == list && current_core_depth->source == source;
}
unsigned afw_current_source_eye() noexcept {
    return current_core_depth && !current_core_depth->ambiguous ? current_core_depth->eye : UINT32_MAX;
}
bool afw_reject_core_reentry() noexcept {
    if (!protected_ngx_core_enabled() || afw_protected_private_depth == 0U) return false;
    ++rejected_core_reentry;
    return true;
}

AfwPrivateWorkScope::AfwPrivateWorkScope() noexcept
    : protect_core_(d3d12_lower_hook_enabled()) {
    ++afw_private_depth;
    if (protect_core_) ++afw_protected_private_depth;
}
AfwPrivateWorkScope::~AfwPrivateWorkScope() {
    if (protect_core_) --afw_protected_private_depth;
    --afw_private_depth;
}

D3D12NgxInterceptionScope::D3D12NgxInterceptionScope() noexcept
    : outermost_(interception_depth++ == 0U) {}

D3D12NgxInterceptionScope::~D3D12NgxInterceptionScope() {
    --interception_depth;
}

bool D3D12NgxInterceptionScope::outermost() const noexcept {
    return outermost_;
}

NgxResult dispatch_d3d12_ngx_evaluation(
    const D3D12NgxEvaluationCall& call,
    const D3D12NgxEvaluateFn original,
    const D3D12NgxEvaluationProcessorFn processor,
    void* const context,
    void (*const skipped)(const D3D12NgxEvaluationCall&)
) noexcept {
    if (original == nullptr) return 0xBAD00007U;
    if (protected_ngx_core_enabled() && call.route == D3D12NgxRoute::core_runtime) {
        // Private lower-runtime features must not re-enter upstream hooks.
        if (afw_reject_core_reentry()) return 0xBAD00007U;
        AfwCoreScope core(call);
        return original(call.command_list, call.handle, call.parameters, call.callback);
    }
    // An independent public call may itself forward to the core runtime. Do
    // not mark that ordinary full-frame passthrough as private interception.
    if (!d3d12_ngx_interception_active() && !afw_claim_lower_evaluation()) {
        if (skipped) skipped(call);
        return original(call.command_list, call.handle, call.parameters, call.callback);
    }
    D3D12NgxInterceptionScope scope;
    if (scope.outermost() && processor != nullptr) {
        return processor(call, original, context);
    }
    return original(
            call.command_list,
            call.handle,
            call.parameters,
            call.callback
        );
}

bool d3d12_ngx_interception_active() noexcept {
    return interception_depth != 0U;
}

const char* d3d12_ngx_route_name(const D3D12NgxRoute route) noexcept {
    switch (route) {
        case D3D12NgxRoute::public_runtime: return "Public nvngx_dlss.dll";
        case D3D12NgxRoute::core_runtime: return "Core _nvngx.dll";
        case D3D12NgxRoute::unknown: break;
    }
    return "Waiting";
}

}  // namespace cheeky::foveated_dlss
