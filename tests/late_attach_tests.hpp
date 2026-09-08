#pragma once
#include "runtime_api.hpp"
#include <filesystem>
struct ID3D11Device;
struct ID3D12Device;
struct ID3D12CommandQueue;
void prepare_late_attach_test(const std::filesystem::path&, ID3D11Device*, ID3D12Device*, ID3D12CommandQueue*, bool c_callback, bool streamline);
void verify_late_attach_test(CheekyUEVRSnapshotFn);
