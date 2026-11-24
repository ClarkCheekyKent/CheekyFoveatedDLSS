#pragma once
#include "../shared/cheeky_fg_bridge.hpp"
#include <openxr/openxr.h>
#include <cmath>
#include <cstdint>

namespace cheeky::fg {
// Recover the full source canvas only when calibration verifies a 1:1 crop.
// The atlas offset alone does not establish a source-to-submission mapping.
inline bool full_source_rect(const CheekyFgEye& g,const XrRect2Di& submitted,
    std::uint64_t texture_width,unsigned texture_height,XrRect2Di& source,XrRect2Di& visible) noexcept {
    if(!g.submitted_crop.valid||!g.width||!g.height||g.width>INT32_MAX||g.height>INT32_MAX||
       submitted.offset.x<0||submitted.offset.y<0||submitted.extent.width<=0||submitted.extent.height<=0)return false;
    const auto* c=g.submitted_crop.rect;
    for(unsigned i=0;i<4;++i)if(!std::isfinite(c[i]))return false;
    if(c[0]<0||c[1]<0||c[2]<=0||c[3]<=0||c[0]+c[2]>1.00001f||c[1]+c[3]>1.00001f)return false;
    // Marker calibration has subpixel estimation error. Never accept scaling
    // beyond one source pixel, or infer a transform from aspect ratio alone.
    if(std::abs(double(c[2])*g.width-submitted.extent.width)>1.||
       std::abs(double(c[3])*g.height-submitted.extent.height)>1.)return false;
    const auto x=std::int64_t(std::llround(double(c[0])*g.width));
    const auto y=std::int64_t(std::llround(double(c[1])*g.height));
    const auto origin_x=std::int64_t(submitted.offset.x)-x,origin_y=std::int64_t(submitted.offset.y)-y;
    if(origin_x<0||origin_y<0||origin_x>INT32_MAX||origin_y>INT32_MAX||
       std::uint64_t(origin_x)+g.width>texture_width||std::uint64_t(origin_y)+g.height>texture_height||
       x+submitted.extent.width>g.width||y+submitted.extent.height>g.height)return false;
    source={{int(origin_x),int(origin_y)},{int(g.width),int(g.height)}};
    visible={{int(x),int(y)},submitted.extent};
    return true;
}

inline XrCompositionLayerProjectionView full_source_view(XrCompositionLayerProjectionView view,
    const XrRect2Di& visible,unsigned width,unsigned height) noexcept {
    if(visible.extent.width<=0||visible.extent.height<=0)return view;
    const float left=std::tan(view.fov.angleLeft),right=std::tan(view.fov.angleRight);
    const float up=std::tan(view.fov.angleUp),down=std::tan(view.fov.angleDown);
    const float dx=(right-left)/visible.extent.width,dy=(down-up)/visible.extent.height;
    view.fov.angleLeft=std::atan(left-dx*visible.offset.x);
    view.fov.angleRight=std::atan(left+dx*(float(width)-visible.offset.x));
    view.fov.angleUp=std::atan(up-dy*visible.offset.y);
    view.fov.angleDown=std::atan(up+dy*(float(height)-visible.offset.y));
    return view;
}
struct SourceEyeRegion {
    bool active{};
    XrRect2Di source{},visible{};
    std::uint64_t revision{};
    bool operator==(const SourceEyeRegion& other) const noexcept {
        const auto equal=[](const XrRect2Di& a,const XrRect2Di& b){
            return a.offset.x==b.offset.x&&a.offset.y==b.offset.y&&
                a.extent.width==b.extent.width&&a.extent.height==b.extent.height;};
        return active==other.active&&(!active||(revision==other.revision&&equal(source,other.source)&&equal(visible,other.visible)));
    }
};
inline XrCompositionLayerProjectionView source_processing_view(XrCompositionLayerProjectionView view,
    const SourceEyeRegion& region) noexcept {
    if(!region.active||region.source.extent.width<=0||region.source.extent.height<=0)return view;
    return full_source_view(view,region.visible,unsigned(region.source.extent.width),unsigned(region.source.extent.height));
}
inline SourceEyeRegion source_eye_region(const CheekyFgEye& g,const XrRect2Di& submitted,
    std::uint64_t texture_width,unsigned texture_height) noexcept {
    SourceEyeRegion out;
    if(!g.full_eye_crop_compatibility||
       (g.width==unsigned(submitted.extent.width)&&g.height==unsigned(submitted.extent.height)))return out;
    out.active=full_source_rect(g,submitted,texture_width,texture_height,out.source,out.visible);
    if(out.active)out.revision=g.submitted_crop.revision;
    return out;
}
}
