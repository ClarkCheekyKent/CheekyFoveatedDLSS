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
// One coarse tile plus a one-sample halo. Threads share the expensive warp
// results; expansion and the final center feather need only arithmetic/stores.
groupshared float4 TileColor[81];
uint pixelBoundary(uint cell,uint full,uint grid){
 uint numerator=uint(max(0,int(2*cell*full)-int(grid+full)));
 return min(full,(numerator+2*grid-1)/(2*grid));
}
float4 storedColor(float4 c){
 if(MotionAdjust.x<9)return round(saturate(c)*255)/255;
 if(MotionAdjust.x<11){float4 levels=float4(1023,1023,1023,3);return round(saturate(c)*levels)/levels;}
 return f16tof32(f32tof16(c));
}
[numthreads(8,8,1)] void Coarse(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
 uint2 full=uint2(Dimensions.xy),grid=uint2(Dimensions.zw);
 uint cells=uint(MotionAdjust.y),side=cells+1;uint2 base=group.xy*cells;
 for(uint index=lane;index<side*side;index+=64){
  int2 q=clamp(int2(base)+int2(index%side,index/side)-1,int2(0,0),int2(grid)-1);
  float2 uv=(float2(q)+.5)/float2(grid);
  float2 edge=min(uv-Rect.xy,Rect.xy+Rect.zw-uv);
  bool covered=all(edge>GuideOffset.x+1.5/float2(grid));
  float4 c=0;[branch]if(!covered)c=storedColor(peripheralAt(uv,grid.x,grid.y));
  TileColor[index]=finalColor(c);
 }
 GroupMemoryBarrierWithGroupSync();
 uint2 first=uint2(pixelBoundary(base.x,full.x,grid.x),pixelBoundary(base.y,full.y,grid.y));
 uint2 end=uint2(pixelBoundary(base.x+cells,full.x,grid.x),pixelBoundary(base.y+cells,full.y,grid.y));
 uint2 size=end-first;uint count=size.x*size.y;
 for(uint index=lane;index<count;index+=64){
  // Avoid variable integer division per final pixel. Correct an approximate
  // reciprocal at row boundaries so odd-sized tiles still own every pixel.
  uint row=min(uint(float(index)/float(size.x)),size.y-1),rowStart=row*size.x;
  if(rowStart>index){--row;rowStart-=size.x;}else if(index-rowStart>=size.x){++row;rowStart+=size.x;}
  uint2 p=first+uint2(index-rowStart,row);float2 uv=(float2(p)+.5)/float2(full);
  float4 texel=0;texel.z=GuideOffset.x;float fade=centerFade(uv,Rect,texel);float4 c=0;
  [branch]if(fade>0){[branch]if(JitterCorrection.z>.5)c=Output[p];else c=Center.SampleLevel(LinearClamp,saturate((uv-Rect.xy)/Rect.zw),0);}
  [branch]if(fade>=1){Output[p]=finalColor(c);continue;}
  // Integer cell ownership avoids gaps/overlaps at odd-size tile boundaries.
  float2 samplePosition=uv*float2(grid)+.5;
  uint2 cell=clamp(uint2(samplePosition),base,base+cells-1),local=cell-base;
  float2 f=saturate(samplePosition-float2(cell));uint slot=local.y*side+local.x;
  float4 a=lerp(TileColor[slot],TileColor[slot+1],f.x);
  float4 b=lerp(TileColor[slot+side],TileColor[slot+side+1],f.x);
  float4 peripheral=lerp(a,b,f.y);
  Output[p]=fade<=0?peripheral:lerp(peripheral,finalColor(c),fade);
 }
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
 if(fade<1){float2 grid=max(float2(1,1),round(float2(w,h)*Dimensions.w));
  float2 q=(floor(uv*grid)+.5)/grid;b=peripheralAt(q,w,h);}
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
void coarseFixed(uint2 group,uint lane,uint rate){
 uint2 full=uint2(Dimensions.xy),grid=uint2(Dimensions.zw),base=group*8;
 for(uint index=lane;index<81;index+=64){
  int2 q=clamp(int2(base)+int2(index%9,index/9)-1,int2(0,0),int2(grid)-1);
  float2 uv=(float2(q)+.5)/float2(grid);float2 edge=min(uv-Rect.xy,Rect.xy+Rect.zw-uv);
  float4 c=0;[branch]if(!all(edge>GuideOffset.x+1.5/float2(grid)))c=storedColor(peripheralAt(uv,grid.x,grid.y));
  TileColor[index]=finalColor(c);
 }
 GroupMemoryBarrierWithGroupSync();
 uint side=8*rate;int2 start=int2(group*side)-int(rate/2);
 [loop]for(uint part=0;part<rate*rate;++part){
  uint pixelIndex=lane+part*64;int2 pixelPosition=start+int2(pixelIndex%side,pixelIndex/side);
  if(any(pixelPosition<0)||any(pixelPosition>=int2(full)))continue;
  uint2 p=uint2(pixelPosition);float2 uv=(float2(p)+.5)/float2(full);
  float4 texel=0;texel.z=GuideOffset.x;float fade=centerFade(uv,Rect,texel);float4 c=0;
  [branch]if(fade>0){[branch]if(JitterCorrection.z>.5)c=Output[p];else c=Center.SampleLevel(LinearClamp,saturate((uv-Rect.xy)/Rect.zw),0);}
  [branch]if(fade>=1){Output[p]=finalColor(c);continue;}
  uint2 cell=(p+rate/2)/rate,local=cell-base;
  float2 f=(float2((p+rate/2)%rate)+.5)/rate;uint slot=local.y*9+local.x;
  float4 a=lerp(TileColor[slot],TileColor[slot+1],f.x),b=lerp(TileColor[slot+9],TileColor[slot+10],f.x);
  float4 peripheral=lerp(a,b,f.y);Output[p]=fade<=0?peripheral:lerp(peripheral,finalColor(c),fade);
 }
}
[numthreads(8,8,1)]void Coarse2(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){coarseFixed(group.xy,lane,2);}
[numthreads(8,8,1)]void Coarse4(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){coarseFixed(group.xy,lane,4);}
uint blockBoundary(uint cell,uint full,uint grid){
 uint numerator=uint(max(0,int(2*cell*full)-int(grid)));
 return min(full,(numerator+2*grid-1)/(2*grid));
}
void blockPixel(uint2 p,float4 peripheral){
 float2 uv=(float2(p)+.5)/Dimensions.xy;float4 texel=0;texel.z=GuideOffset.x;
 float fade=centerFade(uv,Rect,texel);
 [branch]if(fade<=0){Output[p]=peripheral;return;}
 float4 c;[branch]if(JitterCorrection.z>.5)c=Output[p];else c=Center.SampleLevel(LinearClamp,saturate((uv-Rect.xy)/Rect.zw),0);
 // Avoid reading an invalid/undefined periphery contribution when this
 // pixel is entirely within the center, including direct XR shading paths.
 [branch]if(fade>=1){Output[p]=finalColor(c);return;}
 Output[p]=lerp(peripheral,finalColor(c),fade);
}
void shadeBlock(uint2 q,uint rate){
 uint2 full=uint2(Dimensions.xy),grid=uint2(Dimensions.zw);if(any(q>=grid))return;
 uint2 first,end;
 if(rate>0){first=q*rate;end=min(full,first+rate);}
 else{first=uint2(blockBoundary(q.x,full.x,grid.x),blockBoundary(q.y,full.y,grid.y));
  end=uint2(blockBoundary(q.x+1,full.x,grid.x),blockBoundary(q.y+1,full.y,grid.y));}
 if(any(end<=first))return;
 float2 edge=min((float2(first)+.5)/float2(full)-Rect.xy,Rect.xy+Rect.zw-(float2(end)-.5)/float2(full));
 float4 b=0;[branch]if(!all(edge>=GuideOffset.x))b=finalColor(storedColor(peripheralAt((float2(q)+.5)/float2(grid),grid.x,grid.y)));
 [loop]for(uint y=first.y;y<end.y;++y)[loop]for(uint x=first.x;x<end.x;++x)blockPixel(uint2(x,y),b);
}
[numthreads(8,8,1)]void Blocks(uint3 id:SV_DispatchThreadID){shadeBlock(id.xy,0);}
[numthreads(8,8,1)]void Blocks2(uint3 id:SV_DispatchThreadID){shadeBlock(id.xy,2);}
[numthreads(8,8,1)]void Blocks4(uint3 id:SV_DispatchThreadID){shadeBlock(id.xy,4);}
)HLSL";
}
