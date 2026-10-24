#pragma once
inline void fg_auto_validation() {
    using namespace cheeky::fg;
    AutoMultiplier controller;controller.mode(true,2,4);
    std::uint64_t sequence{};double now{};
    const auto run=[&](double duration,double cpu,double gpu,double fg){
        const auto end=now+duration;
        while(now<end){controller.observe(now,{++sequence,cpu,gpu},fg,1000./120);now+=25;}
    };
    run(500,6,8,1);check(controller.selected==2,"Auto keeps low multiplier when rendering fits");
    const auto changed_at=now;
    run(125,20,20,1);check(controller.selected==2,"Auto does not switch immediately on a load increase");
    run(200,20,20,1);check(controller.selected==3&&now-changed_at<=325,"Auto responds to sustained CPU/GPU overload");
    run(400,29,30,1);check(controller.selected==4,"Auto can increase to 4x under further load");
    run(1000,4,6,1);check(controller.selected==4,"Recovery waits for sustained headroom");
    run(3000,4,6,1);check(controller.selected==2,"Auto returns stepwise to 2x after recovery");
    const auto switches=controller.switches;
    run(1000,14,15.5,1);check(controller.selected==2&&controller.switches==switches,"Near-threshold load does not flap");
    controller.mode(true,2,2);run(1000,40,40,1);check(controller.selected==2,"Auto respects hardware's 2x limit");
    controller={};controller.mode(true,2,4);run(1000,2,2,15);
    check(controller.selected==2&&controller.reason==6,"Expensive interpolation never causes an unhelpful multiplier increase");
    controller={};controller.mode(true,2,4);
    controller.observe(now,{1,25,25},1,8.333);
    for(unsigned i=0;i<20;++i)controller.observe(now+=25,{1,25,25},1,8.333);
    check(controller.selected==2,"Repeated GPU samples cannot count as sustained new work");
    controller.observe(now+=1000,{2,25,25},1,8.333);
    check(controller.selected==2,"Missing timing intervals break sustained-overload detection");
    controller.mode(false,4,2);check(controller.selected==4&&!controller.enabled,"Manual selection stays independent of Auto's capability clamp");
    std::cout<<"PASS Auto FG controller: load/recovery response, hysteresis, CPU/GPU load, FG cost, stale samples and capability limits\n";
}
