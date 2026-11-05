#pragma once
#include <openxr/openxr.h>
#include <array>
#include <vector>
#include <memory>
#include "submission_diagnostics.hpp"

namespace cheeky::fg {
using RetainFrameSpace = std::shared_ptr<void> (*)(XrSession, XrSpace, XrSpace&);
// EndFrame's structures are usually stack allocated. Own copies of the
// auxiliary layer descriptions until the extra WaitFrame slot is submitted.
// Their released swapchains remain valid until the app renders its next frame.
struct FrameLayers {
    struct Layer {
        XrStructureType type{};
        bool scene{};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::array<XrCompositionLayerProjectionView,2> views{};
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
        XrCompositionLayerCubeKHR cube{XR_TYPE_COMPOSITION_LAYER_CUBE_KHR};
        XrCompositionLayerEquirectKHR equirect{XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR};
        XrCompositionLayerEquirect2KHR equirect2{XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
        bool has_color_scale_bias{};
        XrCompositionLayerColorScaleBiasKHR color_scale_bias{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
        const void* extension_chain() const noexcept {
            return has_color_scale_bias?&color_scale_bias:nullptr;
        }
        const XrCompositionLayerBaseHeader* header() {
            XrCompositionLayerBaseHeader* result{};
            switch(type) {
            case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
                projection.views=views.data();result=reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection);break;
            case XR_TYPE_COMPOSITION_LAYER_QUAD:result=reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);break;
            case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:result=reinterpret_cast<XrCompositionLayerBaseHeader*>(&cylinder);break;
            case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:result=reinterpret_cast<XrCompositionLayerBaseHeader*>(&cube);break;
            case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:result=reinterpret_cast<XrCompositionLayerBaseHeader*>(&equirect);break;
            case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:result=reinterpret_cast<XrCompositionLayerBaseHeader*>(&equirect2);break;
            default:return nullptr;
            }
            // Queued frames copy/move Layer values. Rebind every borrowed
            // pointer to this copy's storage when building a submission.
            result->next=extension_chain();return result;
        }
    };
    std::vector<Layer> layers;
    // Copies of queued layers share ownership of their independent XR spaces.
    std::vector<std::shared_ptr<void>> space_leases;
    XrCompositionLayerFlags scene_flags{};
    XrCompositionLayerProjection replacement_scene{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    void clear() {layers.clear();space_leases.clear();scene_flags=0;replacement_scene={XR_TYPE_COMPOSITION_LAYER_PROJECTION};}
    bool retain_spaces(XrSession session, RetainFrameSpace retain,
        cheeky::openxr::SubmissionDiagnostic* diagnostic=nullptr) {
        if(diagnostic)*diagnostic={};
        if(!retain)return true; // Callers without asynchronous space ownership.
        for(auto& layer:layers) {
            auto* header=const_cast<XrCompositionLayerBaseHeader*>(layer.header());
            if(!header)return false;
            XrSpace replacement{};auto lease=retain(session,header->space,replacement);
            if(!lease||!replacement){
                if(diagnostic){diagnostic->reason="OpenXR layer space could not be retained";
                    diagnostic->layer=int(&layer-layers.data());diagnostic->structure_type=header->type;
                    diagnostic->space=header->space;}
                clear();return false;}
            header->space=replacement;space_leases.push_back(std::move(lease));
        }
        return true;
    }
    XrSpace scene_space() const noexcept {
        for(const auto& layer:layers)if(layer.scene)return layer.projection.space;
        return XR_NULL_HANDLE;
    }
    bool prepare(const XrFrameEndInfo* info,const XrCompositionLayerProjection* scene,
        cheeky::openxr::SubmissionDiagnostic* diagnostic=nullptr) {
        clear();
        if(diagnostic)*diagnostic={};
        const auto reject=[&](const char* reason,int layer=-1,int eye=-1,
            XrStructureType type=XR_TYPE_UNKNOWN,unsigned views=0){
            if(diagnostic){diagnostic->reason=reason;diagnostic->layer=layer;diagnostic->eye=eye;
                diagnostic->structure_type=type;diagnostic->view_count=views;}
            clear();return false;};
        if(!info||!scene)return reject("OpenXR stereo projection is unavailable");
        if(info->next)return reject("OpenXR frame has an unsupported extension chain",-1,-1,
            static_cast<const XrBaseInStructure*>(info->next)->type);
        if(!info->layers||!info->layerCount)return reject("OpenXR frame has no composition layers");
        if(info->layerCount>32)return reject("OpenXR frame exceeds the supported layer count");
        bool found{};
        layers.reserve(info->layerCount);
        for(unsigned i=0;i<info->layerCount;++i) {
            const auto* input=info->layers[i];
            if(!input)return reject("OpenXR frame contains a null layer",int(i));
            Layer copy;copy.type=input->type;
            for(auto* extension=static_cast<const XrBaseInStructure*>(input->next);extension;extension=extension->next){
                if(extension->type!=XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR)
                    return reject("OpenXR layer has an unsupported extension chain",int(i),-1,extension->type);
                if(copy.has_color_scale_bias)
                    return reject("OpenXR layer repeats the color scale/bias extension",int(i),-1,extension->type);
                copy.has_color_scale_bias=true;
                copy.color_scale_bias=*reinterpret_cast<const XrCompositionLayerColorScaleBiasKHR*>(extension);
                copy.color_scale_bias.next=nullptr;
            }
            if(input==reinterpret_cast<const XrCompositionLayerBaseHeader*>(scene)) {
                if(found)return reject("OpenXR scene projection is submitted more than once",int(i));
                found=true;copy.scene=true;scene_flags=scene->layerFlags;
                copy.projection=*scene;
                if(scene->viewCount!=2||!scene->views)return reject("OpenXR scene projection has an unsupported view layout",
                    int(i),-1,input->type,scene->viewCount);
                for(unsigned eye=0;eye<2;++eye) {
                    copy.views[eye]=scene->views[eye];
                    // Depth describes the upstream image, and is not reused
                    // for a privately generated color image.
                    copy.views[eye].next=nullptr;
                }
            } else switch(input->type) {
            case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
                copy.projection=*reinterpret_cast<const XrCompositionLayerProjection*>(input);
                if(copy.projection.viewCount!=2||!copy.projection.views)return reject("OpenXR auxiliary projection has an unsupported view layout",
                    int(i),-1,input->type,copy.projection.viewCount);
                for(unsigned eye=0;eye<2;++eye) {
                    if(copy.projection.views[eye].next)return reject("OpenXR auxiliary view has an unsupported extension chain",int(i),int(eye),
                        static_cast<const XrBaseInStructure*>(copy.projection.views[eye].next)->type);
                    copy.views[eye]=copy.projection.views[eye];
                }
                break;
            }
            case XR_TYPE_COMPOSITION_LAYER_QUAD:copy.quad=*reinterpret_cast<const XrCompositionLayerQuad*>(input);break;
            case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:copy.cylinder=*reinterpret_cast<const XrCompositionLayerCylinderKHR*>(input);break;
            case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:copy.cube=*reinterpret_cast<const XrCompositionLayerCubeKHR*>(input);break;
            case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:copy.equirect=*reinterpret_cast<const XrCompositionLayerEquirectKHR*>(input);break;
            case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:copy.equirect2=*reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(input);break;
            default:return reject("OpenXR composition layer type is unsupported",int(i),-1,input->type);
            }
            layers.push_back(copy);
        }
        if(!found)return reject("OpenXR selected scene is missing from the frame layers");
        return found;
    }
    std::vector<const XrCompositionLayerBaseHeader*> replace_scene(const XrCompositionLayerProjection& scene) {
        std::vector<const XrCompositionLayerBaseHeader*> result;
        result.reserve(layers.size());
        replacement_scene=scene;
        for(auto& layer:layers){
            if(layer.scene){
                replacement_scene.next=layer.extension_chain();
                result.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&replacement_scene));
            }else result.push_back(layer.header());
        }
        return result;
    }
    std::vector<const XrCompositionLayerBaseHeader*> original() {
        std::vector<const XrCompositionLayerBaseHeader*> result;
        result.reserve(layers.size());
        for(auto& layer:layers)result.push_back(layer.header());
        return result;
    }
};
}
