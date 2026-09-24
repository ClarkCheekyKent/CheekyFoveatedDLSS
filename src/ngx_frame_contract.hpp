#pragma once
#include "frame_contract.hpp"
#include "ngx_abi.hpp"
namespace cheeky::foveated_dlss {
inline float ngx_float(const NgxParameters* p, const char* name, float fallback=0.0F) noexcept {
    float value{};
    return p && ngx_succeeded(p->Get(name,&value)) ? value : fallback;
}
// Metadata is independent of the resource API and must have identical meaning
// in every backend. Resource bindings are extracted by the API-specific caller.
inline bool read_ngx_frame_contract(const NgxParameters* p, DlssViewId view,
    unsigned feature, DlssFrameContract& c) noexcept {
    if (!p) return false;
    c={}; c.view_id=view; c.feature_id=feature;
    c.render_width=get_ui(p,"DLSS.Render.Subrect.Dimensions.Width");
    c.render_height=get_ui(p,"DLSS.Render.Subrect.Dimensions.Height");
    if (!c.render_width) c.render_width=get_ui(p,"Width");
    if (!c.render_height) c.render_height=get_ui(p,"Height");
    c.output_width=get_ui(p,"OutWidth"); c.output_height=get_ui(p,"OutHeight");
    c.color_base_x=get_ui(p,"DLSS.Input.Color.Subrect.Base.X"); c.color_base_y=get_ui(p,"DLSS.Input.Color.Subrect.Base.Y");
    c.depth_base_x=get_ui(p,"DLSS.Input.Depth.Subrect.Base.X"); c.depth_base_y=get_ui(p,"DLSS.Input.Depth.Subrect.Base.Y");
    c.mv_base_x=get_ui(p,"DLSS.Input.MV.Subrect.Base.X"); c.mv_base_y=get_ui(p,"DLSS.Input.MV.Subrect.Base.Y");
    c.output_base_x=get_ui(p,"DLSS.Output.Subrect.Base.X"); c.output_base_y=get_ui(p,"DLSS.Output.Subrect.Base.Y");
    if (!try_get_ngx_integer_bits(p,"DLSS.Feature.Create.Flags",c.create_flags) ||
        !try_get_ngx_integer_bits(p,"PerfQualityValue",c.perf_quality)) return false;
    c.motion_vectors_low_res=(c.create_flags & 2U)!=0;
    c.depth_inverted=(c.create_flags & 8U)!=0;
    c.reset=get_ui(p,"Reset")!=0;
    c.motion_vector_scale_x=ngx_float(p,"MV.Scale.X",1.0F);
    c.motion_vector_scale_y=ngx_float(p,"MV.Scale.Y",1.0F);
    c.jitter_x=ngx_float(p,"Jitter.Offset.X"); c.jitter_y=ngx_float(p,"Jitter.Offset.Y");
    c.pre_exposure=ngx_float(p,"DLSS.Pre.Exposure",1.0F); c.exposure_scale=ngx_float(p,"DLSS.Exposure.Scale",1.0F);
    return c.render_width && c.render_height && c.output_width && c.output_height;
}
}
