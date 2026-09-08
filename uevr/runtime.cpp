#include "runtime_api.hpp"
#include "settings_io.hpp"
#include "graphics_observer.hpp"
#include "processing_owner.hpp"
#include "runtime.hpp"
#include "diagnostics.hpp"
#include "gaze_foveation.hpp"
#include "dlss_nr.hpp"
#include "version.h"
#include <atomic>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <charconv>

namespace cheeky::foveated_dlss {
namespace {
// Intentionally process-resident: workers and game detours never reference the
// unloadable UEVR adapter, nor call into Lua or UEVR.
struct State {
    std::mutex mutex, log_mutex;
    std::filesystem::path directory, config;
    HANDLE log{INVALID_HANDLE_VALUE}, save_event{}, owner{};
    bool started{}, graphics_ready{}, failed{};
    std::uint32_t renderer{};
    void* observed_device{};
    void* observed_queue{};
    std::uint64_t revision{}, saved_revision{}, request{}, applied_request{};
    std::uint64_t attachment_sequence{};
    std::string message{"Not initialized"};
    std::atomic<bool> save_requested{}, report_requested{};
};
State& state() { static auto* instance = new State; return *instance; }
// DllMain detach uses only these atomics, never state() construction or a lock.
std::atomic<bool> adapter_attached{};
std::atomic<std::uint64_t> active_attachment{};

std::string snapshot_locked(State& s) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::boolalpha << std::setprecision(6);
    const auto observer = native_observer_status();
    const auto gaze = gaze_diagnostics();
    const auto views = stereo_view_statistics();
    const auto nr = dlss_nr_snapshot();
    out << "{\"protocol\":1,\"version\":\"" CHEEKY_VERSION "-uevr-preview\",\"request\":" << s.request
        << ",\"revision\":" << s.revision << ",\"saved_revision\":" << s.saved_revision
        << ",\"applied_request\":" << s.applied_request
        << ",\"attached\":" << adapter_attached.load() << ",\"ready\":" << (s.started && s.graphics_ready)
        << ",\"processing\":" << current_settings().enabled
        << ",\"renderer\":" << s.renderer << ",\"message\":\"" << json_escape(s.message)
        << "\",\"settings\":" << settings_json(configured_settings())
        << ",\"observer\":{\"ready\":" << observer.ready << ",\"submissions\":" << observer.submissions
        << ",\"copies\":" << observer.copies << ",\"resets\":" << observer.resets << ",\"destroyed\":" << observer.destroyed << '}'
        << ",\"gaze\":{\"layer\":" << gaze.layer_present << ",\"abi\":" << gaze.abi_compatible
        << ",\"using_gaze\":" << gaze.using_gaze << ",\"alignment\":" << gaze.alignment_source
        << ",\"ambiguous\":" << gaze.mapping_ambiguous << ",\"views\":" << views.active
        << ",\"submitted_copies\":" << gaze.submitted_copies
        << ",\"left_mapped\":" << gaze.views[0].resource_mapped << ",\"right_mapped\":" << gaze.views[1].resource_mapped << '}'
        << ",\"nr\":\"" << json_escape(dlss_nr_state_name(nr.state)) << "\",\"apis\":[";
    for (unsigned index = 0; index < 2; ++index) {
        const auto d = diagnostic_snapshot(index ? DiagnosticApi::d3d12 : DiagnosticApi::d3d11);
        if (index) out << ',';
        out << "{\"state\":\"" << json_escape(diagnostic_state_name(d.state)) << "\",\"hook\":" << d.hook_discovered
            << ",\"creates\":" << d.create_calls << ",\"evaluations\":" << d.evaluate_calls << ",\"active\":" << d.active_calls
            << ",\"input_width\":" << d.received_input_width << ",\"input_height\":" << d.received_input_height
            << ",\"output_width\":" << d.received_output_width << ",\"output_height\":" << d.received_output_height
            << ",\"foveated_ms\":" << d.foveated_dlss_gpu_ms << ",\"native_ms\":" << d.native_dlss_gpu_ms
            << ",\"peripheral_ms\":" << d.peripheral_dlaa_gpu_ms << ",\"result\":" << d.last_result << '}';
    }
    out << "]}"; return out.str();
}
DWORD WINAPI persistence_worker(void*) {
    auto& s = state();
    for (;;) {
        WaitForSingleObject(s.save_event, INFINITE);
        try {
            if (s.save_requested.exchange(false)) {
                Settings settings; std::uint64_t revision;
                { std::lock_guard lock(s.mutex); settings = configured_settings(); revision = s.revision; }
                std::string error;
                const auto success = write_settings_file(s.config, settings, error);
                std::lock_guard lock(s.mutex);
                if (success) s.saved_revision = revision;
                else { s.message = error; log_error(error.c_str()); }
            }
            if (s.report_requested.exchange(false)) {
                std::string text;
                { std::lock_guard lock(s.mutex); text = snapshot_locked(s); }
                std::ofstream report(s.directory / L"CheekyFoveatedDLSS-diagnostics.json", std::ios::binary);
                report << text; report.flush();
                std::lock_guard lock(s.mutex);
                s.message = report ? "Diagnostics saved beside CheekyFoveatedDLSS.ini" : "Could not write diagnostics";
            }
        } catch (...) { log_error("UEVR persistence/report worker failed"); }
    }
}
void request_save(State& s) { s.save_requested = true; SetEvent(s.save_event); }
bool configure_graphics(State& s, std::uint32_t renderer, void* device, void* queue) {
    s.renderer = renderer;
    if (!device) {
        s.graphics_ready = false;
        s.message = "Graphics device reset; waiting for the next renderer";
        reset_gaze_foveation();
        return false;
    }
    if (s.observed_device == device && s.observed_queue == queue && s.graphics_ready) return true;
    if (renderer == 1 && !initialize_native_observer(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue))) {
        s.graphics_ready = false;
        s.message = "D3D12 submission observer unavailable; processing paused";
        return false;
    }
    if (renderer > 1) { s.graphics_ready = false; return false; }
    s.observed_device = device; s.observed_queue = queue; s.graphics_ready = true;
    s.message = "Ready. Enable DLSS in the game; if evaluation stays unavailable after late injection, toggle DLSS off/on.";
    return true;
}
}
void set_addon_modules(HMODULE, HMODULE) noexcept {}
void trace_event(const char* format, ...) noexcept {
    if (!format) return;
    auto& s = state(); std::lock_guard lock(s.log_mutex);
    if (s.log == INVALID_HANDLE_VALUE) return;
    char message[3072]{};
    va_list args; va_start(args, format); vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args); va_end(args);
    SYSTEMTIME now; GetLocalTime(&now); char line[3328]{};
    const auto n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [T%lu] %s\r\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentThreadId(), message);
    if (n > 0) { DWORD written; WriteFile(s.log, line, static_cast<DWORD>((std::min)(static_cast<std::size_t>(n), sizeof(line)-1)), &written, nullptr); }
}
void close_trace_log() noexcept {} // The runtime and log stay resident until process exit.
void log_message(int level, const char* message) noexcept { trace_event("[%d] %s", level, message ? message : ""); }
void log_info(const char* message) noexcept { log_message(3, message); }
void log_warning(const char* message) noexcept { log_message(2, message); }
void log_error(const char* message) noexcept { log_message(1, message); }
}

