#pragma once
#include <openxr/openxr.h>
#include "../shared/gaze_math.hpp"
#include "../shared/cheeky_fg_shader11.hpp"
#include "../shared/cheeky_nr_universal.hpp"
#include "../shared/cheeky_xr_borders.hpp"
#include "../shared/fg_border.hpp"
#include <array>
#include <algorithm>
#include <cmath>

namespace cheeky::fg {
// Eye-local UV, independent of swapchain packing. Owned by the native frame,
// never read from a newer global gaze snapshot on the generation thread.
struct StereoAlignment {
    std::array<float,2> u{.5f,.5f},v{.5f,.5f};
    bool valid{};
    // Eye tracking or simulated gaze for this frame, projected onto each
    // submitted eye view. Independent of the game's DLSS views.
    std::array<float,2> gaze_u{.5f,.5f},gaze_v{.5f,.5f};
    bool gaze_valid{};
};
inline bool alignment_rotation(XrQuaternionf& q) noexcept {
    const double n=std::sqrt(double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w);
    if(!std::isfinite(n)||n<1.e-6)return false;
    q={float(q.x/n),float(q.y/n),float(q.z/n),float(q.w/n)};return true;
}
inline gaze_math::Pose alignment_pose(XrQuaternionf q) noexcept {return {{q.x,q.y,q.z,q.w},{}};}
inline StereoAlignment project_stereo_alignment(const XrCompositionLayerProjection* projection,
    const std::array<XrPosef,2>& tracked,XrQuaternionf projection_from_tracked={0,0,0,1}) noexcept {
    StereoAlignment out;
    if(!projection||projection->viewCount!=2||!projection->views||!alignment_rotation(projection_from_tracked))return out;
    auto left=tracked[0].orientation,right=tracked[1].orientation;
    if(!alignment_rotation(left)||!alignment_rotation(right))return out;
    gaze_math::Pose forward;
    if(!gaze_math::stereo_forward_pose(alignment_pose(left),alignment_pose(right),forward))return out;
    const auto a=projection_from_tracked;const auto b=forward.orientation;
    forward.orientation={a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
        a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
    for(unsigned eye=0;eye<2;++eye){
        auto q=projection->views[eye].pose.orientation;
        if(!alignment_rotation(q))return {};
        const auto& f=projection->views[eye].fov;
        if(!gaze_math::project_gaze_to_view(forward,alignment_pose(q),
            {f.angleLeft,f.angleRight,f.angleUp,f.angleDown},out.u[eye],out.v[eye]))return {};
    }
    out.valid=true;return out;
}
// Projects a gaze orientation (tracked space) onto both submitted eye views.
inline bool project_stereo_gaze(const XrCompositionLayerProjection* projection,XrQuaternionf gaze,
    XrQuaternionf projection_from_tracked,StereoAlignment& out) noexcept {
    out.gaze_valid=false;
    if(!projection||projection->viewCount!=2||!projection->views||!alignment_rotation(gaze)||
        !alignment_rotation(projection_from_tracked))return false;
    const auto a=projection_from_tracked;const auto b=gaze;
    gaze_math::Pose pose=alignment_pose({a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
        a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z});
    std::array<float,2> u{},v{};
    for(unsigned eye=0;eye<2;++eye){
        auto q=projection->views[eye].pose.orientation;
        if(!alignment_rotation(q))return false;
        const auto& f=projection->views[eye].fov;
        if(!gaze_math::project_gaze_to_view(pose,alignment_pose(q),
            {f.angleLeft,f.angleRight,f.angleUp,f.angleDown},u[eye],v[eye]))return false;
        if(!std::isfinite(u[eye])||!std::isfinite(v[eye]))return false;
    }
    out.gaze_u=u;out.gaze_v=v;out.gaze_valid=true;return true;
}
inline std::array<float,2> gaze_eye_center(const StereoAlignment& alignment,unsigned eye) noexcept {
    return {std::clamp(alignment.gaze_u[eye],0.f,1.f),std::clamp(alignment.gaze_v[eye],0.f,1.f)};
}
inline std::array<float,2> aligned_eye_center(const StereoAlignment& alignment,unsigned eye,float height_offset) noexcept {
    return {std::clamp(alignment.u[eye],0.f,1.f),std::clamp(alignment.v[eye]+.5f*height_offset,0.f,1.f)};
}
inline void align_color_options(CheekyFgColorOptionsV1& options,const StereoAlignment& alignment) noexcept {
    const unsigned gaze_mask=options.abi>=4?options.gaze_mask:0u;
    for(unsigned eye=0;eye<2;++eye){
        // Gaze replaces settings-derived centers first; otherwise direct XR alignment.
        if((gaze_mask&(1u<<eye))&&alignment.gaze_valid){
            const auto center=gaze_eye_center(alignment,eye);
            options.eyes[eye].focus_x=center[0]-.5f;options.eyes[eye].focus_y=center[1]-.5f;
        }else if((options.automatic_alignment_mask&(1u<<eye))&&alignment.valid){
            const auto center=aligned_eye_center(alignment,eye,options.aligned_height_offset);
            options.eyes[eye].focus_x=center[0]-.5f;options.eyes[eye].focus_y=center[1]-.5f;
        }
    }
}
// Newest border ABI first; an older runtime still answers v1.
inline bool query_border_options(CheekyXrGetBorderOptionsV1Fn get,CheekyXrBorderOptionsV1& out) noexcept {
    if(!get)return false;
    out={};
    if(get(&out))return true;
    out={};out.abi=1;out.size=CHEEKY_XR_BORDER_OPTIONS_V1_SIZE;
    return get(&out);
}
inline std::array<BorderRect,2> xr_alignment_borders(const CheekyXrBorderOptionsV1& options,
    const StereoAlignment& alignment,unsigned eye,unsigned width,unsigned height) noexcept {
    std::array<BorderRect,2> out{};
    if(!options.enabled)return out;
    const auto placed=options.automatic_alignment&&alignment.valid?
        aligned_eye_center(alignment,eye,options.aligned_height_offset):
        std::array<float,2>{options.center_u[eye],options.center_v[eye]};
    const unsigned gaze_mask=options.abi>=2&&alignment.gaze_valid?options.gaze_mask:0u;
    for(unsigned region=0;region<2;++region){
        const auto center=(gaze_mask&(1u<<region))?gaze_eye_center(alignment,eye):placed;
        out[region]=fg_border_rect(width,height,
            options.width[region],options.height[region],center[0]-.5f,center[1]-.5f,
            region?options.show_nr!=0:options.show_sr!=0);
    }
    return out;
}
// The NR box is drawn where NR actually ran: re-center it on the evaluated
// center (FG's SR region, projected gaze, alignment or manual UV).
inline void center_border(BorderRect& box,float u,float v) noexcept {
    if(!box.visible||!std::isfinite(u)||!std::isfinite(v))return;
    box.x=std::clamp(u-box.w*.5f,0.f,std::max(0.f,1.f-box.w));
    box.y=std::clamp(v-box.h*.5f,0.f,std::max(0.f,1.f-box.h));
}
// One center drives BOTH the flow window and the NR crop. FG's proven region
// has priority when requested (it carries gaze itself); otherwise this frame's
// projected gaze when eye tracking is the center source, then direct XR
// alignment or manual UV.
inline std::array<float,2> universal_eye_center(const CheekyNrUniversalOptionsV1& options,
    const CheekyFgEye& guide,const StereoAlignment& alignment,unsigned eye) noexcept {
    if(options.center_from_fg&&(guide.focus_width<.999f||guide.focus_height<.999f))
        return {std::clamp(.5f+guide.focus_x,0.f,1.f),std::clamp(.5f+guide.focus_y,0.f,1.f)};
    if(options.abi>=3&&options.follow_gaze&&alignment.gaze_valid)return gaze_eye_center(alignment,eye);
    if(options.automatic_alignment&&alignment.valid)return aligned_eye_center(alignment,eye,options.aligned_height_offset);
    return {options.center_u[eye],options.center_v[eye]};
}
} // namespace cheeky::fg
