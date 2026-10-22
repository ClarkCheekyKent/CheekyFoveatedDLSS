#include "../shared/app_quad_views.hpp"
#include "../src/peripheral_contract.hpp"
#include <cstdio>
#include <cstdlib>
using cheeky::foveated_dlss::AppQuadViews;
static void check(bool ok,const char* reason){if(!ok){std::fprintf(stderr,"%s\n",reason);std::exit(1);}}
static void frame(AppQuadViews& map,uint64_t key,bool quad=true,bool mirror=false,bool reverse=false) {
    for(unsigned n=0;n<(quad?4u:2u);++n){const unsigned i=reverse?3-n:n;
        map.observe(1,key,key*10,100+i,10+i,1047,981);}
    if(mirror)map.observe(1,key,key*10,200,20,2560,1440);
}
int main(){
    AppQuadViews map;
    for(unsigned i=1;i<=3;++i)frame(map,i,true,true);
    auto s=map.snapshot(1,4,40);
    check(s.active&&s.status==2&&!s.manual_mode,"Four evaluated views plus a unique desktop view must activate");
    const unsigned order[]{0,2,1,3};
    for(unsigned i=0;i<4;++i)check(s.views[i].dlss_view==100+order[i]&&s.views[i].focus==unsigned(i>=2)&&s.views[i].eye_index==i%2,"Default quad order");
    for(const auto scale:{.2F,.75F,1.F,1.5F,2.F,3.F}){
        const auto dimensions=cheeky::foveated_dlss::peripheral_dlaa_dimensions(800,600,scale);
        const auto clamped=(std::min)(scale,1.F);
        check(dimensions.width==unsigned(800*clamped+.5F)&&dimensions.height==unsigned(600*clamped+.5F),"Peripheral processing scale must stop at 100 percent");
    }
    check(!s.sources[4].dlss_view,"Desktop view must not enter assignments");
    frame(map,4,true,true,true);s=map.snapshot(1,5,50);
    check(s.sources[0].dlss_view==100,"Evaluation thread order must not renumber an established set");
    map.configure(true,true,nullptr,0);s=map.snapshot(1,5,50);
    check(s.status!=2,"Incomplete manual roles must not become an automatic assignment");
    check(map.assign(s.session_generation,1,3,100,10),"Manual assignment");
    check(!map.assign(s.session_generation,2,3,101,11),"Reject duplicate manual role");
    check(!map.assign(s.session_generation+1,2,1,101,11),"Reject stale session assignment");
    check(!map.assign(s.session_generation,2,1,101,99),"Reject stale handle generation");
    unsigned saved[]{3,4,1,2,0,0,0,0};map.configure(true,false,saved,4);
    map.forget(100,10);check(!map.detected(),"Released view invalidates the set and FG block");
    for(unsigned i=5;i<=7;++i)frame(map,i);
    s=map.snapshot(1,8,80);
    check(s.active&&s.status==2&&s.views[2].dlss_view==100&&s.views[0].dlss_view==102,"Restore saved manual layout on rediscovery");
    frame(map,8,false);frame(map,9,false);s=map.snapshot(1,10,100);
    check(!s.active&&!map.detected(),"Stereo frames must end quad mode");
    AppQuadViews pair;
    for(unsigned key=1;key<=3;++key)for(unsigned i=0;i<2;++i)pair.observe(1,key,key*10,300+i,30+i,1024,1024);
    check(!pair.snapshot(1,4,40).active,"Stereo must never activate from recurring handles");
    AppQuadViews different;
    for(unsigned key=1;key<=3;++key)for(unsigned i=0;i<4;++i)different.observe(1,key,key*10,400+i,40+i,i<2?800:1200,1000);
    check(different.snapshot(1,4,40).active,"Two differently sized stereo pairs must activate");
    check(!different.snapshot(2,1,100).active,"New XR session clears old sources");
    check(!map.snapshot(1,10,3000).active,"Idle views must not remain active");
    std::puts("App quad views: detection, default order, manual overrides, saved roles, stereo/session transitions passed");
}
