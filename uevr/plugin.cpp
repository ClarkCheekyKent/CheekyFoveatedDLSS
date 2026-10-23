#include "runtime_api.hpp"
#include "../shared/cheeky_fg_bridge.hpp"
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
CheekyUEVRAttachOpenVRFn runtime_attach_openvr{};
CheekyUEVRPublishStereoFn runtime_publish_stereo{};
CheekyUEVRPublishRenderingModeFn runtime_publish_mode{};
CheekyUEVRCommandFn runtime_command{};
CheekyUEVRSnapshotFn runtime_snapshot{};
CheekyFgDetachFrameSourceFn runtime_detach_frame_source{};
CheekyFgAttachFrameSourceFn runtime_attach_frame_source{};
using NeedsFrameSourceFn=bool(__cdecl*)();
NeedsFrameSourceFn runtime_needs_frame_source{};
bool frame_source_policy_known{},frame_source_required{};

std::atomic<bool> initialized{};
std::atomic<bool> should_load{};
bool callbacks_registered{}, load_attempted{};
std::atomic<bool> loader_status_requested{true};
std::mutex lifecycle_mutex;
constexpr char load_event[] = "cheeky.foveated_dlss.load.v1";
constexpr char load_state_event[] = "cheeky.foveated_dlss.load_state.v1";
std::uint64_t attachment{};
std::atomic<void*> attached_compositor{};
std::atomic<ULONGLONG> next_openvr_attach{};
std::mutex commands_mutex;
std::deque<std::string> commands;
constexpr char command_event[] = "cheeky.foveated_dlss.command.v1";
constexpr char snapshot_event[] = "cheeky.foveated_dlss.snapshot.v1";

// Forward declarations
bool initialize_dll();
void detach_runtime();
// The plugin's public API is stable across UEVR builds. Only the legacy
// frame-source branch touches the private UEVR vtable/RHI implementation.
void sync_frame_source() {
    if(!api)return;
    // Older runtimes lack the policy export and retain their legacy hook path.
    const bool required=!runtime_needs_frame_source||runtime_needs_frame_source();
    if(frame_source_policy_known&&frame_source_required==required)return;
    if(frame_source_policy_known&&frame_source_required&&runtime_detach_frame_source)
        runtime_detach_frame_source();
    frame_source_policy_known=true;
    frame_source_required=required;
    if(required&&runtime_attach_frame_source&&api->functions->register_inline_hook&&api->functions->unregister_inline_hook)
        runtime_attach_frame_source(api->uevr_module,api->functions->register_inline_hook,api->functions->unregister_inline_hook);
}

void on_custom_event(const char* event, const char* data) {
    if (event && data && strcmp(event, load_event) == 0) {
        if (strcmp(data, "true") != 0 && strcmp(data, "false") != 0) return;
        should_load.store(strcmp(data, "true") == 0, std::memory_order_release);
        loader_status_requested = true;
        return; // Never load or detach from a Lua/custom-event callback.
    }
    if (!initialized || !event || !data || strcmp(event, command_event) != 0 || strnlen_s(data, 8193) > 8192) return;
    try {
        std::lock_guard lock(commands_mutex);
        if (commands.size() < 32) commands.emplace_back(data);
    } catch (...) {}
}

std::string read_snapshot() {
    std::array<char, cheeky_uevr_message_capacity> text{};
    if (runtime_snapshot && runtime_snapshot(text.data(), static_cast<std::uint32_t>(text.size())))
        return text.data();
    return {};
}

