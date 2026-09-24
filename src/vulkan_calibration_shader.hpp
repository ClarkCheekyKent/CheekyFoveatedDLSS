#pragma once
namespace cheeky::foveated_dlss {
inline constexpr char vulkan_calibration_shader[] = R"(
RWTexture2DArray<float4> Output : register(u0);
RWTexture2DArray<float4> Proof : register(u1);
cbuffer Params : register(b0) {
    uint4 Points[16]; // x, y, code, locator
    uint Count; uint Phase; uint Candidate; uint Reserved;
};
[numthreads(8,8,1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint index = tid.x / 72;
    uint2 p = uint2(tid.x % 72, tid.y);
    if (index >= Count) return;
    uint4 marker = Points[index];
    uint pad = marker.w != 0 ? 16 : 0;
    uint size = marker.w != 0 ? 72 : 40;
    if (p.x >= size || p.y >= size) return;
    uint2 at = marker.xy + p - pad;
    float4 before = Output[uint3(at,0)];
    bool interior = all(p >= pad) && all(p < pad + 40);
    if (index == 0 && interior)
        Proof[uint3(p - pad + uint2(0,Phase * 40),0)] = before;
    if (Phase != 0) return;
    uint2 cell = (p - pad) / 8;
    uint code = marker.z;
    if (code == 0) code = Candidate == 0 ? 0xca8e8b : 0x129b09d;
    bool light = interior ? ((code >> (cell.y * 5 + cell.x)) & 1) != 0 :
        (p.x < 8 || p.y < 8 || p.x >= 64 || p.y >= 64);
    Output[uint3(at,0)] = float4(light ? 1.0 : 0.0,light ? 1.0 : 0.0,light ? 1.0 : 0.0,1.0);
}
)";
}

