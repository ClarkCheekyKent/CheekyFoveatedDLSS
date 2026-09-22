#pragma once
#include "support_zip.hpp"
#include <Windows.h>
#include <sstream>

namespace cheeky::foveated_dlss {
// Explicit settings/log allowlist only; never traverse saves or installation trees.
inline void append_external_support_files(std::vector<SupportFile>& files, std::ostringstream& manifest) {
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) { manifest << "External settings: executable path unavailable\n"; return; }
    const std::filesystem::path exe(executable);
    auto utf8 = [](const std::filesystem::path& p) { const auto s=p.u8string(); return std::string(reinterpret_cast<const char*>(s.data()),s.size()); };
    manifest << "External settings are saved values, not necessarily current in-memory values.\n";
    std::size_t settings_bytes{};
    auto collect = [&](const std::filesystem::path& source, const std::string& name, bool tail=false) {
        manifest << name << " <- " << utf8(source) << ": ";
        HANDLE file=CreateFileW(source.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) { manifest << "unavailable, Windows error " << GetLastError() << '\n'; return; }
        struct Close { HANDLE h; ~Close(){CloseHandle(h);} } close{file};
        LARGE_INTEGER size{},offset{};
        if(!GetFileSizeEx(file,&size) || size.QuadPart<0) { manifest << "size unavailable\n"; return; }
        const LONGLONG limit=tail ? 2*1024*1024 : 512*1024;
        if(!tail && (size.QuadPart>limit || settings_bytes+size.QuadPart>2*1024*1024)) { manifest << "skipped: settings size limit\n"; return; }
        const DWORD count=static_cast<DWORD>((std::min)(size.QuadPart,limit));
        offset.QuadPart=size.QuadPart-count;
        if(!SetFilePointerEx(file,offset,nullptr,FILE_BEGIN)) { manifest << "seek failed\n"; return; }
        std::string data(count,'\0'); DWORD read{};
        if(!ReadFile(file,data.data(),count,&read,nullptr) || read!=count) { manifest << "read failed or file changed\n"; return; }
        if(!tail) settings_bytes+=read;
        manifest << "captured " << read << " of " << size.QuadPart << " bytes" << (tail ? " (tail)" : "") << '\n';
        files.push_back({name,std::move(data)});
    };
    for(const auto* name : {L"RealVR.ini",L"RealVR64.ini"}) collect(exe.parent_path()/name,utf8(name));
    for(const auto* name : {L"RealVR64.log",L"RealVR.log"}) collect(exe.parent_path()/name,utf8(name),true);
    wchar_t local[32768]{};
    const auto local_length=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
    if(local_length && local_length<32768) {
        if(_wcsicmp(exe.filename().c_str(),L"Cyberpunk2077.exe")==0)
            collect(std::filesystem::path(local)/L"CD Projekt Red/Cyberpunk 2077/UserSettings.json","game-UserSettings.json");
        else if(_wcsicmp(exe.filename().c_str(),L"HogwartsLegacy.exe")==0) {
            for(const auto* platform : {L"WindowsNoEditor",L"Windows"})
                for(const auto* name : {L"GameUserSettings.ini",L"Engine.ini"})
                    collect(std::filesystem::path(local)/L"Hogwarts Legacy/Saved/Config"/platform/name,"game-"+utf8(platform)+"-"+utf8(name));
        } else manifest << "Game settings file lookup: no known path for " << utf8(exe.filename()) << '\n';
    } else manifest << "Game settings: LOCALAPPDATA unavailable\n";
}
}
