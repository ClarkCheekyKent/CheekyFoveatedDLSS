#include "eye_calibration_pixels.hpp"
#include "eye_calibration.hpp"
#include "settings.hpp"
#include <wrl/client.h>
#include <iostream>
#include <vector>

namespace {
using namespace cheeky::foveated_dlss;
using Microsoft::WRL::ComPtr;
void require(bool ok, const char* text) {
    if (!ok)
        throw std::runtime_error(text);
}
void check(HRESULT hr) {
    require(SUCCEEDED(hr), "Eye calibration GPU operation failed");
}
void run_calibration(bool hardware = false, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
                     bool obscure = false, bool duplicate_eye = false) {
    register_stereo_view(101);
    register_stereo_view(202);
    Settings settings{};
    (void)settings_for_view(settings, 101);
    (void)settings_for_view(settings, 202);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0,
                            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    const auto bytes = calibration_pixel_bytes(format);
    std::vector<unsigned char> black(256 * 128 * bytes, 0);
    D3D11_TEXTURE2D_DESC desc{128, 128, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
    ComPtr<ID3D11Texture2D> a, b, packed;
    check(device->CreateTexture2D(&desc, nullptr, &a));
    check(device->CreateTexture2D(&desc, nullptr, &b));
    desc.Width = 256;
    check(device->CreateTexture2D(&desc, nullptr, &packed));
    eye_calibration_reset_stats();
    eye_calibration_enable(true);
    auto frame = [&](bool swapped) {
        eye_calibration_frame();
        context->UpdateSubresource(a.Get(), 0, nullptr, black.data(), 128 * bytes, 0);
        context->UpdateSubresource(b.Get(), 0, nullptr, black.data(), 128 * bytes, 0);
        eye_calibration_stamp(context.Get(), a.Get(), 101, 0, 0, 128, 128);
        eye_calibration_stamp(context.Get(), b.Get(), 202, 0, 0, 128, 128);
        context->CopySubresourceRegion(packed.Get(), 0, 0, 0, 0, swapped ? b.Get() : a.Get(), 0, nullptr);
        context->CopySubresourceRegion(packed.Get(), 0, 128, 0, 0, swapped ? a.Get() : b.Get(), 0, nullptr);
        if (obscure)
            context->UpdateSubresource(packed.Get(), 0, nullptr, black.data(), 256 * bytes, 0);
        auto ticket = eye_calibration_submit(packed.Get(), 0, 0, 0, 0.5F, 1);
        eye_calibration_result(ticket, 0);
        ticket = eye_calibration_submit(packed.Get(), duplicate_eye ? 0 : 1, 0.5F, 0, 1, 1);
        eye_calibration_result(ticket, 0);
        context->Flush();
        Sleep(2);
        eye_calibration_tick(); // Explicit GPU progress is test-only.
    };
    for (unsigned i = 0; i < 320; ++i)
        frame(true);
    const auto warm = eye_calibration_stats();
    if (obscure || duplicate_eye) {
        require(warm.rejected > 0 && warm.rejected + warm.valid == warm.completed,
                "Rejected captures must be accounted for");
        require(warm.rejection_counts[duplicate_eye ? 2 : 7] > 0,
                "Diagnostics must distinguish duplicate eyes from unreadable markers");
        const auto json = eye_calibration_json();
        require(json.find("\"rejection_counts\":{") != std::string::npos &&
                    json.find("\"scores\":[") != std::string::npos,
                "Support diagnostics must include rejection evidence");
        require(warm.completed > 0 && warm.valid == 0 && warm.applied == 0 &&
                    !stereo_eye_assignment(101).calibrated,
                "Occluded or duplicate-eye submissions must never publish a mapping");
        eye_calibration_enable(false);
        eye_calibration_frame();
        context->Flush();
        const auto deadline = GetTickCount64() + 5000;
        while (eye_calibration_stats().in_flight && GetTickCount64() < deadline) {
            Sleep(1);
            eye_calibration_tick();
        }
        require(eye_calibration_stats().in_flight == 0, "Invalid captures must still drain");
        eye_calibration_stop();
        unregister_stereo_view(101);
        unregister_stereo_view(202);
        return;
    }
    require(warm.valid > 0 && warm.left_view == 202 && warm.right_view == 101,
            "Continuous mode did not identify swapped destinations");
    require(warm.applied > 0 && warm.corrections == 1 && stereo_eye_assignment(101).calibrated &&
                stereo_eye_assignment(101).eye_index == 1,
            "Initial reversed eye mapping was not corrected exactly once");
    for (unsigned i = 0; i < 320; ++i)
        frame(false);
    require(eye_calibration_stats().corrections == 2 && stereo_eye_assignment(101).eye_index == 0,
            "Transition must correct the mapping once, not once per confirming frame");
    eye_calibration_enable(false);
    eye_calibration_frame();
    context->Flush();
    const auto deadline = GetTickCount64() + 5000;
    while (eye_calibration_stats().in_flight && GetTickCount64() < deadline) {
        Sleep(1);
        eye_calibration_tick();
    }
    const auto result = eye_calibration_stats();
    require(result.valid > warm.valid && result.left_view == 101 && result.right_view == 202,
            "Continuous mode missed changed destinations");
    require(result.allocations == warm.allocations, "GPU objects were allocated again after pool warm-up");
    require(result.in_flight == 0 && result.frames == 640 && result.captures == 64, "Continuous readbacks did not drain");
    require(!stereo_eye_assignment(101).calibrated,
            "Disabling must clear correction and reject outstanding results");
    require(result.gpu_us > 0 && result.cpu_us_per_frame > 0, "Continuous timing counters unavailable");
    std::cout << (hardware ? "Eye calibration hardware: frames=" : "Eye calibration WARP: frames=")
              << result.frames << " valid=" << result.valid << " skipped=" << result.skipped
              << " allocated=" << result.allocations << " CPU us/frame=" << result.cpu_us_per_frame
              << " GPU us=" << result.gpu_us << '\n';
    eye_calibration_enable(true);
    for (unsigned i = 0; i < 4; ++i)
        frame(false);
    // Flush submits work but does not complete hardware readbacks. Check the
    // resumed mapping only after these four frames have had time to finish.
    const auto resume_deadline = GetTickCount64() + 5000;
    while (eye_calibration_stats().in_flight && GetTickCount64() < resume_deadline) {
        Sleep(1);
        eye_calibration_tick();
    }
    require(eye_calibration_stats().correction_active, "Calibration resumes after enabling");
    eye_calibration_reset_stats();
    const auto reset = eye_calibration_stats();
    require(reset.correction_active && reset.corrections == 0 && reset.applied == 0 && reset.frames == 0,
            "Counter reset must preserve live mapping and clear only measurements");
    eye_calibration_enable(false);
    eye_calibration_frame();
    context->Flush();
    const auto reset_deadline = GetTickCount64() + 5000;
    while (eye_calibration_stats().in_flight && GetTickCount64() < reset_deadline) {
        Sleep(1);
        eye_calibration_tick();
    }
    require(eye_calibration_stats().in_flight == 0 && eye_calibration_stats().applied == 0,
            "Pre-reset readbacks must not refill counters or republish after disabling");
    eye_calibration_stop();
    unregister_stereo_view(101);
    unregister_stereo_view(202);
}
void pattern_tests() {
    constexpr unsigned side = 60;
    std::array<CalibrationPixel, side * side> pixels{};
    auto score = [&](unsigned candidate) {
        return calibration_pattern_score(pixels.data(), side * sizeof(CalibrationPixel), side, side,
                                          DXGI_FORMAT_R32G32B32A32_FLOAT, candidate, true);
    };
    for (unsigned candidate = 0; candidate < 2; ++candidate)
        for (int offset : {-8, -3, 0, 5, 8})
            for (unsigned mirror = 0; mirror < 4; ++mirror) {
                pixels.fill({.7F, .7F, .7F, 1});
                for (unsigned y = 0; y < 40; ++y)
                    for (unsigned x = 0; x < 40; ++x) {
                        const bool light = calibration_pattern_bit(candidate, (mirror & 1 ? 39 - x : x) / 8,
                                                                    (mirror & 2 ? 39 - y : y) / 8);
                        // Strong lifted blacks and unequal channel gains resemble washed-out grading.
                        const float v = light ? 1.F : 0.F;
                        pixels[(10 - offset + y) * side + 10 + offset + x] =
                            {.75F + .12F * v, .65F + .2F * v, .8F + .15F * v, 1};
                    }
                require(calibration_pattern_classify(score(0), score(1)) == int(candidate),
                        "Pattern must survive grading, bounded displacement and mirrored bounds");
            }
    for (unsigned mode = 0; mode < 4; ++mode) {
        for (unsigned y = 0; y < side; ++y)
            for (unsigned x = 0; x < side; ++x) {
                const float v = mode == 0 ? 1.F : mode == 1 ? x / 60.F :
                                mode == 2 ? float((x / 8 + y / 8) % 2) : .75F;
                pixels[y * side + x] = {v, v, v, 1};
            }
        require(calibration_pattern_classify(score(0), score(1)) == -1,
                "Uniform, clipped, gradient and checkerboard patches must remain unmapped");
    }
    pixels[0].r = std::numeric_limits<float>::quiet_NaN();
    require(score(0) == 0, "Nonfinite readback must not match a pattern");
}
void run_calibration_policy() {
    clear_stereo_calibration();
    register_stereo_view(8001);
    register_stereo_view(8002);
    Settings settings{};
    settings.x_offset = 0.4F;
    (void)settings_for_view(settings, 8001);
    (void)settings_for_view(settings, 8002);
    const auto a = stereo_view_generation(8001), b = stereo_view_generation(8002);
    bool corrected{};
    require(publish_stereo_calibration(8002, 8001, b, a, 10, GetTickCount64(), &corrected) && corrected,
            "Reversed pair must be accepted and counted as a correction");
    require(settings_for_view(settings, 8001).x_offset < 0 && settings_for_view(settings, 8002).x_offset > 0,
            "Manual fixed offsets must follow the calibrated anatomical eye");
    require(publish_stereo_calibration(8002, 8001, b, a, 11, GetTickCount64(), &corrected) && !corrected,
            "Confirmation must not count as another correction");
    require(!publish_stereo_calibration(8001, 8002, a, b, 9, GetTickCount64()),
            "Out-of-order result must be rejected");
    require(!publish_stereo_calibration(8001, 8001, a, a, 12, GetTickCount64()),
            "One handle cannot own both eyes");
    require(!publish_stereo_calibration(8001, 8002, a, b, 12, GetTickCount64() - 1100),
            "Stale readback must be rejected");
    Sleep(1100);
    require(stereo_eye_assignment(8001).calibrated && stereo_eye_assignment(8001).eye_index == 1 &&
                settings_for_view(settings, 8001).x_offset < 0,
            "Missing fresh markers must not resurrect the contradictory fallback eye role");
    require(publish_stereo_calibration(8001, 8002, a, b, 12, GetTickCount64(), &corrected) && corrected &&
                stereo_eye_assignment(8001).eye_index == 0,
            "Fresh marker evidence must replace the retained identity");
    unregister_stereo_view(8001);
    register_stereo_view(8001);
    require(!stereo_eye_assignment(8002).calibrated, "Releasing a member must invalidate the pair");
    require(!publish_stereo_calibration(8002, 8001, b, a, 12, GetTickCount64()),
            "Reused handle address must not accept old-generation data");
    clear_stereo_calibration();
    unregister_stereo_view(8001);
    unregister_stereo_view(8002);
}
} // namespace
int run_eye_calibration_tests() {
    try {
        run_calibration_policy();
        require(calibration_half(0x3c00) == 1 && calibration_half(0x3800) == 0.5F,
                "Half-float decoding failed");
        // Independent packed values exercise channel layout, non-unit HDR
        // values, subnormals, and invalid signals rather than only round trips.
        const auto packed_pixel = [](std::uint32_t bits) {
            return calibration_decode(reinterpret_cast<const unsigned char*>(&bits), DXGI_FORMAT_R11G11B10_FLOAT);
        };
        const auto hdr = packed_pixel(0x380U | (0x400U << 11) | (0x230U << 22));
        require(hdr.r == 0.5F && hdr.g == 2.0F && hdr.b == 6.0F && hdr.a == 1.0F,
                "R11G11B10 HDR channels must retain their different mantissa widths");
        const auto subnormal_pixel = packed_pixel(1U | (1U << 11) | (1U << 22));
        require(subnormal_pixel.r == std::ldexp(1.0F, -20) && subnormal_pixel.g == subnormal_pixel.r &&
                    subnormal_pixel.b == std::ldexp(1.0F, -19), "R11G11B10 subnormal decoding failed");
        require(std::isinf(packed_pixel(0x7c0U).r) && std::isnan(packed_pixel(0x7c1U << 11).g),
                "R11G11B10 invalid channels must remain detectable");
        for (unsigned c = 0; c < 2; ++c)
            for (unsigned y = 0; y < 5; ++y)
                for (unsigned x = 0; x < 5; ++x) {
                    std::uint32_t marker{};
                    calibration_encode_pattern(reinterpret_cast<unsigned char*>(&marker),
                                               DXGI_FORMAT_R11G11B10_FLOAT, c, x * 8, y * 8);
                    const auto pixel = packed_pixel(marker);
                    const float expected = calibration_pattern_bit(c, x, y) ? 1.F : 0.F;
                    require(pixel.r == expected && pixel.g == expected && pixel.b == expected,
                            "Packed HDR pattern must encode exact neutral light/dark cells");
                }
        require(calibration_pattern_classify(.89F, 0.F) == -1 &&
                    calibration_pattern_classify(.95F, .85F) == -1 &&
                    calibration_pattern_classify(1.F, .5F) == 0 &&
                    calibration_pattern_classify(.5F, 1.F) == 1 &&
                    calibration_pattern_classify(calibration_half(0x7e00), 1.F) == -1,
                "Pattern classification must reject weak, ambiguous and nonfinite scores");
        pattern_tests();
        run_calibration();
        for (auto format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                            DXGI_FORMAT_R11G11B10_FLOAT,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT})
            run_calibration(false, format);
        run_calibration(false, DXGI_FORMAT_R8G8B8A8_UNORM, true);
        run_calibration(false, DXGI_FORMAT_R8G8B8A8_UNORM, false, true);
        run_calibration(false, DXGI_FORMAT_R11G11B10_FLOAT, true);
        run_calibration(false, DXGI_FORMAT_R11G11B10_FLOAT, false, true);
        run_calibration(true);
        eye_calibration_enable(true);
        eye_calibration_frame();
        eye_calibration_unsupported_submit();
        const auto unsupported = eye_calibration_stats();
        require(unsupported.unsupported_submission &&
                    std::string(eye_calibration_status(unsupported)).starts_with("Unsupported"),
                "Unsupported submissions must be visible in diagnostics");
        require(eye_calibration_json().find("\"unsupported_submission\":true") != std::string::npos,
                "Support reports must contain calibration status");
        eye_calibration_stop();
        std::cout
            << "OpenVR eye calibration: mapping transitions, pooled readbacks and lifetime policy passed\n";
        return 0;
    } catch (const std::exception& e) {
        eye_calibration_stop();
        std::cerr << "Eye calibration test failed: " << e.what() << '\n';
        return 1;
    }
}
