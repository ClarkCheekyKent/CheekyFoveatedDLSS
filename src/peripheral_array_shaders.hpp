#pragma once
namespace cheeky::foveated_dlss {
// Native NGX addresses mip 0/slice 0. A one-slice array SRV also accepts
// multi-slice game allocations without changing that resource contract.
inline constexpr char peripheral_array_motion[] = R"(
Texture2DArray<float2> Source : register(t0);
RWTexture2D<float2> Destination : register(u0);
cbuffer Constants : register(b0) { uint2 SourceBase; uint2 SourceSize; uint2 DestSize; };
[numthreads(16,16,1)] void Main(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=DestSize))return;
 uint2 local=min(((2U*id.xy+1U)*SourceSize)/(2U*DestSize),SourceSize-1U);
 Destination[id.xy]=Source.Load(int4(SourceBase+local,0,0));
})";
inline constexpr char peripheral_array_color[] = R"(
Texture2DArray<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer Constants : register(b0) { uint2 SourceBase; uint2 SourceSize; uint2 DestSize; };
float4 LoadClamped(int2 p) { return Source.Load(int4(int2(SourceBase)+clamp(p,int2(0,0),int2(SourceSize)-1),0,0)); }
[numthreads(16,16,1)] void Main(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=DestSize))return;
 float2 p=(float2(id.xy)+0.5)*float2(SourceSize)/float2(DestSize)-0.5;
 int2 base=int2(floor(p));float2 f=frac(p);
 Destination[id.xy]=lerp(lerp(LoadClamped(base),LoadClamped(base+int2(1,0)),f.x),
                       lerp(LoadClamped(base+int2(0,1)),LoadClamped(base+int2(1,1)),f.x),f.y);
})";
inline constexpr char peripheral_array_depth[] = R"(
Texture2DArray<float> Source : register(t0);
RWTexture2D<float> Destination : register(u0);
cbuffer Constants : register(b0) { uint2 SourceBase; uint2 SourceSize; uint2 DestSize; };
[numthreads(16,16,1)] void Main(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=DestSize))return;
 uint2 local=min(((2U*id.xy+1U)*SourceSize)/(2U*DestSize),SourceSize-1U);
 Destination[id.xy]=Source.Load(int4(SourceBase+local,0,0));
})";
inline constexpr char peripheral_array_rr[] = R"(
Texture2DArray<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer Constants : register(b0) { uint2 SourceBase; uint2 SourceSize; uint2 DestSize; };
[numthreads(16,16,1)] void Main(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=DestSize))return;
 uint2 local=min(((2U*id.xy+1U)*SourceSize)/(2U*DestSize),SourceSize-1U);
 Destination[id.xy]=Source.Load(int4(SourceBase+local,0,0));
})";
}
