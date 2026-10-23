#pragma once
#include "fg_compact.hpp"
namespace cheeky::fg {
inline constexpr char interpolation_shader[]=CHEEKY_FG_COMPACT_HLSL R"HLSL(
Texture2D<float4> Previous:register(t0);Texture2D<float4> Current:register(t1);
Texture2D<float2> Motion:register(t2);Texture2D<float> Depth:register(t3);Texture2D<float> PreviousDepth:register(t4);
Texture2D<float4> Center:register(t5);
RWTexture2D<float4> Output:register(u0);
cbuffer Params:register(b0){float4 Rect;float4 GuideOffset;float4 Dimensions;float4 MotionAdjust;float4 Options;float4 JitterCorrection;float4 AtlasCrop;float4 AtlasSpans;float4 AtlasTexel;float4 PriorCrop;float4 PriorSpans;row_major float4x4 ClipToPrevious;}
SamplerState LinearClamp:register(s0);
float4 samplePacked(Texture2D<float4> image,float2 uv,float4 crop,float4 spans,float4 texel){
 if(spans.x<=0)return image.SampleLevel(LinearClamp,uv,0);
 uint sw,sh;image.GetDimensions(sw,sh);texel.xy=.5/float2(sw,sh);
 float fade=centerFade(uv,crop,texel);
 [branch]if(fade>=1)return image.SampleLevel(LinearClamp,centerUV(uv,crop,spans,texel),0);
 float4 base=image.SampleLevel(LinearClamp,contextUV(uv,spans,texel),0);
 [branch]if(fade<=0)return base;
 return lerp(base,image.SampleLevel(LinearClamp,centerUV(uv,crop,spans,texel),0),fade);}
float4 encodeColor(float4 c){if(Options.w>.5)c.rgb=lerp(12.92*c.rgb,1.055*pow(max(c.rgb,0),1.0/2.4)-.055,step(.0031308,c.rgb));return c;}
bool inside(float2 p){return all(isfinite(p))&&all(p>=0)&&all(p<=1);}
float2 guideUV(float2 uv){return GuideOffset.w>.5?(uv-Rect.xy)/Rect.zw:uv;}
bool motionAt(float2 uv,out float2 mv){uv=guideUV(uv);uint w,h;Motion.GetDimensions(w,h);if(GuideOffset.w>.5&&!inside(uv)){mv=0;return false;}mv=Motion.Load(int3(min(uint2(saturate(uv)*float2(w,h)),uint2(w,h)-1),0));if(GuideOffset.w>.5)mv*=Rect.zw;return all(isfinite(mv))&&all(abs(mv)<=2);}
float4 interpolateAt(float2 uv,uint w,uint h,float method){
 if(Options.z<.5)return encodeColor(samplePacked(Current,uv,AtlasCrop,AtlasSpans,AtlasTexel));
 if(method<.5)return encodeColor(samplePacked(Previous,uv,PriorCrop,PriorSpans,AtlasTexel));
 float2 cuv=uv,mv=0;bool valid=true;
 [unroll]for(uint i=0;i<4;++i){if(!inside(cuv)||!motionAt(cuv,mv)){valid=false;break;}cuv=uv-(1-Options.y)*mv;}
 valid=valid&&inside(cuv)&&motionAt(cuv,mv);float2 puv=cuv+mv;
 valid=valid&&all(abs((cuv+(1-Options.y)*mv-uv)*float2(w,h))<=2);
 float4 result=method<1.5?samplePacked(Previous,uv,PriorCrop,PriorSpans,AtlasTexel):samplePacked(Current,uv,AtlasCrop,AtlasSpans,AtlasTexel);
 if(valid){float4 b=0;if(method>=1.5)b=samplePacked(Current,cuv,AtlasCrop,AtlasSpans,AtlasTexel);
  if(inside(puv)){float4 a=samplePacked(Previous,puv,PriorCrop,PriorSpans,AtlasTexel);
   if(method<1.5)result=a;
   else{uint dw,dh;Depth.GetDimensions(dw,dh);float z=Depth.Load(int3(min(uint2(saturate(guideUV(cuv))*float2(dw,dh)),uint2(dw,dh)-1),0));
    float4 clip=mul(ClipToPrevious,float4(cuv.x*2-1,1-cuv.y*2,z,1));
    PreviousDepth.GetDimensions(dw,dh);float old=PreviousDepth.Load(int3(min(uint2(saturate(guideUV(puv))*float2(dw,dh)),uint2(dw,dh)-1),0));
    bool agrees=(GuideOffset.w<.5||inside(guideUV(puv)))&&all(isfinite(clip))&&clip.w>0.000001&&isfinite(old)&&abs(old-clip.z/clip.w)<=.02;
    result=agrees?lerp(a,b,Options.y):b;}
  }else if(method>=1.5)result=b;
 }
 return encodeColor(result);
}
float4 peripheralAt(float2 uv,uint w,uint h){return interpolateAt(uv,w,h,Options.x);}
float4 finalColor(float4 c){
 if(GuideOffset.y>.5)c.rgb=lerp(c.rgb/12.92,pow((max(c.rgb,0)+.055)/1.055,2.4),step(.04045,c.rgb));
 return c;
}
// Native D3D11 writes both regions directly to the final image. Guides remain
// in full-eye coordinates, so gaze motion does not introduce crop motion.
float4 regionsAt(uint2 pixel){
 uint w=uint(Dimensions.x),h=uint(Dimensions.y);
 float2 uv=(float2(pixel)+.5)/float2(w,h);float4 texel=0;texel.z=GuideOffset.x;
 float fade=MotionAdjust.w>.5?centerFade(uv,Rect,texel):1;
 float4 c=0,b=0;
 if(fade>0){float2 grid=max(float2(1,1),round(Rect.zw*float2(w,h)*Dimensions.z));
  float2 q=Rect.xy+(floor(saturate((uv-Rect.xy)/Rect.zw)*grid)+.5)/grid*Rect.zw;
  c=interpolateAt(q,w,h,MotionAdjust.z);}
 if(fade<1)b=peripheralAt(uv,w,h);
 float4 result=fade>=1?c:fade<=0?b:lerp(b,c,fade);
 if(GuideOffset.z>.5&&MotionAdjust.w>.5){float2 edge=abs(min(uv-Rect.xy,Rect.xy+Rect.zw-uv))*float2(w,h);
  if(all(uv>=Rect.xy-2/float2(w,h))&&all(uv<=Rect.xy+Rect.zw+2/float2(w,h))&&min(edge.x,edge.y)<2)result=float4(0,.25,1,1);}
 return result;
}
[numthreads(8,8,1)] void RegionsMain(uint3 id:SV_DispatchThreadID){
 if(id.x<uint(Dimensions.x)&&id.y<uint(Dimensions.y))Output[id.xy]=regionsAt(id.xy);
}
float4 RegionsVS(uint id:SV_VertexID):SV_Position{
 float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
float4 RegionsPS(float4 position:SV_Position):SV_Target{return regionsAt(uint2(position.xy));}
[numthreads(8,8,1)] void CenterMain(uint3 id:SV_DispatchThreadID){
 uint w=uint(Dimensions.x),h=uint(Dimensions.y);if(id.x>=w||id.y>=h)return;
 float2 uv=Rect.xy+(float2(id.xy)+.5)/float2(w,h)*Rect.zw;
 Output[id.xy+uint2(Dimensions.zw)]=peripheralAt(uv,uint(w/Rect.z),uint(h/Rect.w));
}
[numthreads(8,8,1)] void Main(uint3 id:SV_DispatchThreadID){
 uint w=uint(Dimensions.x),h=uint(Dimensions.y);if(id.x>=w||id.y>=h)return;
 float2 uv=(float2(id.xy)+.5)/float2(w,h);
 if(JitterCorrection.w>.5){
  float4 texel=0;texel.z=GuideOffset.x;float fade=centerFade(uv,Rect,texel);float4 c=0;
  [branch]if(fade>0)c=JitterCorrection.z>.5?Output[id.xy]:Center.SampleLevel(LinearClamp,saturate((uv-Rect.xy)/Rect.zw),0);
  [branch]if(fade>=1){Output[id.xy]=finalColor(c);return;}
  float4 b=peripheralAt(uv,w,h);Output[id.xy]=finalColor(fade<=0?b:lerp(b,c,fade));return;
 }
 // Keep bilinear support around the feather boundary of compact output.
 if(JitterCorrection.z>.5){float2 edge=min(uv-Rect.xy,Rect.xy+Rect.zw-uv);
  [branch]if(all(edge>JitterCorrection.xy))return;}
 Output[id.xy+uint2(Dimensions.zw)]=peripheralAt(uv,w,h);
}
)HLSL";
}
