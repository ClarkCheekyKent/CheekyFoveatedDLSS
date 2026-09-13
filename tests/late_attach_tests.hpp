#pragma once
#include "runtime_api.hpp"
#include <filesystem>
#include <string_view>
struct ID3D11Device;
struct ID3D12Device;
struct ID3D12CommandQueue;
void prepare_late_attach_test(const std::filesystem::path&, ID3D11Device*, ID3D12Device*, ID3D12CommandQueue*, bool c_callback, bool streamline, const std::filesystem::path& ngx_path = {});
void verify_late_attach_test(CheekyUEVRSnapshotFn, void (*command)(const char*));
void prepare_afw_test(const std::filesystem::path& bin, const std::filesystem::path& root, ID3D12Device*, ID3D12CommandQueue*, std::string_view mode);
void verify_afw_test(CheekyUEVRSnapshotFn, void (*command)(const char*));
