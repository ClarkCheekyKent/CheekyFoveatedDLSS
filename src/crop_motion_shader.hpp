#pragma once

namespace cheeky::foveated_dlss {
inline constexpr char crop_motion_shader_source[] = R"(
Texture2DArray<float2> Source : register(t0);
RWTexture2D<float2> Destination : register(u0);
cbuffer Constants : register(b0) {
    uint2 Base; uint2 Size;
    float2 Offset; uint2 SourceSize;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size)) return;
    // Point sampling preserves distinct object velocities and invalid markers;
    // blending across a silhouette would invent a third, incorrect velocity.
    const uint2 source_pixel = min(uint2((float2(id.xy) + 0.5) *
        float2(SourceSize) / float2(Size)), SourceSize - 1U);
    const float2 mv = Source.Load(int4(Base + source_pixel, 0, 0));
    // Resize output-space displacement along with the output grid. Offset is
    // expressed in the original stored-vector units before this conversion.
    Destination[id.xy] = any(!isfinite(mv)) || any(abs(mv) > 1e15)
        ? mv : (mv + Offset) * float2(Size) / float2(SourceSize);
}
)";
} // namespace cheeky::foveated_dlss
