#pragma once
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#pragma comment(lib, "pdh.lib")

namespace cheeky::foveated_dlss {
// Whole-system and game-process CPU/GPU load, sampled once per second on a
// background thread (PDH GPU engine enumeration can take several ms, so it
// never runs on the game's frame tick). GPU numbers follow Task Manager: an
// engine's busy time is the sum over processes, the GPU is its busiest engine.
struct SystemLoadSample {
    bool valid{},gpu_valid{};
    std::uint64_t tick{},samples{};
    double cpu_total{},cpu_busiest_core{},cpu_process{}; // Percent; process is of all logical cores.
    double gpu_busy{},gpu_game{};                       // Percent of the busiest render/compute engine on the game's adapter.
    double gpu_encode{};                                // Video encode engine (Virtual Desktop streaming).
    char gpu_engine[16]{};                              // Engine type of the busiest render/compute engine.
    double collect_ms{},collect_max_ms{};               // Last and worst PDH collection time.
};

class SystemLoadSampler {
public:
    // Starts the sampler once; the thread keeps this module loaded while it runs.
    void ensure_started() noexcept {
        if(running.exchange(true)){stop_requested=false;return;}
        HMODULE self{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&thread_main),&self)){running=false;return;}
        stop_requested=false;
        if(auto thread=CreateThread(nullptr,0,&thread_main,this,0,nullptr))CloseHandle(thread);
        else {FreeLibrary(self);running=false;}
    }
    // The thread exits within a second and releases its module reference.
    void stop() noexcept {stop_requested=true;}
    void set_adapter(LUID value) noexcept {std::lock_guard lock(mutex);luid=value;}
    SystemLoadSample latest() const noexcept {std::lock_guard lock(mutex);return sample;}

