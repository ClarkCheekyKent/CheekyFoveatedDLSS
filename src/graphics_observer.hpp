#pragma once
#include <d3d12.h>
#include <cstdint>
namespace cheeky::foveated_dlss {
struct NativeObserverStatus {
    bool ready{};
    std::uint64_t submissions{}, copies{}, resets{}, destroyed{};
};
bool initialize_native_observer(ID3D12Device*, ID3D12CommandQueue*) noexcept;
NativeObserverStatus native_observer_status() noexcept;
} // namespace cheeky::foveated_dlss
