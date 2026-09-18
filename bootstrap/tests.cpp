#include <Windows.h>
#include <dxgi1_3.h>
#include <d3d11.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
using StatusFn = unsigned (*)();
using ValueFn = unsigned (*)(unsigned);
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
unsigned wait_for_start(StatusFn status) {
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (status() == 1 && GetTickCount64() < deadline) Sleep(10);
    return status();
}

void check_exports(HMODULE proxy, HMODULE system) {
    const auto base = reinterpret_cast<const BYTE*>(system);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    const auto names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    for (DWORD i = 0; i != exports->NumberOfNames; ++i) {
        const auto name = reinterpret_cast<LPCSTR>(base + names[i]);
        const auto ordinal = static_cast<WORD>(exports->Base + ordinals[i]);
        const auto named = GetProcAddress(proxy, name);
        require(named != nullptr, name);
        require(named == GetProcAddress(proxy, MAKEINTRESOURCEA(ordinal)), "DXGI ordinal differs from system");
    }
    std::printf("Verified %lu system DXGI exports and ordinals\n", exports->NumberOfNames);
}

void check_factories(HMODULE proxy, HMODULE system) {
    using FactoryFn = HRESULT (WINAPI*)(REFIID, void**);
    using Factory2Fn = HRESULT (WINAPI*)(UINT, REFIID, void**);
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        for (const char* name : {"CreateDXGIFactory", "CreateDXGIFactory1"}) {
            auto factory = reinterpret_cast<FactoryFn>(GetProcAddress(proxy, name));
            auto original = reinterpret_cast<FactoryFn>(GetProcAddress(system, name));
            const auto& iid = std::strcmp(name, "CreateDXGIFactory") == 0 ? __uuidof(IDXGIFactory) : __uuidof(IDXGIFactory1);
            IDXGIFactory* result{}; IDXGIFactory* expected{};
            require(factory && original, name);
            const auto expected_hr = original(iid, reinterpret_cast<void**>(&expected));
            const auto actual_hr = factory(iid, reinterpret_cast<void**>(&result));
            require(actual_hr == expected_hr && SUCCEEDED(actual_hr), name);
            require(result != nullptr, "factory result missing");
            result->Release();
            expected->Release();
        }
        auto factory = reinterpret_cast<Factory2Fn>(GetProcAddress(proxy, "CreateDXGIFactory2"));
        IDXGIFactory2* result{};
        require(factory && SUCCEEDED(factory(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&result))), "CreateDXGIFactory2");
        result->Release();
    }
}

