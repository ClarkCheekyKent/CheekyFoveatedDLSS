#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstddef>

// Private, versioned boundary between the unloadable UEVR adapter and the
// process-resident rendering module. No STL or host callbacks cross this ABI.
constexpr std::uint32_t cheeky_uevr_abi = 1;
constexpr std::size_t cheeky_uevr_message_capacity = 32768;
struct CheekyUEVRStart {
    std::uint32_t size{sizeof(CheekyUEVRStart)};
    std::uint32_t abi{cheeky_uevr_abi};
    const wchar_t* config_directory{};
    std::uint32_t renderer{}; // UEVR: 0 DX11, 1 DX12
    void* device{};
    void* queue{};
    std::uint64_t* attachment{};
};
using CheekyUEVRStartFn = bool (*)(const CheekyUEVRStart*);
using CheekyUEVRDetachFn = void (*)(std::uint64_t);
using CheekyUEVRTickFn = void (*)(std::uint64_t, std::uint32_t, void*, void*);
using CheekyUEVRCommandFn = bool (*)(std::uint64_t, const char*);
using CheekyUEVRSnapshotFn = bool (*)(char*, std::uint32_t);
// Every export contains exceptions. Detach is a loader-lock-safe atomic store.
