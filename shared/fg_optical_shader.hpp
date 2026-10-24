#pragma once
#include "fg_compact.hpp"
namespace cheeky::fg {
// Both endpoints use the same flow crop. Gaze movement therefore cannot become
// apparent scene motion. Color is sampled from the original HDR/linear images;
// only the optical flow input is converted to 8-bit luminance.
inline constexpr char optical_shader[]=CHEEKY_FG_COMPACT_HLSL R"HLSL(
Texture2D<float4> Old:register(t0);Texture2D<float4> Now:register(t1);
Texture2D<int2> Forward:register(t2);Texture2D<int2> Backward:register(t3);
Texture2D<uint> ForwardCost:register(t4);Texture2D<uint> BackwardCost:register(t5);
RWTexture2D<float4> Result:register(u0);RWTexture2D<float4> Reference:register(u1);
SamplerState ClampLinear:register(s0);
cbuffer Params:register(b0){float4 FlowRect;float4 OutputRect;float4 Sizes;float4 Flags;
 float4 Crop;float4 Spans;float4 Texel;float4 OldCrop;float4 OldSpans;}
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
float4 flow(Texture2D<int2> field,Texture2D<uint> cost,float2 p){
 uint w,h;field.GetDimensions(w,h);
 // NVOFA grid is 4x4. Coordinates are in pixels of the scaled flow input.
 float2 pos=p*Sizes.xy/4-.5;int2 i=int2(floor(pos));float2 a=frac(pos);
 int2 hi=int2(w,h)-1;
 int2 q0=clamp(i,0,hi),q1=clamp(i+int2(1,0),0,hi),q2=clamp(i+int2(0,1),0,hi),q3=clamp(i+1,0,hi);
 float2 v=lerp(lerp(float2(field.Load(int3(q0,0))),float2(field.Load(int3(q1,0))),a.x),
               lerp(float2(field.Load(int3(q2,0))),float2(field.Load(int3(q3,0))),a.x),a.y);
 float c=lerp(lerp(float(cost.Load(int3(q0,0))),float(cost.Load(int3(q1,0))),a.x),
              lerp(float(cost.Load(int3(q2,0))),float(cost.Load(int3(q3,0))),a.x),a.y);
 return float4(v/(32*Sizes.xy),1/(1+c/32),0);
}
float3 inputColor(float3 c){float y=dot(max(c,0),float3(.2126,.7152,.0722));return (y/(1+y)).xxx;}
[numthreads(8,8,1)]void Prepare(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=uint2(Sizes.xy)))return;
 float2 uv=FlowRect.xy+(float2(id.xy)+.5)/Sizes.xy*FlowRect.zw;
 Result[id.xy]=float4(inputColor(packed(Old,uv,OldCrop,OldSpans).rgb),1);
 Reference[id.xy]=float4(inputColor(packed(Now,uv,Crop,Spans).rgb),1);
}
float quality(float2 p,float2 other,float4 v,float4 reverse,float2 residual){
 if(!within(p)||!within(other)||!all(isfinite(v))||!all(isfinite(reverse)))return 0;
 float error=length((v.xy+reverse.xy)*Sizes.xy);
 float limit=2.5+.05*length(v.xy*Sizes.xy);
 return v.z*reverse.z*saturate(1-error/limit)*saturate(1-length(residual*Sizes.xy)/3);
}
[numthreads(8,8,1)]void Interpolate(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=uint2(Sizes.zw)))return;
 float2 uv=OutputRect.xy+(float2(id.xy)+.5)/Sizes.zw*OutputRect.zw;
 float2 q=(uv-FlowRect.xy)/FlowRect.zw;float t=Flags.x;
 float2 a=q,b=q;float4 f=0,r=0;
 [unroll]for(uint k=0;k<5;++k){f=flow(Forward,ForwardCost,a);a=q-t*f.xy;r=flow(Backward,BackwardCost,b);b=q-(1-t)*r.xy;}
 f=flow(Forward,ForwardCost,a);r=flow(Backward,BackwardCost,b);
 float2 ab=a+f.xy,ba=b+r.xy;
 float wa=quality(a,ab,f,flow(Backward,BackwardCost,ab),a+t*f.xy-q);
 float wb=quality(b,ba,r,flow(Forward,ForwardCost,ba),b+(1-t)*r.xy-q);
 // Invalid vectors at an occlusion edge can often borrow a neighboring
 // trajectory. Validate every candidate in both directions before using it.
 [branch]if(wa+wb<.05){
  [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
   float2 delta=float2(x,y)*4/Sizes.xy;
   float4 v=flow(Forward,ForwardCost,q+delta);float2 p=q-t*v.xy;
   float z=quality(p,p+v.xy,v,flow(Backward,BackwardCost,p+v.xy),float2(0,0))*.5;
   if(z>wa){wa=z;a=p;}
   v=flow(Backward,BackwardCost,q+delta);p=q-(1-t)*v.xy;
   z=quality(p,p+v.xy,v,flow(Forward,ForwardCost,p+v.xy),float2(0,0))*.5;
   if(z>wb){wb=z;b=p;}
  }
 }
 float4 old=packed(Old,FlowRect.xy+a*FlowRect.zw,OldCrop,OldSpans);
 float4 now=packed(Now,FlowRect.xy+b*FlowRect.zw,Crop,Spans);
 wa*=1-t;wb*=t;
 float4 c;
 if(wa+wb>.02)c=(old*wa+now*wb)/(wa+wb);
 else c=t<.5?packed(Old,uv,OldCrop,OldSpans):packed(Now,uv,Crop,Spans);
 if(Flags.y>.5)c.rgb=lerp(12.92*c.rgb,1.055*pow(max(c.rgb,0),1.0/2.4)-.055,step(.0031308,c.rgb));
 Result[id.xy]=c;
}
)HLSL";
struct OpticalConstants {
    float flow_rect[4]{0,0,1,1},output_rect[4]{0,0,1,1},sizes[4]{},flags[4]{};
    float crop[4]{},spans[4]{},texel[4]{},old_crop[4]{},old_spans[4]{};
};
}
