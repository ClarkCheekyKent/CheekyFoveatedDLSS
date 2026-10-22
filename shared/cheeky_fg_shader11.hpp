#pragma once
#include "cheeky_fg_bridge.hpp"
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
// Optional native D3D11 ABI. Resources are borrowed for the duration of observe().
struct CheekyFgShaderOptionsV1 {
    unsigned abi{1},size{sizeof(CheekyFgShaderOptionsV1)},enabled{},center_method{},peripheral_method{1},foveated{1};
};
using CheekyFgShaderOptionsFn=bool(__cdecl*)(CheekyFgShaderOptionsV1*);
struct CheekyFgShaderSource11V1 {
    unsigned abi{1},size{sizeof(CheekyFgShaderSource11V1)},eye{},vertical_flip{};
    std::uint64_t session_generation{},mapping_revision{},generation{};
    ID3D11DeviceContext* context{};
    ID3D11Texture2D *depth{},*motion{};
    float crop[4]{0,0,1,1};
    CheekyFgEye meta;
};
using CheekyFgShaderObserve11Fn=bool(__cdecl*)(const CheekyFgShaderSource11V1*);
inline bool cheeky_fg_shader_only(unsigned center,unsigned periphery,bool foveated) noexcept {
    return center>=1&&center<=2&&(!foveated||periphery<=2);
}
