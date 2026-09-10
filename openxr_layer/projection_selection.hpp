#pragma once
#include "../third_party/openxr/include/openxr/openxr.h"

namespace cheeky::openxr {
struct ProjectionSelection {
    const XrCompositionLayerProjection* projection{};
    bool ambiguous{};
    bool unsupported{};
};

// UEVR's Virtual Desktop timewarp workaround submits an alpha-blended
// 4x4 placeholder before the game projection. Check both the layout and the
// backing swapchain; a small crop of a real scene is not a placeholder.
template <class IsDummySwapchain>
bool is_dummy_projection(const XrCompositionLayerProjection& projection,
                         IsDummySwapchain is_dummy_swapchain) {
    if (projection.viewCount != 2 || !projection.views ||
        !(projection.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT))
        return false;
    const auto chain = projection.views[0].subImage.swapchain;
    if (!is_dummy_swapchain(chain)) return false;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto& image = projection.views[eye].subImage;
        if (image.swapchain != chain || image.imageArrayIndex != 0 ||
            image.imageRect.offset.x != static_cast<int>(eye * 2) || image.imageRect.offset.y != 0 ||
            image.imageRect.extent.width != 2 || image.imageRect.extent.height != 4)
            return false;
    }
    return true;
}

// Shared by gaze publication and calibration. Never guess between real scenes.
template <class IsDummySwapchain>
ProjectionSelection select_projection(const XrFrameEndInfo* info, IsDummySwapchain is_dummy_swapchain) {
    ProjectionSelection selected;
    if (!info || !info->layers) return selected;
    for (unsigned index = 0; index < info->layerCount; ++index) {
        const auto* layer = info->layers[index];
        if (!layer || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (projection->viewCount != 2 || !projection->views) {
            selected.unsupported = true;
            continue;
        }
        if (is_dummy_projection(*projection, is_dummy_swapchain)) continue;
        if (selected.projection) selected.ambiguous = true;
        selected.projection = projection;
    }
    if (selected.ambiguous || selected.unsupported) selected.projection = nullptr;
    return selected;
}
} // namespace cheeky::openxr
