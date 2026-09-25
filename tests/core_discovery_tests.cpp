#include "runtime_host_api.hpp"
#include "mock_ngx_parameters.hpp"
#include <Windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cheeky::foveated_dlss;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T proc(HMODULE module, const char* name) {
    auto value = reinterpret_cast<T>(GetProcAddress(module, name));
    require(value != nullptr, name); return value;
}
std::string snapshot(CheekyRuntimeSnapshotFn get) {
    std::vector<char> text(cheeky_runtime_message_capacity);
    require(get(text.data(), static_cast<unsigned>(text.size())), "runtime snapshot");
    return text.data();
}
unsigned dx12_field(const std::string& text, const char* name) {
    const auto apis = text.find("\"apis\":[");
    require(apis != text.npos, "API diagnostic list");
    const auto token = std::string("\"") + name + "\":";
    const auto dx11 = text.find(token, apis);
    const auto dx12 = dx11 == text.npos ? text.npos : text.find(token, dx11 + token.size());
    require(dx12 != text.npos, "DX12 diagnostic field");
    return static_cast<unsigned>(std::stoul(text.substr(dx12 + token.size())));
}
struct Calls {
    using Create = NgxResult (*)(ID3D12GraphicsCommandList*, unsigned, NgxParameters*, NgxHandle**);
    using Evaluate = NgxResult (*)(ID3D12GraphicsCommandList*, const NgxHandle*, const NgxParameters*, NgxProgressCallback);
    using Release = NgxResult (*)(NgxHandle*);
    Create create; Evaluate evaluate; Release release;
    explicit Calls(HMODULE module)
        : create(proc<Create>(module, "NVSDK_NGX_D3D12_CreateFeature")),
          evaluate(proc<Evaluate>(module, "NVSDK_NGX_D3D12_EvaluateFeature")),
          release(proc<Release>(module, "NVSDK_NGX_D3D12_ReleaseFeature")) {}
    void run() const {
        // No GPU work: this verifies actual cached-export interception and
        // callback ownership while the generic host awaits its graphics device.
        MockNgxParameters parameters;
        parameters.Set("Width", 128U); parameters.Set("Height", 128U);
        parameters.Set("OutWidth", 256U); parameters.Set("OutHeight", 256U);
        NgxHandle* handle{};
        require(ngx_succeeded(create(nullptr, 1, &parameters, &handle)), "cached core CreateFeature");
        require(ngx_succeeded(evaluate(nullptr, handle, &parameters, nullptr)), "cached core EvaluateFeature");
        require(ngx_succeeded(release(handle)), "cached core ReleaseFeature");
    }
};
unsigned dx11_evaluations(const std::string& text) {
    const auto pos = text.find("\"evaluations\":", text.find("\"apis\":["));
    require(pos != text.npos, "DX11 evaluations field");
    return static_cast<unsigned>(std::stoul(text.substr(pos + 14)));
}
struct Calls11 {
    using Create = NgxResult(*)(ID3D11DeviceContext*, unsigned, NgxParameters*, NgxHandle**);
    using Evaluate = NgxResult(*)(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallback);
    using EvaluateC = NgxResult(*)(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallbackC);
    using Release = NgxResult(*)(NgxHandle*);
    using Counter = unsigned(*)();
    Create create; Evaluate evaluate; EvaluateC evaluate_c; Release release;
    Counter creates, evaluates, releases;
    explicit Calls11(HMODULE module) :
        create(proc<Create>(module, "NVSDK_NGX_D3D11_CreateFeature")),
        evaluate(proc<Evaluate>(module, "NVSDK_NGX_D3D11_EvaluateFeature")),
        evaluate_c(proc<EvaluateC>(module, "NVSDK_NGX_D3D11_EvaluateFeature_C")),
        release(proc<Release>(module, "NVSDK_NGX_D3D11_ReleaseFeature")),
        creates(proc<Counter>(module, "CheekyFakeCreates")),
        evaluates(proc<Counter>(module, "CheekyFakeEvaluates")),
        releases(proc<Counter>(module, "CheekyFakeReleases")) {}
    void run(bool c = false) const {
        const auto before_create = creates(), before_eval = evaluates(), before_release = releases();
        MockNgxParameters params;
        params.Set("Width", 128U); params.Set("Height", 128U);
        params.Set("OutWidth", 256U); params.Set("OutHeight", 256U);
        NgxHandle* handle{};
        require(ngx_succeeded(create(nullptr, 1, &params, &handle)), "DX11 runtime create");
        require(ngx_succeeded(c ? evaluate_c(nullptr, handle, &params, nullptr) :
            evaluate(nullptr, handle, &params, nullptr)), "DX11 runtime evaluate");
        require(ngx_succeeded(release(handle)), "DX11 runtime release");
        require(creates() == before_create + 1 && evaluates() == before_eval + 1 &&
            releases() == before_release + 1, "DX11 callbacks must stay with their owning module");
    }
};
void test_dx11_ota(const wchar_t* runtime_path, const wchar_t* fixture_path) {
    const auto directory = fs::absolute(fs::path(runtime_path)).parent_path() /
        (L"dx11-ota-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(directory);
    std::ofstream(directory / "settings.ini") << "[CheekyFoveatedDLSS]\nEnabled=0\n";
    const auto load = [&](const fs::path& relative) {
        const auto path = directory / relative;
        fs::create_directories(path.parent_path()); fs::copy_file(fixture_path, path);
        const auto module = LoadLibraryW(path.c_str()); require(module != nullptr, "load DX11 fixture"); return module;
    };
    const auto named = load(L"nvngx_dlss.dll");
    const Calls11 named_calls(named); // Cached before interception starts.
    const auto runtime = LoadLibraryW(runtime_path); require(runtime != nullptr, "load runtime");
    const auto start = proc<CheekyRuntimeStartFn>(runtime, "CheekyRuntime_Start");
    const auto get = proc<CheekyRuntimeSnapshotFn>(runtime, "CheekyRuntime_Snapshot");
    const auto detach = proc<CheekyRuntimeDetachFn>(runtime, "CheekyRuntime_Detach");
    const auto directory_text = directory.wstring(); std::uint64_t attachment{};
    CheekyRuntimeStart input; input.config_directory = directory_text.c_str(); input.renderer = 0;
    input.host = CheekyRuntimeHost::standalone; input.attachment = &attachment;
    require(start(&input), "start DX11 OTA discovery runtime");
    const auto wait_hook = [&](const std::string& marker) {
        for (unsigned i = 0; i < 400; ++i) {
            std::ifstream log(directory / "CheekyFoveatedDLSS-Standalone.log");
            const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
            const auto found = text.rfind(marker);
            if (found != text.npos && text.find("Direct detour installed export=NVSDK_NGX_D3D11_ReleaseFeature", found) != text.npos) return;
            Sleep(25);
        }
        throw std::runtime_error("DX11 runtime detours were not installed");
    };
    wait_hook("NGX runtime");
    named_calls.run(); require(dx11_evaluations(snapshot(get)) == 1, "named DLL intercepted");
    // Reproduce the real reports: the named DLL is already hooked when OTA loads.
    const auto ota = load(L"NVIDIA/NGX/models/dlss/versions/20318464/files/160_E658700.bin");
    const Calls11 ota_calls(ota);
    wait_hook("DX11 OTA runtime discovered slot=1");
    ota_calls.run(); ota_calls.run(true); named_calls.run();
    require(dx11_evaluations(snapshot(get)) == 4, "late OTA native/C and named routes all intercepted");
    const auto ota2 = load(L"NVIDIA/NGX/models/dlss/versions/20318465/files/160_E658700.bin");
    const Calls11 ota2_calls(ota2);
    wait_hook("DX11 OTA runtime discovered slot=2");
    ota2_calls.run(); ota_calls.run(); named_calls.run(true);
    require(dx11_evaluations(snapshot(get)) == 7, "second OTA module does not replace first owner's callbacks");
    const auto decoy = load(L"NVIDIA/NGX/models/dlssd/versions/20318464/files/160_E658700.bin");
    const Calls11 decoy_calls(decoy);
    Sleep(1000); decoy_calls.run();
    require(dx11_evaluations(snapshot(get)) == 7, "RR cache must not be treated as DX11 SR");
    // Forwarding through another intercepted runtime counts/processes once.
    proc<void(*)(HMODULE)>(named, "CheekyFakeForwardDX11To")(ota);
    named_calls.run(); named_calls.run(true);
    require(dx11_evaluations(snapshot(get)) == 9, "nested native/C evaluation must not be double processed");
    require(snapshot(get).find("Active (NVIDIA cached runtime)") != std::string::npos, "Forwarded OTA use is reported as active");
    detach(attachment);
    std::puts("PASS: DX11 late OTA modules, independent callbacks, native/C ABI, SR-only discovery and nested forwarding");
}

}

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 5, "Usage: core-tests accept|reject <runtime.dll> <nvidia-metadata-fixture.dll> <proxy-metadata-fixture.dll>");
        if (std::wstring(argv[1]) == L"dx11-ota") {
            test_dx11_ota(argv[2], argv[3]); return 0;
        }
        const bool reject = std::wstring(argv[1]) == L"reject";
        wchar_t path[32768]{};
        require(GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) != 0, "executable path");
        const auto directory = fs::path(path).parent_path() /
            (L"core-discovery-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(directory);
        fs::copy_file(argv[reject ? 4 : 3], directory / L"nvngx.dll");
        const auto alias = LoadLibraryW((directory / L"nvngx.dll").c_str());
        require(alias != nullptr, "load nvngx alias fixture");
        const Calls alias_calls(alias); // Exactly how OptiScaler caches NGX calls.
        const auto runtime = LoadLibraryW(argv[2]);
        require(runtime != nullptr, "load resident runtime");
        const auto start = proc<CheekyRuntimeStartFn>(runtime, "CheekyRuntime_Start");
        const auto get = proc<CheekyRuntimeSnapshotFn>(runtime, "CheekyRuntime_Snapshot");
        const auto detach = proc<CheekyRuntimeDetachFn>(runtime, "CheekyRuntime_Detach");
        const auto directory_text = directory.wstring();
        std::uint64_t attachment{};
        CheekyRuntimeStart input;
        input.config_directory = directory_text.c_str(); input.renderer = 1;
        input.host = CheekyRuntimeHost::optiscaler; input.attachment = &attachment;
        require(start(&input), "start runtime for OptiScaler alias discovery");
        const auto log_contains = [&](const char* part) {
            std::ifstream log(directory / L"CheekyFoveatedDLSS-OptiScaler.log");
            const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
            return text.find(part) != text.npos;
        };
        bool classified{};
        for (unsigned i = 0; i < 200 && !classified; ++i) {
            if (reject) {
                classified = log_contains("NGX core alias ignored");
            } else classified = snapshot(get).find("\"direct_detour\":true") != std::string::npos;
            if (!classified) Sleep(25);
        }
        require(classified, "core alias classification completed");
        alias_calls.run();
        const auto first = snapshot(get);
        require(dx12_field(first, "creates") == (reject ? 0U : 1U), "only identified NVIDIA alias is intercepted");
        require(dx12_field(first, "evaluations") == (reject ? 0U : 1U), "cached alias evaluation routes through expected owner");
        if (!reject) require(dx12_field(first, "ngx_route") == 2U, "alias evaluation uses core ABI route");

        fs::copy_file(argv[3], directory / L"_nvngx.dll");
        const auto canonical = LoadLibraryW((directory / L"_nvngx.dll").c_str());
        require(canonical != nullptr, "load canonical core after alias classification");
        const Calls canonical_calls(canonical);
        // A rejected alias must not suppress discovery of the real core later;
        // an accepted alias must keep ownership when its inner core appears.
        bool canonical_ready = !reject;
        for (unsigned i = 0; i < 200; ++i) {
            // direct_detour becomes true after the first D3D11 export; wait
            // for the final D3D12 export before using the cached callback set.
            if (reject && log_contains("Direct detour installed export=NVSDK_NGX_D3D12_ReleaseFeature")) {
                canonical_ready = true; break;
            }
            if (!reject && i >= 100) break;
            Sleep(25);
        }
        require(canonical_ready, "canonical core callback set fully installed");
        canonical_calls.run();
        alias_calls.run();
        const auto final = snapshot(get);
        require(dx12_field(final, "creates") == (reject ? 1U : 2U), "core owner is stable when canonical module appears");
        require(dx12_field(final, "evaluations") == (reject ? 1U : 2U), "later canonical load does not cross-wire evaluation callbacks");
        detach(attachment);
        std::puts("PASS: NGX alias metadata, cached export routing, proxy rejection and stable core ownership");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL core discovery: %s (Win32=%lu)\n", error.what(), GetLastError());
        return 1;
    }
}
