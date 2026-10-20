#pragma once
// Opt-in, hotkey-triggered OpenXR texture readback.  GPU copies are recorded on
// the *same* command list as the OpenXR upload, BEFORE xrReleaseSwapchainImage.
// The disk writer observes the upload fence; the presentation thread never
// maps or writes readback data.  No graphics API calls are made from writer.
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <dxgiformat.h>
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <utility>

namespace cheeky::fg {
struct FgBurstCapture {
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct Readback {
        ComPtr<ID3D12Resource> bytes;
        ComPtr<ID3D12Fence> fence;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        std::uint64_t ticket{}, size{}, frame{}, native{}, current{};
        unsigned eye{}, dxgi{},xr_index{};
        bool generated{};
        std::string label;
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Readback> queued;
    std::thread writer;
    bool ending{};
    bool f9_down{},f10_down{},f5_down{},deep_capture{},strict{};
    ULONGLONG strict_until{};
    bool capturing{},recording_frame{};
    unsigned count_per_segment[4]{};
    unsigned serial{};
    ULONGLONG began{};
    std::uint64_t reserved_bytes{};
    std::atomic<std::uint64_t> outstanding_bytes{0};
    static constexpr std::uint64_t budget=2048ull*1024*1024;
    static constexpr unsigned sample_window_ms=2200;
    std::vector<Readback> in_recording;
    unsigned dropped{};
    void (*log)(const char*) noexcept {};
    FgBurstCapture() {writer=std::thread([this]{write_loop();});}
    FgBurstCapture(const FgBurstCapture&)=delete;
    FgBurstCapture& operator=(const FgBurstCapture&)=delete;
    ~FgBurstCapture(){
        {std::lock_guard lock(mutex);ending=true;cv.notify_all();}
        if(writer.joinable())writer.join();
    }
    static std::wstring root() {
        wchar_t path[MAX_PATH]{};
        const DWORD len=GetTempPathW(MAX_PATH,path);
        if(!len||len>=MAX_PATH)return L".";
        std::wstring target(path);target+=L"CheekyFG-Burst";
        CreateDirectoryW(target.c_str(),nullptr);return target;
    }
    void poll_keys() {
        const bool f9=(GetAsyncKeyState(VK_F9)&0x8000)!=0;
        const bool f5=(GetAsyncKeyState(VK_F5)&0x8000)!=0;
        const bool f10=(GetAsyncKeyState(VK_F10)&0x8000)!=0;
        if(f5&&!f5_down){capturing=true;deep_capture=true;began=GetTickCount64();++serial;
            for(auto& n:count_per_segment)n=0;reserved_bytes=0;dropped=0;
            if(log)log("FG DEEP F5: 5 stereo presentation submissions armed; images in %TEMP%\\CheekyFG-Burst");}
        if(f9&&!f9_down){deep_capture=false;capturing=true;began=GetTickCount64();++serial;
            for(auto& n:count_per_segment)n=0;
            reserved_bytes=0;dropped=0;
            if(log)log("FG BURST: F9 armed ~2.2s, up to 2 frames per 550ms segment, 768MiB max; asynchronous .raw + .txt files in %TEMP%\\CheekyFG-Burst");
        }
        if(f10&&!f10_down){strict=true;strict_until=GetTickCount64()+2200;
            if(log)log("FG BURST: F10 STRICT upload fence wait ENABLED for 2.2s (may change pacing)");}
        if(strict&&GetTickCount64()>=strict_until){strict=false;
            if(log)log("FG BURST: strict fence-wait test finished; normal uploads resumed");}
        f9_down=f9;f10_down=f10;f5_down=f5;
        if(capturing && GetTickCount64()-began>=(deep_capture?4000u:sample_window_ms)){capturing=false;deep_capture=false;
            if(log)log("FG BURST: capture window closed; queued GPU readbacks will be written asynchronously");}
    }
    bool begin_submission() {
        abandon();
        if(!capturing || reserved_bytes>=budget)return false;
        const auto elapsed=GetTickCount64()-began;
        const unsigned segment=unsigned(elapsed/550);
        if(deep_capture){if(count_per_segment[0]>=10)return false;++count_per_segment[0];recording_frame=true;
            if(count_per_segment[0]==10){capturing=false;deep_capture=false;}return true;}
        if(segment>=4||count_per_segment[segment]>=2)return false;
        ++count_per_segment[segment];recording_frame=true;return true;
    }
    // Assumes source has exactly one 2D color subresource, and known D3D12 state.
    // Does not touch game-provided depth/motion resources with unknown states.
    bool record(ID3D12Device* dev,ID3D12GraphicsCommandList* cmd,
        ID3D12Resource* texture,D3D12_RESOURCE_STATES state,
        const char* label,unsigned eye,unsigned xr_index,std::uint64_t native,std::uint64_t current,
        bool generated,std::uint64_t presentation) {
        if(!recording_frame||!dev||!cmd||!texture)return false;
        const auto desc=texture->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.DepthOrArraySize!=1||desc.MipLevels!=1||
           desc.SampleDesc.Count!=1)return false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows{};UINT64 row_bytes{},size{};
        dev->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&row_bytes,&size);
        if(!size||size>128ull*1024*1024||reserved_bytes+size>budget||
            outstanding_bytes.load(std::memory_order_relaxed)+size>budget)return false;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=size;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;
        buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Readback out;
        if(FAILED(dev->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
            D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out.bytes))))return false;
        color_transition(cmd,texture,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=texture;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;source.SubresourceIndex=0;
        D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=out.bytes.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=footprint;
        cmd->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        color_transition(cmd,texture,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
        out.footprint=footprint;out.size=size;out.eye=eye;out.xr_index=xr_index;out.dxgi=unsigned(desc.Format);
        out.native=native;out.current=current;out.frame=presentation;out.generated=generated;
        out.label=label;
        reserved_bytes+=size;outstanding_bytes.fetch_add(size,std::memory_order_relaxed);
        in_recording.push_back(std::move(out));return true;
    }
    void commit(ID3D12Fence* fence,std::uint64_t ticket) {
        if(!fence||!ticket){abandon();return;}
        std::lock_guard lock(mutex);
        for(auto& item:in_recording){item.fence=fence;item.ticket=ticket;queued.push_back(std::move(item));}
        in_recording.clear();cv.notify_one();
    }
    void abandon(){
        for(const auto& entry:in_recording)outstanding_bytes.fetch_sub(entry.size,std::memory_order_relaxed);
        in_recording.clear();recording_frame=false;
    }
    // Diagnostic-only bounded CPU wait before xrReleaseSwapchainImage.
    // Do NOT block indefinitely: a cross-API game may still need to issue GPU signals.
    bool wait_upload(ID3D12Fence* fence,std::uint64_t ticket) {
        if(!strict||!fence||!ticket)return true;
        if(fence->GetCompletedValue()>=ticket)return true;
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        bool ready=false;
        if(event){
            if(SUCCEEDED(fence->SetEventOnCompletion(ticket,event)))
                ready=WaitForSingleObject(event,150)==WAIT_OBJECT_0;
            CloseHandle(event);
        }
        if(!ready){strict=false;
            if(log)log("FG BURST: STRICT upload fence TIMEOUT after 150ms; ending strict test, releasing normally");}
        return ready;
    }
    static void write(Readback& p) {
        if(!p.bytes)return;
        const auto folder=root();
        wchar_t name[256]{};
        swprintf_s(name,L"\\pid%lu_f%llu_%s_%s_e%u",GetCurrentProcessId(),static_cast<unsigned long long>(p.frame),
            p.generated?L"gen":L"native",p.label=="openxr"?L"openxr":L"source",p.eye);
        const std::wstring base=folder+name;
        void* bits{};
        D3D12_RANGE read{0,static_cast<SIZE_T>(p.size)};
        if(FAILED(p.bytes->Map(0,&read,&bits))||!bits)return;
        FILE* f = nullptr;
        if(_wfopen_s(&f,(base+L".raw").c_str(),L"wb")==0 && f){
            fwrite(bits,1,static_cast<size_t>(p.size),f);fclose(f);
        }
        D3D12_RANGE write_range{0,0};p.bytes->Unmap(0,&write_range);
        f = nullptr;
        if(_wfopen_s(&f,(base+L".txt").c_str(),L"w")==0 && f){
            fprintf(f,"frame=%llu\nnative_id=%llu\ncurrent_id=%llu\ngenerated=%u\neye=%u\nxr_index=%u\nlabel=%s\nwidth=%llu\nheight=%u\nformat=%u\nrow_pitch=%u\nsize=%llu\n",
                static_cast<unsigned long long>(p.frame),static_cast<unsigned long long>(p.native),
                static_cast<unsigned long long>(p.current),unsigned(p.generated),p.eye,p.xr_index,p.label.c_str(),
                static_cast<unsigned long long>(p.footprint.Footprint.Width),p.footprint.Footprint.Height,p.dxgi,
                p.footprint.Footprint.RowPitch,static_cast<unsigned long long>(p.size));fclose(f);
        }
    }
    void write_loop() {
        for(;;){
            Readback p;
            {std::unique_lock lock(mutex);
                cv.wait(lock,[&]{return ending||!queued.empty();});
                if(queued.empty()){if(ending)return;continue;}
                p=std::move(queued.front());queued.pop_front();
            }
            // Poll rather than blocking the rendering thread; must not map early.
            const auto deadline=GetTickCount64()+10000;
            while(p.fence&&p.fence->GetCompletedValue()<p.ticket&&GetTickCount64()<deadline){
                Sleep(3);
                // Device removal is handled by the normal presenter teardown.
                if(p.fence->GetCompletedValue()==UINT64_MAX)break;
            }
            if(p.fence&&p.fence->GetCompletedValue()!=UINT64_MAX&&p.fence->GetCompletedValue()>=p.ticket)write(p);
            outstanding_bytes.fetch_sub(p.size,std::memory_order_relaxed);
        }
    }
};
} // namespace cheeky::fg
