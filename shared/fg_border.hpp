#pragma once
#include <algorithm>
#include <cmath>
namespace cheeky::fg {
// Center rectangle in normalized output-eye coordinates. Pure geometry; no GPU dependency.
struct BorderRect {float x{},y{},w{},h{};bool visible{};};
inline BorderRect fg_border_rect(unsigned width,unsigned height,float fw,float fh,float ox,float oy,bool enabled) noexcept {
    if(!enabled||!width||!height||!std::isfinite(fw)||!std::isfinite(fh)||!std::isfinite(ox)||!std::isfinite(oy))return {};
    const unsigned w=std::clamp(unsigned(std::lround(width*std::clamp(fw,0.01f,1.f))),1u,width);
    const unsigned h=std::clamp(unsigned(std::lround(height*std::clamp(fh,0.01f,1.f))),1u,height);
    if(w==width&&h==height)return {};
    const auto x=std::clamp((width-w)*.5f+ox*width,0.f,float(width-w));
    const auto y=std::clamp((height-h)*.5f+oy*height,0.f,float(height-h));
    return {x/width,y/height,float(w)/width,float(h)/height,true};
}
}
