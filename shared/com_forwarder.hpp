#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cheeky::com_forwarder {

// Some COM proxies implement wrapped methods as one-line forwarders:
//   mov rcx,[rcx+N]; mov rax,[rcx]; jmp [rax+slot*8]
// Linkers fold byte-identical forwarders, so one stub can serve unrelated
// interfaces: ControlVR's dxgi.dll shares ID3D12GraphicsCommandList::CopyResource
// with IDXGISwapChain::GetLastPresentCount. Hooking such a stub reinterprets the
// other interfaces' calls and drops their extra arguments. A pure forwarder only
// replaces `this`, so callers hook the forwarded object's method instead.
struct Method {
    void* object{}; // The `this` that code receives for calls made through the original object.
    void* code{};   // Never a pure forwarder; null when it cannot be resolved safely.
};

namespace detail {
inline bool read(const void* address, void* out, std::size_t size) noexcept {
    __try {
        std::memcpy(out, address, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// A short stub can end at the last readable byte of its section.
inline std::size_t read_code(const void* code, std::uint8_t* out, std::size_t size) noexcept {
    std::size_t count{};
    __try {
        for (; count < size; ++count) out[count] = static_cast<const volatile std::uint8_t*>(code)[count];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}
struct Forwarder {
    std::int32_t object_offset{};
    std::int32_t method_offset{};
};
// Matches only complete forwarding shapes; anything else is real code.
inline bool decode(const void* code, Forwarder& out) noexcept {
    std::uint8_t b[20]{};
    const auto count = read_code(code, b, sizeof(b));
    std::size_t i{};
    const auto has = [&](std::size_t n) { return i + n <= count; };
    const auto disp8 = [&](std::size_t at) { return static_cast<std::int32_t>(static_cast<std::int8_t>(b[at])); };
    const auto disp32 = [&](std::size_t at) { std::int32_t v; std::memcpy(&v, b + at, sizeof(v)); return v; };
    // mov rcx,[rcx+disp8|disp32]
    if (!has(4) || b[0] != 0x48 || b[1] != 0x8B) return false;
    if (b[2] == 0x49) { out.object_offset = disp8(3); i = 4; }
    else if (b[2] == 0x89 && has(7)) { out.object_offset = disp32(3); i = 7; }
    else return false;
    // mov rax,[rcx]
    if (!has(3) || b[i] != 0x48 || b[i + 1] != 0x8B || b[i + 2] != 0x01) return false;
    i += 3;
    // Load the method from [rax+disp], then transfer control to it.
    const auto method = [&](std::uint8_t short_form, std::uint8_t long_form) {
        if (has(2) && b[i] == short_form) { out.method_offset = disp8(i + 1); i += 2; return true; }
        if (has(5) && b[i] == long_form) { out.method_offset = disp32(i + 1); i += 5; return true; }
        return false;
    };
    bool matched{};
    if (has(1) && b[i] == 0xFF) { // jmp [rax+disp]
        ++i; matched = method(0x60, 0xA0);
    } else if (has(2) && b[i] == 0x48 && b[i + 1] == 0xFF) {
        i += 2; matched = method(0x60, 0xA0);
    } else if (has(2) && b[i] == 0x4C && b[i + 1] == 0x8B) { // mov r10,[rax+disp]; jmp r10
        i += 2;
        matched = method(0x50, 0x90) && has(3) && (b[i] == 0x41 || b[i] == 0x49) && b[i + 1] == 0xFF && b[i + 2] == 0xE2;
    } else if (has(2) && b[i] == 0x48 && b[i + 1] == 0x8B) { // mov rax,[rax+disp]; jmp rax
        i += 2;
        matched = method(0x40, 0x80) &&
            ((has(2) && b[i] == 0xFF && b[i + 1] == 0xE0) || (has(3) && b[i] == 0x48 && b[i + 1] == 0xFF && b[i + 2] == 0xE0));
    }
    // Offset zero would dereference the vtable itself, not a wrapped object.
    return matched && out.object_offset > 0 && out.method_offset >= 0 && out.method_offset % sizeof(void*) == 0;
}
}

// Follows pure forwarders from object's vtable slot to the code that runs. A
// forwarder to a different method or an unreadable object resolves to no code:
// hooking that stub could intercept calls from unrelated interfaces.
inline Method resolve(void* object, unsigned slot) noexcept {
    for (unsigned depth = 0; object && depth < 8; ++depth) {
        void** table{};
        void* code{};
        if (!detail::read(object, &table, sizeof(table)) || !table ||
            !detail::read(table + slot, &code, sizeof(code)) || !code) return {};
        detail::Forwarder forwarder{};
        if (!detail::decode(code, forwarder)) return {object, code};
        if (forwarder.method_offset != static_cast<std::int32_t>(slot * sizeof(void*))) return {};
        if (!detail::read(static_cast<const std::byte*>(object) + forwarder.object_offset, &object, sizeof(object)))
            return {};
    }
    return {};
}

} // namespace cheeky::com_forwarder
