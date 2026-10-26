#include "../standalone/menu_cursor_draw.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={800,600};io.DeltaTime=1.f/60;
    io.Fonts->Build();io.MousePos={250,200};io.MouseDrawCursor=false;
    ImGui::NewFrame();ImGui::SetNextWindowPos({10,10});ImGui::SetNextWindowSize({300,200});
    ImGui::Begin("Menu");ImGui::TextUnformatted("Menu contents stay visible");ImGui::End();
    cheeky::standalone::draw_desktop_menu_cursor();
    ImGui::GetForegroundDrawList()->AddRectFilled({400,300},{410,310},IM_COL32_WHITE);ImGui::Render();
    {
        auto* desktop=ImGui::GetDrawData();int before=0,cursor_indices=0;
        for(auto* list:desktop->CmdLists){bool cursor=false;for(const auto& cmd:list->CmdBuffer){
            if(cmd.UserCallback==cheeky::standalone::menu_cursor_marker)cursor=cmd.UserCallbackData==reinterpret_cast<void*>(1);
            else if(!cmd.UserCallback){before+=int(cmd.ElemCount);if(cursor)cursor_indices+=int(cmd.ElemCount);}
        }}
        cheeky::standalone::MenuHeadsetDrawData filter;auto* headset=filter.prepare(desktop,true);int after=0;
        for(auto* list:headset->CmdLists)for(const auto& cmd:list->CmdBuffer){
            if(cmd.UserCallback==cheeky::standalone::menu_cursor_marker)
                throw std::runtime_error("Cursor markers leaked into headset output");
            if(!cmd.UserCallback)after+=int(cmd.ElemCount);
        }
        if(!(before>after&&after>0)||before-after!=cursor_indices||filter.prepare(desktop,false)!=desktop)
            throw std::runtime_error("Menu geometry lost or desktop cursor not preserved");
    }
    ImGui::DestroyContext();std::cout<<"PASS headset omits cursor pixels while desktop and menu geometry remain intact\n";
}
