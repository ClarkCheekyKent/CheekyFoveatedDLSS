#pragma once

#include <d3d12.h>

#include <cstdint>

namespace cheeky::foveated_dlss {

struct D3D12OutputPlan {
    bool compatible{};
    D3D12_RESOURCE_DESC private_description{};
};

// The D3D NGX/Streamline resource contract used here has no array-slice selector.
// Composite mip zero of the first slice, never infer a slice from the eye ID.
// One-slice array views also work for ordinary single-slice Texture2D resources.
[[nodiscard]] D3D12_SHADER_RESOURCE_VIEW_DESC d3d12_composite_srv(DXGI_FORMAT format) noexcept;
[[nodiscard]] D3D12_UNORDERED_ACCESS_VIEW_DESC d3d12_composite_uav(DXGI_FORMAT format) noexcept;

[[nodiscard]] bool is_dlss_nr_output_compatible(
    const D3D12_RESOURCE_DESC& game_output
) noexcept;

[[nodiscard]] D3D12OutputPlan plan_d3d12_output(
    const D3D12_RESOURCE_DESC& game_output,
    std::uint32_t private_width,
    std::uint32_t private_height
) noexcept;

}  // namespace cheeky::foveated_dlss
