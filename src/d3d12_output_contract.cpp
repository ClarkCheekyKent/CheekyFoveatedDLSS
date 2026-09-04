#include "d3d12_output_contract.hpp"

namespace cheeky::foveated_dlss {

D3D12OutputPlan plan_d3d12_output(
    const D3D12_RESOURCE_DESC& game_output,
    const std::uint32_t private_width,
    const std::uint32_t private_height
) noexcept {
    D3D12OutputPlan plan{};
    plan.compatible = private_width != 0U && private_height != 0U &&
        game_output.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        game_output.MipLevels != 0U &&
        game_output.DepthOrArraySize == 1U &&
        game_output.SampleDesc.Count == 1U &&
        game_output.Format != DXGI_FORMAT_UNKNOWN &&
        (game_output.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0U;
    if (!plan.compatible) return plan;

    plan.private_description = game_output;
    plan.private_description.Width = private_width;
    plan.private_description.Height = private_height;
    plan.private_description.MipLevels = 1U;
    return plan;
}

}  // namespace cheeky::foveated_dlss
