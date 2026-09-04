#pragma once

#include <d3d12.h>

#include <cstdint>

namespace cheeky::foveated_dlss {

struct D3D12OutputPlan {
    bool compatible{};
    D3D12_RESOURCE_DESC private_description{};
};

[[nodiscard]] D3D12OutputPlan plan_d3d12_output(
    const D3D12_RESOURCE_DESC& game_output,
    std::uint32_t private_width,
    std::uint32_t private_height
) noexcept;

}  // namespace cheeky::foveated_dlss
