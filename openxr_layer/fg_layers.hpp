#pragma once
#include <openxr/openxr.h>
#include <array>
#include <vector>
#include <memory>

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
        const XrCompositionLayerBaseHeader* header() {
            switch(type) {
            case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
                projection.views=views.data();return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            case XR_TYPE_COMPOSITION_LAYER_QUAD:return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
            case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder);
            case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cube);
            case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&equirect);
            case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&equirect2);
            default:return nullptr;
            }
        }
    };
    std::vector<Layer> layers;
    // Copies of queued layers share ownership of their independent XR spaces.
    std::vector<std::shared_ptr<void>> space_leases;
    XrCompositionLayerFlags scene_flags{};
    void clear() {layers.clear();space_leases.clear();scene_flags=0;}
    bool retain_spaces(XrSession session, RetainFrameSpace retain) {
        if(!retain)return true; // Callers without asynchronous space ownership.
        for(auto& layer:layers) {
            auto* header=const_cast<XrCompositionLayerBaseHeader*>(layer.header());
            if(!header)return false;
            XrSpace replacement{};auto lease=retain(session,header->space,replacement);
            if(!lease||!replacement){clear();return false;}
            header->space=replacement;space_leases.push_back(std::move(lease));
        }
        return true;
    }
    XrSpace scene_space() const noexcept {
        for(const auto& layer:layers)if(layer.scene)return layer.projection.space;
        return XR_NULL_HANDLE;
    }
    bool prepare(const XrFrameEndInfo* info,const XrCompositionLayerProjection* scene) {
        clear();
        if(!info||!scene||info->next||!info->layers||!info->layerCount||info->layerCount>32)return false;
        bool found{};
        layers.reserve(info->layerCount);
        for(unsigned i=0;i<info->layerCount;++i) {
            const auto* input=info->layers[i];
            if(!input||input->next) {clear();return false;}
            Layer copy;copy.type=input->type;
            if(input==reinterpret_cast<const XrCompositionLayerBaseHeader*>(scene)) {
                if(found) {clear();return false;}
                found=true;copy.scene=true;scene_flags=scene->layerFlags;
                copy.projection=*scene;
                if(scene->viewCount!=2||!scene->views) {clear();return false;}
                for(unsigned eye=0;eye<2;++eye) {
                    copy.views[eye]=scene->views[eye];
                    // Depth describes the upstream image, and is not reused
                    // for a privately generated color image.
                    copy.views[eye].next=nullptr;
                }
            } else switch(input->type) {
            case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
                copy.projection=*reinterpret_cast<const XrCompositionLayerProjection*>(input);
                if(copy.projection.viewCount!=2||!copy.projection.views) {clear();return false;}
                for(unsigned eye=0;eye<2;++eye) {
                    if(copy.projection.views[eye].next) {clear();return false;}
                    copy.views[eye]=copy.projection.views[eye];
                }
                break;
            }
            case XR_TYPE_COMPOSITION_LAYER_QUAD:copy.quad=*reinterpret_cast<const XrCompositionLayerQuad*>(input);break;
            case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:copy.cylinder=*reinterpret_cast<const XrCompositionLayerCylinderKHR*>(input);break;
            case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:copy.cube=*reinterpret_cast<const XrCompositionLayerCubeKHR*>(input);break;
            case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:copy.equirect=*reinterpret_cast<const XrCompositionLayerEquirectKHR*>(input);break;
            case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:copy.equirect2=*reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(input);break;
            default:clear();return false;
            }
            layers.push_back(copy);
        }
        if(!found)clear();
        return found;
    }
    std::vector<const XrCompositionLayerBaseHeader*> replace_scene(const XrCompositionLayerProjection& scene) {
        std::vector<const XrCompositionLayerBaseHeader*> result;
        result.reserve(layers.size());
        for(auto& layer:layers) result.push_back(layer.scene?
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&scene):layer.header());
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
