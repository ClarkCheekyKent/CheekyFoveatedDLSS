#pragma once
#include <Windows.h>
#include <array>
namespace cheeky::foveated_dlss {
struct NrRuntimeModule { HMODULE module{};DWORD error{};std::array<wchar_t,32768> directory{}; };
NrRuntimeModule load_nr_runtime_module() noexcept;
// NVIDIA's NGX core (_nvngx.dll) from the display driver, for games that never
// load it themselves. Null when the driver does not provide one.
HMODULE load_ngx_core_runtime() noexcept;
}
