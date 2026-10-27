#pragma once

namespace release_gate_fixture {
template<class Predicate> bool wait_until(Predicate&& predicate) {
    for(unsigned i=0;i<1000;++i){if(predicate())return true;Sleep(1);}
    return predicate();
}
}

void fg_pipelined_capture_validation() {
    using namespace cheeky::fg;using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))&&
        SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"Pipelined capture software device");
    for(unsigned point:{8u,5u})for(bool interrupt:{false,true}){
        BufferedPresenter presenter;presenter.started=true;
        check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&presenter.producer.fence))),"Pipelined capture fence");
        presenter.active_release_point=presenter.native11_release_point=point;
        presenter.pacing_active=true;presenter.limit_gpu_ahead=point==5u;
        presenter.predicted.predictedDisplayPeriod=10000000;
        presenter.waited=presenter.begun=1;
        // The next WaitFrame succeeds before EndFrame publishes the preceding capture.
        XrFrameState frame{XR_TYPE_FRAME_STATE};
        check(XR_SUCCEEDED(presenter.wait_frame(nullptr,&frame))&&presenter.waited==2,"Pipelined WaitFrame precedes capture publication");
        presenter.latest_native=BufferedPair{};presenter.latest_native->ready=1;presenter.ended=1;
        std::atomic<bool> entered{},returned{};XrResult result{};
        std::thread caller([&]{entered=true;result=presenter.begin_frame(nullptr);returned=true;});
        const bool started=release_gate_fixture::wait_until([&]{return entered.load();});
        Sleep(20);const bool held=!returned;
        bool capture_still_gated{};
        {std::lock_guard lock(presenter.mx);const auto now=TimingClock::now();
            presenter.release_gate_watchdog(now);presenter.release_gate_watchdog(now+std::chrono::seconds(1));
            capture_still_gated=presenter.capture_release_gate()&&presenter.gate_timeouts==0;}
        // Neither fence retirement nor a session interruption needs an FG callback.
        if(interrupt){std::lock_guard lock(presenter.mx);presenter.stopping=true;presenter.changed.notify_all();}
        else presenter.producer.fence->Signal(1);
        const bool released=release_gate_fixture::wait_until([&]{return returned.load();});
        if(!released){std::lock_guard lock(presenter.mx);presenter.stopping=true;presenter.changed.notify_all();}
        caller.join();
        check(started&&held&&released&&capture_still_gated,"Pipelined BeginFrame holds until capture retires or the session stops; the FG watchdog cannot bypass its fence");
        check(interrupt?(result==XR_ERROR_SESSION_NOT_RUNNING&&presenter.begun==1):
            (XR_SUCCEEDED(result)&&presenter.begun==2),"Pipelined capture gate preserves success and interruption results");
        presenter.started=false;
    }
    std::cout<<"PASS pipelined capture gate: BeginFrame waits for the fence, polls without FG callbacks, and cancels on stop\n";
}

