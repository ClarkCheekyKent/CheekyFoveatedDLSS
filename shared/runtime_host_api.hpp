#pragma once
#include <cstddef>
#include <cstdint>

// Versioned, process-local C export boundary. No STL objects or callbacks from
// an unloadable host cross into the resident runtime. The caller keeps device
// and queue alive for Start/Tick; the runtime does not own these pointers.
constexpr std::uint32_t cheeky_runtime_abi = 1;
constexpr std::size_t cheeky_runtime_message_capacity = 32768;
enum class CheekyRuntimeHost : std::uint32_t {
    uevr = 0,
    standalone = 1,
    optiscaler = 2,
};
struct CheekyRuntimeStart {
    std::uint32_t size{sizeof(CheekyRuntimeStart)};
    std::uint32_t abi{cheeky_runtime_abi};
    const wchar_t* config_directory{};
    std::uint32_t renderer{}; // 0 D3D11, 1 D3D12
    void* device{};
    void* queue{}; // Required for D3D12 processing.
    std::uint64_t* attachment{};
    CheekyRuntimeHost host{CheekyRuntimeHost::standalone};
};
using CheekyRuntimeStartFn = bool (*)(const CheekyRuntimeStart*);
using CheekyRuntimeDetachFn = void (*)(std::uint64_t);
using CheekyRuntimeTickFn = void (*)(std::uint64_t, std::uint32_t, void*, void*);
using CheekyRuntimeCommandFn = bool (*)(std::uint64_t, const char*);
using CheekyRuntimeSnapshotFn = bool (*)(char*, std::uint32_t);

// Start may precede graphics discovery (device/queue null). A successful Start
// returns an attachment; processing stays paused until Tick supplies graphics.
// Tick is a frame callback, not a polling timer. Passing a null device pauses
// processing during a device reset. Detach is safe under the loader lock and
// does not unload the process-resident runtime. Stale attachments are rejected.
//
// Command: UTF-8 "1\n<request-id>\n<action>\n", bounded to 8192 bytes. "set"
// accepts newline-separated Name=value pairs and applies them atomically.
// Snapshot: bounded null-terminated JSON including settings, setting_groups,
// host, request/applied_request, readiness, and processing diagnostics. False
// means failure or insufficient capacity; no partial JSON is returned.
// All exports contain C++ exceptions. Start/Tick/Command/Snapshot must be
// called outside DllMain. Legacy CheekyUEVR_* exports remain ABI-compatible.
