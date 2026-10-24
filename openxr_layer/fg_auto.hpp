#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cheeky::fg {
// Work bracket starts AFTER the application pacing gate and ends after native
// capture. GPU query collection is asynchronous; sequence identifies fresh work.
struct ApplicationTiming {
    std::uint64_t sequence{};
    double cpu_ms{},gpu_ms{};
    double recorded_ms{};
};
struct AutoMultiplier {
    bool enabled{},valid{};
    unsigned selected{2},maximum{4},reason{}; // 0 manual, 1 measuring, 2 stable, 3 load, 4 headroom, 5 capability, 6 FG bound.
    std::uint64_t sequence{},samples{},switches{};
    double render_ms{},fg_ms{},cpu_ms{},gpu_ms{},overload_since{-1},headroom_since{-1},last_sample_ms{-1};
    unsigned overload_samples{};
    void mode(bool automatic,unsigned fixed,unsigned limit) {
        limit=std::clamp(limit,2u,4u);
        if(enabled!=automatic){enabled=automatic;valid=false;sequence=0;render_ms=fg_ms=0;
            overload_since=headroom_since=last_sample_ms=-1;overload_samples=0;selected=std::clamp(fixed,2u,limit);}
        maximum=limit;
        if(!enabled){selected=std::clamp(fixed,2u,4u);reason=0;}
        else if(selected>maximum){selected=maximum;++switches;reason=5;overload_since=headroom_since=-1;overload_samples=0;}
        else if(!valid)reason=1;
    }
    bool observe(double now,const ApplicationTiming& sample,double generated_ms,double display_ms) {
        if(!enabled||!sample.sequence||sample.sequence==sequence||!std::isfinite(now)||
           (sample.recorded_ms>0&&(now<sample.recorded_ms||now-sample.recorded_ms>500))||
           !std::isfinite(sample.cpu_ms)||!std::isfinite(sample.gpu_ms)||sample.cpu_ms<0||sample.gpu_ms<=0||
           sample.cpu_ms>1000||sample.gpu_ms>1000||!std::isfinite(generated_ms)||generated_ms<=0||
           !std::isfinite(display_ms)||display_ms<=0)return false;
        sequence=sample.sequence;++samples;cpu_ms=sample.cpu_ms;gpu_ms=sample.gpu_ms;
        // Missing/old work must not count toward either sustained condition.
        if(last_sample_ms<0||now-last_sample_ms>500||now<last_sample_ms){valid=false;
            overload_since=headroom_since=-1;overload_samples=0;}
        last_sample_ms=now;
        const auto work=std::max(cpu_ms,gpu_ms);
        render_ms=valid?render_ms*.65+work*.35:work;
        fg_ms=valid?fg_ms*.75+generated_ms*.25:generated_ms;valid=true;
        const auto utilization=[&](unsigned n){return (render_ms+(n-1)*fg_ms)/(display_ms*n);};
        const auto current=utilization(selected);
        if(current>1.0&&selected<maximum){
            headroom_since=-1;
            // Extra FG must buy headroom; GPU-bound interpolation can make a
            // higher multiplier worse even when the native work is late.
            if(utilization(selected+1)>=current*.99){overload_since=-1;overload_samples=0;reason=6;return false;}
            if(overload_since<0)overload_since=now;
            if(++overload_samples>=3&&now-overload_since>=150){++selected;++switches;reason=3;
                overload_since=headroom_since=-1;overload_samples=0;return true;}
        }else {overload_since=-1;overload_samples=0;
            if(selected>2&&utilization(selected-1)<=.82){
                if(headroom_since<0)headroom_since=now;
                if(now-headroom_since>=1500){--selected;++switches;reason=4;headroom_since=-1;return true;}
            }else headroom_since=-1;
        }
        reason=2;return false;
    }
};
}
