#include "fg_focus.hpp"
#include "fg_color_format.hpp"
#include "fg_timing.hpp"
#include "../shared/fg_guide_layout.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
namespace cheeky::fg {
namespace {
constexpr char prepare_shader[]=CHEEKY_FG_COMPACT_HLSL R"HLSL(
Texture2D<float4> Color:register(t0); Texture2D<float> Depth:register(t1); Texture2D<float2> Motion:register(t2);
RWTexture2D<float4> OutColor:register(u0); RWTexture2D<float> OutDepth:register(u1); RWTexture2D<float2> OutMotion:register(u2);
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
[numthreads(8,8,1)] void Main(uint3 id:SV_DispatchThreadID){
 uint w,h;
 if(Options.x>.5){OutColor.GetDimensions(w,h);if(id.x<w&&id.y<h){
  float2 uv=Rect.xy+(float2(id.xy)+.5)/float2(w,h)*Rect.zw;
  OutColor[id.xy]=encodeColor(samplePacked(Color,uv,AtlasCrop,AtlasSpans,AtlasTexel));}}
 if(Options.z<.5)return;
 OutMotion.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 float2 uv=Rect.xy+(float2(id.xy)+.5)/float2(w,h)*Rect.zw;
 uint mw,mh;Motion.GetDimensions(mw,mh);
 if(Options.y>.5){uint dw,dh;Depth.GetDimensions(dw,dh);uint2 d=min(uint2(uv*Dimensions.xy+GuideOffset.xy),uint2(dw,dh)-1);
  OutDepth[id.xy]=Depth.Load(int3(d,0));}
 uint2 m=min(uint2(uv*Dimensions.zw+GuideOffset.zw),uint2(mw,mh)-1);
 float2 mv=Motion.Load(int3(m,0));
 bool good=all(isfinite(mv))&&all(abs(mv)<65504.0);
 mv=clamp(mv*MotionAdjust.xy-JitterCorrection.xy+JitterCorrection.zw,-MotionAdjust.z,MotionAdjust.z);
 OutMotion[id.xy]=good?mv:float2(MotionAdjust.z,MotionAdjust.z);
})HLSL";
constexpr char peripheral_shader[]=CHEEKY_FG_COMPACT_HLSL R"HLSL(
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
bool motionAt(float2 uv,out float2 mv){uint w,h;Motion.GetDimensions(w,h);mv=Motion.Load(int3(min(uint2(saturate(uv)*float2(w,h)),uint2(w,h)-1),0));return all(isfinite(mv))&&all(abs(mv)<=2);}
float4 peripheralAt(float2 uv,uint w,uint h){
 if(Options.z<.5)return encodeColor(samplePacked(Current,uv,AtlasCrop,AtlasSpans,AtlasTexel));
 if(Options.x<.5)return encodeColor(samplePacked(Previous,uv,PriorCrop,PriorSpans,AtlasTexel));
 float2 cuv=uv,mv=0;bool valid=true;
 [unroll]for(uint i=0;i<4;++i){if(!inside(cuv)||!motionAt(cuv,mv)){valid=false;break;}cuv=uv-(1-Options.y)*mv;}
 valid=valid&&inside(cuv)&&motionAt(cuv,mv);float2 puv=cuv+mv;
 valid=valid&&all(abs((cuv+(1-Options.y)*mv-uv)*float2(w,h))<=2);
 float4 result=Options.x<1.5?samplePacked(Previous,uv,PriorCrop,PriorSpans,AtlasTexel):samplePacked(Current,uv,AtlasCrop,AtlasSpans,AtlasTexel);
 if(valid){float4 b=0;if(Options.x>=1.5)b=samplePacked(Current,cuv,AtlasCrop,AtlasSpans,AtlasTexel);
  if(inside(puv)){float4 a=samplePacked(Previous,puv,PriorCrop,PriorSpans,AtlasTexel);
   if(Options.x<1.5)result=a;
   else{uint dw,dh;Depth.GetDimensions(dw,dh);float z=Depth.Load(int3(min(uint2(cuv*float2(dw,dh)),uint2(dw,dh)-1),0));
    float4 clip=mul(ClipToPrevious,float4(cuv.x*2-1,1-cuv.y*2,z,1));
    PreviousDepth.GetDimensions(dw,dh);float old=PreviousDepth.Load(int3(min(uint2(puv*float2(dw,dh)),uint2(dw,dh)-1),0));
    bool agrees=all(isfinite(clip))&&clip.w>0.000001&&isfinite(old)&&abs(old-clip.z/clip.w)<=.02;
    result=agrees?lerp(a,b,Options.y):b;}
  }else if(Options.x>=1.5)result=b;
 }
 return encodeColor(result);
}
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
constexpr char compose_shader[]=R"HLSL(
Texture2D<float4> Periphery:register(t0);Texture2D<float4> Center:register(t1);RWTexture2D<float4> Output:register(u0);
cbuffer Params:register(b0){float4 Rect;float4 GuideOffset;float4 Dimensions;float4 MotionAdjust;float4 Options;float4 JitterCorrection;float4 AtlasCrop;float4 AtlasSpans;float4 AtlasTexel;float4 PriorCrop;float4 PriorSpans;row_major float4x4 ClipToPrevious;}
SamplerState LinearClamp:register(s0);
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 uint w,h;Output.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 float2 uv=(float2(id.xy)+.5)/float2(w,h);
 if(Options.z<.5){Output[id.xy]=Periphery.SampleLevel(LinearClamp,uv,0);return;}
 float2 local=(uv-Rect.xy)/Rect.zw;
 float2 edge=min(uv-Rect.xy,Rect.xy+Rect.zw-uv);
 float fade=Options.w>0?saturate(min(edge.x,edge.y)/Options.w):(all(local>=0)&&all(local<=1)?1:0);
 // Fully covered fovea must never access the periphery. Sampling it first
 // can propagate NaNs even when lerp's center weight is exactly one.
 [branch]if(fade>=1){Output[id.xy]=Center.SampleLevel(LinearClamp,saturate(local),0);return;}
 float4 base=Periphery.SampleLevel(LinearClamp,uv,0);
 [branch]if(fade<=0){Output[id.xy]=base;return;}
 Output[id.xy]=lerp(base,Center.SampleLevel(LinearClamp,saturate(local),0),fade);
})HLSL";
constexpr char pack_shader[]=R"HLSL(
Texture2D<float4> Periphery:register(t0);Texture2D<float4> Center:register(t1);RWTexture2D<float4> Output:register(u0);
cbuffer Params:register(b0){float4 Rect;float4 GuideOffset;float4 Dimensions;float4 MotionAdjust;float4 Options;float4 JitterCorrection;
 float4 AtlasCrop;float4 AtlasSpans;float4 AtlasTexel;float4 PriorCrop;float4 PriorSpans;row_major float4x4 ClipToPrevious;}
