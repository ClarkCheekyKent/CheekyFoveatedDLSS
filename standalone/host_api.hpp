#pragma once
#include <Windows.h>
#include <cstdint>

// Start once, outside DllMain. Host values match CheekyRuntimeHost.
using CheekyHostStartFn = bool (*)(std::uint32_t);
using CheekyHostSnapshotFn = bool (*)(char*, std::uint32_t);
