#include "fg_focus.hpp"
#include "../shared/fg_interpolation_shader.hpp"
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
 if(Options.y<.5&&Options.z<.5)return;
 // Depth and motion share one grid; either may be absent (depth-only optical flow).
 if(Options.z>.5)OutMotion.GetDimensions(w,h);else OutDepth.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 float2 uv=Rect.xy+(float2(id.xy)+.5)/float2(w,h)*Rect.zw;
 if(Options.y>.5){uint dw,dh;Depth.GetDimensions(dw,dh);uint2 d=min(uint2(uv*Dimensions.xy+GuideOffset.xy),uint2(dw,dh)-1);
  OutDepth[id.xy]=Depth.Load(int3(d,0));}
 if(Options.z<.5)return;
 uint mw,mh;Motion.GetDimensions(mw,mh);
 uint2 m=min(uint2(uv*Dimensions.zw+GuideOffset.zw),uint2(mw,mh)-1);
 float2 mv=Motion.Load(int3(m,0));
 bool good=all(isfinite(mv))&&all(abs(mv)<65504.0);
 mv=clamp(mv*MotionAdjust.xy-JitterCorrection.xy+JitterCorrection.zw,-MotionAdjust.z,MotionAdjust.z);
 OutMotion[id.xy]=good?mv:float2(MotionAdjust.z,MotionAdjust.z);
})HLSL";
constexpr char compose_shader[]=R"HLSL(
Texture2D<float4> Periphery:register(t0);Texture2D<float4> Center:register(t1);RWTexture2D<float4> Output:register(u0);
cbuffer Params:register(b0){float4 Rect;float4 GuideOffset;float4 Dimensions;float4 MotionAdjust;float4 Options;float4 JitterCorrection;float4 AtlasCrop;float4 AtlasSpans;float4 AtlasTexel;float4 PriorCrop;float4 PriorSpans;row_major float4x4 ClipToPrevious;}
SamplerState LinearClamp:register(s0);
float4 composeColor(float4 c){if(Options.y>.5)c.rgb=lerp(c.rgb/12.92,pow((max(c.rgb,0)+.055)/1.055,2.4),step(.04045,c.rgb));return c;}
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 uint w,h;Output.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 float2 uv=(float2(id.xy)+.5)/float2(w,h);
 if(Options.z<.5){Output[id.xy]=composeColor(Periphery.SampleLevel(LinearClamp,uv,0));return;}
 float2 local=(uv-Rect.xy)/Rect.zw;
 float2 edge=min(uv-Rect.xy,Rect.xy+Rect.zw-uv);
 float fade=Options.w>0?saturate(min(edge.x,edge.y)/Options.w):(all(local>=0)&&all(local<=1)?1:0);
 // Fully covered fovea must never access the periphery. Sampling it first
 // can propagate NaNs even when lerp's center weight is exactly one.
 [branch]if(fade>=1){Output[id.xy]=composeColor(Center.SampleLevel(LinearClamp,saturate(local),0));return;}
 float4 base=Periphery.SampleLevel(LinearClamp,uv,0);
 [branch]if(fade<=0){Output[id.xy]=composeColor(base);return;}
 Output[id.xy]=composeColor(lerp(base,Center.SampleLevel(LinearClamp,saturate(local),0),fade));
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
 const unsigned base=(eye*7+operation)*9;auto cpu=p.descriptors->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=base*p.stride;
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
 for(auto& eye:optical)for(auto& session:eye)session.reset();
 root.Reset();prepare.Reset();peripheral.Reset();center_shader.Reset();compose.Reset();pack.Reset();recenter.Reset();descriptors.Reset();stride=0;
 for(auto& eye:inputs)for(auto& input:eye)input={};
 for(unsigned i=0;i<2;++i){peripheral_output[i].Reset();full[i].Reset();previous_depth[i].Reset();history[i]=false;center_output[i].Reset();previous_center_depth[i].Reset();center_history[i]=false;}
}
bool FocusPipeline::initialize(ID3D12Device* dev){
 if(root&&prepare&&center_shader&&peripheral&&compose&&pack&&recenter&&descriptors)return true;
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
 if(!compile(interpolation_shader,center_shader,"CenterMain")||!compile(prepare_shader,prepare)||!compile(interpolation_shader,peripheral)||!compile(compose_shader,compose)||!compile(pack_shader,pack)||!compile(recenter_shader,recenter))return false;
 D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=126;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
 if(FAILED(dev->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&descriptors))))return false;
 stride=dev->GetDescriptorHandleIncrementSize(heap.Type);return true;
}
bool FocusPipeline::prepare_inputs(ID3D12Device* dev,ID3D12GraphicsCommandList* list,unsigned eye,unsigned pass,
 ID3D12Resource* color,const CheekyFgEye& g,const FocusRect& r,unsigned w,unsigned h,
 D3D12_RESOURCE_STATES color_state,GpuTimings* timings,float crop_motion_dx,float crop_motion_dy){
 if(eye>1||pass>1||!color||!w||!h||!r.w||!r.h||!initialize(dev))return false;
 const unsigned method=pass?g.peripheral_method:g.center_method;
 // Only NGX (DLSS) reads the prepared color; the shader methods and optical
 // flow sample the original images. Matches guide_grid's depth/motion choice.
 const bool need_color=pass==0?method==0:method==3;
 const bool need_motion=pass==0?method<=2:(method>=1&&method<=3);
 const bool need_depth=pass==0?(method==0||method==2||method==5):(method==2||method==3||method==5);
 const bool prepared=g.prepared_guides!=0;
 const bool adjust_motion=pass==0&&(crop_motion_dx!=0.f||crop_motion_dy!=0.f);
 auto& s=inputs[eye][pass];s.output_width=w;s.output_height=h;
 if(method==cheeky_fg_optical_flow){s.depth.Reset();s.motion.Reset();return true;}
 if(s.borrowed_guides!=prepared){s.depth.Reset();s.motion.Reset();}
 s.borrowed_guides=prepared;
 if(!need_color)s.color.Reset();if(!need_depth)s.depth.Reset();if(!need_motion)s.motion.Reset();
 if(!need_color&&!need_motion&&!need_depth){
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
  if(!valid(s.depth.Get(),DXGI_FORMAT_R32_FLOAT)||!valid(s.motion.Get(),DXGI_FORMAT_R32G32_FLOAT))return false;
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
  const unsigned sw=need_depth?(need_motion?std::min(dw,mw):dw):mw,sh=need_depth?(need_motion?std::min(dh,mh):dh):mh;
  gw=std::clamp(unsigned(std::round(double(sw)*r.w/cd.Width*std::min(1.,double(w)/r.w))),1u,w);
  gh=std::clamp(unsigned(std::round(double(sh)*r.h/cd.Height*std::min(1.,double(h)/r.h))),1u,h);
 }
 if(prepared&&adjust_motion&&(!need_motion||!allocate(dev,gw,gh,DXGI_FORMAT_R32G32_FLOAT,s.corrected_motion)))return false;
 if((need_color&&!allocate(dev,w,h,color_format,s.color))||
    (!prepared&&need_depth&&!allocate(dev,gw,gh,DXGI_FORMAT_R32_FLOAT,s.depth))||
    (!prepared&&need_motion&&!allocate(dev,gw,gh,DXGI_FORMAT_R32G32_FLOAT,s.motion)))return false;
 if(prepared&&!need_color){
  if(timings)for(unsigned phase=0;phase<4;++phase)timings->preparation_stamp(list,eye,pass,phase);
  return true;
 }
 Constants c{};color_layouts[eye].constants(c.atlas_crop,c.atlas_spans,c.atlas_texel);
 c.rect[0]=float(r.x)/cd.Width;c.rect[1]=float(r.y)/cd.Height;c.rect[2]=float(r.w)/cd.Width;c.rect[3]=float(r.h)/cd.Height;
 c.offset[0]=float(g.depth_base_x);c.offset[1]=float(g.depth_base_y);c.offset[2]=float(g.motion_base_x);c.offset[3]=float(g.motion_base_y);
 c.dimensions[0]=float(dw);c.dimensions[1]=float(dh);c.dimensions[2]=float(mw);c.dimensions[3]=float(mh);
 if(!prepared){c.motion[0]=g.motion_scale_x/(mw*c.rect[2]);c.motion[1]=g.motion_scale_y/(mh*c.rect[3]);
  c.motion[2]=3.402823466e38f;c.jitter[0]=g.jitter_correction_x/c.rect[2];c.jitter[1]=g.jitter_correction_y/c.rect[3];}
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
  adjusted.motion[2]=3.402823466e38f;
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
 ID3D12Resource* destination,D3D12_RESOURCE_STATES destination_state,const CompactLayout* layout,const FinalOutput* final,const FocusRect* region){
 if(eye>1)return false;const unsigned pass=region?0:1;auto& s=inputs[eye][pass];
 const unsigned method=region?g.center_method:g.peripheral_method;
 auto& prior_depth=region?previous_center_depth[eye]:previous_depth[eye];
 auto& valid_history=region?center_history[eye]:history[eye];
 auto& scratch=region?center_output[eye]:peripheral_output[eye];
 if(!s.output_width||!s.output_height||!previous||!current||(method>2&&!cheeky_fg_optical(method)))return false;
 if(cheeky_fg_optical(method)){
  auto c=optical_constants[eye][pass];
  c.flags[1]=srgb_color(current->GetDesc().Format)&&color_format==DXGI_FORMAT_R8G8B8A8_UNORM?1.f:0.f;
  auto* depth=method==cheeky_fg_optical_depth?s.depth.Get():nullptr;
  if(method==cheeky_fg_optical_depth&&!depth)return false;
  // Prepared depth covers this pass's output region in eye coordinates.
  c.flags[2]=depth?1.f:0.f;c.flags[3]=float(g.depth_inverted);std::copy_n(c.output_rect,4,c.depth_rect);
  if(camera.valid)optical::pose_mapping(c,camera.previous_pose,camera.previous_fov,camera.pose,camera.view_fov,interpolation_fraction);
  if(!optical[eye][pass].render(list,previous,current,depth,c,interpolation_fraction,color_format,color_state,
       previous->GetDesc().Format,current->GetDesc().Format,scratch))return false;
  // Optical periphery uses the ordinary composition/packing path. The worker
  // never requests a partial packed destination for this method.
  if(destination&&final){
   if(!compose_frame(dev,list,eye,final->center,scratch.Get(),final->region,final->width,final->height,destination,destination_state,final->linear_sdr))return false;
   transition(list,destination,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_STATE_COMMON);return true;
  }
  return destination==nullptr;
 }
 const bool need_motion=method!=0,need_depth=method==2;
 if((need_motion&&!s.motion)||(need_depth&&!s.depth))return false;
 if(destination){
  const auto d=destination->GetDesc();
  const bool dimensions=region?(d.Width==s.output_width&&d.Height==s.output_height):final?(final->width&&final->height&&d.Width==final->width&&d.Height==final->height&&
      final->region.w&&final->region.h&&(final->center_direct||final->center)):
      (layout&&layout->packed()&&d.Width==layout->stored_width()&&d.Height==layout->stored_height()&&
      layout->context_width==s.output_width&&layout->context_height==s.output_height);
  if(!dimensions||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||!writable_color(d.Format,color_format)||
     !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)||
     destination==previous||destination==current||destination==s.depth.Get()||destination==s.motion.Get()||destination==prior_depth.Get())return false;
 }else if(!allocate(dev,s.output_width,s.output_height,color_format,scratch))return false;
 if(need_depth&&(!prior_depth||prior_depth->GetDesc().Width!=s.depth->GetDesc().Width||
     prior_depth->GetDesc().Height!=s.depth->GetDesc().Height))valid_history=false;
 Constants c{};color_layouts[eye].constants(c.atlas_crop,c.atlas_spans,c.atlas_texel);float unused[4];previous_layouts[eye].constants(c.prior_crop,c.prior_spans,unused);c.options[0]=float(method);c.options[1]=interpolation_fraction;
 c.options[2]=(!need_depth||valid_history)&&!g.reset?1.f:0.f;c.previous=camera.clip_to_prev;
 c.options[3]=srgb_color(current->GetDesc().Format)&&color_format==DXGI_FORMAT_R8G8B8A8_UNORM?1.f:0.f;
 auto* mv=need_motion?s.motion.Get():nullptr;auto* depth=need_depth?s.depth.Get():nullptr;
 auto* old=need_depth?(valid_history?prior_depth.Get():depth):nullptr;
 c.dimensions[0]=float(s.output_width);c.dimensions[1]=float(s.output_height);
 if(final){
  c.dimensions[0]=float(final->width);c.dimensions[1]=float(final->height);
  c.rect[0]=float(final->region.x)/final->width;c.rect[1]=float(final->region.y)/final->height;
  c.rect[2]=float(final->region.w)/final->width;c.rect[3]=float(final->region.h)/final->height;
  c.offset[0]=final->region.feather;c.offset[1]=final->linear_sdr?1.f:0.f;
  c.jitter[2]=final->center_direct?1.f:0.f;c.jitter[3]=1.f;
 }else if(region){
  c.rect[0]=float(region->x)/g.width;c.rect[1]=float(region->y)/g.height;
  c.rect[2]=float(region->w)/g.width;c.rect[3]=float(region->h)/g.height;c.offset[3]=1.f;
 }else if(destination){
  c.dimensions[3]=float(layout->center_height);
  c.rect[0]=float(layout->x)/layout->width;c.rect[1]=float(layout->y)/layout->height;
  c.rect[2]=float(layout->w)/layout->width;c.rect[3]=float(layout->h)/layout->height;
  c.jitter[0]=layout->feather+1.5f/s.output_width;c.jitter[1]=layout->feather+1.5f/s.output_height;c.jitter[2]=1.f;
 }
 auto* output=destination?destination:scratch.Get();
 transition(list,output,destination?destination_state:D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 for(auto* source:{mv,depth})transition(list,source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 if(color_state==D3D12_RESOURCE_STATE_COMMON){transition(list,previous,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if(current!=previous)transition(list,current,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
 if(old!=depth)transition(list,old,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 auto* center=final&&!final->center_direct?final->center:nullptr;
 transition(list,center,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 auto* shader=region?center_shader.Get():peripheral.Get();
 bind(*this,dev,list,eye,region?6:2,shader,{previous,current,mv,depth,old},
  {previous->GetDesc().Format,current->GetDesc().Format,mv?mv->GetDesc().Format:DXGI_FORMAT_R32G32_FLOAT,
   depth?depth->GetDesc().Format:DXGI_FORMAT_R32_FLOAT,old?old->GetDesc().Format:DXGI_FORMAT_R32_FLOAT},{output,nullptr,nullptr},c,center);
 list->Dispatch((unsigned(c.dimensions[0])+7)/8,(unsigned(c.dimensions[1])+7)/8,1);
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
 ID3D12Resource* destination,D3D12_RESOURCE_STATES destination_state,bool linear_sdr){
 if(!center||!periphery||eye>=2)return false;
 if(destination){const auto d=destination->GetDesc();
  if(destination==center||destination==periphery||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||
     d.Width!=w||d.Height!=h||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||
     !writable_color(d.Format,color_format)||!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return false;
 }else if(!allocate(dev,w,h,color_format,full[eye]))return false;
 Constants c{};c.rect[0]=float(r.x)/w;c.rect[1]=float(r.y)/h;c.rect[2]=float(r.w)/w;c.rect[3]=float(r.h)/h;c.options[1]=linear_sdr?1.f:0.f;c.options[2]=r.active?1.f:0.f;c.options[3]=r.feather;
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
bool FocusPipeline::save_depth(ID3D12Device* dev,ID3D12GraphicsCommandList* list,unsigned eye,unsigned pass){
 auto& saved=pass?previous_depth[eye]:previous_center_depth[eye];
 auto* source=inputs[eye][pass].depth.Get();if(!source)return true;auto desc=source->GetDesc();
 if(!allocate(dev,unsigned(desc.Width),desc.Height,desc.Format,saved))return false;
 transition(list,source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
 transition(list,saved.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
 list->CopyResource(saved.Get(),source);
 transition(list,saved.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
 transition(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);return true;
}
}
