#pragma once
#include "cheeky_quad_views.h"
#include <algorithm>
#include <array>
#include <mutex>

namespace cheeky::foveated_dlss {
// Some games composite their four reconstructions before OpenXR. This tracks
// evaluated game features, never private NGX features or merely created handles.
// Geometry identifies a four-view set. Default roles follow first evaluation
// order (peripheral L, center L, peripheral R, center R, or the right eye first
// when the game's stereo views are declared right-first); manual roles override it.
class AppQuadViews {
    struct Source { uint64_t id{},generation{}; unsigned width{},height{}; };
    std::mutex mutex;
    std::array<Source,8> frame{},candidate{},sources{};
    std::array<unsigned,8> roles{},saved{};
    uint64_t xr_session{},frame_key{},epoch{},revision{},last_seen{};
    unsigned confirmations{},misses{},saved_count{};
    bool overflow{},active{},manual{},numbers{},restore_pending{true},right_first{};
    static bool same(const Source& a,const Source& b) {
        return a.id==b.id&&a.generation==b.generation&&a.width==b.width&&a.height==b.height;
    }
    void clear() { active=false; sources={};candidate={};roles={};confirmations=misses=0;restore_pending=true;++epoch;++revision; }
    void restore() {
        if(!active||!manual||!restore_pending||saved_count!=4)return;
        roles=saved;restore_pending=false;++revision;
    }
    void finish_frame() {
        std::array<Source,8> selected{};unsigned count{};
        if(!overflow)for(const auto& a:frame)if(a.id){
            const auto matches=std::count_if(frame.begin(),frame.end(),[&](const Source& b){return b.id&&a.width==b.width&&a.height==b.height;});
            // Accept two equal-size pairs or four equal-size views. A unique
            // desktop/mirror reconstruction is deliberately excluded.
            if(matches==2||matches==4)selected[count++]=a;
        }
        // Keep a stable source number even if evaluation threads arrive in a
        // different order later. The initial completed frame supplies the order.
        const auto& order=active?sources:candidate;
        std::stable_sort(selected.begin(),selected.begin()+count,[&](const auto& a,const auto& b){
            const auto rank=[&](const auto& s){return std::find_if(order.begin(),order.end(),[&](const auto& old){return old.id==s.id&&old.generation==s.generation;})-order.begin();};
            return rank(a)<rank(b);
        });
        if(count!=4){confirmations=0;candidate={};if(active&&++misses>=2)clear();return;}
        misses=0;
        if(active&&!std::equal(selected.begin(),selected.end(),sources.begin(),same))clear();
        if(!std::equal(selected.begin(),selected.end(),candidate.begin(),same)){candidate=selected;confirmations=0;}
        if(confirmations<3)++confirmations;
        if(!active&&confirmations>=3){sources=selected;active=true;roles={};restore_pending=true;++epoch;++revision;restore();}
    }
    void advance(uint64_t session,uint64_t key,uint64_t now) {
        if(xr_session!=session){clear();frame={};frame_key=0;xr_session=session;}
        if(frame_key&&key!=frame_key){finish_frame();frame={};overflow=false;}
        frame_key=key;
        if(last_seen&&now-last_seen>2000){clear();frame={};last_seen=0;}
    }
  public:
    bool detected() {std::lock_guard lock(mutex);return active;}
    void observe(uint64_t session,uint64_t key,uint64_t now,uint64_t id,uint64_t generation,unsigned w,unsigned h) {
        if(!session||!key||!id||!generation||!w||!h)return;
        std::lock_guard lock(mutex);advance(session,key,now);last_seen=now;
        auto found=std::find_if(frame.begin(),frame.end(),[&](const auto& s){return s.id==id||!s.id;});
        if(found==frame.end()){overflow=true;return;}
        *found={id,generation,w,h};
    }
    CheekyQuadViewsV1 snapshot(uint64_t session,uint64_t key,uint64_t now) {
        std::lock_guard lock(mutex);advance(session,key,now);
        CheekyQuadViewsV1 out{};out.version=1;out.size=sizeof(out);out.active=active;out.graphics_api=12;
        out.session_generation=(uint64_t(1)<<63)|epoch;out.revision=revision;out.confirmations=confirmations;
        out.manual_mode=manual;out.show_numbers=numbers;out.status=active?2:0;
        unsigned assigned{};
        for(unsigned i=0;i<8;++i){const auto& s=sources[i];out.sources[i]={s.id,s.generation,i+1,roles[i],s.width,s.height};
            static constexpr unsigned defaults[]{1,3,2,4},right_defaults[]{2,4,1,3};
            const auto role=manual?roles[i]:(i<4?(right_first?right_defaults:defaults)[i]:0);
            if(s.id&&role){auto& v=out.views[role-1];v.dlss_view=s.id;v.registration_generation=s.generation;
                v.width=s.width;v.height=s.height;/* Default order is a convention, not pixel verification. */++assigned;}}
        for(unsigned i=0;i<4;++i){out.views[i].view_index=i;out.views[i].eye_index=i%2;out.views[i].focus=i>=2;}
        if(active&&manual)out.status=assigned==4?2:1;
        return out;
    }
    // The game evaluates its right eye first (the stereo "first DLSS view is
    // right eye" declaration). A change re-roles both eyes, so FG history resets.
    void default_right_first(bool value) {std::lock_guard lock(mutex);if(right_first!=value){right_first=value;++revision;}}
    void configure(bool m,bool n,const unsigned* layout,unsigned count) {
        std::lock_guard lock(mutex);if(manual!=m){manual=m;++revision;}numbers=n;
        if(layout&&count<=8){unsigned used{};bool valid=true;
            for(unsigned i=0;i<8;++i){const auto r=layout[i];if(r>4||(r&&(i>=count||(used&(1u<<r)))))valid=false;used|=1u<<(std::min)(r,4u);}
            if(valid){std::copy_n(layout,8,saved.begin());saved_count=count;}}
        restore();
    }
    bool assign(uint64_t session,unsigned number,unsigned role,uint64_t id,uint64_t generation) {
        std::lock_guard lock(mutex);if(!active||!manual||session!=((uint64_t(1)<<63)|epoch)||!number||number>8||role>4)return false;
        const auto i=number-1;if(sources[i].id!=id||sources[i].generation!=generation)return false;
        for(unsigned j=0;j<8;++j)if(j!=i&&role&&roles[j]==role)return false;
        roles[i]=role;restore_pending=false;++revision;return true;
    }
    void forget(uint64_t id,uint64_t generation) {
        std::lock_guard lock(mutex);
        for(auto& s:frame)if(s.id==id&&s.generation==generation)s={};
        for(const auto& s:sources)if(s.id==id&&s.generation==generation){clear();break;}
    }
    void reset() {std::lock_guard lock(mutex);roles={};saved={};saved_count=0;restore_pending=false;++revision;}
};
inline AppQuadViews& app_quad_views(){static AppQuadViews value;return value;}
}
