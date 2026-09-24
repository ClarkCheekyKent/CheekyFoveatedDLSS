#pragma once
#include "ngx_abi.hpp"
#include "frame_contract.hpp"
#include "settings.hpp"
#include "crop_motion.hpp"
#include <array>
#include <cstring>

namespace cheeky::foveated_dlss {
// Parameter names follow NVIDIA's public NGX RR contract:
// https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs_dlssd.h
struct RrGuide { const char* resource; const char* x; const char* y; };
inline constexpr RrGuide rr_guides[]{
    {"DLSS.Input.DiffuseAlbedo", "DLSS.Input.DiffuseAlbedo.Subrect.Base.X", "DLSS.Input.DiffuseAlbedo.Subrect.Base.Y"},
    {"DLSS.Input.SpecularAlbedo", "DLSS.Input.SpecularAlbedo.Subrect.Base.X", "DLSS.Input.SpecularAlbedo.Subrect.Base.Y"},
    {"GBuffer.Normals", "DLSS.Input.Normals.Subrect.Base.X", "DLSS.Input.Normals.Subrect.Base.Y"},
    {"GBuffer.Roughness", "DLSS.Input.Roughness.Subrect.Base.X", "DLSS.Input.Roughness.Subrect.Base.Y"},
    {"DLSSD.Alpha", "DLSSD.Alpha.Subrect.Base.X", "DLSSD.Alpha.Subrect.Base.Y"},
    {"DLSSD.ReflectedAlbedo", "DLSSD.ReflectedAlbedo.Subrect.Base.X", "DLSSD.ReflectedAlbedo.Subrect.Base.Y"},
    {"DLSSD.ColorBeforeParticles", "DLSSD.ColorBeforeParticles.Subrect.Base.X", "DLSSD.ColorBeforeParticles.Subrect.Base.Y"},
    {"DLSSD.ColorAfterParticles", "DLSSD.ColorAfterParticles.Subrect.Base.X", "DLSSD.ColorAfterParticles.Subrect.Base.Y"},
    {"DLSSD.ColorBeforeTransparency", "DLSSD.ColorBeforeTransparency.Subrect.Base.X", "DLSSD.ColorBeforeTransparency.Subrect.Base.Y"},
    {"DLSSD.ColorAfterTransparency", "DLSSD.ColorAfterTransparency.Subrect.Base.X", "DLSSD.ColorAfterTransparency.Subrect.Base.Y"},
    {"DLSSD.ColorBeforeFog", "DLSSD.ColorBeforeFog.Subrect.Base.X", "DLSSD.ColorBeforeFog.Subrect.Base.Y"},
    {"DLSSD.ColorAfterFog", "DLSSD.ColorAfterFog.Subrect.Base.X", "DLSSD.ColorAfterFog.Subrect.Base.Y"},
    {"DLSSD.ScreenSpaceSubsurfaceScatteringGuide", "DLSSD.ScreenSpaceSubsurfaceScatteringGuide.Subrect.Base.X", "DLSSD.ScreenSpaceSubsurfaceScatteringGuide.Subrect.Base.Y"},
    {"DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering", "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering.Subrect.Base.X", "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering.Subrect.Base.Y"},
    {"DLSSD.ColorAfterScreenSpaceSubsurfaceScattering", "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering.Subrect.Base.X", "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering.Subrect.Base.Y"},
    {"DLSSD.ScreenSpaceRefractionGuide", "DLSSD.ScreenSpaceRefractionGuide.Subrect.Base.X", "DLSSD.ScreenSpaceRefractionGuide.Subrect.Base.Y"},
    {"DLSSD.ColorBeforeScreenSpaceRefraction", "DLSSD.ColorBeforeScreenSpaceRefraction.Subrect.Base.X", "DLSSD.ColorBeforeScreenSpaceRefraction.Subrect.Base.Y"},
    {"DLSSD.ColorAfterScreenSpaceRefraction", "DLSSD.ColorAfterScreenSpaceRefraction.Subrect.Base.X", "DLSSD.ColorAfterScreenSpaceRefraction.Subrect.Base.Y"},
    {"DLSSD.DepthOfFieldGuide", "DLSSD.DepthOfFieldGuide.Subrect.Base.X", "DLSSD.DepthOfFieldGuide.Subrect.Base.Y"},
    {"DLSSD.ColorBeforeDepthOfField", "DLSSD.ColorBeforeDepthOfField.Subrect.Base.X", "DLSSD.ColorBeforeDepthOfField.Subrect.Base.Y"},
    {"DLSSD.ColorAfterDepthOfField", "DLSSD.ColorAfterDepthOfField.Subrect.Base.X", "DLSSD.ColorAfterDepthOfField.Subrect.Base.Y"},
    {"DLSSD.DiffuseHitDistance", "DLSSD.DiffuseHitDistance.Subrect.Base.X", "DLSSD.DiffuseHitDistance.Subrect.Base.Y"},
    {"DLSSD.SpecularHitDistance", "DLSSD.SpecularHitDistance.Subrect.Base.X", "DLSSD.SpecularHitDistance.Subrect.Base.Y"},
    {"DLSSD.DiffuseRayDirection", "DLSSD.DiffuseRayDirection.Subrect.Base.X", "DLSSD.DiffuseRayDirection.Subrect.Base.Y"},
    {"DLSSD.SpecularRayDirection", "DLSSD.SpecularRayDirection.Subrect.Base.X", "DLSSD.SpecularRayDirection.Subrect.Base.Y"},
    {"DLSSD.DiffuseRayDirectionHitDistance", "DLSSD.DiffuseRayDirectionHitDistance.Subrect.Base.X", "DLSSD.DiffuseRayDirectionHitDistance.Subrect.Base.Y"},
    {"DLSSD.SpecularRayDirectionHitDistance", "DLSSD.SpecularRayDirectionHitDistance.Subrect.Base.X", "DLSSD.SpecularRayDirectionHitDistance.Subrect.Base.Y"},
    {"DLSSD.ResponsivityMask", "DLSSD.ResponsivityMask.Subrect.Base.X", "DLSSD.ResponsivityMask.Subrect.Base.Y"},
    {"TransparencyMask", "DLSS.Input.Translucency.Subrect.Base.X", "DLSS.Input.Translucency.Subrect.Base.Y"},
    {"DLSS.Input.Bias.Current.Color.Mask", "DLSS.Input.Bias.Current.Color.Subrect.Base.X", "DLSS.Input.Bias.Current.Color.Subrect.Base.Y"},
};
inline constexpr auto rr_guide_count = std::size(rr_guides);
inline constexpr const char* rr_presets[]{
    "RayReconstruction.Hint.Render.Preset.DLAA", "RayReconstruction.Hint.Render.Preset.Quality",
    "RayReconstruction.Hint.Render.Preset.Balanced", "RayReconstruction.Hint.Render.Preset.Performance",
    "RayReconstruction.Hint.Render.Preset.UltraPerformance", "RayReconstruction.Hint.Render.Preset.UltraQuality"};

// Only validated, input-resolution guide rectangles may be cropped. Output
// alpha and legacy research buffers lack a matching private-output/crop path.
inline bool rr_crop_supported(const NgxParameters* p, const DlssFrameContract& c) {
    if (!p || !c.render_width || !c.render_height || !c.motion_vectors_low_res) return false;
    for (const auto* name : {"DLSSD.OutputAlpha", "MotionVectorsReflection", "RayTracingHitDistance",
            "GBuffer.SpecularMvec", "MotionVectors3D", "DepthHighRes", "Position.ViewSpace",
            "IsParticleMask", "AnimatedTextureMask", "GBuffer.Emissive"}) {
        ID3D12Resource* resource{};
        if (ngx_succeeded(p->Get(name, &resource)) && resource) return false;
    }
    for (unsigned i = 0; i < std::size(rr_guides); ++i) {
        const auto& guide = rr_guides[i];
        ID3D12Resource* resource{};
        p->Get(guide.resource, &resource);
        const bool required = i < 3 || (i == 3 && !get_ngx_integer_bits(p, "DLSS.Roughness.Mode"));
        if (!resource) { if (required) return false; else continue; }
        const auto desc = resource->GetDesc();
        const auto x = get_ui(p, guide.x), y = get_ui(p, guide.y);
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
            desc.SampleDesc.Count != 1 || x > desc.Width || c.render_width > desc.Width - x ||
            y > desc.Height || c.render_height > desc.Height - y) return false;
    }
    return true;
}