SamplerState LinearClamp:register(s0);
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 uint w,h;Output.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 if(id.y<uint(Dimensions.y)){
  if(id.x<uint(Dimensions.x)&&Options.x<.5)Output[id.xy]=Center.SampleLevel(LinearClamp,(float2(id.xy)+.5)/Dimensions.xy,0);
 }else if(id.x<uint(Dimensions.z)&&id.y<uint(Dimensions.y+Dimensions.w)&&Options.y<.5){
  Output[id.xy]=Periphery.SampleLevel(LinearClamp,(float2(id.x,id.y-Dimensions.y)+.5)/Dimensions.zw,0);
 }
})HLSL";
constexpr char recenter_shader[]=R"HLSL(
Texture2D<float4> Generated:register(t0);Texture2D<float4> Native:register(t1);RWTexture2D<float4> Output:register(u0);
cbuffer Params:register(b0){float4 Rect;float4 GuideOffset;float4 Dimensions;float4 MotionAdjust;float4 Options;float4 JitterCorrection;
 float4 AtlasCrop;float4 AtlasSpans;float4 AtlasTexel;float4 PriorCrop;float4 PriorSpans;row_major float4x4 ClipToPrevious;}
SamplerState LinearClamp:register(s0);
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 uint w,h;Output.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 float2 uv=(float2(id.xy)+.5)/float2(w,h),source=uv+MotionAdjust.xy;
 // Never stretch the last FG pixel across a newly exposed strip. Fade the
 // last texel into the current native crop, which covers the whole window.
 float2 edge=min(source,1-source)*float2(w,h)-.5;
 float weight=saturate(min(edge.x,edge.y));
 [branch]if(weight<=0){Output[id.xy]=Native.SampleLevel(LinearClamp,uv,0);return;}
 float4 c=Generated.SampleLevel(LinearClamp,source,0);
 [branch]if(weight>=1){Output[id.xy]=c;return;}
 Output[id.xy]=lerp(Native.SampleLevel(LinearClamp,uv,0),c,weight);
})HLSL";
void transition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 if(!r||a==b)return;D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;t.Transition.pResource=r;
 t.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;t.Transition.StateBefore=a;t.Transition.StateAfter=b;l->ResourceBarrier(1,&t);
}
bool allocate(ID3D12Device* d,unsigned w,unsigned h,DXGI_FORMAT fmt,ComPtr<ID3D12Resource>& out){
 if(out&&out->GetDesc().Width==w&&out->GetDesc().Height==h&&out->GetDesc().Format==fmt)return true;
 out.Reset();D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
 D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=w;rd.Height=h;
 rd.MipLevels=rd.DepthOrArraySize=1;rd.Format=fmt;rd.SampleDesc.Count=1;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 return SUCCEEDED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)));
}
struct Constants {float rect[4]{},offset[4]{},dimensions[4]{},motion[4]{},options[4]{},jitter[4]{},atlas_crop[4]{},atlas_spans[4]{},atlas_texel[4]{},prior_crop[4]{},prior_spans[4]{};Matrix previous{identity()};};
void bind(FocusPipeline& p,ID3D12Device* dev,ID3D12GraphicsCommandList* list,unsigned eye,unsigned operation,
 ID3D12PipelineState* state,const std::array<ID3D12Resource*,5>& in,const std::array<DXGI_FORMAT,5>& formats,
 const std::array<ID3D12Resource*,3>& out,const Constants& constants,ID3D12Resource* center=nullptr){
 const unsigned base=(eye*6+operation)*9;auto cpu=p.descriptors->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=base*p.stride;
 for(unsigned i=0;i<5;++i){D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=formats[i];v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
  v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;dev->CreateShaderResourceView(in[i],&v,cpu);cpu.ptr+=p.stride;}
 D3D12_SHADER_RESOURCE_VIEW_DESC cv{};cv.Format=center?center->GetDesc().Format:p.color_format;cv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
 cv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;cv.Texture2D.MipLevels=1;dev->CreateShaderResourceView(center,&cv,cpu);cpu.ptr+=p.stride;
 unsigned slot{};
 for(auto* r:out){D3D12_UNORDERED_ACCESS_VIEW_DESC v{};const DXGI_FORMAT null_formats[]{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G32_FLOAT};
  v.Format=r?r->GetDesc().Format:null_formats[slot];
  if(r&&slot==0&&typeless_color(v.Format))v.Format=p.color_format;
  ++slot;
  v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;dev->CreateUnorderedAccessView(r,nullptr,&v,cpu);cpu.ptr+=p.stride;}
 ID3D12DescriptorHeap* heaps[]{p.descriptors.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(p.root.Get());list->SetPipelineState(state);
 auto gpu=p.descriptors->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=base*p.stride;list->SetComputeRootDescriptorTable(0,gpu);
 list->SetComputeRoot32BitConstants(1,sizeof(Constants)/4,&constants,0);
}
}
DXGI_FORMAT depth_srv_format(DXGI_FORMAT f) noexcept {return guide_depth_format(f);}
DXGI_FORMAT motion_srv_format(DXGI_FORMAT f) noexcept {return guide_motion_format(f);}
void FocusPipeline::reset() noexcept {
 root.Reset();prepare.Reset();peripheral.Reset();peripheral_coarse.Reset();peripheral_coarse2.Reset();peripheral_coarse4.Reset();peripheral_blocks.Reset();peripheral_blocks2.Reset();peripheral_blocks4.Reset();compose.Reset();pack.Reset();recenter.Reset();descriptors.Reset();stride=0;coarse_output={};
 for(auto& eye:inputs)for(auto& input:eye)input={};
 for(unsigned i=0;i<2;++i){peripheral_output[i].Reset();full[i].Reset();previous_depth[i].Reset();history[i]=false;}
}
bool FocusPipeline::initialize(ID3D12Device* dev){
 if(root&&prepare&&peripheral&&peripheral_coarse&&peripheral_coarse2&&peripheral_coarse4&&peripheral_blocks&&peripheral_blocks2&&peripheral_blocks4&&compose&&pack&&recenter&&descriptors)return true;
 D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,6,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,6}};
 D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
 params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,sizeof(Constants)/4};
 D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
 sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;sampler.MaxLOD=D3D12_FLOAT32_MAX;
 D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=params;rs.NumStaticSamplers=1;rs.pStaticSamplers=&sampler;
 ComPtr<ID3DBlob> blob,error;
 if(!root&&(FAILED(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))||FAILED(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)))))return false;
 auto compile=[&](const char* source,ComPtr<ID3D12PipelineState>& target,const char* entry="Main"){ComPtr<ID3DBlob> code,errors;
  if(target)return true;
  if(FAILED(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,entry,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors))){
   if(errors)std::fprintf(stderr,"FG shader %s: %s\n",entry,static_cast<const char*>(errors->GetBufferPointer()));return false;}
  D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS={code->GetBufferPointer(),code->GetBufferSize()};return SUCCEEDED(dev->CreateComputePipelineState(&ps,IID_PPV_ARGS(&target)));};
 if(!compile(prepare_shader,prepare)||!compile(peripheral_shader,peripheral)||!compile(peripheral_shader,peripheral_coarse,"Coarse")||!compile(compose_shader,compose)||!compile(pack_shader,pack)||!compile(recenter_shader,recenter))return false;
 if(!compile(peripheral_shader,peripheral_coarse2,"Coarse2")||!compile(peripheral_shader,peripheral_coarse4,"Coarse4"))return false;
 if(!compile(peripheral_shader,peripheral_blocks,"Blocks")||!compile(peripheral_shader,peripheral_blocks2,"Blocks2")||!compile(peripheral_shader,peripheral_blocks4,"Blocks4"))return false;
 D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=108;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
 if(FAILED(dev->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&descriptors))))return false;
 stride=dev->GetDescriptorHandleIncrementSize(heap.Type);return true;
}
bool FocusPipeline::prepare_inputs(ID3D12Device* dev,ID3D12GraphicsCommandList* list,unsigned eye,unsigned pass,
 ID3D12Resource* color,const CheekyFgEye& g,const FocusRect& r,unsigned w,unsigned h,unsigned depth_bits,unsigned motion_bits,
 D3D12_RESOURCE_STATES color_state,GpuTimings* timings,float crop_motion_dx,float crop_motion_dy){
 if(eye>1||pass>1||!color||!w||!h||!r.w||!r.h||!initialize(dev))return false;
 const bool need_color=pass==0||g.peripheral_method==3;
 const bool need_motion=need_color||g.peripheral_method!=0;
 const bool need_depth=need_color||g.peripheral_method==2;
 const bool prepared=g.prepared_guides!=0;
 const bool adjust_motion=pass==0&&(crop_motion_dx!=0.f||crop_motion_dy!=0.f);
 auto& s=inputs[eye][pass];s.output_width=w;s.output_height=h;
 if(s.borrowed_guides!=prepared){s.depth.Reset();s.motion.Reset();}
 s.borrowed_guides=prepared;
 if(!need_color)s.color.Reset();if(!need_depth)s.depth.Reset();if(!need_motion)s.motion.Reset();
 if(!need_color&&!need_motion){
  if(timings)for(unsigned phase=0;phase<4;++phase)timings->preparation_stamp(list,eye,pass,phase);
  return true;
 }
 auto cd=color->GetDesc();if(color_layouts[eye].width){cd.Width=color_layouts[eye].width;cd.Height=color_layouts[eye].height;}
 unsigned dw{},dh{},mw{},mh{},gw{},gh{};
 DXGI_FORMAT df=DXGI_FORMAT_R32_FLOAT,mf=DXGI_FORMAT_R32G32_FLOAT;
 if(prepared){
  s.depth=need_depth?(pass?g.peripheral_depth:g.depth):nullptr;
  s.motion=need_motion?(pass?g.peripheral_motion:g.motion):nullptr;
  if((need_depth&&!s.depth)||(need_motion&&!s.motion))return false;
  const auto grid=guide_grid(g,g.depth_width,g.depth_height,g.motion_width,g.motion_height,pass);
  gw=grid.width;gh=grid.height;
  auto valid=[&](ID3D12Resource* resource,DXGI_FORMAT format){if(!resource)return true;const auto d=resource->GetDesc();
   return d.Width==gw&&d.Height==gh&&d.Format==format;};
  if(!valid(s.depth.Get(),depth_bits==16?DXGI_FORMAT_R16_FLOAT:DXGI_FORMAT_R32_FLOAT)||
     !valid(s.motion.Get(),motion_bits==16?DXGI_FORMAT_R16G16_FLOAT:DXGI_FORMAT_R32G32_FLOAT))return false;
 }else{
  if((need_depth&&!g.depth)||(need_motion&&!g.motion))return false;
  df=need_depth?depth_srv_format(g.depth->GetDesc().Format):DXGI_FORMAT_R32_FLOAT;
  mf=need_motion?motion_srv_format(g.motion->GetDesc().Format):DXGI_FORMAT_R32G32_FLOAT;
  if(df==DXGI_FORMAT_UNKNOWN||mf==DXGI_FORMAT_UNKNOWN)return false;
  auto active=[](ID3D12Resource* resource,unsigned bx,unsigned by,unsigned aw,unsigned ah,unsigned& ow,unsigned& oh){
   if(!resource){ow=oh=0;return true;}const auto d=resource->GetDesc();if(bx>=d.Width||by>=d.Height)return false;
   ow=std::min(aw?aw:unsigned(d.Width),unsigned(d.Width)-bx);oh=std::min(ah?ah:d.Height,d.Height-by);return ow&&oh;
  };
  if(!active(need_depth?g.depth:nullptr,g.depth_base_x,g.depth_base_y,g.depth_width,g.depth_height,dw,dh)||
     !active(need_motion?g.motion:nullptr,g.motion_base_x,g.motion_base_y,g.motion_width,g.motion_height,mw,mh))return false;
  const unsigned sw=need_depth?std::min(dw,mw):mw,sh=need_depth?std::min(dh,mh):mh;
  gw=std::clamp(unsigned(std::round(double(sw)*r.w/cd.Width*std::min(1.,double(w)/r.w))),1u,w);
  gh=std::clamp(unsigned(std::round(double(sh)*r.h/cd.Height*std::min(1.,double(h)/r.h))),1u,h);
 }
 if(prepared&&adjust_motion&&(!need_motion||!allocate(dev,gw,gh,motion_bits==16?DXGI_FORMAT_R16G16_FLOAT:DXGI_FORMAT_R32G32_FLOAT,s.corrected_motion)))return false;
 if((need_color&&!allocate(dev,w,h,color_format,s.color))||
    (!prepared&&need_depth&&!allocate(dev,gw,gh,depth_bits==16?DXGI_FORMAT_R16_FLOAT:DXGI_FORMAT_R32_FLOAT,s.depth))||
    (!prepared&&need_motion&&!allocate(dev,gw,gh,motion_bits==16?DXGI_FORMAT_R16G16_FLOAT:DXGI_FORMAT_R32G32_FLOAT,s.motion)))return false;
 if(prepared&&!need_color){
  if(timings)for(unsigned phase=0;phase<4;++phase)timings->preparation_stamp(list,eye,pass,phase);
  return true;
 }
 Constants c{};color_layouts[eye].constants(c.atlas_crop,c.atlas_spans,c.atlas_texel);
 c.rect[0]=float(r.x)/cd.Width;c.rect[1]=float(r.y)/cd.Height;c.rect[2]=float(r.w)/cd.Width;c.rect[3]=float(r.h)/cd.Height;
 c.offset[0]=float(g.depth_base_x);c.offset[1]=float(g.depth_base_y);c.offset[2]=float(g.motion_base_x);c.offset[3]=float(g.motion_base_y);
 c.dimensions[0]=float(dw);c.dimensions[1]=float(dh);c.dimensions[2]=float(mw);c.dimensions[3]=float(mh);
 if(!prepared){c.motion[0]=g.motion_scale_x/(mw*c.rect[2]);c.motion[1]=g.motion_scale_y/(mh*c.rect[3]);
  c.motion[2]=motion_bits==16?65504.f:3.402823466e38f;c.jitter[0]=g.jitter_correction_x/c.rect[2];c.jitter[1]=g.jitter_correction_y/c.rect[3];}
 c.jitter[2]=crop_motion_dx;c.jitter[3]=crop_motion_dy;
 c.options[0]=need_color?1.f:0.f;c.options[1]=!prepared&&need_depth?1.f:0.f;c.options[2]=!prepared&&need_motion?1.f:0.f;
 c.options[3]=srgb_color(cd.Format)&&color_format==DXGI_FORMAT_R8G8B8A8_UNORM?1.f:0.f;
 auto* out_depth=prepared?nullptr:s.depth.Get();auto* out_motion=prepared?nullptr:s.motion.Get();
 if(timings)timings->preparation_stamp(list,eye,pass,0);
 for(auto* out:{s.color.Get(),out_depth,out_motion})transition(list,out,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 if(need_color&&color_state==D3D12_RESOURCE_STATE_COMMON)transition(list,color,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 bind(*this,dev,list,eye,pass,prepare.Get(),{need_color?color:nullptr,!prepared&&need_depth?g.depth:nullptr,!prepared&&need_motion?g.motion:nullptr,nullptr,nullptr},
  {cd.Format,df,mf,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32_FLOAT},{s.color.Get(),out_depth,out_motion},c);
 if(timings)timings->preparation_stamp(list,eye,pass,1);
 list->Dispatch((std::max(need_color?w:0u,prepared?0u:gw)+7)/8,(std::max(need_color?h:0u,prepared?0u:gh)+7)/8,1);
 if(timings)timings->preparation_stamp(list,eye,pass,2);
 for(auto* out:{s.color.Get(),out_depth,out_motion})transition(list,out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
 if(need_color&&color_state==D3D12_RESOURCE_STATE_COMMON)transition(list,color,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 if(prepared&&adjust_motion){
  // Prepared NGX guides are immutable leased input resources. Correct a
  // worker-owned copy, never modify the producer's captured motion texture.
  Constants adjusted{};
  adjusted.rect[2]=adjusted.rect[3]=1.f;
  adjusted.dimensions[2]=float(gw);adjusted.dimensions[3]=float(gh);
  adjusted.motion[0]=adjusted.motion[1]=1.f;
  adjusted.motion[2]=motion_bits==16?65504.f:3.402823466e38f;
  adjusted.jitter[2]=crop_motion_dx;adjusted.jitter[3]=crop_motion_dy;
  adjusted.options[2]=1.f;
  transition(list,s.corrected_motion.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  bind(*this,dev,list,eye,4,prepare.Get(),{nullptr,nullptr,g.motion,nullptr,nullptr},
   {cd.Format,df,mf,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32_FLOAT},
   {nullptr,nullptr,s.corrected_motion.Get()},adjusted);
  list->Dispatch((gw+7)/8,(gh+7)/8,1);
  transition(list,s.corrected_motion.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
  s.motion=s.corrected_motion;
 }
 if(timings)timings->preparation_stamp(list,eye,pass,3);
 return true;
}
bool FocusPipeline::recenter_frame(ID3D12Device* dev,ID3D12GraphicsCommandList* list,unsigned eye,
 ID3D12Resource* generated,float shift_x,float shift_y){
 if(eye>1||!generated||!std::isfinite(shift_x)||!std::isfinite(shift_y))return false;
 auto& s=inputs[eye][0];if(!s.color||!initialize(dev))return false;
 const auto d=generated->GetDesc(),native=s.color->GetDesc();
 if(d.Width!=native.Width||d.Height!=native.Height||d.Format!=color_format||native.Format!=color_format||
    !allocate(dev,unsigned(d.Width),d.Height,color_format,s.recentered_color))return false;
 Constants c{};c.motion[0]=shift_x;c.motion[1]=shift_y;
 transition(list,generated,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 if(s.color.Get()!=generated)transition(list,s.color.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 transition(list,s.recentered_color.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 // Dedicated descriptor slots: prepared-guide correction can also be recorded
 // on this list, and descriptors must survive until the whole list executes.
 bind(*this,dev,list,eye,5,recenter.Get(),{generated,s.color.Get(),nullptr,nullptr,nullptr},
  {color_format,color_format,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32_FLOAT},{s.recentered_color.Get(),nullptr,nullptr},c);
 list->Dispatch((unsigned(d.Width)+7)/8,(d.Height+7)/8,1);
 transition(list,s.recentered_color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
 transition(list,generated,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 if(s.color.Get()!=generated)transition(list,s.color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 return true;
}
bool FocusPipeline::generate_periphery(ID3D12Device* dev,ID3D12GraphicsCommandList* list,unsigned eye,
 ID3D12Resource* previous,ID3D12Resource* current,const CheekyFgEye& g,const Camera& camera,D3D12_RESOURCE_STATES color_state,
 ID3D12Resource* destination,D3D12_RESOURCE_STATES destination_state,const CompactLayout* layout,const FinalOutput* final){
 if(eye>1)return false;auto& s=inputs[eye][1];
 if(!s.output_width||!s.output_height||!previous||!current||g.peripheral_method>2)return false;
 const bool need_motion=g.peripheral_method!=0,need_depth=g.peripheral_method==2;
 if((need_motion&&!s.motion)||(need_depth&&!s.depth))return false;
 if(destination){
  const auto d=destination->GetDesc();
  const bool dimensions=final?(final->width&&final->height&&d.Width==final->width&&d.Height==final->height&&
      final->region.w&&final->region.h&&(final->center_direct||final->center)):
      (layout&&layout->packed()&&d.Width==layout->stored_width()&&d.Height==layout->stored_height()&&
      layout->context_width==s.output_width&&layout->context_height==s.output_height);
  if(!dimensions||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||!writable_color(d.Format,color_format)||
     !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)||
     destination==previous||destination==current||destination==s.depth.Get()||destination==s.motion.Get()||destination==previous_depth[eye].Get())return false;
 }else if(!allocate(dev,s.output_width,s.output_height,color_format,peripheral_output[eye]))return false;
 if(need_depth&&(!previous_depth[eye]||previous_depth[eye]->GetDesc().Width!=s.depth->GetDesc().Width||
     previous_depth[eye]->GetDesc().Height!=s.depth->GetDesc().Height))history[eye]=false;
 Constants c{};color_layouts[eye].constants(c.atlas_crop,c.atlas_spans,c.atlas_texel);float unused[4];previous_layouts[eye].constants(c.prior_crop,c.prior_spans,unused);c.options[0]=float(g.peripheral_method);c.options[1]=.5f;
 c.options[2]=(!need_depth||history[eye])&&!g.reset?1.f:0.f;c.previous=camera.clip_to_prev;
 c.options[3]=srgb_color(current->GetDesc().Format)&&color_format==DXGI_FORMAT_R8G8B8A8_UNORM?1.f:0.f;
 auto* mv=need_motion?s.motion.Get():nullptr;auto* depth=need_depth?s.depth.Get():nullptr;
 auto* old=need_depth?(history[eye]?previous_depth[eye].Get():depth):nullptr;
 c.dimensions[0]=float(s.output_width);c.dimensions[1]=float(s.output_height);
 const bool coarse=final&&final->shading_mode!=0&&(s.output_width<final->width||s.output_height<final->height);
 const bool blocks=coarse&&final->shading_mode==1;
 coarse_output[eye]=coarse;unsigned cells=8;
 if(final){
  c.dimensions[0]=float(final->width);c.dimensions[1]=float(final->height);
  c.rect[0]=float(final->region.x)/final->width;c.rect[1]=float(final->region.y)/final->height;
  c.rect[2]=float(final->region.w)/final->width;c.rect[3]=float(final->region.h)/final->height;
  c.offset[0]=final->region.feather;c.offset[1]=final->linear_sdr?1.f:0.f;
  c.jitter[2]=final->center_direct?1.f:0.f;c.jitter[3]=1.f;
  if(coarse){c.dimensions[2]=float(s.output_width);c.dimensions[3]=float(s.output_height);
   c.motion[0]=color_format==DXGI_FORMAT_R8G8B8A8_UNORM?8.f:color_format==DXGI_FORMAT_R10G10B10A2_UNORM?10.f:16.f;
   cells=std::clamp(unsigned(32.*std::min(double(s.output_width)/final->width,double(s.output_height)/final->height)),1u,8u);c.motion[1]=float(cells);}
 }else if(destination){
  c.dimensions[3]=float(layout->center_height);
  c.rect[0]=float(layout->x)/layout->width;c.rect[1]=float(layout->y)/layout->height;
  c.rect[2]=float(layout->w)/layout->width;c.rect[3]=float(layout->h)/layout->height;
  c.jitter[0]=layout->feather+1.5f/s.output_width;c.jitter[1]=layout->feather+1.5f/s.output_height;c.jitter[2]=1.f;
 }
 auto* output=destination?destination:peripheral_output[eye].Get();
 transition(list,output,destination?destination_state:D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 for(auto* source:{mv,depth})transition(list,source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 if(color_state==D3D12_RESOURCE_STATE_COMMON){transition(list,previous,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if(current!=previous)transition(list,current,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
 if(old!=depth)transition(list,old,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 auto* center=final&&!final->center_direct?final->center:nullptr;
 transition(list,center,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 auto* shader=coarse?peripheral_coarse.Get():peripheral.Get();
 if(coarse&&s.output_width*2==final->width&&s.output_height*2==final->height){shader=peripheral_coarse2.Get();cells=8;}
 if(coarse&&s.output_width*4==final->width&&s.output_height*4==final->height){shader=peripheral_coarse4.Get();cells=8;}
 if(blocks){shader=peripheral_blocks.Get();
  if(s.output_width*2==final->width&&s.output_height*2==final->height)shader=peripheral_blocks2.Get();
  if(s.output_width*4==final->width&&s.output_height*4==final->height)shader=peripheral_blocks4.Get();}
 bind(*this,dev,list,eye,2,shader,{previous,current,mv,depth,old},
  {previous->GetDesc().Format,current->GetDesc().Format,mv?mv->GetDesc().Format:DXGI_FORMAT_R32G32_FLOAT,
   depth?depth->GetDesc().Format:DXGI_FORMAT_R32_FLOAT,old?old->GetDesc().Format:DXGI_FORMAT_R32_FLOAT},{output,nullptr,nullptr},c,center);
 if(blocks)list->Dispatch((s.output_width+7)/8,(s.output_height+7)/8,1);
 else if(coarse)list->Dispatch((s.output_width+cells)/cells,(s.output_height+cells)/cells,1);
 else list->Dispatch((unsigned(c.dimensions[0])+7)/8,(unsigned(c.dimensions[1])+7)/8,1);
 transition(list,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
 transition(list,center,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 for(auto* source:{mv,depth})transition(list,source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 if(color_state==D3D12_RESOURCE_STATE_COMMON){transition(list,previous,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
  if(current!=previous)transition(list,current,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);}
 if(old!=depth)transition(list,old,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 return true;
}
bool FocusPipeline::compose_frame(ID3D12Device* dev,ID3D12GraphicsCommandList* list,unsigned eye,
 ID3D12Resource* center,ID3D12Resource* periphery,const FocusRect& r,unsigned w,unsigned h,
 ID3D12Resource* destination,D3D12_RESOURCE_STATES destination_state){
 if(!center||!periphery||eye>=2)return false;
 if(destination){const auto d=destination->GetDesc();
  if(destination==center||destination==periphery||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||
     d.Width!=w||d.Height!=h||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||
     d.Format!=color_format||!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return false;
 }else if(!allocate(dev,w,h,color_format,full[eye]))return false;
 Constants c{};c.rect[0]=float(r.x)/w;c.rect[1]=float(r.y)/h;c.rect[2]=float(r.w)/w;c.rect[3]=float(r.h)/h;c.options[2]=r.active?1.f:0.f;c.options[3]=r.feather;
 auto* output=destination?destination:full[eye].Get();
 transition(list,output,destination?destination_state:D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 transition(list,center,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 if(periphery!=center)transition(list,periphery,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 bind(*this,dev,list,eye,3,compose.Get(),{periphery,center,center,center,center},
  {periphery->GetDesc().Format,center->GetDesc().Format,center->GetDesc().Format,center->GetDesc().Format,center->GetDesc().Format},{output,nullptr,nullptr},c);
 list->Dispatch((w+7)/8,(h+7)/8,1);
 transition(list,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,destination?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COMMON);
 transition(list,center,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 if(periphery!=center)transition(list,periphery,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 return true;
}
bool FocusPipeline::pack_frame(ID3D12Device* dev,ID3D12GraphicsCommandList* list,unsigned eye,ID3D12Resource* center,ID3D12Resource* context,
 ID3D12Resource* destination,D3D12_RESOURCE_STATES state,const CompactLayout& layout,bool center_direct,bool context_direct){
 if(!layout.packed()||!destination||eye>=2||(!center_direct&&!center)||(!context_direct&&!context))return false;
 const auto target=destination->GetDesc();
 if(target.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||target.Width!=layout.stored_width()||target.Height!=layout.stored_height()||
    target.DepthOrArraySize!=1||target.MipLevels!=1||target.SampleDesc.Count!=1||target.Format!=color_format||
    (!center_direct&&center==destination)||(!context_direct&&context==destination))return false;
 if(center_direct&&context_direct)return true;
 auto exact=[&](ID3D12Resource* source,unsigned w,unsigned h){const auto d=source->GetDesc();
  return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width==w&&d.Height==h&&d.DepthOrArraySize==1&&
   d.MipLevels==1&&d.SampleDesc.Count==1&&d.Format==target.Format;};
 if((center_direct||exact(center,layout.center_width,layout.center_height))&&
    (context_direct||exact(context,layout.context_width,layout.context_height))){
  // No filtering is needed when NGX/context dimensions already match the atlas.
  transition(list,destination,state,D3D12_RESOURCE_STATE_COPY_DEST);
  auto copy=[&](ID3D12Resource* source,unsigned y){
   transition(list,source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
   D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   to.pResource=destination;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   list->CopyTextureRegion(&to,0,y,0,&from,nullptr);
   transition(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);};
  if(!center_direct)copy(center,0);if(!context_direct)copy(context,layout.center_height);
  transition(list,destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);return true;
 }
 if(!(target.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return false;
 Constants c;c.dimensions[0]=float(layout.center_width);c.dimensions[1]=float(layout.center_height);
 c.dimensions[2]=float(layout.context_width);c.dimensions[3]=float(layout.context_height);
 c.options[0]=center_direct?1.f:0.f;c.options[1]=context_direct?1.f:0.f;
 if(center_direct)center=nullptr;if(context_direct)context=nullptr;
 transition(list,destination,state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 transition(list,center,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 if(context!=center)transition(list,context,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 bind(*this,dev,list,eye,3,pack.Get(),{context,center,nullptr,nullptr,nullptr},
  {color_format,color_format,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32_FLOAT},{destination,nullptr,nullptr},c);
 list->Dispatch((layout.stored_width()+7)/8,(layout.stored_height()+7)/8,1);
 transition(list,destination,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
 transition(list,center,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 if(context!=center)transition(list,context,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 return true;
}
bool FocusPipeline::save_depth(ID3D12Device* dev,ID3D12GraphicsCommandList* list,unsigned eye){
 auto* source=inputs[eye][1].depth.Get();if(!source)return true;auto desc=source->GetDesc();
 if(!allocate(dev,unsigned(desc.Width),desc.Height,desc.Format,previous_depth[eye]))return false;
 transition(list,source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
 transition(list,previous_depth[eye].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
 list->CopyResource(previous_depth[eye].Get(),source);
 transition(list,previous_depth[eye].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
 transition(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);return true;
}
}
