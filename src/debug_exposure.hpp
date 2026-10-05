#pragma once
#include <d3d12.h>
#include <cmath>
namespace cheeky::foveated_dlss {
struct DebugExposure {
    ID3D12Resource* texture{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    float pre{1}, scale{1};
};
inline thread_local DebugExposure debug_exposure;
// NR's codec also needs the game's exposure on the native lower-hook route,
// where debug rendering keeps its original brightness.
inline thread_local DebugExposure nr_exposure;
struct DebugExposureScope {
    DebugExposure& target;
    DebugExposure previous;
    explicit DebugExposureScope(DebugExposure value, DebugExposure& slot = debug_exposure)
        : target(slot), previous(slot) { target = value; }
    ~DebugExposureScope() { target = previous; }
    DebugExposureScope(const DebugExposureScope&) = delete;
    DebugExposureScope& operator=(const DebugExposureScope&) = delete;
};
inline bool debug_exposure_supported(const DebugExposure& e) {
    if (!e.texture || !std::isfinite(e.pre) || !std::isfinite(e.scale) || e.pre <= 0 || e.scale <= 0 ||
        !std::isfinite(e.pre / e.scale)) return false;
    const auto d=e.texture->GetDesc();
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width==1 && d.Height==1 &&
        d.DepthOrArraySize==1 && d.SampleDesc.Count==1 && !(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
        (d.Format==DXGI_FORMAT_R32_FLOAT || d.Format==DXGI_FORMAT_R32G32_FLOAT ||
         d.Format==DXGI_FORMAT_R16_FLOAT || d.Format==DXGI_FORMAT_R16G16_FLOAT ||
         d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT || d.Format==DXGI_FORMAT_R32G32B32A32_FLOAT);
}
}