// Scope lasts through private create/evaluate only; neither the game's matrix
// nor its guide offsets are changed after the call (including failure paths).
class RrCropScope {
    NgxParameters* parameters_{};
    std::array<std::array<unsigned, 2>, std::size(rr_guides)> bases_{};
    std::array<bool, std::size(rr_guides)> active_{};
    void* original_projection_{};
    std::array<float,16> projection_{};
public:
    RrCropScope(NgxParameters* p, const DlssFrameContract& c, const CropGeometry& crop) : parameters_(p) {
        for (unsigned i=0; i<std::size(rr_guides); ++i) {
            const auto& g=rr_guides[i]; ID3D12Resource* resource{};
            p->Get(g.resource, &resource); if (!resource) continue;
            active_[i]=true; bases_[i]={get_ui(p,g.x),get_ui(p,g.y)};
            p->Set(g.x,bases_[i][0]+crop.input_base_x);
            p->Set(g.y,bases_[i][1]+crop.input_base_y);
        }
        if (ngx_succeeded(p->Get("ViewToClipMatrix", &original_projection_)) && original_projection_) {
            std::memcpy(projection_.data(),original_projection_,sizeof(projection_));
            const float sx=float(c.render_width)/crop.input_width, sy=float(c.render_height)/crop.input_height;
            const float tx=(float(c.render_width)-2.F*crop.input_base_x-crop.input_width)/crop.input_width;
            const float ty=(2.F*crop.input_base_y+crop.input_height-float(c.render_height))/crop.input_height;
            // NGX uses row-major matrices with row vectors.
            for (unsigned row=0;row<4;++row) {
                projection_[row*4]=projection_[row*4]*sx+projection_[row*4+3]*tx;
                projection_[row*4+1]=projection_[row*4+1]*sy+projection_[row*4+3]*ty;
            }
            p->Set("ViewToClipMatrix",static_cast<void*>(projection_.data()));
        }
    }
    ~RrCropScope() {
        for(unsigned i=0;i<std::size(rr_guides);++i) if(active_[i]) {
            parameters_->Set(rr_guides[i].x,bases_[i][0]); parameters_->Set(rr_guides[i].y,bases_[i][1]);
        }
        if(original_projection_) parameters_->Set("ViewToClipMatrix",original_projection_);
    }
    RrCropScope(const RrCropScope&)=delete;
    RrCropScope& operator=(const RrCropScope&)=delete;
};

