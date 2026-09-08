#pragma once

#include <Windows.h>

namespace cheeky::foveated_dlss {
void draw_support_report(HMODULE addon);
// Called from AddonUninit, outside the loader lock, before unloading code.
void finish_support_report() noexcept;
}
