#include "dlss_nr_input.hpp"
#include "eye_calibration_d3d12.hpp"
#include "gaze_foveation.hpp"
#include "runtime.hpp"

// This executable links the real native observer and NR lifetime/input code.
// Unrelated gaze/timing/calibration consumers are inert; NVIDIA evaluation is
// substituted only for the copy test. Lifetime assertions use NrLifetime itself.
namespace cheeky::foveated_dlss {
void log_info(const char*) noexcept {}
void log_warning(const char*) noexcept {}
void log_error(const char*) noexcept {}
void trace_event(const char*, ...) noexcept {}
void record_gaze_copy(std::uint64_t, GazeCopyEdge) noexcept {}
void submit_gaze_copies(std::uint64_t) noexcept {}
void reset_gaze_copies(std::uint64_t) noexcept {}
void forget_gaze_resource(std::uint64_t) noexcept {}
void note_d3d12_command_list_submission(ID3D12CommandQueue*, ID3D12GraphicsCommandList*) noexcept {}
void note_d3d12_command_list_reset(ID3D12GraphicsCommandList*) noexcept {}
void note_d3d12_present(ID3D12CommandQueue*) noexcept { collect_dlss_nr_input_submissions(); }
bool calibration12_internal_work() noexcept { return false; }
std::recursive_mutex& calibration12_execution_mutex() noexcept {
    static auto* mutex = new std::recursive_mutex;
    return *mutex;
}
void calibration12_submitted(ID3D12CommandQueue*, ID3D12GraphicsCommandList*) noexcept {}
void calibration12_retired(ID3D12GraphicsCommandList*) noexcept {}
int nr_test_evaluations{};
bool nr_test_succeeds{true};
DlssNrFrame nr_test_frame{};
bool evaluate_dlss_nr(const DlssNrFrame& frame, const Settings&) noexcept {
    ++nr_test_evaluations;
    nr_test_frame = frame;
    return nr_test_succeeds && frame.color;
}
void note_dlss_nr_skipped(DlssNrRoute, const Settings&, const char*) noexcept {}
}
int run_nr_lifetime_tests();
int run_d3d12_composite_tests();
int main() {
    cheeky::foveated_dlss::Settings settings;
    settings.enabled = false;
    settings.nr_enabled = false;
    settings.auto_stereo_alignment = false;
    cheeky::foveated_dlss::update_settings(settings);
    const auto lifetime = run_nr_lifetime_tests();
    return lifetime ? lifetime : run_d3d12_composite_tests();
}
