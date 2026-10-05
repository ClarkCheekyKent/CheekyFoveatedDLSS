#include "varjo_gaze.hpp"
#include "openvr_gaze_math.hpp"
#include "runtime.hpp"
#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string>
#include <type_traits>

namespace cheeky::foveated_dlss {
namespace {
// Minimal VarjoLib C ABI from the Varjo SDK (Varjo.h, Varjo_types.h). The DLL is
// loaded from the user's Varjo Base installation, never redistributed.
struct VarjoRay { double origin[3]; double forward[3]; };
struct VarjoGaze {
    VarjoRay left_eye, right_eye, gaze;
    double focus_distance, stability;
    std::int64_t capture_time, left_status, right_status, status, frame_number;
    double left_pupil_size, right_pupil_size;
};
static_assert(sizeof(VarjoGaze) == 216, "varjo_Gaze layout");
struct VarjoSession;
using IsAvailable = std::int32_t (*)();
using SessionInit = VarjoSession* (*)();
using GazeInit = void (*)(VarjoSession*);
using IsGazeAllowed = std::int32_t (*)(VarjoSession*);
using GetGaze = VarjoGaze (*)(VarjoSession*);
using GetError = std::int64_t (*)(VarjoSession*);
using GetErrorDesc = const char* (*)(std::int64_t);
using GetVersionString = const char* (*)();

std::mutex poll_mutex; // serializes library loading and session creation
std::mutex session_mutex; // protects session and sample freshness
HMODULE library{};
bool library_failed{}, unavailable_reported{}, open_failure_reported{};
ULONGLONG next_attempt{};
IsAvailable is_available{};
SessionInit session_init{};
GazeInit gaze_init{};
IsGazeAllowed is_gaze_allowed{};
GetGaze get_gaze{};
GetError get_error{};
GetErrorDesc get_error_desc{};
VarjoSession* session{};
std::int64_t last_frame{-1};
ULONGLONG last_frame_change{};
std::atomic<bool> stopped{};
void* pid_target{};
decltype(&GetCurrentProcessId) original_pid{};

// Varjo treats a session opened by a process that also renders through SteamVR
// as a competing application and ends that SteamVR session. Report a different
// process ID to Varjo's own modules while the session is created, as
// PimaxMagic4All does (MIT, Matthieu Bucchianeri).
DWORD WINAPI spoofed_process_id() {
    const auto pid = original_pid();
    HMODULE caller{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(_ReturnAddress()), &caller)) return pid;
    if (caller != library && caller != GetModuleHandleW(L"VarjoRuntime.dll")) return pid;
    return pid + 42;
}

HMODULE load_from(const std::wstring& directory) {
    if (directory.empty()) return nullptr;
    const auto path = directory + L"\\VarjoLib.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

std::wstring registered_openxr_directory() {
    // Varjo Base registers its OpenXR runtime beside a matching VarjoLib.dll.
    HKEY key{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0,
            KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return {};
    std::wstring result;
    wchar_t name[MAX_PATH * 2]{};
    for (DWORD index = 0;; ++index) {
        DWORD length = static_cast<DWORD>(std::size(name));
        if (RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring path(name, length);
        const auto slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos && _wcsicmp(path.c_str() + slash + 1, L"VarjoOpenXR.json") == 0) {
            result = path.substr(0, slash);
            break;
        }
    }
    RegCloseKey(key);
    return result;
}

bool load_library() {
    library = GetModuleHandleW(L"VarjoLib.dll");
    if (!library) library = load_from(registered_openxr_directory());
    wchar_t program_files[MAX_PATH]{};
    const auto length = GetEnvironmentVariableW(L"ProgramW6432", program_files, MAX_PATH);
    if (length && length < MAX_PATH) {
        const std::wstring varjo = std::wstring(program_files) + L"\\Varjo\\";
        if (!library) library = load_from(varjo + L"varjo-openxr");
        if (!library) library = load_from(varjo + L"varjo-compositor");
    }
    if (!library) return false;
    auto resolve = [](auto& function, const char* name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(library, name));
        return function != nullptr;
    };
    GetVersionString version{};
    const bool resolved = resolve(is_available, "varjo_IsAvailable") && resolve(session_init, "varjo_SessionInit") &&
        resolve(gaze_init, "varjo_GazeInit") && resolve(is_gaze_allowed, "varjo_IsGazeAllowed") &&
        resolve(get_gaze, "varjo_GetGaze") && resolve(get_error, "varjo_GetError") &&
        resolve(get_error_desc, "varjo_GetErrorDesc");
    resolve(version, "varjo_GetVersionString");
    trace_event("Varjo gaze: loaded VarjoLib %s exports=%s", version ? version() : "(unknown)", resolved ? "complete" : "incomplete");
    return resolved;
}

VarjoSession* open_session() {
    if (!pid_target) {
        auto* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId"));
        if (!target || MH_CreateHook(target, reinterpret_cast<void*>(&spoofed_process_id),
                reinterpret_cast<void**>(&original_pid)) != MH_OK) return nullptr;
        pid_target = target;
    }
    if (MH_EnableHook(pid_target) != MH_OK) return nullptr;
    auto* result = session_init();
    // Disable, never remove: another thread may still be returning through the
    // trampoline. The shared MinHook owner removes it at teardown.
    MH_DisableHook(pid_target);
    return result;
}
}

void poll_varjo_gaze(bool wanted) noexcept {
    if (!wanted || stopped.load()) return;
    std::unique_lock poll(poll_mutex, std::try_to_lock);
    if (!poll.owns_lock() || library_failed || stopped.load()) return;
    {
        std::lock_guard lock(session_mutex);
        if (session) return;
    }
    const auto now = GetTickCount64();
    if (now < next_attempt) return;
    next_attempt = now + 5000;
    if (!library && !load_library()) {
        library_failed = true;
        log_info("Varjo gaze: VarjoLib.dll not found; native Varjo eye tracking unavailable");
        return;
    }
    if (!is_available()) {
        if (!unavailable_reported) log_info("Varjo gaze: Varjo Base is not running; retrying every 5 seconds");
        unavailable_reported = true;
        return;
    }
    auto* opened = open_session();
    if (!opened) {
        if (!open_failure_reported) log_warning("Varjo gaze: could not open a Varjo session; retrying every 5 seconds");
        open_failure_reported = true;
        return;
    }
    gaze_init(opened);
    if (const auto error = get_error(opened)) {
        const auto* description = get_error_desc(error);
        trace_event("Varjo gaze: gaze initialization error %lld: %s", static_cast<long long>(error), description ? description : "");
    }
    if (!is_gaze_allowed(opened))
        log_warning("Varjo gaze: eye tracking is disabled in Varjo Base; enable it under System > Eye tracking");
    std::lock_guard lock(session_mutex);
    if (stopped.load()) return; // Abandon, as stop_varjo_gaze does.
    session = opened; last_frame = -1; last_frame_change = GetTickCount64();
    log_info("Varjo gaze: session opened; reading eye tracking from the Varjo runtime");
}

bool read_varjo_gaze(float ray[3], bool& available) noexcept {
    std::lock_guard lock(session_mutex);
    available = session != nullptr;
    if (!session) return false;
    const auto gaze = get_gaze(session);
    const auto now = GetTickCount64();
    if (gaze.frame_number != last_frame) { last_frame = gaze.frame_number; last_frame_change = now; }
    // The tracker runs at 100 Hz or more; a frozen frame counter is stale data.
    if (now - last_frame_change > 100) return false;
    const auto* forward = varjo_select_forward(gaze.status, gaze.left_status, gaze.right_status,
        gaze.gaze.forward, gaze.left_eye.forward, gaze.right_eye.forward);
    return forward && varjo_gaze_direction(forward, ray);
}

void stop_varjo_gaze() noexcept {
    stopped.store(true);
    std::lock_guard poll(poll_mutex);
    std::lock_guard lock(session_mutex);
    // Shutdown may run under the loader lock (ReShade add-on unload). Do not
    // call into Varjo IPC there; the runtime releases the session at exit.
    session = nullptr;
}
}