void on_present() {
    if (!api) return;
    try {
        std::vector<std::string> snapshots;
        const char* loader_state{};
        std::unique_lock lifecycle(lifecycle_mutex);
        const bool requested = should_load.load(std::memory_order_acquire);
        bool changed{};
        if (!requested && (initialized || load_attempted)) {
            detach_runtime(); load_attempted = false; changed = true;
        } else if (requested && !initialized && !load_attempted) {
            load_attempted = true; initialize_dll(); changed = true;
        }
        if (loader_status_requested.exchange(false) || changed)
            loader_state = !requested ? "disabled" : initialized ? "enabled" : "failed";
        if (!initialized) {
            if (requested && loader_state) snapshots.push_back(read_snapshot());
            lifecycle.unlock();
            if (loader_state) api->functions->dispatch_lua_event(load_state_event, loader_state);
            for (const auto& snapshot : snapshots)
                if (!snapshot.empty()) api->functions->dispatch_lua_event(snapshot_event, snapshot.c_str());
            return;
        }
        if (api->vr && api->vr->is_openvr && api->vr->is_openvr() && api->openvr && api->openvr->get_vr_compositor) {
            auto* compositor = reinterpret_cast<void*>(api->openvr->get_vr_compositor());
            const auto now = GetTickCount64();
            if (compositor && compositor != attached_compositor && now >= next_openvr_attach && runtime_attach_openvr && attachment) {
                next_openvr_attach = now + 250;
                if (runtime_attach_openvr(attachment, compositor)) attached_compositor = compositor;
            }
            if (!compositor) attached_compositor = nullptr;
        } else {
            attached_compositor = nullptr;
            next_openvr_attach = 0;
        }
        const auto* r = api->renderer;
        if (runtime_tick && r && attachment) runtime_tick(attachment, r->renderer_type, r->device, r->command_queue);
        if (runtime_publish_mode && attachment) {
            char value[32]{};
            if (api->vr && api->vr->get_mod_value) api->vr->get_mod_value("VR_RenderingMethod", value, sizeof(value));
            const auto mode = value[0] >= '0' && value[0] <= '3' && value[1] == '\0'
                ? static_cast<unsigned>(value[0] - '0') : UINT32_MAX;
            runtime_publish_mode(attachment, mode);
        }
        if (runtime_publish_stereo && attachment) {
            CheekyUEVRStereoProjection projection{};
            if (r && r->renderer_type == UEVR_RENDERER_D3D12 && r->device && r->command_queue &&
                    api->vr && api->vr->is_hmd_active && api->vr->is_hmd_active() &&
                    api->vr->get_ue_projection_matrix && api->vr->get_hmd_width && api->vr->get_hmd_height) {
                projection.output_width = api->vr->get_hmd_width();
                projection.output_height = api->vr->get_hmd_height();
                projection.active = 1;
                UEVR_Matrix4x4f first[2]{}, second[2]{};
                for (int eye = 0; eye < 2; ++eye) {
                    api->vr->get_ue_projection_matrix(eye, &first[eye]);
                }
                for (int eye = 0; eye < 2; ++eye) api->vr->get_ue_projection_matrix(eye, &second[eye]);
                
                if (memcmp(first, second, sizeof(first)) != 0 || projection.output_width != api->vr->get_hmd_width() ||
                        projection.output_height != api->vr->get_hmd_height()) projection.active = 0;
                memcpy(projection.matrices, first, sizeof(first));
            }
            runtime_publish_stereo(attachment, &projection);
        }
        std::deque<std::string> pending;
        { std::lock_guard lock(commands_mutex); pending.swap(commands); }
        for (const auto& command : pending) {
            if (runtime_command && attachment) runtime_command(attachment, command.c_str());
            sync_frame_source(); // Live toggle: detach/attach before the next native frame.
            snapshots.push_back(read_snapshot());
        }
        // The status JSON is only shown in the UEVR menu. While it is closed,
        // refresh slowly enough that Lua still sees a live host (it times out
        // after 600 frames), and refresh at once when the menu opens.
        static ULONGLONG next_snapshot{};
        static bool menu_was_open{};
        const auto now = GetTickCount64();
        const bool menu_open = !api->functions->is_drawing_ui || api->functions->is_drawing_ui();
        const bool menu_opened = menu_open && !menu_was_open;
        menu_was_open = menu_open;
        if (pending.empty() && (now >= next_snapshot || loader_state || menu_opened)) {
            next_snapshot = now + (menu_open ? 250 : 2000);
            snapshots.push_back(read_snapshot());
        }
        // Lua may synchronously send another command; release every plugin lock.
        lifecycle.unlock();
        if (loader_state) api->functions->dispatch_lua_event(load_state_event, loader_state);
        for (const auto& snapshot : snapshots)
            if (!snapshot.empty()) api->functions->dispatch_lua_event(snapshot_event, snapshot.c_str());
    } catch (...) { if (api->functions->log_error) api->functions->log_error("Cheeky UEVR callback failed"); }
}

void on_device_reset() {
    std::lock_guard lifecycle(lifecycle_mutex);
    attached_compositor = nullptr;
    next_openvr_attach = 0;
    if (runtime_tick && attachment) runtime_tick(attachment, 0, nullptr, nullptr);
}

template<class T> bool load_export(HMODULE dll, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(dll, name)); return out != nullptr;
}