using namespace cheeky::foveated_dlss;
extern "C" __declspec(dllexport) bool CheekyUEVR_Start(const CheekyUEVRStart* input) {
    try {
        if (!input || input->size != sizeof(*input) || input->abi != cheeky_uevr_abi || !input->config_directory || !input->attachment) return false;
        *input->attachment = 0;
        auto& s = state(); std::lock_guard lock(s.mutex);
        if (adapter_attached) { log_warning("Duplicate UEVR plugin attachment rejected"); return false; }
        if (s.failed) return false;
        if (!s.started) {
            set_processing_allowed(false);
            s.directory = input->config_directory;
            std::filesystem::create_directories(s.directory);
            s.config = s.directory / L"CheekyFoveatedDLSS.ini";
            s.log = CreateFileW((s.directory / L"CheekyFoveatedDLSS-UEVR.log").c_str(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            HMODULE resident{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&CheekyUEVR_Start), &resident)) return false;
            log_info("Cheeky UEVR preview " CHEEKY_VERSION " initializing; runtime remains resident until game exit");
            if (GetModuleHandleW(L"CheekyFoveatedDLSS.addon64") || !(s.owner = claim_processing_owner())) {
                s.message = "Another Cheeky integration is loaded. Remove its add-on and restart the game.";
                s.failed = true; log_error(s.message.c_str()); return false;
            }
            Settings settings;
            if (std::filesystem::exists(s.config)) {
                std::string error;
                if (!read_settings_file(s.config, settings, error)) {
                    s.message = error + "; fix or rename CheekyFoveatedDLSS.ini and restart";
                    s.failed = true; log_error(s.message.c_str()); return false;
                }
            }
            // DX11 transport requires observing the private transport queue,
            // which is not exposed by UEVR. Keep this preview on DX11 direct.
            if (input->renderer == 0) settings.d3d11_use_d3d12_transport = false;
            update_settings(settings); s.revision = 1;
            s.save_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!s.save_event) { s.failed = true; s.message = "Could not create persistence event"; return false; }
            HANDLE worker = CreateThread(nullptr, 0, persistence_worker, nullptr, 0, nullptr);
            if (!worker) { s.failed = true; s.message = "Could not create persistence worker"; return false; }
            CloseHandle(worker);
            if (!start_interception()) { s.failed = true; s.message = "NGX interception initialization failed"; return false; }
            s.started = true;
        }
        configure_graphics(s, input->renderer, input->device, input->queue);
        active_attachment = ++s.attachment_sequence;
        *input->attachment = s.attachment_sequence;
        adapter_attached = true;
        set_processing_allowed(s.graphics_ready);
        request_save(s);
        return true;
    } catch (...) { set_processing_allowed(false); return false; }
}
extern "C" __declspec(dllexport) void CheekyUEVR_Detach(std::uint64_t attachment) {
    if (!attachment || active_attachment.load(std::memory_order_acquire) != attachment) return;
    adapter_attached.store(false, std::memory_order_release);
    set_processing_allowed(false);
}
extern "C" __declspec(dllexport) void CheekyUEVR_Tick(std::uint64_t attachment, std::uint32_t renderer, void* device, void* queue) {
    try {
        if (!adapter_attached.load() || attachment != active_attachment.load()) return;
        auto& s = state(); std::lock_guard lock(s.mutex);
        configure_graphics(s, renderer, device, queue);
        set_processing_allowed(s.started && s.graphics_ready);
        if (s.graphics_ready && renderer == 1) note_d3d12_present(nullptr);
        static bool was_down{};
        const bool down = (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_OEM_2) & 0x8000);
        if (down && !was_down) {
            auto settings = configured_settings(); settings.enabled = !settings.enabled; update_settings(settings); ++s.revision; request_save(s);
        }
        was_down = down;
    } catch (...) { set_processing_allowed(false); }
}
extern "C" __declspec(dllexport) bool CheekyUEVR_Command(std::uint64_t attachment, const char* command) {
    try {
        if (!command || strnlen_s(command, 8193) > 8192 || !adapter_attached.load() || attachment != active_attachment.load()) return false;
        auto& s = state(); std::lock_guard lock(s.mutex);
        std::istringstream in(command); std::string protocol, request, action;
        if (!std::getline(in, protocol) || protocol != "1" || !std::getline(in, request) || !std::getline(in, action)) return false;
        std::uint64_t id{};
        const auto parsed = std::from_chars(request.data(), request.data()+request.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != request.data()+request.size()) return false;
        s.request = id;
        if (action == "get") return true;
        if (action == "report") { s.report_requested = true; SetEvent(s.save_event); s.message = "Preparing diagnostics"; return true; }
        if (action == "save") { request_save(s); return true; }
        if (action == "reset_nr") { reset_dlss_nr(); return true; }
        auto settings = configured_settings();
        if (action == "defaults") {
            settings = Settings{};
            if (s.renderer == 0) settings.d3d11_use_d3d12_transport = false;
        }
        else if (action == "set") {
            std::string line; unsigned count{};
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                const auto eq = line.find('=');
                if (eq == std::string::npos || !set_named_setting(settings, std::string_view(line).substr(0,eq), std::string_view(line).substr(eq+1))) {
                    s.message = "Rejected invalid settings transaction"; return false;
                }
                if (++count > 64) { s.message = "Too many settings"; return false; }
            }
            if (!count) return false;
        } else { s.message = "Unknown command"; return false; }
        if (s.renderer == 0 && settings.d3d11_use_d3d12_transport) {
            s.message = "DX11-to-DX12 transport is unavailable in this UEVR preview; use DX11 direct or a DX12 game"; return false;
        }
        update_settings(settings); ++s.revision; s.applied_request = id;
        s.message = "Settings applied"; request_save(s); return true;
    } catch (...) { return false; }
}
extern "C" __declspec(dllexport) bool CheekyUEVR_Snapshot(char* output, std::uint32_t capacity) {
    try {
        if (!output || !capacity) return false;
        output[0] = 0; auto& s = state(); std::lock_guard lock(s.mutex);
        const auto text = snapshot_locked(s);
        if (text.size() >= capacity) return false;
        memcpy(output, text.c_str(), text.size()+1); return true;
    } catch (...) { return false; }
}
