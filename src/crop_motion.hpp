#pragma once

#include "foveation.hpp"
#include <d3d11.h>
#include <d3d12.h>
#include <cmath>
#include <memory>

namespace cheeky::foveated_dlss {

struct CropMotionOffset { float x{}, y{}; };

// NGX vectors point from current to previous coordinates. In crop space:
// previousLocal = currentLocal + sceneMotion + currentOrigin - previousOrigin.
// MV.Scale converts stored vectors to pixels in the selected MV resolution.
inline bool crop_motion_offset(const FoveationGeometry& previous,
    const FoveationGeometry& current, bool low_res, float scale_x,
    float scale_y, CropMotionOffset& offset) noexcept {
    offset = {};
    if (!std::isfinite(scale_x) || !std::isfinite(scale_y) ||
        scale_x == 0.0F || scale_y == 0.0F) return false;
    const auto dx = static_cast<double>(low_res ? current.input_base_x : current.output_base_x) -
        static_cast<double>(low_res ? previous.input_base_x : previous.output_base_x);
    const auto dy = static_cast<double>(low_res ? current.input_base_y : current.output_base_y) -
        static_cast<double>(low_res ? previous.input_base_y : previous.output_base_y);
    offset = {static_cast<float>(dx / scale_x), static_cast<float>(dy / scale_y)};
    return std::isfinite(offset.x) && std::isfinite(offset.y);
}

// Each pass owns private vectors; the game's vectors are never modified.
struct CropMotion11;
std::shared_ptr<CropMotion11> create_crop_motion11(ID3D11DeviceContext*,
    ID3D11Resource*, unsigned x, unsigned y, unsigned width, unsigned height,
    CropMotionOffset) noexcept;
ID3D11Resource* crop_motion_resource(const std::shared_ptr<CropMotion11>&) noexcept;
void release_crop_motion11() noexcept;

// D3D12 passes are retained until the command list's submission fence completes.
ID3D12Resource* prepare_crop_motion12(ID3D12GraphicsCommandList*,
    ID3D12Resource*, unsigned x, unsigned y, unsigned width, unsigned height,
    CropMotionOffset, D3D12_RESOURCE_STATES source_state =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) noexcept;
void crop_motion12_submitted(ID3D12CommandQueue*, unsigned, ID3D12CommandList* const*) noexcept;
void collect_crop_motion12() noexcept;
void release_crop_motion12() noexcept;
}
