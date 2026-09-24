#pragma once

#include <Windows.h>

// Both loaders are deliberately tiny. The host owns all graphics hooks, settings
// and processing. These values match CheekyRuntimeHost in runtime_host_api.hpp.
enum class CheekyBootstrapHost : unsigned { standalone = 1, optiscaler = 2 };

void cheeky_bootstrap_attach(HMODULE module) noexcept;
void cheeky_bootstrap_start(CheekyBootstrapHost host) noexcept;
// Call only after a successful real DXGI factory creation (DXGI rejects calls
// from DllMain). The bootstrap worker itself bypasses this startup barrier.
void cheeky_bootstrap_after_factory() noexcept;

// 0 = idle, 1 = worker queued/starting, 2 = host started, 3 = startup failed.
extern "C" __declspec(dllexport) unsigned CheekyBootstrap_Status() noexcept;
