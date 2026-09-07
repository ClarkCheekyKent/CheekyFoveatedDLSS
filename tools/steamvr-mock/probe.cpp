#include <Windows.h>
#include <openvr.h>
#include <cmath>
#include <cstdio>

int main() {
    vr::EVRInitError error{};
    auto* system = vr::VR_Init(&error, vr::VRApplication_Background);
    if (!system) {
        std::printf("OpenVR initialization failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        return 1;
    }
    std::printf("Runtime: %s\nSampling native OpenVR gaze for 20 seconds (NDC coordinates).\n", system->GetRuntimeVersion());
    unsigned validCount{}, invalidCount{};
    float minX = 100, maxX = -100;
    for (unsigned i = 0; i < 100; ++i) {
        vr::HmdVector2_t left{}, right{};
        const bool valid = system->GetEyeTrackedFoveationCenter(&left, &right) &&
            std::isfinite(left.v[0]) && std::isfinite(left.v[1]) &&
            std::isfinite(right.v[0]) && std::isfinite(right.v[1]);
        if (valid) {
            ++validCount;
            if (left.v[0] < minX) minX = left.v[0];
            if (left.v[0] > maxX) maxX = left.v[0];
        } else ++invalidCount;
        std::printf("valid=%d left=(%.4f, %.4f) right=(%.4f, %.4f)\n", valid, left.v[0], left.v[1], right.v[0], right.v[1]);
        std::fflush(stdout);
        Sleep(200);
    }
    std::printf("RESULT valid=%u invalid=%u left-X-range=%.4f\n", validCount, invalidCount, validCount ? maxX-minX : 0);
    vr::VR_Shutdown();
    return validCount ? 0 : 2;
}
