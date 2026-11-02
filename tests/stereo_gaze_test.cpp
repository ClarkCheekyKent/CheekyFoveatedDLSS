// Layer-side gaze placement: projected eye tracking / simulated gaze drives
// Universal NR, FG focus and the border overlay without game DLSS views.
#include "../openxr_layer/stereo_alignment.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
// Checked in every configuration, unlike assert.
#define check(x) do{if(!(x)){std::fprintf(stderr,"FAILED %s:%d %s\n",__FILE__,__LINE__,#x);std::exit(1);}}while(0)
static bool near_eq(float a,float b){return std::fabs(a-b)<.0005f;}
static XrQuaternionf yaw_pitch(float yaw,float pitch){
    const XrQuaternionf y{0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)},p{std::sin(pitch*.5f),0,0,std::cos(pitch*.5f)};
    return {y.w*p.x+y.x*p.w+y.y*p.z-y.z*p.y,y.w*p.y-y.x*p.z+y.y*p.w+y.z*p.x,y.w*p.z+y.x*p.y-y.y*p.x+y.z*p.w,y.w*p.w-y.x*p.x-y.y*p.y-y.z*p.z};
}
int main(){
    using namespace cheeky::fg;
    const float t=std::atan(1.f); // 45 degree half-angles: tan = 1.
    XrCompositionLayerProjectionView views[2]{};
    for(auto& v:views){v.pose.orientation={0,0,0,1};v.fov={-t,t,t,-t};}
    XrCompositionLayerProjection projection{};projection.viewCount=2;projection.views=views;
    StereoAlignment a;
    // Straight ahead lands in the eye center.
    check(project_stereo_gaze(&projection,{0,0,0,1},{0,0,0,1},a)&&a.gaze_valid);
    check(near_eq(a.gaze_u[0],.5f)&&near_eq(a.gaze_v[0],.5f)&&near_eq(a.gaze_u[1],.5f));
    // Looking left (positive yaw) moves u left; looking up moves v up (smaller).
    check(project_stereo_gaze(&projection,yaw_pitch(std::atan(.5f),0),{0,0,0,1},a));
    check(near_eq(a.gaze_u[0],.25f)&&near_eq(a.gaze_v[0],.5f));
    check(project_stereo_gaze(&projection,yaw_pitch(0,std::atan(.5f)),{0,0,0,1},a));
    check(near_eq(a.gaze_u[0],.5f)&&near_eq(a.gaze_v[0],.25f));
    // Tracked-space gaze is rotated into the projection space first.
    check(project_stereo_gaze(&projection,{0,0,0,1},yaw_pitch(std::atan(.5f),0),a));
    check(near_eq(a.gaze_u[1],.25f));
    // Behind the viewer is invalid and clears the previous gaze.
    check(!project_stereo_gaze(&projection,yaw_pitch(3.f,0),{0,0,0,1},a)&&!a.gaze_valid);

    // Universal NR: own region follows gaze; FG's SR region keeps priority.
    a.gaze_valid=true;a.gaze_u={.3f,.35f};a.gaze_v={.6f,.6f};
    CheekyNrUniversalOptionsV1 o;o.center_u[0]=o.center_u[1]=.5f;o.follow_gaze=1;
    CheekyFgEye guide;
    auto c=universal_eye_center(o,guide,a,1);check(near_eq(c[0],.35f)&&near_eq(c[1],.6f));
    o.center_from_fg=1;guide.focus_width=.5f;guide.focus_x=.1f;
    c=universal_eye_center(o,guide,a,1);check(near_eq(c[0],.6f));
    o.center_from_fg=0;o.abi=2; // Older runtime: no gaze field, static center.
    c=universal_eye_center(o,guide,a,1);check(near_eq(c[0],.5f));
    o.abi=3;o.follow_gaze=0;
    c=universal_eye_center(o,guide,a,1);check(near_eq(c[0],.5f));
    o.follow_gaze=1;a.gaze_valid=false;
    c=universal_eye_center(o,guide,a,1);check(near_eq(c[0],.5f));
    a.gaze_valid=true;

    // FG color focus: gaze mask first, then automatic alignment.
    CheekyFgColorOptionsV1 color;color.gaze_mask=1;color.automatic_alignment_mask=3;
    a.valid=true;a.u={.4f,.6f};a.v={.5f,.5f};
    align_color_options(color,a);
    check(near_eq(color.eyes[0].focus_x,-.2f)&&near_eq(color.eyes[0].focus_y,.1f));
    check(near_eq(color.eyes[1].focus_x,.1f)&&near_eq(color.eyes[1].focus_y,0));
    CheekyFgColorOptionsV1 old;old.abi=3;old.gaze_mask=3;old.automatic_alignment_mask=0;
    align_color_options(old,a);check(near_eq(old.eyes[0].focus_x,0));

    // Border overlay: NR box follows gaze, SR box keeps its placement.
    CheekyXrBorderOptionsV1 b;b.enabled=1;b.show_sr=b.show_nr=1;b.width[0]=b.height[0]=.5f;b.width[1]=b.height[1]=.2f;
    b.gaze_mask=2;a.valid=false;
    auto boxes=xr_alignment_borders(b,a,0,1000,1000);
    check(boxes[0].visible&&near_eq(boxes[0].x,.25f));
    check(boxes[1].visible&&near_eq(boxes[1].x,.2f)&&near_eq(boxes[1].y,.5f));
    b.abi=1;boxes=xr_alignment_borders(b,a,0,1000,1000);check(near_eq(boxes[1].x,.4f));
    // The NR box re-centers on the evaluated NR center, clamped to the eye.
    BorderRect box{.4f,.4f,.2f,.2f,true};center_border(box,.95f,.3f);
    check(near_eq(box.x,.8f)&&near_eq(box.y,.2f)&&near_eq(box.w,.2f));
    BorderRect hidden{};center_border(hidden,.5f,.5f);check(!hidden.visible&&near_eq(hidden.x,0));
    std::puts("stereo gaze tests passed");
}
