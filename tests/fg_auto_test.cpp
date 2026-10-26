#include "../openxr_layer/fg_auto.hpp"
#include <iostream>
#include <stdexcept>

static void require(bool result,const char* message){if(!result)throw std::runtime_error(message);}
// Feeds one sample every native interval until `ms` of simulated time passes.
static void run(cheeky::fg::AutoMultiplier& automatic,double& now,std::uint64_t& sequence,double render_ms,double fg_ms,
    double display_ms,double presenter_ms,double ms){
    for(const auto end=now+ms;now<end;now+=25){
        cheeky::fg::ApplicationTiming sample;sample.sequence=++sequence;sample.cpu_ms=render_ms;sample.gpu_ms=render_ms-.3;sample.recorded_ms=now;
        automatic.observe(now,sample,fg_ms,display_ms,presenter_ms);
    }
}
int main(){
    using cheeky::fg::AutoMultiplier;
    try{
        // Support bundle: Oculus reports 120 Hz but accepts ~90 submissions/s
        // (xrWaitFrame+xrEndFrame ~11 ms). 3x already saturates the runtime.
        {AutoMultiplier automatic;double now=1000;std::uint64_t sequence{};
            automatic.mode(true,3,4);run(automatic,now,sequence,22.5,1.6,1000./120,11.06,5000);
            require(automatic.selected==3,"A runtime-limited 120 Hz session keeps 3x instead of lowering native FPS for 4x");
            automatic.mode(true,3,4);automatic.selected=4;run(automatic,now,sequence,22.5,1.6,1000./120,11.06,5000);
            require(automatic.selected==3,"4x steps back to 3x when the runtime cannot present the extra frames");}
        // The same load on a runtime that keeps up with 120 Hz still needs 4x.
        {AutoMultiplier automatic;double now=1000;std::uint64_t sequence{};
            automatic.mode(true,3,4);run(automatic,now,sequence,22.5,1.6,1000./120,8.2,5000);
            require(automatic.selected==4,"A runtime that presents every 120 Hz slot still raises 3x to 4x");}
        // Legacy callers without a presenter measurement keep the old model.
        {AutoMultiplier automatic;double now=1000;std::uint64_t sequence{};
            automatic.mode(true,3,4);run(automatic,now,sequence,22.5,1.6,1000./120,0,5000);
            require(automatic.selected==4,"No presenter measurement falls back to the reported refresh");}
        std::cout<<"fg_auto_test passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
