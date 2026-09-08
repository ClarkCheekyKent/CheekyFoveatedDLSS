#include "runtime_api.hpp"
#include <uevr/API.h>
#include <atomic>
#include <array>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace {
HMODULE module{};
const UEVR_PluginInitializeParam* api{};
CheekyUEVRStartFn runtime_start{};
CheekyUEVRDetachFn runtime_detach{};
CheekyUEVRTickFn runtime_tick{};
CheekyUEVRCommandFn runtime_command{};
CheekyUEVRSnapshotFn runtime_snapshot{};
std::atomic<bool> initialized{};
std::uint64_t attachment{};
std::mutex commands_mutex;
std::deque<std::string> commands;
constexpr char command_event[] = "cheeky.foveated_dlss.command.v1";
constexpr char snapshot_event[] = "cheeky.foveated_dlss.snapshot.v1";

void on_custom_event(const char* event, const char* data) {
    if (!initialized || !event || !data || strcmp(event, command_event) != 0 || strnlen_s(data, 8193) > 8192) return;
    try {
        std::lock_guard lock(commands_mutex);
        if (commands.size() < 32) commands.emplace_back(data);
    } catch (...) {}
}
void publish_snapshot() {
    std::array<char, cheeky_uevr_message_capacity> text{};
    if (runtime_snapshot && runtime_snapshot(text.data(), static_cast<std::uint32_t>(text.size()))) {
        api->functions->dispatch_lua_event(snapshot_event, text.data());
    }
}
void on_present() {
    if (!initialized || !api) return;
    try {
        const auto* r = api->renderer;
        if (runtime_tick && r && attachment) runtime_tick(attachment, r->renderer_type, r->device, r->command_queue);
        std::deque<std::string> pending;
        { std::lock_guard lock(commands_mutex); pending.swap(commands); }
        for (const auto& command : pending) {
            if (runtime_command && attachment) runtime_command(attachment, command.c_str());
            // Acknowledge every command, even when several arrive in one frame.
            publish_snapshot();
        }
        static ULONGLONG next_snapshot{};
        const auto now = GetTickCount64();
        if (pending.empty() && now < next_snapshot) return;
        next_snapshot = now + 250;
        // Never dispatch while holding a plugin mutex or from a game NGX hook.
        if (pending.empty()) publish_snapshot();
    } catch (...) { if (api->functions->log_error) api->functions->log_error("Cheeky UEVR callback failed"); }
}
void on_device_reset() {
    if (runtime_tick && attachment) runtime_tick(attachment, 0, nullptr, nullptr);
}
template<class T> bool load_export(HMODULE dll, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(dll, name)); return out != nullptr;
}
}
extern "C" __declspec(dllexport) void uevr_plugin_required_version(UEVR_PluginVersion* version) {
    if (version) *version = {UEVR_PLUGIN_VERSION_MAJOR, UEVR_PLUGIN_VERSION_MINOR, UEVR_PLUGIN_VERSION_PATCH};
}
extern "C" __declspec(dllexport) bool uevr_plugin_initialize(const UEVR_PluginInitializeParam* input) {
    try {
        if (!input || !input->version || input->version->major != UEVR_PLUGIN_VERSION_MAJOR ||
            input->version->minor < UEVR_PLUGIN_VERSION_MINOR || !input->functions || !input->callbacks || !input->renderer) return false;
        if (!input->functions->get_persistent_dir || !input->functions->dispatch_lua_event || !input->functions->remove_callback ||
            !input->callbacks->on_present || !input->callbacks->on_device_reset || !input->callbacks->on_custom_event) return false;
        if (initialized) return true;
        api = input;
        std::vector<wchar_t> path(32768);
        const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return false;
        const auto runtime_path = std::filesystem::path(path.data()).parent_path() / L"CheekyFoveatedDLSS" / L"CheekyFoveatedDLSSRuntime.dll";
        const auto dll = LoadLibraryExW(runtime_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll) {
            if (api->functions->log_error) api->functions->log_error("Cheeky: cannot load runtime DLL (error %lu). Extract the complete UEVR package including the plugin subfolder.", GetLastError());
            return false;
        }
        if (!load_export(dll, "CheekyUEVR_Start", runtime_start) || !load_export(dll, "CheekyUEVR_Detach", runtime_detach) ||
            !load_export(dll, "CheekyUEVR_Tick", runtime_tick) || !load_export(dll, "CheekyUEVR_Command", runtime_command) ||
            !load_export(dll, "CheekyUEVR_Snapshot", runtime_snapshot)) {
            runtime_detach = nullptr; FreeLibrary(dll); return false;
        }
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(runtime_start), &pinned)) {
            runtime_detach = nullptr; FreeLibrary(dll); return false;
        }
        const auto size = api->functions->get_persistent_dir(nullptr, 0);
        if (!size || size > 32767) { FreeLibrary(dll); return false; }
        std::vector<wchar_t> directory(size + 1, L'\0');
        api->functions->get_persistent_dir(directory.data(), static_cast<unsigned>(directory.size()));
        const auto* r = api->renderer;
        CheekyUEVRStart start;
        start.config_directory = directory.data(); start.renderer = r->renderer_type; start.device = r->device; start.queue = r->command_queue;
        start.attachment = &attachment;
        const bool started = runtime_start(&start);
        // The runtime pins itself before installing any hooks. Release the
        // adapter's load reference; only the thin adapter will actually unload.
        FreeLibrary(dll);
        if (!started && api->functions->log_error) api->functions->log_error("Cheeky runtime could not start; see the Cheeky panel/log for details.");
        // Keep diagnostics visible on runtime initialization failures.
        const bool a = api->callbacks->on_present(on_present);
        const bool b = api->callbacks->on_device_reset(on_device_reset);
        const bool c = api->callbacks->on_custom_event(on_custom_event);
        if (!a || !b || !c) {
            if (a) api->functions->remove_callback(reinterpret_cast<void*>(&on_present));
            if (b) api->functions->remove_callback(reinterpret_cast<void*>(&on_device_reset));
            if (c) api->functions->remove_callback(reinterpret_cast<void*>(&on_custom_event));
            runtime_detach(attachment); return false;
        }
        initialized = true;
        if (api->functions->log_info) api->functions->log_info("Cheeky UEVR plugin initialized. Controls: LuaLoader > Cheeky Foveated DLSS. Game testing pending.");
        return true;
    } catch (...) { if (runtime_detach) runtime_detach(attachment); return false; }
}
BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { module = self; DisableThreadLibraryCalls(self); }
    if (reason == DLL_PROCESS_DETACH && runtime_detach) runtime_detach(attachment);
    return TRUE;
}