class RrInputCopiesScope {
    struct Binding { RrGuide guide{}; ID3D12Resource* source{}; ID3D12Resource* copy{}; unsigned x{},y{}; };
    std::array<Binding,rr_guide_count+3> bindings_{};
    unsigned count_{};
    NgxParameters* parameters_{};
    bool ready_{true};
public:
    RrInputCopiesScope(ID3D12GraphicsCommandList* list, NgxParameters* p, const CropGeometry& crop) : parameters_(p) {
        const auto bind = [&](const RrGuide& guide) {
            if (!ready_) return;
            ID3D12Resource* source{}; p->Get(guide.resource,&source);
            if (!source) return;
            const auto x=get_ui(p,guide.x),y=get_ui(p,guide.y);
            ID3D12Resource* copy{};
            // Several optional guides may alias the same source rectangle.
            for(unsigned i=0;i<count_;++i) if(bindings_[i].source==source && bindings_[i].x==x && bindings_[i].y==y)
                copy=bindings_[i].copy;
            if (!copy) copy=prepare_crop_texture12(list,source,x,y,crop.input_width,crop.input_height);
            if (!copy) { ready_=false; return; }
            bindings_[count_++]={guide,source,copy,x,y};
            p->Set(guide.resource,copy); p->Set(guide.x,0U); p->Set(guide.y,0U);
        };
        bind({"Color","DLSS.Input.Color.Subrect.Base.X","DLSS.Input.Color.Subrect.Base.Y"});
        bind({"Depth","DLSS.Input.Depth.Subrect.Base.X","DLSS.Input.Depth.Subrect.Base.Y"});
        bind({"MotionVectors","DLSS.Input.MV.Subrect.Base.X","DLSS.Input.MV.Subrect.Base.Y"});
        for (const auto& guide:rr_guides) bind(guide);
    }
    bool ready() const noexcept { return ready_; }
    ~RrInputCopiesScope() {
        for(unsigned i=0;i<count_;++i) {
            const auto& b=bindings_[i];
            parameters_->Set(b.guide.resource,b.source);
            parameters_->Set(b.guide.x,b.x); parameters_->Set(b.guide.y,b.y);
        }
    }
    RrInputCopiesScope(const RrInputCopiesScope&)=delete;
    RrInputCopiesScope& operator=(const RrInputCopiesScope&)=delete;
};
}
