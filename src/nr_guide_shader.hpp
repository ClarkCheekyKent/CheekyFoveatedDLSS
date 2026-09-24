#pragma once
namespace cheeky::foveated_dlss {
// Resource ownership is supplied by the NR cache's recording/fence lifetime.
struct NrGuideConstants {
    unsigned output[2], region_base[2], region_size[2], processing[2];
    unsigned motion_full[2], depth_full[2];
    float motion_origin[2], depth_origin[2];
    float motion_scale[2], motion_offset[2];
};
static_assert(sizeof(NrGuideConstants) == 80);
inline constexpr char nr_guide_shader[] = R"(
Texture2D<float2> Motion : register(t0);
Texture2D<float> Depth : register(t1);
RWTexture2D<float2> OutMotion : register(u0);
RWTexture2D<float> OutDepth : register(u1);
cbuffer Constants : register(b0) {
 uint2 Size, RegionBase, RegionSize, Processing;
 uint2 MotionFull, DepthFull;
 float2 MotionOrigin, DepthOrigin, MotionScale, MotionOffset;
};
// Exact floor(a*b/divisor) without optional shader-double support. The float
// estimate is within one at texture-sized quotients; verify it with 64-bit
// products expressed as pairs of uints.
void multiplyWide(uint a,uint b,out uint lo,out uint hi) {
 uint p0=(a&65535)*(b&65535), p1=(a>>16)*(b&65535);
 uint p2=(a&65535)*(b>>16), p3=(a>>16)*(b>>16);
 uint middle=(p0>>16)+(p1&65535)+(p2&65535);
 lo=(p0&65535)|(middle<<16); hi=p3+(p1>>16)+(p2>>16)+(middle>>16);
}
bool greater(uint lo, uint hi, uint otherLo, uint otherHi) {
 return hi > otherHi || (hi == otherHi && lo > otherLo);
}
uint divideProduct(uint a, uint b, uint divisor) {
 uint lo,hi,ql,qh; multiplyWide(a,b,lo,hi);
 uint q=uint(float(a)*float(b)/float(divisor));
 multiplyWide(q,divisor,ql,qh);
 if(greater(ql,qh,lo,hi)) --q;
 else { multiplyWide(q+1,divisor,ql,qh); if(!greater(ql,qh,lo,hi)) ++q; }
 return q;
}
uint sampleAxis(uint pixel,uint base,uint region,uint outputSize,uint fullSize,uint processingSize) {
 return divideProduct(2*base*outputSize+(2*pixel+1)*region,fullSize,2*outputSize*processingSize);
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
 if (any(id.xy >= Size)) return;
 // Use full-view pixel centers, never the rounded enclosing guide rectangle.
 uint mw,mh,dw,dh; Motion.GetDimensions(mw,mh); Depth.GetDimensions(dw,dh);
 uint2 mpFull=uint2(sampleAxis(id.x,RegionBase.x,RegionSize.x,Size.x,MotionFull.x,Processing.x),
                    sampleAxis(id.y,RegionBase.y,RegionSize.y,Size.y,MotionFull.y,Processing.y));
 uint2 dpFull=uint2(sampleAxis(id.x,RegionBase.x,RegionSize.x,Size.x,DepthFull.x,Processing.x),
                    sampleAxis(id.y,RegionBase.y,RegionSize.y,Size.y,DepthFull.y,Processing.y));
 int2 mp=clamp(int2(MotionOrigin)+int2(mpFull),int2(0,0),int2(mw,mh)-1);
 int2 dp=clamp(int2(DepthOrigin)+int2(dpFull),int2(0,0),int2(dw,dh)-1);
 float2 mv=Motion.Load(int3(mp,0));
 OutMotion[id.xy] = any(!isfinite(mv)) || any(abs(mv)>1e15) ? mv : mv*MotionScale+MotionOffset;
 OutDepth[id.xy] = Depth.Load(int3(dp,0));
}
)";
}
