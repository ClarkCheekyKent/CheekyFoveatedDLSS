#pragma once
#include "fg_compact.hpp"
namespace cheeky::fg {
// Both endpoints use the same flow crop. Gaze movement therefore cannot become
// apparent scene motion. Color is sampled from the original HDR/linear images;
// only the optical flow input is converted to 8-bit luminance.
// With pose compensation (Texel.y), the previous frame is reprojected into the
// current camera by the head-rotation delta before flow estimation, so flow
// measures only residual motion (head translation, the scene itself). The
// generated image is interpolated in the current camera and then reprojected
// into the submitted in-between camera.
inline constexpr char optical_shader[]=CHEEKY_FG_COMPACT_HLSL R"HLSL(
Texture2D<float4> Old:register(t0);Texture2D<float4> Now:register(t1);
Texture2D<int2> Forward:register(t2);Texture2D<int2> Backward:register(t3);
Texture2D<uint> ForwardCost:register(t4);Texture2D<uint> BackwardCost:register(t5);
Texture2D<float> Depth:register(t6);
Texture2D<float4> CachedForward:register(t7);Texture2D<float4> CachedBackward:register(t8);
RWTexture2D<float4> Result:register(u0);RWTexture2D<float4> Reference:register(u1);
SamplerState ClampLinear:register(s0);
cbuffer Params:register(b0){float4 FlowRect;float4 OutputRect;float4 Sizes;float4 Flags;
 float4 Crop;float4 Spans;float4 Texel;float4 OldCrop;float4 OldSpans;float4 DepthRect;
 float4 NowToOld;float4 OldTangents;float4 NowTangents;float4 MidToNow;float4 MidTangents;}
float4 packed(Texture2D<float4> im,float2 uv,float4 crop,float4 spans){
 if(spans.x<=0)return im.SampleLevel(ClampLinear,uv,0);
 uint w,h;im.GetDimensions(w,h);float4 texel=Texel;texel.xy=.5/float2(w,h);
 float a=centerFade(uv,crop,texel);
 [branch]if(a>=1)return im.SampleLevel(ClampLinear,centerUV(uv,crop,spans,texel),0);
 float4 b=im.SampleLevel(ClampLinear,contextUV(uv,spans,texel),0);
 [branch]if(a<=0)return b;
 return lerp(b,im.SampleLevel(ClampLinear,centerUV(uv,crop,spans,texel),0),a);
}
bool within(float2 p){return all(isfinite(p))&&all(p>=0)&&all(p<=1);}
// Eye UV in one camera to eye UV in another rotated camera. Tangents are
// (left,right,up,down) of the OpenXR FOV; the quaternion rotates view-space rays.
float2 rotateView(float2 uv,float4 rotation,float4 from,float4 to){
 float3 ray=float3(lerp(from.x,from.y,uv.x),lerp(from.z,from.w,uv.y),-1);
 ray+=2*cross(rotation.xyz,cross(rotation.xyz,ray)+rotation.w*ray);
 if(ray.z>-1e-5)return float2(-10,-10);
 float2 tangent=ray.xy/-ray.z;
 return float2((tangent.x-to.x)/(to.y-to.x),(tangent.y-to.z)/(to.w-to.z));
}
float2 oldUV(float2 uv){return Texel.y>.5?rotateView(uv,NowToOld,NowTangents,OldTangents):uv;}
// Captured depth of the current frame, as nearness (larger is closer). Device
// depth is ~near/z for both conventions, so a ratio compares view distances.
float nearness(float2 p){
 float2 uv=(FlowRect.xy+p*FlowRect.zw-DepthRect.xy)/DepthRect.zw;
 float d=Depth.SampleLevel(ClampLinear,saturate(uv),0);
 return max(Flags.w>.5?d:1-d,1e-6);
}
float4 flow(Texture2D<int2> field,Texture2D<uint> cost,Texture2D<float4> cached,float2 p,bool guided=false){
 uint w,h;field.GetDimensions(w,h);
 // Packing is pair-invariant. Hardware bilinear filtering replaces four
 // vector loads plus four cost loads when depth guidance is not requested.
 if(Texel.w>3.5&&(!guided||Flags.z<.5)){
  float4 v=cached.SampleLevel(ClampLinear,p*Sizes.xy/(Texel.x*float2(w,h)),0)/1.000001;
  return float4(v.xy/(32*Sizes.xy),1/(1+v.z/32),0);
 }
 // One NVOFA vector per Texel.x square of flow-input pixels.
 float2 pos=p*Sizes.xy/Texel.x-.5;int2 i=int2(floor(pos));float2 a=frac(pos);
 int2 hi=int2(w,h)-1;
 int2 q[4]={clamp(i,0,hi),clamp(i+int2(1,0),0,hi),clamp(i+int2(0,1),0,hi),clamp(i+1,0,hi)};
 float k[4]={(1-a.x)*(1-a.y),a.x*(1-a.y),(1-a.x)*a.y,a.x*a.y};
 // Depth-guided upsampling keeps a foreground vector from bleeding across
 // its silhouette onto the background, and vice versa.
 [branch]if(guided&&Flags.z>.5){float n=nearness(p);
  [unroll]for(uint j=0;j<4;++j){float e=(nearness((float2(q[j])+.5)*Texel.x/Sizes.xy)-n)/(.03*n);k[j]*=1/(1+e*e);}}
 float2 v=0;float c=0,sum=1e-6;
 [unroll]for(uint j=0;j<4;++j){
  float4 sample=Texel.w>3.5?cached.Load(int3(q[j],0)):float4(float2(field.Load(int3(q[j],0))),float(cost.Load(int3(q[j],0))),0);
  v+=k[j]*sample.xy;c+=k[j]*sample.z;sum+=k[j];}
 v/=sum;c/=sum;
 return float4(v/(32*Sizes.xy),1/(1+c/32),0);
}
// Fast reconstruction does not read the matching-cost surfaces. Native fields
// remain in their own grid; changing interpolator never repeats estimation.
float2 vectorAt(Texture2D<int2> field,float2 p,bool guided=false){
 uint w,h;field.GetDimensions(w,h);
 float2 pos=p*Sizes.xy/Texel.x-.5;int2 i=int2(floor(pos));float2 a=frac(pos);int2 hi=int2(w,h)-1;
 int2 q[4]={clamp(i,0,hi),clamp(i+int2(1,0),0,hi),clamp(i+int2(0,1),0,hi),clamp(i+1,0,hi)};
 float k[4]={(1-a.x)*(1-a.y),a.x*(1-a.y),(1-a.x)*a.y,a.x*a.y};
 if(guided&&Flags.z>.5){float n=nearness(p);
  [unroll]for(uint j=0;j<4;++j){float e=(nearness((float2(q[j])+.5)*Texel.x/Sizes.xy)-n)/(.03*n);k[j]*=1/(1+e*e);}}
 float2 v=0;float sum=1e-6;
 [unroll]for(uint j=0;j<4;++j){v+=k[j]*float2(field.Load(int3(q[j],0)));sum+=k[j];}
 return v/(sum*32*Sizes.xy);
}
float4 fastInterpolation(float2 uv,float2 q,float t){
 float2 a=q,b=q;
 [unroll]for(uint i=0;i<4;++i){
  b=q-(1-t)*vectorAt(Backward,b,true);
  if(Texel.w>.5)a=q-t*vectorAt(Forward,a);
 }
 // Simple warping uses the same current->previous trajectory as DLSS Vectors.
 if(Texel.w<.5)a=b+vectorAt(Backward,b,true);
 float2 source=oldUV(FlowRect.xy+a*FlowRect.zw);
 float wa=within(a)&&within(source)?1-t:0,wb=within(b)?t:0;
 if(Texel.w<.5){wa=within(a)&&within(source)?1:0;wb=0;}
 if(Flags.z>.5&&wa>0&&wb>0){
  float na=nearness(a+vectorAt(Forward,a)),nb=nearness(b);
  float rel=(na-nb)/max(na,nb);wa*=rel<-.03?0:1;wb*=rel>.03?0:1;
 }
 float4 c=wa+wb>1e-6?(packed(Old,source,OldCrop,OldSpans)*wa+packed(Now,FlowRect.xy+b*FlowRect.zw,Crop,Spans)*wb)/(wa+wb):packed(Now,uv,Crop,Spans);
 if(Flags.y>.5)c.rgb=lerp(12.92*c.rgb,1.055*pow(max(c.rgb,0),1.0/2.4)-.055,step(.0031308,c.rgb));
 return c;
}
float3 inputColor(float3 c){float y=dot(max(c,0),float3(.2126,.7152,.0722));return (y/(1+y)).xxx;}
[numthreads(8,8,1)]void Prepare(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=uint2(Sizes.xy)))return;
 float2 uv=FlowRect.xy+(float2(id.xy)+.5)/Sizes.xy*FlowRect.zw;
 float2 source=oldUV(uv);
 Result[id.xy]=float4(within(source)?inputColor(packed(Old,source,OldCrop,OldSpans).rgb):0,1);
 Reference[id.xy]=float4(inputColor(packed(Now,uv,Crop,Spans).rgb),1);
}
// Store the original cost before its nonlinear confidence mapping so cached
// and uncached filtering have the same meaning, including depth-guided reads.
[numthreads(8,8,1)]void CacheVectors(uint3 id:SV_DispatchThreadID){
 uint w,h;Forward.GetDimensions(w,h);if(any(id.xy>=uint2(w,h)))return;
 Result[id.xy]=float4(float2(Forward.Load(int3(id.xy,0))),float(ForwardCost.Load(int3(id.xy,0))),0);
 Reference[id.xy]=float4(float2(Backward.Load(int3(id.xy,0))),float(BackwardCost.Load(int3(id.xy,0))),0);
}
float quality(float2 p,float2 other,float4 v,float4 reverse,float2 residual){
 if(!within(p)||!all(isfinite(v)))return 0;
 // Content entering or leaving the flow rectangle (head turns, the halo
 // edge) has no counterpart to verify against. Trust its own match at a
 // reduced weight rather than repeating an endpoint.
 if(!within(other)||!all(isfinite(reverse)))return .5*v.z*saturate(1-length(residual*Sizes.xy)/3);
 float error=length((v.xy+reverse.xy)*Sizes.xy);
 float limit=2.5+.05*length(v.xy*Sizes.xy);
 return v.z*reverse.z*saturate(1-error/limit)*saturate(1-length(residual*Sizes.xy)/3);
}
[numthreads(8,8,1)]void Interpolate(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=uint2(Sizes.zw)))return;
 float2 uv=OutputRect.xy+(float2(id.xy)+.5)/Sizes.zw*OutputRect.zw;
 if(Texel.y>.5)uv=rotateView(uv,MidToNow,MidTangents,NowTangents);
 float2 q=(uv-FlowRect.xy)/FlowRect.zw;float t=Flags.x;float mode=fmod(Texel.w,4);
 [branch]if(mode<1.5){Result[id.xy]=fastInterpolation(uv,q,t);return;}
 float2 a=q,b=q;float4 f=0,r=0;
 [unroll]for(uint k=0;k<5;++k){if(mode>2.5&&k>=2)break;f=flow(Forward,ForwardCost,CachedForward,a);a=q-t*f.xy;r=flow(Backward,BackwardCost,CachedBackward,b,true);b=q-(1-t)*r.xy;}
 f=flow(Forward,ForwardCost,CachedForward,a);r=flow(Backward,BackwardCost,CachedBackward,b,true);
 float2 ab=a+f.xy,ba=b+r.xy;
 float wa=quality(a,ab,f,flow(Backward,BackwardCost,CachedBackward,ab,true),a+t*f.xy-q);
 float wb=quality(b,ba,r,flow(Forward,ForwardCost,CachedForward,ba),b+(1-t)*r.xy-q);
 float2 fa=f.xy;
 // Invalid vectors at an occlusion edge can often borrow a neighboring
 // trajectory. Validate every candidate in both directions before using it.
 [branch]if(mode<2.5&&wa+wb<.05){
  // Disocclusions reveal background, so with depth prefer farther neighbors
  // over the foreground that just moved away.
  float near_max=0;
  [branch]if(Flags.z>.5)[unroll]for(int j=-1;j<=1;++j)[unroll]for(int i=-1;i<=1;++i)near_max=max(near_max,nearness(q+float2(i,j)*4/Sizes.xy));
  [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
   float2 delta=float2(x,y)*4/Sizes.xy;
   float4 v=flow(Forward,ForwardCost,CachedForward,q+delta);float2 p=q-t*v.xy;
   float z=quality(p,p+v.xy,v,flow(Backward,BackwardCost,CachedBackward,p+v.xy,true),float2(0,0))*.5;
   if(Flags.z>.5)z*=1+3*saturate(1-nearness(p+v.xy)/near_max);
   if(z>wa){wa=z;a=p;fa=v.xy;}
   v=flow(Backward,BackwardCost,CachedBackward,q+delta,true);p=q-(1-t)*v.xy;
   z=quality(p,p+v.xy,v,flow(Forward,ForwardCost,CachedForward,p+v.xy),float2(0,0))*.5;
   if(Flags.z>.5)z*=1+3*saturate(1-nearness(p)/near_max);
   if(z>wb){wb=z;b=p;}
  }
 }
 // Depth arbitrates where both trajectories pass the flow checks but land on
 // different surfaces: the nearer one covers the pixel at time t.
 [branch]if(Flags.z>.5&&wa>0&&wb>0){
  float na=nearness(a+fa),nb=nearness(b);
  float rel=(na-nb)/max(na,nb);
  wa*=rel<-.03?.05:1;wb*=rel>.03?.05:1;
 }
 float2 source=oldUV(FlowRect.xy+a*FlowRect.zw);if(!within(source))wa=0;
 float4 old=packed(Old,source,OldCrop,OldSpans);
 float4 now=packed(Now,FlowRect.xy+b*FlowRect.zw,Crop,Spans);
 wa*=1-t;wb*=t;
 // Two trajectories that land on different surfaces cannot both be right;
 // blending them draws a ghosted double edge. Compare where they land in the
 // current frame (not their colors, which texture detail and per-pixel
 // vector noise make flicker) and keep the more trusted one.
 [branch]if(wa>0&&wb>0){
  float apart=length((a+fa-b)*Sizes.xy);
  float k=saturate((apart-2)/2);
  if(wa>=wb)wb*=1-k;else wa*=1-k;
 }
 float4 c;
 if(wa+wb>.02)c=(old*wa+now*wb)/(wa+wb);
 else c=t<.5&&within(oldUV(uv))?packed(Old,oldUV(uv),OldCrop,OldSpans):packed(Now,uv,Crop,Spans);
 if(Flags.y>.5)c.rgb=lerp(12.92*c.rgb,1.055*pow(max(c.rgb,0),1.0/2.4)-.055,step(.0031308,c.rgb));
 Result[id.xy]=c;
}
)HLSL";
struct OpticalConstants {
    float flow_rect[4]{0,0,1,1},output_rect[4]{0,0,1,1},sizes[4]{},flags[4]{};
    // texel: vector grid, pose compensation, packed feather, interpolation mode + 4 when cached.
    float crop[4]{},spans[4]{},texel[4]{4,0,0,2},old_crop[4]{},old_spans[4]{},depth_rect[4]{0,0,1,1};
    float now_to_old[4]{0,0,0,1},old_tangents[4]{},now_tangents[4]{},mid_to_now[4]{0,0,0,1},mid_tangents[4]{};
};
}
