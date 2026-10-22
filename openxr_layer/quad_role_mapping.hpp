#pragma once
#include "../shared/cheeky_quad_views.h"
#include "../src/eye_calibration_pixels.hpp"
#include <d3d11_4.h>
#include <wrl/client.h>
#include <array>
#include <mutex>
#include <vector>
#include <algorithm>

namespace cheeky::quad_capture {
// Four roles are proved from the raw, pre-composition submissions, never from
// NGX handle order or identical image dimensions. DCS's tone-map pass prevents
// using texture-pointer equality for this association.
class RoleMapping11 {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    using Pixel=cheeky::foveated_dlss::CalibrationPixel;
    static constexpr unsigned count=8, block=40, sample=60;
    static constexpr uint32_t codes[count]{0x1305e6aU,0x15c952cU,0xb5447cU,0x9aa60fU,0x19333a4U,0x426d5dU,0x14cda46U,0xcd6968U};
    struct Source {
        uint64_t id{},generation{},last_ms{};unsigned width{},height{};
        DXGI_FORMAT format{};Ptr<ID3D11Texture2D> marker;
        std::array<Ptr<ID3D11Texture2D>,2> number_cells;
        unsigned cell_width[2]{},cell_height[2]{};
    };
    struct Proof {uint64_t id{},generation{};unsigned width{},height{};};
    struct Frame {
        bool busy{},finalized{},submitted{};uint64_t revision{},ticket{};int64_t time{};
        Ptr<ID3D11Query> done;std::array<Ptr<ID3D11Texture2D>,4> patches;
        std::array<DXGI_FORMAT,4> formats{};
        std::array<CheekyQuadViewV1,4> targets{};
        std::array<Proof,count> sources{};
    };
    std::mutex mutex;
    Ptr<ID3D11Device> device;Ptr<ID3D11DeviceContext> context;Ptr<ID3D11Multithread> protection;
    std::array<Source,count> sources;
    std::array<Frame,3> frames;
    std::array<unsigned,count> manual_roles{};
    std::array<unsigned,count> saved_roles{};unsigned saved_count{};bool restore_pending{true};
    CheekyQuadViewsV1 value{};
    std::array<unsigned,4> candidate{};
    uint64_t revision{1},ticket{},last_confirmation{};unsigned attempts{},confirmations{};
    bool verified{},manual{},numbers{};
    struct Guard {ID3D11Multithread* p;explicit Guard(ID3D11Multithread* v):p(v){p->Enter();}~Guard(){p->Leave();}};
    void invalidate(bool clear_manual) {
        ++revision;verified=false;confirmations=attempts=0;last_confirmation=0;
        if(clear_manual){manual_roles={};restore_pending=true;}
        for(auto& v:value.views){v.dlss_view=v.registration_generation=0;v.verified=0;}
    }
    // Numbers follow slots, so keep live sources in slots 1..n. A game that
    // creates its next views before releasing the old ones (DCS mission
    // change) would otherwise number them 5-8 and miss the saved layout.
    // Markers and number cells encode the slot and are rebuilt after a move.
    bool compact() {
        bool moved{};unsigned next{};
        for(unsigned i=0;i<count;++i){if(!sources[i].id)continue;
            if(i!=next){sources[next]=std::move(sources[i]);sources[i]={};
                auto& source=sources[next];source.marker.Reset();source.number_cells={};
                std::fill(std::begin(source.cell_width),std::end(source.cell_width),0u);
                std::fill(std::begin(source.cell_height),std::end(source.cell_height),0u);moved=true;}
            ++next;}
        return moved;
    }
    // Views the game stopped evaluating without releasing them.
    bool evict_idle(uint64_t now) {
        bool evicted{};
        for(auto& source:sources)if(source.id&&now-source.last_ms>3000){source={};evicted=true;}
        return evicted;
    }
    void restore_saved() {
        if(!manual||!restore_pending||!saved_count)return;
        const auto present=unsigned(std::count_if(sources.begin(),sources.end(),[](const auto& s){return s.id!=0;}));
        if(present!=saved_count)return;
        for(unsigned i=0;i<count;++i)if(saved_roles[i]&&!sources[i].id)return;
        manual_roles=saved_roles;restore_pending=false;++revision;
    }
    bool prepare() {
        if(context)return true;
        if(!device)return false;
        device->GetImmediateContext(&context);
        if(FAILED(context.As(&protection))){context.Reset();return false;}
        protection->SetMultithreadProtected(TRUE);return true;
    }
    void poll() {
        using namespace cheeky::foveated_dlss;
        for(auto& frame:frames) {
            if(!frame.busy||!frame.finalized)continue;
            const auto ready=context->GetData(frame.done.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(ready==S_FALSE)continue;
            if(FAILED(ready)||!frame.submitted||frame.revision!=revision){frame.busy=false;continue;}
            std::array<unsigned,4> matches{};bool valid=true,waiting=false;
            for(unsigned view=0;view<4&&valid;++view){D3D11_MAPPED_SUBRESOURCE data{};
                const auto mapped=context->Map(frame.patches[view].Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&data);
                if(mapped==DXGI_ERROR_WAS_STILL_DRAWING){waiting=true;break;}
                if(FAILED(mapped)){valid=false;break;}
                float best{},second{};unsigned selected=count;
                for(unsigned source=0;source<count;++source){const auto& proof=frame.sources[source];
                    if(!proof.id||proof.width!=frame.targets[view].width||proof.height!=frame.targets[view].height)continue;
                    const auto score=calibration_pattern_score(data.pData,data.RowPitch,sample,sample,frame.formats[view],0,true,codes[source],1);
                    if(score>best){second=best;best=score;selected=source;}else second=(std::max)(second,score);
                }
                context->Unmap(frame.patches[view].Get(),0);
                if(selected==count||best<calibration_pattern_min_score||best-second<calibration_pattern_min_gap){valid=false;break;}
                matches[view]=selected;
                for(unsigned previous=0;previous<view;++previous)if(matches[previous]==selected)valid=false;
            }
            if(waiting)continue;
            frame.busy=false;
            if(!valid){++value.rejected;confirmations=0;continue;}
            if(matches!=candidate){candidate=matches;verified=false;confirmations=0;last_confirmation=0;
                for(auto& view:value.views){view.dlss_view=view.registration_generation=0;view.verified=0;}}
            if(uint64_t(frame.time)!=last_confirmation){++confirmations;last_confirmation=uint64_t(frame.time);}
            if(confirmations<2)continue;
            verified=true;
            for(unsigned i=0;i<4;++i){auto& target=value.views[i];const auto& source=frame.sources[matches[i]];
                target=frame.targets[i];target.dlss_view=source.id;target.registration_generation=source.generation;target.verified=1;}
        }
    }
    void draw_number(Source& source,unsigned slot,const CheekyQuadSourceD3D11V1& input,ID3D11Texture2D* texture,const D3D11_TEXTURE2D_DESC& desc) {
        using namespace cheeky::foveated_dlss;
        static constexpr const char* glyphs[]{"00100" "01100" "00100" "00100" "00100" "00100" "01110",
            "01110" "10001" "00001" "00010" "00100" "01000" "11111",
            "11110" "00001" "00001" "01110" "00001" "00001" "11110",
            "00010" "00110" "01010" "10010" "11111" "00010" "00010",
            "11111" "10000" "10000" "11110" "00001" "00001" "11110",
            "01110" "10000" "10000" "11110" "10001" "10001" "01110",
            "11111" "00001" "00010" "00100" "01000" "01000" "01000",
            "01110" "10001" "10001" "01110" "10001" "10001" "01110"};
        const auto bytes=calibration_pixel_bytes(desc.Format);if(!bytes)return;
        // A small repeated grid leaves many peripheral labels outside the inset.
        // Cache the whole glyph so each label takes one GPU copy.
        const unsigned cell=(std::max)(2u,(std::min)(input.width,input.height)/240u);
        const unsigned w=5*cell+2,h=7*cell+2;
        if(w>input.width||h>input.height)return;
        if(!source.number_cells[0]||source.cell_width[0]!=w||source.cell_height[0]!=h){
            std::vector<unsigned char> pixels(w*h*bytes);
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
                const bool light=x>0&&y>0&&x<w-1&&y<h-1&&glyphs[slot][((y-1)/cell)*5+(x-1)/cell]=='1';
                calibration_encode_pattern(pixels.data()+(y*w+x)*bytes,desc.Format,0,0,0,light?0x1ffffffU:2U);
            }
            D3D11_TEXTURE2D_DESC glyph{};glyph.Width=w;glyph.Height=h;glyph.MipLevels=glyph.ArraySize=1;
            glyph.Format=desc.Format;glyph.SampleDesc.Count=1;
            D3D11_SUBRESOURCE_DATA data{pixels.data(),w*bytes,0};source.number_cells[0].Reset();
            if(FAILED(device->CreateTexture2D(&glyph,&data,&source.number_cells[0])))return;
            source.cell_width[0]=w;source.cell_height[0]=h;
        }
        for(unsigned row=0;row<5;++row)for(unsigned column=0;column<7;++column){
            const unsigned left=unsigned(uint64_t(input.width-w)*(2*column+1)/14);
            const unsigned top=unsigned(uint64_t(input.height-h)*(2*row+1)/10);
            // Keep the automatic identification patch unobstructed on tiny views.
            if(left<52&&left+w>12&&top<52&&top+h>12)continue;
            context->CopySubresourceRegion(texture,input.array_slice*desc.MipLevels,input.x+left,input.y+top,0,source.number_cells[0].Get(),0,nullptr);
        }
    }
public:
    struct Input {ID3D11Texture2D* texture{};unsigned slice{},x{},y{},width{},height{};uint64_t identity{};};
    explicit RoleMapping11(ID3D11Device* d):device(d) {prepare();}
    void reset(){std::lock_guard lock(mutex);invalidate(true);}
    void configure(bool use_manual,bool show){std::lock_guard lock(mutex);
        if(manual!=use_manual){manual=use_manual;invalidate(false);}numbers=show;}
    bool saved_layout(const CheekyQuadSavedLayoutV1& layout){std::lock_guard lock(mutex);
        if(layout.source_count>count)return false;std::array<unsigned,count> roles{};unsigned used{};
        for(unsigned i=0;i<count;++i){const auto role=layout.roles[i];if(role>4||(role&&(used&(1u<<role))))return false;
            if(role)used|=1u<<role;roles[i]=role;}
        saved_roles=roles;saved_count=layout.source_count;restore_saved();return true;}
    void forget(uint64_t id,uint64_t generation){std::lock_guard lock(mutex);for(auto& source:sources)
        if(source.id==id&&source.generation==generation){source={};invalidate(true);}
        compact();}
    bool assign(unsigned number,unsigned role,uint64_t id,uint64_t generation) {
        std::lock_guard lock(mutex);if(!number||number>count||role>4)return false;
        const auto slot=number-1;if(sources[slot].id!=id||sources[slot].generation!=generation)return false;
        if(role)for(unsigned i=0;i<count;++i)if(i!=slot&&manual_roles[i]==role)return false;
        manual_roles[slot]=role;restore_pending=false;++revision;return true;
    }
    void observe(const CheekyQuadSourceD3D11V1& input) {
        if(!context||!input.dlss_view||!input.registration_generation)return;
        Guard guard(protection.Get());std::lock_guard lock(mutex);
        auto* caller=static_cast<ID3D11DeviceContext*>(input.context);if(caller!=context.Get())return;
        auto* texture=static_cast<ID3D11Texture2D*>(input.texture);D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        if(desc.SampleDesc.Count!=1||input.array_slice>=desc.ArraySize||!input.width||!input.height||
            uint64_t(input.x)+input.width>desc.Width||uint64_t(input.y)+input.height>desc.Height)return;
        poll();unsigned slot=count;
        for(unsigned i=0;i<count;++i)if(sources[i].id==input.dlss_view){slot=i;break;}
        if(slot==count){const bool evicted=evict_idle(GetTickCount64());if(compact()||evicted)invalidate(true);}
        if(slot==count)for(unsigned i=0;i<count;++i)if(!sources[i].id){slot=i;break;}
        if(slot==count){slot=unsigned(std::min_element(sources.begin(),sources.end(),[](const auto& a,const auto& b){return a.last_ms<b.last_ms;})-sources.begin());}
        auto& source=sources[slot];
        if(source.id!=input.dlss_view||source.generation!=input.registration_generation||source.width!=input.width||source.height!=input.height||source.format!=desc.Format){
            source={};source.id=input.dlss_view;source.generation=input.registration_generation;source.width=input.width;source.height=input.height;source.format=desc.Format;invalidate(true);}
        source.last_ms=GetTickCount64();
        restore_saved();
        const auto bytes=cheeky::foveated_dlss::calibration_pixel_bytes(desc.Format);
        if(!manual&&!verified&&attempts<120&&bytes&&input.width>=64&&input.height>=64){
            if(!source.marker){std::vector<unsigned char> data(block*block*bytes);
                for(unsigned y=0;y<block;++y)for(unsigned x=0;x<block;++x)
                    cheeky::foveated_dlss::calibration_encode_pattern(data.data()+(y*block+x)*bytes,desc.Format,0,x,y,codes[slot]);
                D3D11_TEXTURE2D_DESC marker{};marker.Width=marker.Height=block;marker.MipLevels=marker.ArraySize=1;marker.Format=desc.Format;marker.SampleDesc.Count=1;marker.Usage=D3D11_USAGE_DEFAULT;
                D3D11_SUBRESOURCE_DATA initial{data.data(),block*bytes,0};device->CreateTexture2D(&marker,&initial,&source.marker);}
            if(source.marker)context->CopySubresourceRegion(texture,input.array_slice*desc.MipLevels,input.x+12,input.y+12,0,source.marker.Get(),0,nullptr);
        }
        if(numbers)draw_number(source,slot,input,texture,desc);
    }
    uint64_t capture(const std::array<Input,4>& inputs,int64_t display_time) {
        if(!context)return 0;Guard guard(protection.Get());std::lock_guard lock(mutex);poll();
        for(unsigned i=0;i<4;++i){auto& view=value.views[i];view.view_index=i;view.eye_index=i%2;view.focus=i>=2;
            view.submitted_resource=inputs[i].identity;view.array_slice=inputs[i].slice;view.width=inputs[i].width;view.height=inputs[i].height;}
        if(manual||verified||attempts>=120)return 0;
        auto found=std::find_if(frames.begin(),frames.end(),[](const auto& frame){return !frame.busy;});if(found==frames.end())return 0;
        auto& frame=*found;
        for(unsigned i=0;i<4;++i){const auto& input=inputs[i];if(!input.texture||input.width<64||input.height<64)return 0;
            D3D11_TEXTURE2D_DESC desc{};input.texture->GetDesc(&desc);
            if(!cheeky::foveated_dlss::calibration_pixel_bytes(desc.Format)||desc.SampleDesc.Count!=1||input.slice>=desc.ArraySize||
                uint64_t(input.x)+input.width>desc.Width||uint64_t(input.y)+input.height>desc.Height)return 0;
            if(!frame.patches[i]||frame.formats[i]!=desc.Format){frame.patches[i].Reset();
                desc.Width=desc.Height=sample;desc.MipLevels=desc.ArraySize=1;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if(FAILED(device->CreateTexture2D(&desc,nullptr,&frame.patches[i])))return 0;frame.formats[i]=desc.Format;}
            const D3D11_BOX box{input.x+2,input.y+2,0,input.x+2+sample,input.y+2+sample,1};
            D3D11_TEXTURE2D_DESC original{};input.texture->GetDesc(&original);
            context->CopySubresourceRegion(frame.patches[i].Get(),0,0,0,0,input.texture,input.slice*original.MipLevels,&box);
            frame.targets[i]=value.views[i];
        }
        if(!frame.done){D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};if(FAILED(device->CreateQuery(&query,&frame.done)))return 0;}
        for(unsigned i=0;i<count;++i)frame.sources[i]={sources[i].id,sources[i].generation,sources[i].width,sources[i].height};
        frame.busy=true;frame.finalized=frame.submitted=false;frame.revision=revision;frame.ticket=++ticket;frame.time=display_time;
        ++value.captures;++attempts;context->End(frame.done.Get());context->Flush();return frame.ticket;
    }
    void complete(uint64_t token,bool submitted){if(!token)return;std::lock_guard lock(mutex);for(auto& frame:frames)
        if(frame.busy&&frame.ticket==token){frame.finalized=true;frame.submitted=submitted;break;}}
    CheekyQuadViewsV1 snapshot(){std::lock_guard lock(mutex);auto out=value;out.version=CHEEKY_QUAD_VIEWS_VERSION;out.size=sizeof(out);
        out.revision=revision;out.confirmations=confirmations;out.manual_mode=manual;out.show_numbers=numbers;
        out.status=verified?2:attempts>=120?3:1;
        if(manual){unsigned assigned{};for(auto& view:out.views){view.dlss_view=view.registration_generation=0;view.verified=0;}
            for(unsigned i=0;i<count;++i)if(manual_roles[i]&&sources[i].id){auto& view=out.views[manual_roles[i]-1];view.dlss_view=sources[i].id;view.registration_generation=sources[i].generation;++assigned;}
            out.status=assigned==4?2:1;}
        for(unsigned i=0;i<4;++i){out.views[i].view_index=i;out.views[i].eye_index=i%2;out.views[i].focus=i>=2;}
        for(unsigned i=0;i<count;++i)out.sources[i]={sources[i].id,sources[i].generation,i+1,manual_roles[i],sources[i].width,sources[i].height};
        return out;
    }
};
}