void check_debug_forward(HMODULE proxy, HMODULE system) {
    using DebugFn = HRESULT (WINAPI*)(UINT, REFIID, void**);
    const auto actual = reinterpret_cast<DebugFn>(GetProcAddress(proxy, "DXGIGetDebugInterface1"));
    const auto expected = reinterpret_cast<DebugFn>(GetProcAddress(system, "DXGIGetDebugInterface1"));
    const GUID missing = {0x21a8dc61,0x632f,0x48ec,{0x9b,0x07,0x77,0x34,0x80,0x50,0x41,0x33}};
    for (unsigned i = 0; i < 3; ++i) {
        void* a{}; void* b{};
        require(actual(0, missing, &a) == expected(0, missing, &b), "ASM forwarded HRESULT differs");
        require(a == b, "ASM forwarded output differs");
    }
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 5 || argc == 6, "Usage: bootstrap-tests proxy|asi|missing|chain|broken-chain|loop-chain <proxy.dll> <plugin.asi> <fake-host.dll> [external-chain.dll]");
        const bool asi = std::wstring(argv[1]) == L"asi";
        const bool missing = std::wstring(argv[1]) == L"missing";
        const bool loop_chain = std::wstring(argv[1]) == L"loop-chain";
        const bool device_chain = std::wstring(argv[1]) == L"device-chain";
        const bool chain = std::wstring(argv[1]) == L"chain" || loop_chain || device_chain;
        SetEnvironmentVariableW(L"CHEEKY_TEST_CHAIN_LOOP", loop_chain ? L"1" : nullptr);
        const bool broken_chain = std::wstring(argv[1]) == L"broken-chain";
        wchar_t executable[32768]{};
        require(GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable)) != 0, "test executable path");
        const auto directory = fs::path(executable).parent_path() /
            (L"bootstrap-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        require(!fs::exists(directory), "fixture directory already exists");
        fs::create_directories(directory / L"CheekyFoveatedDLSS");
        const auto loader_path = directory / (asi ? L"CheekyFoveatedDLSS.asi" : L"dxgi.dll");
        fs::copy_file(argv[asi ? 3 : 2], loader_path);
        const auto host_path = directory / L"CheekyFoveatedDLSS" / L"CheekyFoveatedDLSSHost.dll";
        if (!missing) fs::copy_file(argv[4], host_path);
        const auto chain_path = directory / L"dxgi2.dll";
        if (chain) fs::copy_file(argv[4], chain_path);
        if (argc == 6) fs::copy_file(argv[5], chain_path);
        if (broken_chain) std::ofstream(chain_path) << "Not a DLL";

        const auto caller = GetCurrentThreadId();
        const HMODULE loader = LoadLibraryExW(loader_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(loader != nullptr, "loader failed to load");
        const auto status = reinterpret_cast<StatusFn>(GetProcAddress(loader, "CheekyBootstrap_Status"));
        require(status != nullptr, "bootstrap status export missing");
        if (asi) {
            require(status() == 0, "ASI started inside DllMain");
            const auto initialize = reinterpret_cast<void (*)()>(GetProcAddress(loader, "InitializeASI"));
            require(initialize != nullptr, "InitializeASI export missing");
            initialize(); // The later worker-thread check covers this caller.
            std::vector<std::thread> callers;
            for (unsigned i = 0; i < 12; ++i) callers.emplace_back([initialize] { initialize(); });
            for (auto& thread : callers) thread.join();
            require(wait_for_start(status) == 2, "ASI startup failed");
            initialize();
        } else {
            wchar_t path[MAX_PATH]{};
            require(GetSystemDirectoryW(path, ARRAYSIZE(path)) != 0, "system path");
            const auto system = LoadLibraryExW((fs::path(path) / L"dxgi.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            require(system != nullptr, "system DXGI failed to load");
            check_exports(loader, system);
            check_factories(loader, system);
            check_debug_forward(loader, system);
            if (std::wstring(argv[1]) == L"device" || device_chain) {
                const auto d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                require(d3d11 != nullptr, "D3D11 library");
                const auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(d3d11, "D3D11CreateDevice"));
                require(create != nullptr, "D3D11CreateDevice export");
                ID3D11Device* device{};
                require(SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                    D3D11_SDK_VERSION, &device, nullptr, nullptr)), "D3D11 device creation with chained proxy");
                device->Release();
            }
            if (chain) {
                const auto second = GetModuleHandleW(chain_path.c_str());
                require(second != nullptr, "dxgi2.dll did not load from loader directory");
                const auto value = reinterpret_cast<ValueFn>(GetProcAddress(second, "CheekyFakeHost_Value"));
                require(value && value(4) >= 3, "factory calls bypassed dxgi2.dll");
                const auto thunk = reinterpret_cast<HRESULT (WINAPI*)()>(GetProcAddress(loader, "DXGIDeclareAdapterRemovalSupport"));
                require(thunk && thunk() == 0x1234 && thunk() == 0x1234, "ASM calls bypassed dxgi2.dll");
            }
            require(fs::file_size(directory / L"CheekyFoveatedDLSS-Loader.log") > 2, "loader log missing");
            require(wait_for_start(status) == (missing ? 3U : 2U), "proxy host startup status");
        }
        if (!missing) {
            const auto host = GetModuleHandleW(host_path.c_str());
            require(host != nullptr, "host did not load beside loader");
            const auto value = reinterpret_cast<ValueFn>(GetProcAddress(host, "CheekyFakeHost_Value"));
            require(value && value(0) == 1, "host started more than once");
            require(value(1) == (asi ? 2U : 1U), "incorrect host kind");
            require(value(2) != caller, "host ran on initialization caller");
            if (!asi) require(value(3) == 1, "recursive factory during startup did not complete");
        }
        FreeLibrary(loader);
        // Bootstrap and host remain resident intentionally; files are left under
        // the ignored build output for inspection and removed with that output.
        std::puts("PASS bootstrap forwarding, worker startup and ownership");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL bootstrap: %s (Win32=%lu)\n", error.what(), GetLastError());
        return 1;
    }
}
