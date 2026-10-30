#pragma once
namespace cheeky::fg {
inline constexpr char fg_snapshot_shader[]=R"HLSL(
Texture2D<float> Depth:register(t0);Texture2D<float2> Motion:register(t1);
RWTexture2D<float> OutDepth:register(u0);RWTexture2D<float2> OutMotion:register(u1);
cbuffer Params:register(b0){uint2 DepthBase;uint2 DepthSize;uint2 MotionBase;uint2 MotionSize;uint2 Size;
 float2 Scale;float2 Jitter;float2 Flip;float4 Crop;};
[numthreads(8,8,1)] void Main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=Size))return;float2 uv=(float2(id.xy)+.5)/Size;
 if(Flip.x>.5)uv.y=1-uv.y;uv=Crop.xy+uv*Crop.zw;
 OutDepth[id.xy]=Depth.Load(int3(DepthBase+min(uint2(uv*DepthSize),DepthSize-1),0));
 float2 mv=Motion.Load(int3(MotionBase+min(uint2(uv*MotionSize),MotionSize-1),0));
 bool valid=all(isfinite(mv))&&all(abs(mv)<65504);
 mv=(mv*Scale-Jitter)/Crop.zw;if(Flip.x>.5)mv.y=-mv.y;
 OutMotion[id.xy]=valid?mv:float2(65504,65504);
}
)HLSL";
}
