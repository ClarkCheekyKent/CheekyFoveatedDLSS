#pragma once
#include <imgui.h>
#include <imgui_internal.h>

namespace cheeky::standalone {
// Desktop output keeps the normal software cursor. Only the headset copy
// omits the tagged cursor geometry, since OpenXR draws its own live cursor.
inline void menu_cursor_marker(const ImDrawList*, const ImDrawCmd*) {}
inline void draw_desktop_menu_cursor() {
    if (ImGui::GetMouseCursor() == ImGuiMouseCursor_None) return;
    auto* list = ImGui::GetForegroundDrawList();
    list->AddCallback(menu_cursor_marker, reinterpret_cast<void*>(1));
    ImGui::RenderMouseCursor(ImGui::GetIO().MousePos, ImGui::GetStyle().MouseCursorScale,
        ImGui::GetMouseCursor(), IM_COL32_WHITE, IM_COL32_BLACK, IM_COL32(0, 0, 0, 48));
    list->AddCallback(menu_cursor_marker, reinterpret_cast<void*>(2));
}
struct MenuHeadsetDrawData {
    ImDrawData data;
    ImDrawList* filtered{};
    MenuHeadsetDrawData() = default;
    MenuHeadsetDrawData(const MenuHeadsetDrawData&) = delete;
    MenuHeadsetDrawData& operator=(const MenuHeadsetDrawData&) = delete;
    ~MenuHeadsetDrawData() { if (filtered) IM_DELETE(filtered); }
    ImDrawData* prepare(ImDrawData* source, bool separate) {
        data.Clear();
        if (filtered) { IM_DELETE(filtered); filtered = nullptr; }
        if (!separate) return source;
        data.Valid = source->Valid; data.DisplayPos = source->DisplayPos;
        data.DisplaySize = source->DisplaySize; data.FramebufferScale = source->FramebufferScale;
        data.OwnerViewport = source->OwnerViewport; data.Textures = source->Textures;
        for (auto* list : source->CmdLists) {
            bool tagged = false;
            for (const auto& command : list->CmdBuffer) if (command.UserCallback == menu_cursor_marker) tagged = true;
            if (!tagged) { data.AddDrawList(list); continue; }
            IM_ASSERT(!filtered); // The menu uses the main viewport's foreground list.
            filtered = list->CloneOutput();
            bool cursor = false; int write = 0;
            for (const auto& command : list->CmdBuffer) {
                if (command.UserCallback == menu_cursor_marker) {
                    cursor = command.UserCallbackData == reinterpret_cast<void*>(1); continue;
                }
                if (!cursor) filtered->CmdBuffer[write++] = command;
            }
            filtered->CmdBuffer.resize(write);
            if (write) {
                // CloneOutput contains render buffers only, without ImGui's
                // construction cursors required by AddDrawList's assertions.
                data.CmdLists.push_back(filtered);
                ++data.CmdListsCount;
                data.TotalVtxCount += filtered->VtxBuffer.Size;
                data.TotalIdxCount += filtered->IdxBuffer.Size;
            }
        }
        return &data;
    }
};
}
