#include "d3d12_ngx_dispatch.hpp"

namespace cheeky::foveated_dlss {
namespace {

thread_local std::uint32_t interception_depth{};

}  // namespace

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
