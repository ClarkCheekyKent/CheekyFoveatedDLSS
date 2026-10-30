#pragma once
#include "fg_optical_shader.hpp"
#include "fg_interpolation_shader.hpp"
#include "fg_snapshot_shader.hpp"
#include "generated/fg_shader_bytecode.hpp"
#include <span>
#include <string_view>
#include <d3dcompiler.h>
#include <cstring>

namespace cheeky::fg::bytecode {
template<std::size_t N> inline HRESULT blob(const unsigned char (&bytes)[N],ID3DBlob** output) noexcept {
    if(!output)return E_POINTER;
    *output=nullptr;const auto result=D3DCreateBlob(N,output);
    if(SUCCEEDED(result))std::memcpy((*output)->GetBufferPointer(),bytes,N);
    return result;
}
// A shader-source edit must regenerate the shipped bytecode. This is checked
// by the C++ compiler in MSBuild and CMake, without an extra build dependency.
constexpr std::uint64_t source_hash(std::string_view text) noexcept {
    std::uint64_t value=14695981039346656037ull;
    for(unsigned char byte:text)value=(value^byte)*1099511628211ull;
    return value;
}
static_assert(source_hash(optical_shader)==optical_source_hash,
    "Optical FG bytecode is stale: run python tools/compile-fg-shaders.py");
static_assert(source_hash(interpolation_shader)==interpolation_source_hash,
    "Interpolation FG bytecode is stale: run python tools/compile-fg-shaders.py");
static_assert(source_hash(fg_snapshot_shader)==snapshot_source_hash,
    "Snapshot FG bytecode is stale: run python tools/compile-fg-shaders.py");

inline std::span<const unsigned char> optical(std::string_view entry,std::string_view profile) noexcept {
    if(profile=="cs_5_0"){
        if(entry=="Prepare")return optical_prepare_cs_5_0;
        if(entry=="Interpolate")return optical_interpolate_cs_5_0;
        if(entry=="CacheVectors")return optical_cachevectors_cs_5_0;
    }else if(profile=="cs_5_1"){
        if(entry=="Prepare")return optical_prepare_cs_5_1;
        if(entry=="Interpolate")return optical_interpolate_cs_5_1;
        if(entry=="CacheVectors")return optical_cachevectors_cs_5_1;
    }
    return {};
}
}