// Load into local pointers first: a missing export must never leave dangling globals.
bool initialize_dll() {
    if (initialized || !api) return initialized;
    try {
        const auto size = api->functions->get_persistent_dir(nullptr, 0);
        if (!size || size > 32767) return false;
        std::vector<wchar_t> directory(size + 1, L'\0');
        api->functions->get_persistent_dir(directory.data(), static_cast<unsigned>(directory.size()));
        if (!directory[0]) return false;
        std::vector<wchar_t> path(32768);
        const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return false;
        const auto runtime_path = std::filesystem::path(path.data()).parent_path() / L"CheekyFoveatedDLSS" / L"CheekyFoveatedDLSSRuntime.dll";
        struct Library {
            HMODULE value;
            ~Library() { if (value) FreeLibrary(value); }
        } dll{LoadLibraryExW(runtime_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)};
        if (!dll.value) {
            if (api->functions->log_error) api->functions->log_error("Cheeky: cannot load runtime DLL (error %lu). Extract the complete UEVR package including the plugin subfolder.", GetLastError());
            return false;
        }
        CheekyUEVRStartFn start_fn{};
        CheekyUEVRDetachFn detach_fn{};
        CheekyUEVRTickFn tick_fn{};
        CheekyUEVRCommandFn command_fn{};
        CheekyUEVRSnapshotFn snapshot_fn{};
        CheekyUEVRAttachOpenVRFn attach_fn{};
        if (!load_export(dll.value, "CheekyUEVR_Start", start_fn) ||
            !load_export(dll.value, "CheekyUEVR_Detach", detach_fn) ||
            !load_export(dll.value, "CheekyUEVR_Tick", tick_fn) ||
            !load_export(dll.value, "CheekyUEVR_Command", command_fn) ||
            !load_export(dll.value, "CheekyUEVR_Snapshot", snapshot_fn) ||
            !load_export(dll.value, "CheekyUEVR_AttachOpenVR", attach_fn)) return false;
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(start_fn), &pinned)) return false;
        runtime_start = start_fn; runtime_detach = detach_fn; runtime_tick = tick_fn;
        runtime_command = command_fn; runtime_snapshot = snapshot_fn; runtime_attach_openvr = attach_fn;
        load_export(dll.value, "CheekyUEVR_PublishStereo", runtime_publish_stereo);
        load_export(dll.value, "CheekyUEVR_PublishRenderingMode", runtime_publish_mode);
        const auto* r = api->renderer;
        CheekyUEVRStart start{};
        start.config_directory = directory.data(); start.renderer = r->renderer_type;
        start.device = r->device; start.queue = r->command_queue; start.attachment = &attachment;
        initialized = runtime_start(&start);
        if(initialized){
            load_export(dll.value,"CheekyFG_DetachUEVRFrameSource",runtime_detach_frame_source);
            load_export(dll.value,"CheekyFG_AttachUEVRFrameSource",runtime_attach_frame_source);
            load_export(dll.value,"CheekyFG_NeedsUEVRFrameSourceV1",runtime_needs_frame_source);
            frame_source_policy_known=false;
            sync_frame_source();
        }
        if (!initialized) {
            if (attachment) runtime_detach(attachment);
            attachment = 0;
            if (api->functions->log_error) api->functions->log_error("Cheeky runtime could not start; see the Cheeky panel/log for details.");
            // Keep pinned snapshot exports available so Lua can show the failure.
        }
        return initialized;
    } catch (...) {
        if (runtime_detach && attachment) runtime_detach(attachment);
        attachment = 0; initialized = false; return false;
    }
}

void detach_runtime() {
    if(runtime_detach_frame_source)runtime_detach_frame_source();
    runtime_detach_frame_source=nullptr;
    runtime_attach_frame_source=nullptr;runtime_needs_frame_source=nullptr;
    frame_source_policy_known=frame_source_required=false;
    if (runtime_detach && attachment) runtime_detach(attachment);
    attachment = 0;
    attached_compositor = nullptr; next_openvr_attach = 0;
    initialized = false;
    runtime_start = nullptr; runtime_detach = nullptr; runtime_tick = nullptr;
    runtime_attach_openvr = nullptr; runtime_publish_stereo = nullptr; runtime_publish_mode = nullptr;
    runtime_command = nullptr; runtime_snapshot = nullptr;
    std::lock_guard lock(commands_mutex);
    commands.clear();
    // Hook code stays pinned until application exit; this is a detach, not an unload.
}
}

extern "C" __declspec(dllexport) void uevr_plugin_required_version(UEVR_PluginVersion* version) {
    if (version) *version = {UEVR_PLUGIN_VERSION_MAJOR, UEVR_PLUGIN_VERSION_MINOR, UEVR_PLUGIN_VERSION_PATCH};
}

extern "C" __declspec(dllexport) bool uevr_plugin_initialize(const UEVR_PluginInitializeParam* input) {
    if (!input || !input->version || input->version->major != UEVR_PLUGIN_VERSION_MAJOR ||
        input->version->minor < UEVR_PLUGIN_VERSION_MINOR || !input->functions || !input->callbacks || !input->renderer) return false;
    
    if (!input->functions->get_persistent_dir || !input->functions->dispatch_lua_event || !input->functions->remove_callback ||
        !input->callbacks->on_present || !input->callbacks->on_device_reset || !input->callbacks->on_custom_event) return false;

    if (callbacks_registered) return true;
    api = input;

    // Register UEVR callbacks right away so custom events can be captured immediately
    const bool a = api->callbacks->on_present(on_present);
    const bool b = api->callbacks->on_device_reset(on_device_reset);
    const bool c = api->callbacks->on_custom_event(on_custom_event);

    if (!a || !b || !c) {
        if (a) api->functions->remove_callback(reinterpret_cast<void*>(&on_present));
        if (b) api->functions->remove_callback(reinterpret_cast<void*>(&on_device_reset));
        if (c) api->functions->remove_callback(reinterpret_cast<void*>(&on_custom_event));
        return false;
    }

    callbacks_registered = true;
    return true;
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { module = self; DisableThreadLibraryCalls(self); }
    // UEVR removes callbacks before unloading the adapter. Only the runtime's
    // loader-lock-safe atomic detach is allowed here: no mutexes or host logging.
    if (reason == DLL_PROCESS_DETACH && runtime_detach) runtime_detach(attachment);
    return TRUE;
}
