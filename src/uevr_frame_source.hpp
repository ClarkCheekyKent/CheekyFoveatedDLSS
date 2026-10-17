#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cheeky::foveated_dlss::uevr_frames {
// Resolve the existing UEVR runtime virtual by its MSVC RTTI, not a build RVA.
// The slot belongs to the VRRuntime ABI (destructor included). Reject unknown
// layouts rather than hooking an arbitrary executable address.
struct Section {std::uint32_t begin{},end{},flags{};};
struct Function {std::uint32_t begin{},end{};};
struct Image {
    const unsigned char* base{};std::uint32_t size{};
    std::vector<Section> sections;
    std::vector<Function> functions;
    bool contains(std::uint64_t rva,std::size_t bytes=1) const {return rva<size&&bytes<=size-rva;}
    template<class T> T read(std::uint32_t rva) const {T value{};if(contains(rva,sizeof(T)))std::memcpy(&value,base+rva,sizeof(T));return value;}
    bool executable(std::uintptr_t address) const {
        if(address<std::uintptr_t(base))return false;auto rva=address-std::uintptr_t(base);
        for(auto s:sections)if(rva>=s.begin&&rva<s.end)return (s.flags&IMAGE_SCN_MEM_EXECUTE)!=0;
        return false;
    }
    bool text(std::uint32_t rva,const char* name) const {
        auto length=std::strlen(name)+1;return contains(rva,length)&&std::memcmp(base+rva,name,length)==0;
    }
    Function function(std::uint32_t rva) const {
        auto it=std::upper_bound(functions.begin(),functions.end(),rva,[](auto value,const Function& f){return value<f.begin;});
        if(it==functions.begin())return {};--it;return rva<it->end?*it:Function{};
    }
};
inline bool headers(HMODULE module,std::uint32_t& size,const IMAGE_NT_HEADERS64*& nt) noexcept {
    __try {
        auto base=reinterpret_cast<const unsigned char*>(module);
        auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if(!module||dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>4096)return false;
        nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||
           nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||nt->FileHeader.NumberOfSections>96)return false;
        size=nt->OptionalHeader.SizeOfImage;return size>=4096&&size<1024u*1024u*1024u;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
inline Image image(HMODULE module) {
    Image result;const IMAGE_NT_HEADERS64* nt{};
    if(!headers(module,result.size,nt))return {};
    result.base=reinterpret_cast<const unsigned char*>(module);
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const auto& s=sections[i];
        const auto length=std::max(s.Misc.VirtualSize,s.SizeOfRawData);
        if(!length||!result.contains(s.VirtualAddress,length))continue;
        result.sections.push_back({s.VirtualAddress,s.VirtualAddress+length,s.Characteristics});
    }
    const auto& exceptions=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if(result.contains(exceptions.VirtualAddress,exceptions.Size))for(unsigned offset=0;offset+12<=exceptions.Size;offset+=12){
        const auto begin=result.read<std::uint32_t>(exceptions.VirtualAddress+offset),end=result.read<std::uint32_t>(exceptions.VirtualAddress+offset+4);
        if(begin<end&&result.contains(begin,end-begin))result.functions.push_back({begin,end});
    }
    std::sort(result.functions.begin(),result.functions.end(),[](const auto& a,const auto& b){return a.begin<b.begin;});
    return result;
}
// These three call-site literals occur together in UEVR's new RHI-command
// dispatcher. Require all three in the same unwind-defined function before
// treating its existing twelve-pointer forwarding ABI as a frame boundary.
inline std::vector<Function> rhi_dispatchers(const Image& image) {
    static const char* volatile names[]{"[ISceneViewExtension] Successfully hijacked command list! {}",
        "[ISceneViewExtension] Command list frame count: {}","[ISceneViewExtension] Original vtable: {:x}"};
    std::vector<std::pair<std::uint32_t,unsigned>> strings,functions;
    for(unsigned n=0;n<3;++n)for(auto s:image.sections)if((s.flags&IMAGE_SCN_MEM_READ)&&!(s.flags&IMAGE_SCN_MEM_EXECUTE))
        for(auto p=s.begin;p+std::strlen(names[n])+1<=s.end;++p)if(image.text(p,names[n]))strings.emplace_back(p,1u<<n);
    for(auto s:image.sections)if(s.flags&IMAGE_SCN_MEM_EXECUTE)for(auto p=s.begin;p+7<=s.end;++p){
        const auto* instruction=image.base+p;
        if((instruction[0]&0xf8)!=0x48||instruction[1]!=0x8d||(instruction[2]&0xc7)!=0x05)continue;
        std::int32_t displacement{};std::memcpy(&displacement,instruction+3,4);
        const auto target=std::int64_t(p)+7+displacement;
        for(auto [text,flag]:strings)if(target==text){const auto function=image.function(p);if(!function.end)continue;
            auto it=std::find_if(functions.begin(),functions.end(),[&](auto f){return f.first==function.begin;});
            if(it==functions.end())functions.emplace_back(function.begin,flag);else it->second|=flag;}
    }
    std::vector<Function> found;for(auto [begin,flags]:functions)if(flags==7)found.push_back(image.function(begin));
    if(found.size()>8)return {};return found;
}
struct Target {void* function{};std::uint32_t table_rva{};};
inline bool frame_method(const Image& image,std::uintptr_t function) {
    if(function<std::uintptr_t(image.base))return false;
    const auto rva=function-std::uintptr_t(image.base);
    if(!image.contains(rva,112))return false;
    const auto* code=image.base+rva;
    if(code[0]!=0x48||code[1]!=0x89||code[2]!=0x5c||code[3]!=0x24)return false;
    // Validate the supported MSVC method body as well as RTTI/slot: save
    // frame=EDX in ESI and this=RCX in RDI, then store frame and its adjacent
    // valid flag. Member displacements and relative call addresses may vary.
    bool frame_argument{},object_argument{};
    for(unsigned p=0;p<32;++p){frame_argument|=code[p]==0x8b&&code[p+1]==0xf2;
        object_argument|=code[p]==0x48&&code[p+1]==0x8b&&code[p+2]==0xf9;}
    if(!frame_argument||!object_argument)return false;
    for(unsigned p=24;p+13<=112;++p)if(code[p]==0x89&&code[p+1]==0xb7&&code[p+6]==0xc6&&code[p+7]==0x87&&code[p+12]==1){
        std::uint32_t frame{},flag{};std::memcpy(&frame,code+p+2,4);std::memcpy(&flag,code+p+8,4);
        if(frame>=16&&frame<1024*1024&&flag==frame+4)return true;
    }
    for(unsigned p=24;p+7<=112;++p)if(code[p]==0x89&&code[p+1]==0x77&&code[p+3]==0xc6&&code[p+4]==0x47&&
        code[p+6]==1&&code[p+2]>=16&&code[p+2]<124&&code[p+5]==code[p+2]+4)return true;
    return false;
}
inline Target locate(const Image& image) {
    if(!image.base)return {};
    std::vector<std::uint32_t> types,locators;
    constexpr char name[]=".?AUOpenXR@runtimes@@";
    for(auto s:image.sections)if((s.flags&IMAGE_SCN_MEM_READ)&&!(s.flags&IMAGE_SCN_MEM_EXECUTE))
        for(auto p=s.begin;p+sizeof(name)<=s.end;++p)
            if(image.text(p,name)&&p>=16)types.push_back(p-16);
    for(auto s:image.sections)if((s.flags&IMAGE_SCN_MEM_READ)&&!(s.flags&IMAGE_SCN_MEM_EXECUTE))
        for(auto p=(s.begin+3)&~3u;p+24<=s.end;p+=4){
            if(image.read<std::uint32_t>(p)!=1||image.read<std::uint32_t>(p+4)!=0||image.read<std::uint32_t>(p+8)!=0||
               image.read<std::uint32_t>(p+20)!=p)continue;
            const auto type=image.read<std::uint32_t>(p+12);
            if(std::find(types.begin(),types.end(),type)==types.end())continue;
            const auto hierarchy=image.read<std::uint32_t>(p+16);
            if(!image.contains(hierarchy,16))continue;
            const auto count=image.read<std::uint32_t>(hierarchy+8),bases=image.read<std::uint32_t>(hierarchy+12);
            if(count<2||count>16||!image.contains(bases,count*4))continue;
            bool parent{};
            for(unsigned i=1;i<count;++i){const auto descriptor=image.read<std::uint32_t>(bases+i*4);
                if(!image.contains(descriptor,4))continue;
                const auto parent_type=image.read<std::uint32_t>(descriptor);
                parent|=image.contains(parent_type,16)&&image.text(parent_type+16,".?AUVRRuntime@@");}
            if(parent)locators.push_back(p);
        }
    Target found;
    for(auto s:image.sections)if((s.flags&IMAGE_SCN_MEM_READ)&&!(s.flags&IMAGE_SCN_MEM_EXECUTE))
        for(auto p=(s.begin+7)&~7u;p+8+24*8<=s.end;p+=8){
            const auto col=image.read<std::uintptr_t>(p);
            if(col<std::uintptr_t(image.base)||col-std::uintptr_t(image.base)>UINT32_MAX||
               std::find(locators.begin(),locators.end(),std::uint32_t(col-std::uintptr_t(image.base)))==locators.end())continue;
            bool valid=true;
            for(unsigned i=0;i<24;++i)valid&=image.executable(image.read<std::uintptr_t>(p+8+i*8));
            if(!valid)continue;
            const auto type=image.read<std::uintptr_t>(p+8+3*8)-std::uintptr_t(image.base);
            // OpenXR's type() is the leaf return of VRRuntime::Type::OPENXR=1.
            constexpr unsigned char leaf8[]{0xb0,1,0xc3},leaf32[]{0xb8,1,0,0,0,0xc3};
            if(!image.contains(type,6)||(std::memcmp(image.base+type,leaf8,sizeof(leaf8))&&std::memcmp(image.base+type,leaf32,sizeof(leaf32))))continue;
            auto function=reinterpret_cast<void*>(image.read<std::uintptr_t>(p+8+14*8));
            if(!frame_method(image,reinterpret_cast<std::uintptr_t>(function)))continue;
            if(found.function&&found.function!=function)return {}; // Ambiguous ABI.
            found={function,p+8};
        }
    return found;
}
inline Target locate(HMODULE module){return locate(image(module));}
}
