#pragma once
namespace cheeky::foveated_dlss {
inline constexpr char motion_convert_shader_source[] = R"(
Texture2D<float2> SourceMotion : register(t0);
RWTexture2D<float2> PackedMotion : register(u0);
cbuffer Constants : register(b0) {
    uint2 SourceBase;
    uint2 SourceSize;
    uint2 DestSize;
};
[numthreads(16, 16, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DestSize)) return;
    const uint2 numerator = (2U * id.xy + 1U) * SourceSize;
    const uint2 denominator = 2U * DestSize;
    const uint2 local = min(numerator / denominator, SourceSize - 1U);
    PackedMotion[id.xy] = SourceMotion.Load(int3(SourceBase + local, 0));
}
)";

inline constexpr char color_downsample_shader_source[] = R"(
Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> PackedColor : register(u0);
cbuffer Constants : register(b0) {
    uint2 SourceBase;
    uint2 SourceSize;
    uint2 DestSize;
};
float4 LoadClamped(int2 local) {
    const int2 maximum = int2(SourceSize) - 1;
    return SourceColor.Load(int3(int2(SourceBase) + clamp(local, int2(0, 0), maximum), 0));
}
[numthreads(16, 16, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DestSize)) return;
    const float2 source =
        (float2(id.xy) + 0.5) * float2(SourceSize) / float2(DestSize) - 0.5;
    const int2 base = int2(floor(source));
    const float2 fraction = source - floor(source);
    const float4 p00 = LoadClamped(base);
    const float4 p10 = LoadClamped(base + int2(1, 0));
    const float4 p01 = LoadClamped(base + int2(0, 1));
    const float4 p11 = LoadClamped(base + int2(1, 1));
    PackedColor[id.xy] = lerp(
        lerp(p00, p10, fraction.x),
        lerp(p01, p11, fraction.x),
        fraction.y
    );
}
)";

inline constexpr char depth_downsample_shader_source[] = R"(
Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> PackedDepth : register(u0);
cbuffer Constants : register(b0) {
    uint2 SourceBase;
    uint2 SourceSize;
    uint2 DestSize;
};
[numthreads(16, 16, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DestSize)) return;
    const uint2 numerator = (2U * id.xy + 1U) * SourceSize;
    const uint2 denominator = 2U * DestSize;
    const uint2 local = min(numerator / denominator, SourceSize - 1U);
    PackedDepth[id.xy] = SourceDepth.Load(int3(SourceBase + local, 0));
}
)";

}
