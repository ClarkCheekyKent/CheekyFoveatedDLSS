#include "openvr_vtable_hook.hpp"
#include <cstdio>
using namespace cheeky::foveated_dlss;
using Call = int (*)(void*, int*);
Call original{};
int hits{};
__declspec(noinline) int shared_forwarder(void*, int* value) { return ++*value; }
__declspec(noinline) int compositor_detour(void* self, int* value) { ++hits; return original(self,value); }
__declspec(noinline) int later_hook(void*, int*) { return 42; }
int main() {
    // Identical forwarding thunk in unrelated interfaces: an inline detour
    // intercepts both. A table hook must preserve the unrelated call and pointer.
    void* compositor[]{reinterpret_cast<void*>(&shared_forwarder)};
    void* system[]{reinterpret_cast<void*>(&shared_forwarder)};
    OpenVRVtableHook hook;
    if (!hook.install(compositor,reinterpret_cast<void*>(&compositor_detour),reinterpret_cast<void**>(&original))) return 1;
    int value=0;
    if (reinterpret_cast<Call>(system[0])(nullptr,&value)!=1 || hits!=0) return 2;
    if (reinterpret_cast<Call>(compositor[0])(nullptr,&value)!=2 || hits!=1) return 3;
    hook.restore();
    if (compositor[0]!=system[0] || reinterpret_cast<Call>(compositor[0])(nullptr,&value)!=3 || hits!=1) return 4;
    if (!hook.install(compositor,reinterpret_cast<void*>(&compositor_detour),reinterpret_cast<void**>(&original))) return 5;
    compositor[0]=reinterpret_cast<void*>(&later_hook);
    hook.restore();
    if (compositor[0]!=reinterpret_cast<void*>(&later_hook)) return 6;
    std::puts("PASS: shared forwarding thunk isolation, pointer preservation, restore, and later-hook ownership.");
}
