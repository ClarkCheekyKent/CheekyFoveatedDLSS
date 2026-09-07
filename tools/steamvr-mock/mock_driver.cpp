// Development-only provider for Sboy's EyeTrackingOutput helper.
#include <Windows.h>
#include <openvr_driver.h>
#include "EyeTrackingOutput.h"
#include "DriverLog.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

class MockProvider final : public vr::IServerTrackedDeviceProvider {
    std::wstring config;
    ULONGLONG lastConfig{}, lastAttempt{}, start{};
    int mode = -1;
    bool originalSupport{}, supportKnown{};
public:
    vr::EVRInitError Init(vr::IVRDriverContext* context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        HMODULE module{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&HmdDriverFactoryAnchor), &module);
        wchar_t path[32768]{};
        GetModuleFileNameW(module, path, 32768);
        config = (std::filesystem::path(path).parent_path().parent_path().parent_path() / L"mock.ini").wstring();
        eyeTrackingOutput.immediateOutput = false;
        DriverLog("Cheeky mock-only provider loaded. Modes: 0 off, 1 center, 2 sweep, 3 dropout. Waiting for Quest 3.");
        return vr::VRInitError_None;
    }
    static void HmdDriverFactoryAnchor() {}
    const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
    void Cleanup() override {
        if (eyeTrackingOutput.initialized) {
            EyeTrackingOutput::EyeTrackingData data;
            data.valid = false;
            eyeTrackingOutput.SetEyeTrackingData(data);
            eyeTrackingOutput.RunFrame();
            if (supportKnown) vr::VRProperties()->SetBoolProperty(
                vr::VRProperties()->TrackedDeviceToPropertyContainer(0),
                vr::Prop_SupportsXrEyeGazeInteraction_Bool, originalSupport);
        }
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }
    void RunFrame() override {
        const auto now = GetTickCount64();
        if (!lastConfig || now - lastConfig >= 250) {
            lastConfig = now;
            const auto next = static_cast<int>(GetPrivateProfileIntW(L"mock", L"mode", 0, config.c_str()));
            const auto safe = next >= 0 && next <= 3 ? next : 0;
            if (safe != mode) { mode = safe; start = now; DriverLog("Cheeky mock mode=%d", mode); }
        }
        if (!eyeTrackingOutput.initialized) {
            if (!mode || (lastAttempt && now - lastAttempt < 2000)) return;
            lastAttempt = now;
            const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(0);
            char model[256]{};
            vr::ETrackedPropertyError error{};
            vr::VRProperties()->GetStringProperty(container, vr::Prop_ModelNumber_String, model, sizeof(model), &error);
            // Deliberately limit this development build to the user's non-eye-tracked Quest 3.
            if (error != vr::TrackedProp_Success || !std::strstr(model, "Quest 3")) return;
            originalSupport = vr::VRProperties()->GetBoolProperty(container, vr::Prop_SupportsXrEyeGazeInteraction_Bool);
            supportKnown = true;
            if (originalSupport) {
                DriverLog("Cheeky mock: headset already advertises eye tracking; refusing to replace it.");
                return;
            }
            eyeTrackingOutput.Initialize();
            if (!eyeTrackingOutput.initialized) return;
            DriverLog("Cheeky mock attached to %s", model);
        }
        const double seconds = (now - start) / 1000.0;
        EyeTrackingOutput::EyeTrackingData data;
        data.valid = mode != 0 && !(mode == 3 && std::fmod(seconds, 8.0) >= 6.0);
        const double yaw = mode >= 2 ? 0.22 * std::sin(seconds * 0.7853981633974483) : 0.0;
        const double pitch = mode >= 2 ? 0.12 * std::sin(seconds * 1.570796326794897) : 0.0;
        data.focalPointZ = 2.0f;
        data.focalPointX = static_cast<float>(2.0 * std::tan(yaw));
        data.focalPointY = static_cast<float>(2.0 * std::tan(pitch));
        eyeTrackingOutput.SetEyeTrackingData(data);
        eyeTrackingOutput.RunFrame();
    }
};

static MockProvider provider;
extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* name, int* error) {
    if (name && std::strcmp(name, vr::IServerTrackedDeviceProvider_Version) == 0) return &provider;
    if (error) *error = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
