#pragma once

#include "ngx_abi.hpp"

#include <cstdint>

namespace cheeky::foveated_dlss {

enum class D3D12NgxRoute : std::uint32_t {
    unknown,
    public_runtime,
    core_runtime,
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
