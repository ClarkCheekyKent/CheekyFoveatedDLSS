#pragma once
#include "overlay.hpp"
#include "overlay_input.hpp"
#include "settings.hpp"
#include <string>
namespace cheeky::standalone {
struct OverlayUiState {
    ULONGLONG next_snapshot{};
    cheeky::foveated_dlss::Settings draft{};
    std::string snapshot,message,menu_key_error;
    std::uint64_t attachment{},request{};
};
void draw_overlay_ui(OverlayUiState&,const OverlayRuntime&,InputState&,const char* renderer,const char* status,bool& open);
}