private:
    mutable std::mutex mutex;
    SystemLoadSample sample;
    LUID luid{};
    std::atomic<bool> running{},stop_requested{};

    static DWORD WINAPI thread_main(void* context) {
        SetThreadDescription(GetCurrentThread(),L"Cheeky system load");
        static_cast<SystemLoadSampler*>(context)->run();
        HMODULE self{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&thread_main),&self);
        FreeLibraryAndExitThread(self,0);
    }
    static std::uint64_t file_time(const FILETIME& t) noexcept {return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
    static bool formatted(PDH_HCOUNTER counter,std::vector<unsigned char>& storage,PDH_FMT_COUNTERVALUE_ITEM_W*& items,DWORD& count) {
        DWORD bytes{};count=0;
        if(PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,&bytes,&count,nullptr)!=PDH_MORE_DATA)return false;
        storage.resize(bytes);items=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
        return PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,&bytes,&count,items)==ERROR_SUCCESS;
    }
    void run() noexcept {
        PDH_HQUERY query{};PDH_HCOUNTER cores{},engines{};
        if(PdhOpenQueryW(nullptr,0,&query)==ERROR_SUCCESS){
            if(PdhAddEnglishCounterW(query,L"\\Processor(*)\\% Processor Time",0,&cores)!=ERROR_SUCCESS)cores=nullptr;
            if(PdhAddEnglishCounterW(query,L"\\GPU Engine(*)\\Utilization Percentage",0,&engines)!=ERROR_SUCCESS)engines=nullptr;
            PdhCollectQueryData(query);
        }
        FILETIME idle0{},kernel0{},user0{},created{},exited{},pkernel0{},puser0{};
        GetSystemTimes(&idle0,&kernel0,&user0);GetProcessTimes(GetCurrentProcess(),&created,&exited,&pkernel0,&puser0);
        wchar_t pid_tag[32]{};swprintf_s(pid_tag,L"pid_%lu_",GetCurrentProcessId());
        std::vector<unsigned char> storage;std::uint64_t samples{};
        double collect_max{};
        const auto ms_since=[](LARGE_INTEGER start){LARGE_INTEGER now{},freq{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&freq);
            return 1000.*double(now.QuadPart-start.QuadPart)/double(freq.QuadPart);};
        while(!stop_requested){
            for(unsigned i=0;i<10&&!stop_requested;++i)Sleep(100);
            if(stop_requested)break;
            SystemLoadSample next;next.tick=GetTickCount64();next.samples=++samples;
            FILETIME idle{},kernel{},user{},pkernel{},puser{};
            if(GetSystemTimes(&idle,&kernel,&user)&&GetProcessTimes(GetCurrentProcess(),&created,&exited,&pkernel,&puser)){
                const auto busy_total=(file_time(kernel)-file_time(kernel0))+(file_time(user)-file_time(user0)); // Kernel time includes idle.
                const auto idle_delta=file_time(idle)-file_time(idle0);
                const auto process=(file_time(pkernel)-file_time(pkernel0))+(file_time(puser)-file_time(puser0));
                if(busy_total){
                    next.cpu_total=100.*double(busy_total-idle_delta)/double(busy_total);
                    // Wall time per core equals system time divided by logical cores.
                    next.cpu_process=100.*double(process)/double(busy_total);
                    next.valid=true;
                }
                idle0=idle;kernel0=kernel;user0=user;pkernel0=pkernel;puser0=puser;
            }
            LARGE_INTEGER collect_start{};QueryPerformanceCounter(&collect_start);
            const bool collected=query&&PdhCollectQueryData(query)==ERROR_SUCCESS;
            next.collect_ms=ms_since(collect_start);collect_max=(std::max)(collect_max,next.collect_ms);next.collect_max_ms=collect_max;
            if(collected){
                PDH_FMT_COUNTERVALUE_ITEM_W* items{};DWORD count{};
                if(cores&&formatted(cores,storage,items,count))
                    for(DWORD i=0;i<count;++i)if(items[i].FmtValue.CStatus==ERROR_SUCCESS&&!std::wcsstr(items[i].szName,L"_Total"))
                        next.cpu_busiest_core=(std::max)(next.cpu_busiest_core,items[i].FmtValue.doubleValue);
                if(engines&&formatted(engines,storage,items,count)){
                    wchar_t luid_tag[64]{};{std::lock_guard lock(mutex);
                        if(luid.LowPart||luid.HighPart)swprintf_s(luid_tag,L"luid_0x%08lX_0x%08lX_",static_cast<unsigned long>(luid.HighPart),luid.LowPart);}
                    // Engine key: everything after "luid_", e.g. luid_..._phys_0_eng_0_engtype_3D.
                    std::map<std::wstring,std::pair<double,double>> per_engine; // total, game
                    for(DWORD i=0;i<count;++i){
                        if(items[i].FmtValue.CStatus!=ERROR_SUCCESS)continue;
                        const std::wstring name=items[i].szName;
                        const auto at=name.find(L"luid_");if(at==std::wstring::npos)continue;
                        if(luid_tag[0]&&name.find(luid_tag)==std::wstring::npos)continue;
                        auto& e=per_engine[name.substr(at)];
                        e.first+=items[i].FmtValue.doubleValue;
                        if(name.find(pid_tag)==0)e.second+=items[i].FmtValue.doubleValue;
                    }
                    for(const auto& [engine,value]:per_engine){
                        if(engine.find(L"engtype_VideoEncode")!=std::wstring::npos){next.gpu_encode=(std::max)(next.gpu_encode,value.first);continue;}
                        if(engine.find(L"engtype_Video")!=std::wstring::npos)continue; // Decode/processing.
                        if(value.first>next.gpu_busy){
                            next.gpu_busy=value.first;
                            const auto type=engine.find(L"engtype_");
                            const auto label=type==std::wstring::npos?std::wstring{}:engine.substr(type+8);
                            for(std::size_t k=0;k+1<sizeof(next.gpu_engine)&&k<label.size();++k)next.gpu_engine[k]=char(label[k]<128?label[k]:'?');
                        }
                        next.gpu_game=(std::max)(next.gpu_game,value.second);
                    }
                    next.gpu_busy=(std::min)(next.gpu_busy,100.);next.gpu_encode=(std::min)(next.gpu_encode,100.);next.gpu_game=(std::min)(next.gpu_game,100.);
                    next.gpu_valid=!per_engine.empty();
                }
            }
            std::lock_guard lock(mutex);sample=next;
        }
        if(query)PdhCloseQuery(query);
        running=false;
    }
};
}
