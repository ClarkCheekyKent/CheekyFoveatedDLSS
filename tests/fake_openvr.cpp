#include <Windows.h>
#include <cstring>

// Cached host interfaces live in a DLL, just like SteamVR's vtables. No headset
// or server is needed; the production hooks must attach to these actual slots.
namespace {
unsigned calls{};
unsigned version = 27;
__declspec(noinline) int wait(void*, void*, unsigned, void*, unsigned) { ++calls; return 0; }
__declspec(noinline) int submit(void*, int, const void*, const void*, unsigned) { return 0; }
__declspec(noinline) int array_submit(void*, int, const void*, unsigned, const void*, unsigned) { return 0; }
void* slots[8]{};
struct Compositor { void** vtable = slots; } compositor;
}
extern "C" __declspec(dllexport) void CheekyFakeOpenVR_SetVersion(unsigned value) {
    version = value;
    slots[2] = reinterpret_cast<void*>(&wait);
    slots[value == 29 ? 6 : 5] = reinterpret_cast<void*>(&submit);
    if (value >= 28) slots[value == 29 ? 7 : 6] = reinterpret_cast<void*>(&array_submit);
}
extern "C" __declspec(dllexport) void* VR_GetGenericInterface(const char* name, int* error) {
    const char* supported = version == 29 ? "IVRCompositor_029" : version == 28 ? "IVRCompositor_028" :
        version == 22 ? "IVRCompositor_022" : "IVRCompositor_027";
    if (name && !std::strcmp(name, supported)) { if (error) *error = 0; return &compositor; }
    if (error) *error = 105;
    return nullptr;
}
extern "C" __declspec(dllexport) void VR_ShutdownInternal() { ++calls; }
