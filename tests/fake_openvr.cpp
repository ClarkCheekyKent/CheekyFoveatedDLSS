#include <Windows.h>
#include <cstring>
#include <atomic>

// Cached host interfaces live in a DLL, just like SteamVR's vtables. No headset
// or server is needed; the production hooks must attach to these actual slots.
namespace {
unsigned calls{};
unsigned version = 27;
std::atomic<bool> initialized{true};
std::atomic<unsigned> interface_queries{}, init_calls{}, validity_queries{}, init_token{1};
const char* compositor_version() {
    return version == 29 ? "IVRCompositor_029" : version == 28 ? "IVRCompositor_028" :
        version == 22 ? "IVRCompositor_022" : "IVRCompositor_027";
}
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
extern "C" __declspec(dllexport) void CheekyFakeOpenVR_SetInitialized(bool value) { initialized = value; ++init_token; }
extern "C" __declspec(dllexport) unsigned CheekyFakeOpenVR_InterfaceQueries() { return interface_queries.load(); }
extern "C" __declspec(dllexport) unsigned CheekyFakeOpenVR_InitCalls() { return init_calls.load(); }
extern "C" __declspec(dllexport) unsigned CheekyFakeOpenVR_ValidityQueries() { return validity_queries.load(); }
extern "C" __declspec(dllexport) unsigned VR_GetInitToken() { return init_token.load(); }
extern "C" __declspec(dllexport) bool VR_IsInterfaceVersionValid(const char* name) {
    ++validity_queries;
    return initialized && name && !std::strcmp(name, compositor_version());
}
extern "C" __declspec(dllexport) unsigned VR_InitInternal(int*, int) { ++init_calls; return 1; }
extern "C" __declspec(dllexport) unsigned VR_InitInternal2(int*, int, const char*) { ++init_calls; return 1; }
extern "C" __declspec(dllexport) void* VR_GetGenericInterface(const char* name, int* error) {
    ++interface_queries;
    if (initialized && name && !std::strcmp(name, compositor_version())) { if (error) *error = 0; return &compositor; }
    if (error) *error = 105;
    return nullptr;
}
extern "C" __declspec(dllexport) void VR_ShutdownInternal() { ++calls; initialized = false; ++init_token; }
