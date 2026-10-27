// Focused standalone regression. Link eye_calibration_d3d12.cpp and the D3D
// libraries; the exported calibration bridge is retained but never invoked.
#include "../src/eye_calibration.cpp"
#include <thread>
#include <iostream>

namespace cheeky::foveated_dlss {
// Unused settings/runtime entrypoints fail immediately if the statistics path
// unexpectedly reaches them. View lookup is exercised separately below.
std::uint64_t stereo_view_generation(std::uint64_t) noexcept {std::abort();}
bool publish_stereo_calibration(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,
    std::uint64_t,std::uint64_t,bool*,std::uint64_t,bool,bool,
    const std::array<StereoSourceCrop,2>*,EyeCalibrationMethod) noexcept {std::abort();}
void invalidate_stereo_crop() noexcept {std::abort();}
bool eye_calibration_continuous_validation(EyeCalibrationMethod) noexcept {std::abort();}
EyeCalibrationMethod eye_calibration_selected_method() noexcept {std::abort();}
void set_eye_calibration_learning(unsigned,std::uint64_t,unsigned) noexcept {std::abort();}
void clear_stereo_calibration() noexcept {std::abort();}
Settings configured_settings() noexcept {std::abort();}
void trace_event(const char*,...) noexcept {std::abort();}
bool ensure_native_observer(ID3D12GraphicsCommandList*) noexcept {std::abort();}
bool stats_lookup_unlocked{true};
unsigned stats_lookup_count{};
StereoEyeAssignment stereo_eye_assignment(std::uint64_t) noexcept {
    // Another thread stands in for EndFrame, which already owns the layer's
    // state lock and needs calibration. No blocking wait is needed to detect
    // whether the menu snapshot still holds the opposite lock here.
    bool acquired{};
    std::thread render([&] {
        acquired=state().mutex.try_lock();
        if(acquired)state().mutex.unlock();
    });
    render.join();
    stats_lookup_unlocked&=acquired;++stats_lookup_count;
    StereoEyeAssignment assignment;assignment.calibrated=true;assignment.vertical_flip=true;
    assignment.source_crops[0].valid=assignment.source_crops[1].valid=true;
    return assignment;
}
}

int main() {
    using namespace cheeky::foveated_dlss;
    auto& calibration=state();
    enabled=true;calibration.stats.left_view=10;calibration.stats.right_view=20;
    calibration.stats.frames=3;calibration.cpu_us=30;
    const auto snapshot=eye_calibration_stats();
    if(!stats_lookup_unlocked||stats_lookup_count<2||!snapshot.correction_active||
       !snapshot.vertical_flip||!snapshot.crop_mapping_active||snapshot.cpu_us_per_frame!=10) {
        std::cerr<<"FAIL calibration statistics retain a lock across view lookup or lose snapshot values\n";
        return 1;
    }
    std::cout<<"PASS calibration statistics release their lock before view lookup and preserve snapshot values\n";
}
