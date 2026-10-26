#include "../openxr_layer/fg_display_clock.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

static void require(bool result,const char* message){if(!result)throw std::runtime_error(message);}
int main(){
    using cheeky::fg::StableDisplayClock;
    try{
        StableDisplayClock clock;
        require(clock.display_ns()==0,"No invented refresh before a runtime prediction or explicit target");
        clock.observe_period(11111111);
        for(auto period:{22222222LL,33333333LL,44444444LL,133333333LL})clock.observe_period(period);
        require(clock.native_ns(3)==33333333,"A 90 Hz fallback retains a 30 FPS prediction through missed runtime frames");
        clock.observe_period(8333333);
        require(clock.native_ns(4)==33333332,"Automatic fallback can discover a faster 120 Hz target");
        clock.observe_refresh(90);
        clock.observe_period(8333333);
        require(clock.display_ns()==11111111,"Authoritative refresh takes precedence over the fallback minimum");
        clock.observe_refresh(72);
        require(clock.display_ns()==13888889,"A real queried refresh change may lower the target");
        clock.set_override(90);
        clock.observe_refresh(120);
        require(clock.native_ns(3)==33333333,"Explicit 90 Hz target survives both query changes and runtime stalls");
        clock.set_override(0);
        require(clock.display_ns()==8333333,"Clearing the override restores the current queried refresh");
        for(double invalid:{0.,-1.,22.5,10000.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
            clock.observe_refresh(invalid);
        require(clock.display_ns()==8333333,"Invalid or reprojection-sized query values cannot corrupt the nominal target");
        clock.set_override(9999);
        require(!clock.override_hz,"Invalid overrides are ignored");
        StableDisplayClock startup;startup.observe_period(44444444);startup.observe_period(11111111);
        startup.observe_period(44444444);
        require(startup.native_ns(3)==33333333,"Startup at a slow period can recover without following later slow reports");
        startup.observe_period(0);startup.observe_period(-1);startup.observe_period(1);
        require(startup.native_ns(3)==33333333,"Invalid period samples leave the stable prediction unchanged");
        std::cout<<"PASS stable FG display target: runtime stalls, fallback recovery, refresh changes, overrides and invalid samples\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
