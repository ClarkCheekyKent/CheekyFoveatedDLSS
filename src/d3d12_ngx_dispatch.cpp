#include "d3d12_ngx_dispatch.hpp"
#include <atomic>

namespace cheeky::foveated_dlss {
namespace {

thread_local std::uint32_t interception_depth{};
thread_local std::uint32_t afw_private_depth{};
std::atomic<bool> afw_enabled{};
std::atomic<std::uint64_t> core_calls{}, lower_calls{}, missing_lower_calls{};
std::atomic<std::uint64_t> standalone_lower_calls{}, rejected_core_reentry{};
std::atomic<unsigned> runtime_candidates{};
std::atomic<bool> runtime_selected{};
thread_local bool* afw_core_lower_seen{};

struct AfwCoreScope {
    bool seen{};
    bool* previous{afw_core_lower_seen};
    AfwCoreScope() noexcept { ++core_calls; afw_core_lower_seen = &seen; }
    ~AfwCoreScope() {
        if (!seen) ++missing_lower_calls;
        afw_core_lower_seen = previous;
    }
};

}  // namespace

void enable_afw_compatibility() noexcept { afw_enabled.store(true, std::memory_order_release); }
bool afw_compatibility_enabled() noexcept { return afw_enabled.load(std::memory_order_acquire); }
AfwCompatibilityStatus afw_compatibility_status() noexcept {
    return {afw_compatibility_enabled(), core_calls.load(), lower_calls.load(),
        missing_lower_calls.load(), standalone_lower_calls.load(), rejected_core_reentry.load(),
        runtime_candidates.load(), runtime_selected.load()};
}
void afw_note_runtime_discovery(unsigned candidates, bool selected) noexcept {
    runtime_candidates.store(candidates, std::memory_order_relaxed);
    runtime_selected.store(selected, std::memory_order_relaxed);
}
bool afw_claim_lower_evaluation() noexcept {
    if (!afw_compatibility_enabled()) return true;
    if (!afw_core_lower_seen) { ++standalone_lower_calls; return false; }
    *afw_core_lower_seen = true;
    ++lower_calls;
    return true;
}
bool afw_reject_core_reentry() noexcept {
    if (!afw_compatibility_enabled() || afw_private_depth == 0U) return false;
    ++rejected_core_reentry;
    return true;
}

AfwPrivateWorkScope::AfwPrivateWorkScope() noexcept { ++afw_private_depth; }
AfwPrivateWorkScope::~AfwPrivateWorkScope() { --afw_private_depth; }

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
    void* const context
) noexcept {
    if (original == nullptr) return 0xBAD00007U;
    if (afw_compatibility_enabled() && call.route == D3D12NgxRoute::core_runtime) {
        // Do not send a private feature back through AFW's full-frame hook.
        // Such a runtime topology is unsuitable for this experiment.
        if (afw_reject_core_reentry()) return 0xBAD00007U;
        AfwCoreScope core;
        return original(call.command_list, call.handle, call.parameters, call.callback);
    }
    // An independent public call may itself forward to the core runtime. Do
    // not mark that ordinary full-frame passthrough as private interception.
    if (!d3d12_ngx_interception_active() && !afw_claim_lower_evaluation())
        return original(call.command_list, call.handle, call.parameters, call.callback);
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
