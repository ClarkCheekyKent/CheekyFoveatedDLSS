#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
namespace cheeky::foveated_dlss {
// Slots verified against Valve's SDK v1.0.17, v1.16.8, v2.2.3 and v2.15.6.
inline unsigned openvr_submit_slot(const char* version) noexcept {
    if (!version) return 0;
    if (!std::strcmp(version,"IVRCompositor_029")) return 6;
    if (!std::strcmp(version,"IVRCompositor_022") ||
        !std::strcmp(version,"IVRCompositor_027") ||
        !std::strcmp(version,"IVRCompositor_028")) return 5;
    return 0;
}
inline bool openvr_ndc_center(float x,float y,float& u,float& v) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    u=(x+1.F)*0.5F; v=(1.F-y)*0.5F;
    return std::isfinite(u) && std::isfinite(v);
}
inline bool openvr_bounds(float lo,float hi,std::uint32_t extent,
    std::int32_t& origin,std::uint32_t& size) noexcept {
    if (!extent || extent>0x7fffffffU || !std::isfinite(lo) || !std::isfinite(hi) ||
        lo<0.F || hi>1.F || hi<=lo) return false;
    const double first=static_cast<double>(lo)*extent, last=static_cast<double>(hi)*extent;
    if (std::abs(first-std::round(first))>0.02 || std::abs(last-std::round(last))>0.02) return false;
    origin=static_cast<std::int32_t>(std::llround(first));
    size=static_cast<std::uint32_t>(std::llround(last)-origin);
    return size!=0;
}
// Varjo gaze uses HMD space with X right and Y up, but Varjo's documentation
// disagrees on the sign of forward Z. A gaze ray always points ahead of the
// viewer, so convert to OpenVR head space (-Z forward) without trusting it.
inline bool varjo_gaze_direction(const double forward[3],float ray[3]) noexcept {
    for (unsigned i=0;i<3;++i) if (!std::isfinite(forward[i])) return false;
    const double z=std::abs(forward[2]);
    if (z<0.0001) return false;
    ray[0]=static_cast<float>(forward[0]);
    ray[1]=static_cast<float>(forward[1]);
    ray[2]=static_cast<float>(-z);
    return true;
}
// Varjo status 0 is invalid for both the tracker and each eye. Prefer the
// combined ray; when one eye is lost (a squint or the edge of a blink), follow
// the remaining eye rather than dropping the sample.
inline const double* varjo_select_forward(std::int64_t status,std::int64_t left_status,
    std::int64_t right_status,const double* combined,const double* left,const double* right) noexcept {
    if (status==0) return nullptr;
    if (left_status!=0 && right_status!=0) return combined;
    if (left_status!=0) return left;
    if (right_status!=0) return right;
    return nullptr;
}
// OpenVR raw top/bottom tangents have down-positive signs. Eye-to-head maps
// the eye's local basis into head space; transpose it for a head-relative ray.
inline bool openvr_project_direction(const float eye[3][4],float left,float right,
    float top,float bottom,const float ray[3],float& u,float& v) noexcept {
    float d[3]{};
    for(unsigned i=0;i<3;++i) for(unsigned j=0;j<3;++j) d[i]+=eye[j][i]*ray[j];
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) ||
        !std::isfinite(bottom) || right<=left || bottom<=top || d[2]>=-0.0001F) return false;
    u=(d[0]/-d[2]-left)/(right-left);
    v=(-d[1]/-d[2]-top)/(bottom-top);
    return std::isfinite(u) && std::isfinite(v);
}
}
