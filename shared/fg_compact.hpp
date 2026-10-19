#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cheeky::fg {
// Physical storage is a center crop above a reduced whole-image context.
// Width/height always describe the original eye, including for packed images.
struct CompactLayout {
    unsigned width{},height{},x{},y{},w{},h{},center_width{},center_height{},context_width{},context_height{};
    float feather{};
    bool packed() const noexcept {return context_width!=0;}
    unsigned stored_width() const noexcept {return packed()?std::max(center_width,context_width):width;}
    unsigned stored_height() const noexcept {return packed()?center_height+context_height:height;}
    bool operator==(const CompactLayout&) const = default;
    void constants(float* crop,float* spans,float* texel) const noexcept {
        std::fill_n(crop,4,0.f);std::fill_n(spans,4,0.f);std::fill_n(texel,4,0.f);
        if(!packed())return;
        crop[0]=float(x)/width;crop[1]=float(y)/height;crop[2]=float(w)/width;crop[3]=float(h)/height;
        spans[0]=float(center_width)/stored_width();spans[1]=float(center_height)/stored_height();
        spans[2]=float(context_width)/stored_width();spans[3]=float(context_height)/stored_height();
        texel[0]=.5f/stored_width();texel[1]=.5f/stored_height();texel[2]=feather;
    }
};
inline CompactLayout compact_layout(unsigned width,unsigned height,bool enabled,float fw,float fh,float ox,float oy,
                                     unsigned peripheral_percent,float feather=0,unsigned center_percent=100) noexcept {
    CompactLayout r;r.width=width;r.height=height;
    if(!enabled||!width||!height||!std::isfinite(fw)||!std::isfinite(fh)||!std::isfinite(ox)||!std::isfinite(oy)||
       (fw>=.999f&&fh>=.999f))return r;
    r.w=std::clamp(unsigned(std::lround(width*std::clamp(fw,.01f,1.f))),1u,width);
    r.h=std::clamp(unsigned(std::lround(height*std::clamp(fh,.01f,1.f))),1u,height);
    r.x=unsigned(std::clamp((width-r.w)*.5f+ox*width,0.f,float(width-r.w)));
    r.y=unsigned(std::clamp((height-r.h)*.5f+oy*height,0.f,float(height-r.h)));
    auto scaled=[](unsigned n,unsigned percent){return std::max(32u,unsigned((std::uint64_t(n)*std::clamp(percent,1u,100u)+50)/100));};
    r.center_width=scaled(r.w,center_percent);r.center_height=scaled(r.h,center_percent);
    r.context_width=scaled(width,peripheral_percent);r.context_height=scaled(height,peripheral_percent);
    r.feather=std::isfinite(feather)?std::max(0.f,feather):0.f;
    // Never enlarge allocations for extreme settings or unsupported extents.
    if(r.stored_width()>16384||r.stored_height()>16384||
       std::uint64_t(r.stored_width())*r.stored_height()>=std::uint64_t(width)*height){r={};r.width=width;r.height=height;}
    return r;
}
}

// Region clamps prevent bilinear filtering across atlas boundaries/padding.
#define CHEEKY_FG_COMPACT_HLSL R"HLSL(
float2 contextUV(float2 uv,float4 spans,float4 texel){
 return clamp(float2(uv.x*spans.z,spans.y+uv.y*spans.w),float2(texel.x,spans.y+texel.y),float2(spans.z-texel.x,spans.y+spans.w-texel.y));}
float2 centerUV(float2 uv,float4 crop,float4 spans,float4 texel){
 return clamp((uv-crop.xy)/crop.zw*spans.xy,texel.xy,spans.xy-texel.xy);}
float centerFade(float2 uv,float4 crop,float4 texel){
 float2 edge=min(uv-crop.xy,crop.xy+crop.zw-uv);
 return texel.z>0?saturate(min(edge.x,edge.y)/texel.z):(all(edge>=0)?1:0);}
)HLSL"
