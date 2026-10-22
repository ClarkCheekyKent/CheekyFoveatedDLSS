#pragma once
#include <d3d12.h>
#include <cstdint>
namespace cheeky::foveated_dlss {
bool quad_view_stamp12(ID3D12GraphicsCommandList*,ID3D12Resource*,uint64_t view,
    unsigned x,unsigned y,unsigned width,unsigned height,
    D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS) noexcept;
}
