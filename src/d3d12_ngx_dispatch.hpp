#pragma once

#include "ngx_abi.hpp"

#include <cstdint>

namespace cheeky::foveated_dlss {

enum class D3D12NgxRoute : std::uint32_t {
    unknown,
    public_runtime,
    core_runtime,
};

// Experimental AFW routing: the core observes the original game evaluation;
// only a feature-runtime evaluation nested inside it may own reconstruction.
struct AfwCompatibilityStatus {
    bool enabled{};
    std::uint64_t core_calls{}, lower_calls{}, missing_lower_calls{};
    std::uint64_t standalone_lower_calls{}, rejected_core_reentry{};
    unsigned runtime_candidates{};
    bool runtime_selected{};
    bool warp_observer_ready{};
    std::uint64_t warp_calls{};
    std::uint64_t last_warp_age_ms{UINT64_MAX};
};
void enable_afw_compatibility() noexcept;
[[nodiscard]] bool afw_compatibility_enabled() noexcept;
[[nodiscard]] AfwCompatibilityStatus afw_compatibility_status() noexcept;
void afw_note_runtime_discovery(unsigned candidates, bool selected) noexcept;
void afw_note_warp_observer(bool ready) noexcept;
void afw_note_warp_call() noexcept;
// Called only by an outermost public/_C evaluation, never private recursion.
[[nodiscard]] bool afw_claim_lower_evaluation() noexcept;
[[nodiscard]] bool afw_reject_core_reentry() noexcept;

// Separate private reconstruction from full-frame passthrough and lifecycle
// hooks: an ordinary public call is allowed to forward to a core runtime.
class AfwPrivateWorkScope final {
public:
    AfwPrivateWorkScope() noexcept;
    ~AfwPrivateWorkScope();
    AfwPrivateWorkScope(const AfwPrivateWorkScope&) = delete;
    AfwPrivateWorkScope& operator=(const AfwPrivateWorkScope&) = delete;
};

using D3D12NgxEvaluateFn = NgxResult (*)(
    ID3D12GraphicsCommandList*,
    const NgxHandle*,
    const NgxParameters*,
    NgxProgressCallback
);

struct D3D12NgxEvaluationCall {
    D3D12NgxRoute route{D3D12NgxRoute::unknown};
    ID3D12GraphicsCommandList* command_list{};
    const NgxHandle* handle{};
    const NgxParameters* parameters{};
    NgxProgressCallback callback{};
};

using D3D12NgxEvaluationProcessorFn = NgxResult (*)(
    const D3D12NgxEvaluationCall&,
    D3D12NgxEvaluateFn,
    void*
);

class D3D12NgxInterceptionScope final {
public:
    D3D12NgxInterceptionScope() noexcept;
    ~D3D12NgxInterceptionScope();

    D3D12NgxInterceptionScope(const D3D12NgxInterceptionScope&) = delete;
    D3D12NgxInterceptionScope& operator=(
        const D3D12NgxInterceptionScope&
    ) = delete;

    [[nodiscard]] bool outermost() const noexcept;

private:
    bool outermost_{};
};

[[nodiscard]] NgxResult dispatch_d3d12_ngx_evaluation(
    const D3D12NgxEvaluationCall& call,
    D3D12NgxEvaluateFn original,
    D3D12NgxEvaluationProcessorFn processor,
    void* context = nullptr
) noexcept;

[[nodiscard]] bool d3d12_ngx_interception_active() noexcept;
[[nodiscard]] const char* d3d12_ngx_route_name(D3D12NgxRoute route) noexcept;

}  // namespace cheeky::foveated_dlss
