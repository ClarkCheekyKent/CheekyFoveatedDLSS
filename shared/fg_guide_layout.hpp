#pragma once
#include "cheeky_fg_bridge.hpp"
#include <algorithm>
#include <cmath>
#include <dxgiformat.h>

namespace cheeky::fg {
inline DXGI_FORMAT guide_depth_format(DXGI_FORMAT f) noexcept {
    switch(f){
    case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_D32_FLOAT:return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_D16_UNORM:return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:return f;
    default:return DXGI_FORMAT_UNKNOWN;
    }
}
inline DXGI_FORMAT guide_motion_format(DXGI_FORMAT f) noexcept {
    switch(f){
    case DXGI_FORMAT_R16G16_TYPELESS:return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R32G32B32A32_FLOAT:return f;
    default:return DXGI_FORMAT_UNKNOWN;
    }
}
struct GuideGrid {
    unsigned x{},y{},w{},h{},color_width{},color_height{},width{},height{};
    bool depth{},motion{};
    unsigned depth_bits{32},motion_bits{32};
};
// The producer and consumer use exactly the same crop, rounding and source-grid
// calculation. Normalization is independent of the physical color atlas.
inline GuideGrid guide_grid(const CheekyFgEye& g,unsigned dw,unsigned dh,unsigned mw,unsigned mh,unsigned pass) noexcept {
    GuideGrid r;r.w=g.width;r.h=g.height;
    const bool focused=g.focus_width<.999f||g.focus_height<.999f;
    if(!g.width||!g.height||pass>1||g.peripheral_method>3)return {};
    if(pass==0&&focused){
        r.w=std::clamp(unsigned(std::round(g.width*g.focus_width)),1u,g.width);
        r.h=std::clamp(unsigned(std::round(g.height*g.focus_height)),1u,g.height);
        r.x=unsigned(std::clamp((g.width-r.w)*.5f+g.focus_x*g.width,0.f,float(g.width-r.w)));
        r.y=unsigned(std::clamp((g.height-r.h)*.5f+g.focus_y*g.height,0.f,float(g.height-r.h)));
    }
    if(pass==1&&!focused)return r;
    const unsigned scale=pass?g.peripheral_percent:g.resolution_percent;
    r.color_width=std::max(32u,(r.w*scale+50u)/100u);r.color_height=std::max(32u,(r.h*scale+50u)/100u);
    r.depth=pass==0||g.peripheral_method>=2;r.motion=pass==0||g.peripheral_method!=0;
    r.depth_bits=pass?g.depth_bits:32;r.motion_bits=pass?g.motion_bits:32;
    const unsigned sw=r.depth?std::min(dw,mw):mw,sh=r.depth?std::min(dh,mh):mh;
    r.width=std::clamp(unsigned(std::round(double(sw)*r.w/g.width*std::min(1.,double(r.color_width)/r.w))),1u,r.color_width);
    r.height=std::clamp(unsigned(std::round(double(sh)*r.h/g.height*std::min(1.,double(r.color_height)/r.h))),1u,r.color_height);
    return r;
}
}
