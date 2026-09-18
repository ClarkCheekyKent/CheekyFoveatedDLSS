#include "loader.hpp"

// OptiScaler's LoadAsiPlugins resolves this exact, undecorated void(void) name.
// Some loaders invoke it while holding the loader lock: queue, never wait.
extern "C" __declspec(dllexport) void InitializeASI() noexcept {
    cheeky_bootstrap_start(CheekyBootstrapHost::optiscaler);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) cheeky_bootstrap_attach(module);
    return TRUE;
}
