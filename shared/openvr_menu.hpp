#pragma once
#include "../third_party/openvr/include/openvr.h"
#include <cstdint>
#include <string>

namespace cheeky {
inline std::string openvr_menu_key(std::uint32_t process_id) {
    return "cheeky.foveated_dlss.menu." + std::to_string(process_id);
}

// Only relax controller-input availability for our visible, interactive menu.
// The caller must still require valid scene frames and fresh/valid gaze data.
// A dashboard or a foreign/hidden overlay never grants this exception.
template<class Overlay>
bool openvr_menu_allows_gaze(Overlay& overlay, std::uint32_t process_id) {
    if (overlay.IsDashboardVisible()) return false;
    vr::VROverlayHandle_t handle{vr::k_ulOverlayHandleInvalid};
    if (overlay.FindOverlay(openvr_menu_key(process_id).c_str(), &handle) != vr::VROverlayError_None ||
        handle == vr::k_ulOverlayHandleInvalid || overlay.GetOverlayRenderingPid(handle) != process_id ||
        !overlay.IsOverlayVisible(handle)) return false;
    std::uint32_t flags{};
    vr::VROverlayInputMethod input{};
    return overlay.GetOverlayFlags(handle, &flags) == vr::VROverlayError_None &&
        (flags & vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible) != 0 &&
        overlay.GetOverlayInputMethod(handle, &input) == vr::VROverlayError_None &&
        input == vr::VROverlayInputMethod_Mouse;
}
}
