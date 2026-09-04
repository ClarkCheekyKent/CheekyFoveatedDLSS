#pragma once

#include "ngx_abi.hpp"
#include "settings.hpp"

#include <d3d11.h>

#include <cstdint>

namespace cheeky::foveated_dlss {

using D3D11PeripheralCreateFeatureFn = NgxResult (*)(
    ID3D11DeviceContext*,
    std::uint32_t,
    NgxParameters*,
    NgxHandle**
);
using D3D11PeripheralEvaluateFeatureFn = NgxResult (*)(
    ID3D11DeviceContext*,
    const NgxHandle*,
    const NgxParameters*,
    NgxProgressCallback
);
using D3D11PeripheralReleaseFeatureFn = NgxResult (*)(NgxHandle*);

struct D3D11PeripheralDlaaResult {
    ID3D11ShaderResourceView* output_srv{};
    std::uint32_t working_width{};
    std::uint32_t working_height{};
};

[[nodiscard]] bool evaluate_d3d11_peripheral_dlaa(
    ID3D11DeviceContext* context,
    const NgxHandle* game_handle,
    const NgxParameters* parameters,
    const Settings& settings,
    D3D11PeripheralCreateFeatureFn create_feature,
    D3D11PeripheralEvaluateFeatureFn evaluate_feature,
    D3D11PeripheralReleaseFeatureFn release_feature,
    D3D11PeripheralDlaaResult& output
) noexcept;

void release_d3d11_peripheral_dlaa_result(
    D3D11PeripheralDlaaResult& result
) noexcept;

void release_d3d11_peripheral_dlaa_view(
    const NgxHandle* game_handle
) noexcept;

[[nodiscard]] float d3d11_peripheral_dlaa_preparation_gpu_ms() noexcept;
[[nodiscard]] float d3d11_peripheral_dlaa_total_gpu_ms() noexcept;

void release_d3d11_peripheral_dlaa_resources() noexcept;

}  // namespace cheeky::foveated_dlss