void fg_release_watchdog_validation() {
    using namespace cheeky::fg;
    for(bool queued:{false,true})for(unsigned point:{2u,0u,1u,3u,4u,6u,7u})for(bool begin:{false,true}){
        BufferedPresenter presenter;presenter.started=true;presenter.predicted.predictedDisplayPeriod=10000000;
        auto job=std::make_shared<BufferedNative>();job->id=11;job->guides_valid=true;job->policy.release_point=point;
        job->guides=std::make_shared<FrameGuides12>();const auto guides=job->guides;
        if(queued)presenter.pending.push_back(job);
        else {presenter.inflight_id=job->id;presenter.inflight_release_point=point;}
        if(begin)presenter.waited=1;
        check(presenter.release_gate_active(),"Watchdog fixture starts with an actual FG release gate");
        std::atomic<bool> entered{},returned{};XrResult result{};XrFrameState frame{XR_TYPE_FRAME_STATE};
        std::thread caller([&]{entered=true;result=begin?presenter.begin_frame(nullptr):presenter.wait_frame(nullptr,&frame);returned=true;});
        const bool started=release_gate_fixture::wait_until([&]{return entered.load();});
        Sleep(20);const bool held=!returned;
        bool before_deadline{},open_after_timeout{},work_retained{};
        {
            std::lock_guard lock(presenter.mx);const auto start=TimingClock::now();
            presenter.release_gate_watchdog(start);
            presenter.release_gate_watchdog(start+std::chrono::milliseconds(499));
            before_deadline=presenter.gate_timeouts==0&&presenter.release_gate_active();
            presenter.release_gate_watchdog(start+std::chrono::milliseconds(500));
            open_after_timeout=presenter.gate_timeouts==1&&!presenter.release_gate_active()&&!presenter.unsubmitted_native();
            work_retained=job->guides==guides&&presenter.submitted_native_id==0&&
                (queued?(presenter.pending.size()==1&&presenter.pending.front()==job):presenter.inflight_id==job->id);
        }
        const bool released=release_gate_fixture::wait_until([&]{return returned.load();});
        if(!released){std::lock_guard lock(presenter.mx);presenter.stopping=true;presenter.changed.notify_all();}
        caller.join();
        check(started&&held&&before_deadline,"Watchdog preserves release gates before its deadline");
        check(open_after_timeout&&released&&XR_SUCCEEDED(result),"Watchdog timeout actually unblocks WaitFrame and BeginFrame for every FG release policy");
        check(work_retained,"Watchdog retains GPU work and guide leases without claiming an inference submission");
        // Timed-out pending work must remain exempt when the worker picks it up.
        presenter.pending.clear();presenter.inflight_id=job->id;presenter.inflight_release_point=point;
        presenter.inflight_prepared=presenter.inflight_generated_gate_released=false;
        check(!presenter.release_gate_active()&&!presenter.generated_submission_pending(),"Worker pickup cannot re-arm a timed-out gate");
        presenter.inference_prepared();presenter.inference_submitted();
        check(!presenter.release_gate_active()&&presenter.release_delay_until==TimingClock::time_point{},"Late inference callbacks cannot re-arm a timed-out gate");
        presenter.inflight_id=job->id+1;presenter.inflight_release_point=point;
        presenter.inflight_prepared=presenter.inflight_generated_gate_released=false;
        check(presenter.release_gate_active(),"A timeout exemption does not disable the next frame's release policy");
        presenter.started=false;
    }
    {
        BufferedPresenter presenter;
        presenter.inflight_id=11;presenter.inflight_release_point=2;
        presenter.awaiting_generated_submission=10;
        for(unsigned id:{12u,13u}){auto job=std::make_shared<BufferedNative>();job->id=id;job->guides_valid=true;
            job->policy.release_point=3;presenter.pending.push_back(job);}
        const auto now=TimingClock::now();presenter.release_gate_watchdog(now);
        presenter.release_gate_watchdog(now+std::chrono::milliseconds(500));
        check(!presenter.release_gate_active()&&!presenter.generated_submission_pending()&&presenter.pending.size()==2,
            "One timeout exempts all work already holding the gate while retaining its queue");
        auto previous=std::make_shared<BufferedNative>(),job=presenter.pending.back();previous->id=12;
        BufferedPair pair;pair.current_id=job->id;pair.generated_count=3;
        for(auto& image:pair.generated)image=std::make_shared<TransportImage>();
        presenter.active_release_point=3;
        presenter.announce_generated_phase(previous,job,pair,std::make_shared<FrameCompletion11>());
        check(!presenter.awaiting_generated_submission&&!presenter.release_gate_active(),"A late generated phase cannot re-arm a timed-out presentation gate");
        auto next=std::make_shared<BufferedNative>();next->id=14;next->guides_valid=true;next->policy.release_point=3;
        presenter.pending.push_back(next);
        check(presenter.release_gate_active()&&presenter.generated_submission_pending(),"New queued work retains its presentation gate after watchdog recovery");
        presenter.arm_generated_gate(next->id);
        BufferedPair late;late.current_id=job->id;
        presenter.announce_generated_phase(previous,job,late,std::make_shared<FrameCompletion11>());
        check(presenter.awaiting_generated_submission==next->id,"Late old output cannot replace a newer frame's generated-submission gate");
        presenter.stop();
        presenter.stopping=false;presenter.inflight_id=1;presenter.inflight_release_point=2;
        check(presenter.release_gate_active(),"Session retirement clears timeout exemptions before a restart");
    }
    std::cout<<"PASS FG release watchdog: all policies unblock both application calls, preserve GPU ownership, and gate new work\n";
}
