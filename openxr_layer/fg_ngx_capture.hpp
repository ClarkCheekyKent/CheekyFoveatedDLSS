#pragma once
// F5 captures five consecutive stereo NGX FrameGeneration evaluations (inputs and output).
// All readbacks are ordered on the NGX worker's own GPU command queue.
// Writer thread waits for the worker's GPU completion fence before mapping.
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>
#include <vector>

namespace cheeky::fg {
struct NgxEvalBurst {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Item {
        Ptr<ID3D12Resource> buffer;
        Ptr<ID3D12Fence> fence;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes{},ticket{},frame{},token{};
        unsigned format{},eye{},pass{},subrect_x{},subrect_y{},subrect_w{},subrect_h{},phase_count{1},phase_index{1},capture{};
        bool direct{};
        std::string stage;
    };
    std::mutex lock;
    std::condition_variable cv;
    std::deque<Item> ready;
    std::vector<Item> pending;
    std::thread writer;
    bool stop{},held{},armed{},active{};
    unsigned remaining{},capture_serial{};
    unsigned capture_count{},failures{};
    UINT64 budget_left{};
    static constexpr UINT64 budget=1024ull*1024*1024;
    void (*trace)(const char*) noexcept{};
    NgxEvalBurst(){writer=std::thread([this]{run();});}
    ~NgxEvalBurst(){
        {std::lock_guard guard(lock);stop=true;cv.notify_all();}
        if(writer.joinable())writer.join();
    }
    static void transition(ID3D12GraphicsCommandList* cmd,ID3D12Resource* tex,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        if(a==b)return;
        D3D12_RESOURCE_BARRIER bar{};bar.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource=tex;bar.Transition.StateBefore=a;bar.Transition.StateAfter=b;
        bar.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1,&bar);
    }
    bool start(UINT64 frame,UINT64 token,unsigned count=1,unsigned index=1){
        const bool down=(GetAsyncKeyState(VK_F5)&0x8000)!=0;
        if(down&&!held){armed=true;remaining=5;capture_serial++;if(trace)trace("FG NGX CAPTURE: F5 armed for five NGX FG evaluates");}
        held=down;
        active=armed && remaining>0;
        if(active){
            --remaining;if(!remaining)armed=false;budget_left=budget;capture_count=0;failures=0;
            frame_id=frame;capture_token=token;phase_count=count;phase_index=index;
            pending.clear();
        }
        return active;
    }
    UINT64 frame_id{},capture_token{};
    unsigned phase_count{1},phase_index{1};
    bool record(ID3D12Device* dev,ID3D12GraphicsCommandList* cmd,ID3D12Resource* tex,
        D3D12_RESOURCE_STATES state,const char* stage,unsigned eye,unsigned pass,
        unsigned ox=0,unsigned oy=0,unsigned rw=0,unsigned rh=0,bool direct=false){
        if(!active||!dev||!cmd||!tex)return false;
        auto desc=tex->GetDesc();
        const bool flag=desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&desc.Width<=64;
        if(!flag&&(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.DepthOrArraySize!=1||
           desc.MipLevels!=1||desc.SampleDesc.Count!=1)){++failures;return false;}
        Item i;UINT rows{};UINT64 row_bytes{};
        if(flag)i.bytes=desc.Width;
        else dev->GetCopyableFootprints(&desc,0,1,0,&i.footprint,&rows,&row_bytes,&i.bytes);
        if(!i.bytes||i.bytes>128ull*1024*1024||i.bytes>budget_left){++failures;return false;}
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buf{};buf.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buf.Width=i.bytes;buf.Height=1;buf.DepthOrArraySize=1;buf.MipLevels=1;
        buf.SampleDesc.Count=1;buf.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(dev->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buf,
           D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&i.buffer)))){++failures;return false;}
        transition(cmd,tex,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        if(flag)cmd->CopyBufferRegion(i.buffer.Get(),0,tex,0,i.bytes);
        else {
            D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=tex;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=i.buffer.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            target.PlacedFootprint=i.footprint;
            cmd->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        }
        transition(cmd,tex,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
        i.format=unsigned(desc.Format);i.eye=eye;i.pass=pass;i.stage=stage;
        i.frame=frame_id;i.token=capture_token;i.subrect_x=ox;i.subrect_y=oy;
        i.phase_count=phase_count;i.phase_index=phase_index;i.capture=capture_serial;
        i.subrect_w=rw;i.subrect_h=rh;i.direct=direct;
        budget_left-=i.bytes;pending.push_back(std::move(i));++capture_count;
        return true;
    }
    void commit(ID3D12Fence* fence,UINT64 ticket){
        if(!active)return;
        active=false;
        if(!fence||!ticket){pending.clear();return;}
        std::lock_guard guard(lock);
        for(auto& i:pending){i.fence=fence;i.ticket=ticket;ready.push_back(std::move(i));}
        pending.clear();cv.notify_one();
        if(trace){char message[224]{};sprintf_s(message,"FG NGX CAPTURE: queued %u buffers, %u skipped, frame=%llu token=%llu phase=%u/%u in %%TEMP%%\\CheekyFG-NGX",capture_count,failures,(unsigned long long)frame_id,(unsigned long long)capture_token,phase_index,phase_count);trace(message);}
    }
    void abandon(){pending.clear();active=false;}
    static std::wstring folder(){
        wchar_t tmp[MAX_PATH]{};DWORD n=GetTempPathW(MAX_PATH,tmp);
        std::wstring result=(n&&n<MAX_PATH)?std::wstring(tmp):L".";
        result+=L"CheekyFG-NGX";CreateDirectoryW(result.c_str(),nullptr);return result;
    }
    static void save(Item& i){
        wchar_t stem[320]{};
        swprintf_s(stem,L"\\pid%lu_c%u_f%llu_t%llu_g%uof%u_e%u_%s_%S",GetCurrentProcessId(),i.capture,
           (unsigned long long)i.frame,(unsigned long long)i.token,i.phase_index,i.phase_count,
           i.eye,i.pass==0?L"center":L"periphery",i.stage.c_str());
        auto path=folder()+stem;
        void* mapped{};
        D3D12_RANGE read{0,static_cast<SIZE_T>(i.bytes)};
        if(FAILED(i.buffer->Map(0,&read,&mapped))||!mapped)return;
        FILE* file{};
        if(_wfopen_s(&file,(path+L".raw").c_str(),L"wb")==0&&file){
            fwrite(mapped,1,static_cast<size_t>(i.bytes),file);fclose(file);
        }
        D3D12_RANGE written{0,0};i.buffer->Unmap(0,&written);
        if(_wfopen_s(&file,(path+L".txt").c_str(),L"w")==0&&file){
            fprintf(file,"phase_index=%u\nphase_count=%u\nfraction=%.9f\nframe=%llu\ntoken=%llu\neye=%u\npass=%u\nstage=%s\nwidth=%u\nheight=%u\nformat=%u\nrow_pitch=%u\nsize=%llu\nsubrect_x=%u\nsubrect_y=%u\nsubrect_w=%u\nsubrect_h=%u\ndirect=%u\n",
                i.phase_index,i.phase_count,double(i.phase_index)/double(i.phase_count+1),
                (unsigned long long)i.frame,(unsigned long long)i.token,i.eye,i.pass,i.stage.c_str(),
                i.footprint.Footprint.Width,i.footprint.Footprint.Height,i.format,
                i.footprint.Footprint.RowPitch,(unsigned long long)i.bytes,
                i.subrect_x,i.subrect_y,i.subrect_w,i.subrect_h,unsigned(i.direct));fclose(file);
        }
    }
    void run(){
        for(;;){Item i;
            {std::unique_lock guard(lock);cv.wait(guard,[&]{return stop||!ready.empty();});
             if(ready.empty()){if(stop)return;continue;}
             i=std::move(ready.front());ready.pop_front();}
            const auto until=GetTickCount64()+10000;
            while(i.fence&&i.fence->GetCompletedValue()<i.ticket&&GetTickCount64()<until)Sleep(2);
            if(i.fence&&i.fence->GetCompletedValue()>=i.ticket&&i.fence->GetCompletedValue()!=UINT64_MAX)save(i);
        }
    }
};
} // namespace cheeky::fg
