#pragma once
#include <Windows.h>
#include <cstdint>

struct IDXGISwapChain;
struct ID3D12CommandQueue;

namespace cheeky::standalone {
struct OverlayRuntime {
    std::uint64_t attachment{};
    bool (*command)(std::uint64_t, const char*){};
    bool (*snapshot)(char*, std::uint32_t){};
    const char* host_name{"Standalone"};
};

// Call immediately before the real Present, with the queue used to create THIS
// swap chain. A guessed queue is unsafe; a null D3D12 queue disables the overlay.
void overlay_present(IDXGISwapChain*, ID3D12CommandQueue*, const OverlayRuntime&) noexcept;
// Call before either ResizeBuffers entry point. GPU work is drained before
// releasing backbuffers. On a GPU timeout resources remain alive and DXGI may
// reject the resize; retrying later is safe.
void overlay_before_resize(IDXGISwapChain*) noexcept;
// Call before factory creation for an HWND. Releasing the previous flip-chain
// backbuffers is necessary even when the game did not call ResizeBuffers.
void overlay_before_create(HWND) noexcept;
// Call after a successful IDXGISwapChain3::SetColorSpace1. DXGI does not expose
// a getter; the setter hook supplies the real presentation transfer function.
void overlay_set_color_space(IDXGISwapChain*, std::uint32_t color_space) noexcept;
// Outside DllMain only. This code, including the subclass, is process-resident.
void overlay_shutdown() noexcept;
// Static strings; useful when logging why the menu cannot render.
const char* overlay_status() noexcept;
}
