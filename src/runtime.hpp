#pragma once

#include <Windows.h>

struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace cheeky::foveated_dlss {

void set_addon_modules(HMODULE addon, HMODULE reshade) noexcept;
void log_message(int level, const char* message) noexcept;
void log_info(const char* message) noexcept;
void log_warning(const char* message) noexcept;
void log_error(const char* message) noexcept;
void trace_event(const char* format, ...) noexcept;
void close_trace_log() noexcept;

void note_d3d12_command_list_submission(
    ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* command_list
) noexcept;
void note_d3d12_present(ID3D12CommandQueue* queue) noexcept;

[[nodiscard]] bool start_interception() noexcept;
void stop_interception() noexcept;
[[nodiscard]] bool install_early_loader_interception() noexcept;
void uninstall_early_loader_interception() noexcept;

}  // namespace cheeky::foveated_dlss
