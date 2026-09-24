#include "eye_calibration_pixels.hpp"
#include "eye_calibration.hpp"
#include "eye_calibration_capture.hpp"
#include "eye_calibration_search.hpp"
#include "eye_calibration_tracking.hpp"
#include "settings.hpp"
#include "support_zip.hpp"
#include <wrl/client.h>
#include <iostream>
#include <vector>
#include <thread>

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
void test_wide_search() {
    CalibrationSearchImage image;
    image.width = 380; image.height = 280;
    image.original_width = image.width; image.original_height = image.height;
    image.pixels.assign(image.width * image.height, .2F);
    const std::vector<CalibrationSearchTarget> targets{
        {{84, 76, 3377866}, 0, 640, 480}, {{276, 172, 10377015}, 0, 640, 480},
        {{508, 60, 8094815}, 1, 640, 480}};
    auto stamp = [&](unsigned target, double left, double top, double sx, double sy, bool flip = false) {
        for (unsigned y = 0; y < image.height; ++y) for (unsigned x = 0; x < image.width; ++x) {
            const int px = int(std::floor((x + .5 - left) / sx)), py = int(std::floor((y + .5 - top) / sy));
            if (px < 0 || py < 0 || px >= 40 || py >= 40) continue;
            const auto& t = targets[target];
            image.pixels[y * image.width + x] = calibration_pattern_bit(t.candidate, px / 8, flip ? 4 - py / 8 : py / 8, t.marker.code) ? .85F : .12F;
        }
    };
    stamp(0, 16.7, 91.3, 1.13, .96);
    stamp(1, 233.66, 183.46, 1.13, .96);
    auto found = calibration_search(image, targets);
    require(found.valid && found.candidate == 0 && !found.flipped && found.placement.marker == targets[0].marker,
        "Wide acquisition must find displaced scaled markers and prefer the complete marker nearest an edge");
    require(std::abs(found.placement.width - 380 / 1.13) < 30 && std::abs(found.placement.height - 280 / .96) < 30,
        "Acquisition must estimate independent horizontal and vertical scale");
    stamp(2, 150, 140, 1, 1);
    found = calibration_search(image, targets);
    require(!found.valid && found.ambiguous, "Two source identities in one eye must not produce a mapping");
    image.pixels.assign(image.width * image.height, .2F);
    stamp(1, 210, 110, .9, 1.1, true);
    found = calibration_search(image, targets);
    require(found.valid && found.flipped, "Wide acquisition must identify a shader vertical flip");
    image.pixels.assign(image.width * image.height, .2F);
    require(!calibration_search(image, targets).valid, "Flat or missing markers must never lock");
    std::cout << "PASS wide marker search: displacement, independent scale, edge preference, ambiguity and flip\n";
}
void test_hogwarts_corner_pair() {
    // Support 13844: source 3440x1440, submitted eye 1493x1440.
    // Reconstruct codes to test clipping/corner choice without lossy ZIP previews.
    const CalibrationPlacement outer{973.5, 0, 1493, 1440, {1020, 12}};
    const auto padded = calibration_padded_corner({973.5, 20, 1493, 1420, {1980, 108}}, 3440, 1440, 0, 1493, 1420);
    require(padded.marker.x == 986 && padded.marker.y == 32,
        "Reacquisition padding must follow the actual crop boundary, not the previous inset marker");
    const auto twice = calibration_padded_corner(padded, 3440, 1440, 0, 1493, 1420);
    require(twice.marker == padded.marker, "Repeated crop padding must not drift toward image center");
    const auto pair = calibration_corner_pair(outer, 3440, 1440, 0);
    require(pair.count == 2 && pair.at(1).marker.x == 1068 && pair.at(1).marker.y == 60,
        "Acquisition must use a close non-overlapping outer/inner corner pair");
    auto points = calibration_marker_points(pair, 0, 0, 3440, 0, 9918158);
    std::vector<CalibrationSearchTarget> targets;
    for (unsigned i = 0; i < points.count; ++i) targets.push_back({points.points[i], 0, 3440, 1440});
    CalibrationMarkerPoint middle{1980, 12};
    middle.code = calibration_location_code(middle, 3440, 0, 9918158);
    targets.push_back({middle, 0, 3440, 1440});
    for (bool clipped : {false, true}) {
        CalibrationSearchImage image;
        image.width = 1493; image.height = 1440;
        image.original_width = 1493; image.original_height = 1440;
        image.pixels.assign(image.width * image.height, .2F);
        for (const auto& t : targets) for (unsigned y = 0; y < 40; ++y) for (unsigned x = 0; x < 40; ++x) {
            const int px = int(t.marker.x + x) - 974;
            const int py = int(t.marker.y + y) - (clipped ? 20 : 0);
            if (px < 0 || py < 0 || px >= int(image.width) || py >= int(image.height)) continue;
            image.pixels[py * image.width + px] = calibration_pattern_bit(0, x / 8, y / 8, t.marker.code) ? .85F : .12F;
        }
        const auto found = calibration_search(image, targets);
        require(found.valid && found.placement.marker == points.points[clipped ? 1 : 0],
            "Prefer outer corner over top-center; use nearby inner marker if outer code clips");
        CalibrationPlacementPlan locked; locked.placements[0] = found.placement;
        require(calibration_marker_points(locked, 0, 0, 3440, 0, 9918158).count == 1,
            "A acquired pair must collapse to one tracking marker");
    }
    std::cout << "PASS Hogwarts corner pair: outer priority, clipping fallback, single-marker tracking\n";
}
void test_hogwarts_tracking() {
    // Support 41612: use the logged stale transforms and actual marker codes.
    // Reconstruct full-resolution pixels; support BMPs are reduced previews.
    constexpr unsigned w = 1493, h = 1440;
    const std::array<CalibrationPlacement, 2> stale{{
        {976, -24.6531, 1484.44, 1431.74, {1020, 12}},
        {874.8, -29.8868, 1629.32, 1482.53, {2332, 60}}}};
    const auto started = GetTickCount64();
    for (unsigned eye = 0; eye < 2; ++eye) for (bool reversed : {false, true}) for (bool flip : {false, true}) {
        auto learned = stale[eye];
        learned.marker.code = calibration_location_code(learned.marker, 3440, eye, eye ? 33300034 : 9918158);
        const float u0 = reversed ? 1.F : .5F, u1 = reversed ? .5F : 1.F;
        const float v0 = reversed ? 1.F : 0.F, v1 = reversed ? 0.F : 1.F;
        std::vector<unsigned> pixels(w * 2 * h, 0xff404040);
        auto render = [&](unsigned step, bool missing = false, bool wrong_code = false) {
            std::fill(pixels.begin(), pixels.end(), 0xff404040);
            if (missing) return;
            const int mx = int(learned.marker.x) - 974 + (eye ? -1 : 1) * int(step * 24);
            const unsigned my = learned.marker.y + step * 20;
            for (unsigned y = 0; y < 40; ++y) for (unsigned x = 0; x < 40; ++x) {
                const unsigned px = w + (reversed ? w - 1 - (mx + x) : mx + x);
                unsigned py = flip ? h - 1 - (my + y) : my + y;
                if (reversed) py = h - 1 - py;
                const bool light = calibration_pattern_bit(eye, x / 8, y / 8,
                    wrong_code ? (learned.marker.code ^ 0x1555555) : learned.marker.code);
                pixels[py * w * 2 + px] = light ? 0xffdddddd : 0xff202020;
            }
        };
        for (unsigned step = 0; step < 5; ++step) {
            render(step);
            if (!step && !reversed && !flip) {
                const auto old = calibration_sample_rect(learned, false, w * 2, h, u0, v0, u1, v1);
                require(old[2] && calibration_pattern_score(pixels.data() + old[1] * w * 2 + old[0], w * 8,
                    old[2], old[3], DXGI_FORMAT_R8G8B8A8_UNORM, eye, true, learned.marker.code, 1) < .9F,
                    "The reported Hogwarts displacement must fail the old fixed patch");
            }
            const auto patch = calibration_tracking_patch(learned, eye, flip, w * 2, h, u0, v0, u1, v1);
            require(patch.enabled && patch.rect[0] >= w && patch.rect[0] + patch.rect[2] <= w * 2 &&
                patch.rect[2] <= 256 && patch.rect[3] <= 256,
                "Local tracking must remain bounded inside its own packed eye");
            const auto found = calibration_track(pixels.data() + patch.rect[1] * w * 2 + patch.rect[0], w * 8,
                DXGI_FORMAT_R8G8B8A8_UNORM, patch);
            require(found.valid && found.flipped == flip && found.candidate == eye,
                "Local tracking must recover displaced corner markers, flips and reversed bounds");
            learned = found.placement;
        }
        const auto patch = calibration_tracking_patch(learned, eye, flip, w * 2, h, u0, v0, u1, v1);
        for (bool missing : {false, true}) {
            render(4, missing, !missing);
            require(!calibration_track(pixels.data() + patch.rect[1] * w * 2 + patch.rect[0], w * 8,
                DXGI_FORMAT_R8G8B8A8_UNORM, patch).valid, "Missing or wrong codes must not refresh tracking");
        }
    }
    std::cout << "PASS Hogwarts moving-marker tracking: reported displacement, recentering, packed/reversed bounds, flip, wrong/missing code ("
        << GetTickCount64() - started << " ms)\n";
}
void test_cyberpunk_capture_search() {
    // Support capture 16380, sequence 42182: exact source/submitted dimensions,
    // source marker positions and epoch codes. The submitted previews show
    // source B cropped at (0,70), source A at (1595,70), with no pixel resize.
    // Reconstruct full-resolution codes; the ZIP's 634x472 previews reduce a
    // 40px code to ~5px and cannot serve as lossless recognition fixtures.
    constexpr std::array<std::array<unsigned, 2>, 13> positions{{
        {12,12},{780,12},{828,60},{1404,12},{1020,204},{1404,492},{1980,924},
        {1548,12},{12,60},{1644,60},{828,12},{828,108},{1596,924}}};
    constexpr std::array<std::uint32_t, 2> bases{3377866,8094815};
    std::vector<CalibrationSearchTarget> targets;
    for (unsigned c = 0; c < 2; ++c) for (const auto& xy : positions) {
        CalibrationMarkerPoint p{c ? 6288 - 40 - xy[0] : xy[0], xy[1]};
        p.code = calibration_location_code(p, 6288, c, bases[c]);
        targets.push_back({p, c, 6288, 3568});
    }
    for (unsigned eye = 0; eye < 2; ++eye) {
        const unsigned candidate = 1 - eye, crop_x = eye ? 1595 : 0;
        CalibrationSearchImage image;
        image.width = 2048; image.height = unsigned(3498. * 2048 / 4693);
        image.original_width = 4693; image.original_height = 3498;
        image.pixels.assign(image.width * image.height, .2F);
        for (unsigned y = 0; y < image.height; ++y) for (unsigned x = 0; x < image.width; ++x) {
            const unsigned sx = crop_x + unsigned((x + .5) * 4693 / image.width);
            const unsigned sy = 70 + unsigned((y + .5) * 3498 / image.height);
            for (const auto& target : targets) {
                const auto& p = target.marker;
                if (target.candidate != candidate || sx < p.x || sy < p.y || sx >= p.x + 40 || sy >= p.y + 40) continue;
                image.pixels[y * image.width + x] = calibration_pattern_bit(candidate, (sx - p.x) / 8, (sy - p.y) / 8, p.code) ? .85F : .12F;
            }
        }
        const auto start = GetTickCount64();
        const auto result = calibration_search(image, targets);
        std::cout << "Cyberpunk eye=" << eye << " valid=" << result.valid << " candidate=" << result.candidate
            << " marker=" << result.placement.marker.x << ',' << result.placement.marker.y
            << " search_ms=" << GetTickCount64() - start << '\n';
        require(result.valid && result.candidate == candidate && !result.flipped,
            "Reported Cyberpunk dimensions, codes and asymmetric crop must identify each physical eye");
        require(result.placement.marker.x == (eye ? 1980U : 4268U) && result.placement.marker.y == 924,
            "Cyberpunk must track the complete side marker, rejecting clipped top and border-adjacent codes");
        const auto rect = calibration_sample_rect(result.placement, false, 4693, 3498, 0, 0, 1, 1);
        require(rect[2] && rect[3], "Chosen Cyberpunk edge marker must admit a complete tiny tracking patch");
        std::vector<unsigned char> patch(std::size_t(rect[2]) * rect[3] * 4);
        const auto& marker = result.placement.marker;
        for (unsigned y = 0; y < rect[3]; ++y) for (unsigned x = 0; x < rect[2]; ++x) {
            const unsigned sx = crop_x + rect[0] + x, sy = 70 + rect[1] + y;
            auto* pixel = patch.data() + (std::size_t(y) * rect[2] + x) * 4;
            pixel[0] = pixel[1] = pixel[2] = 51; pixel[3] = 255;
            if (sx >= marker.x && sy >= marker.y && sx < marker.x + 40 && sy < marker.y + 40)
                calibration_encode_pattern(pixel, DXGI_FORMAT_R8G8B8A8_UNORM, candidate, sx - marker.x, sy - marker.y, marker.code);
        }
        require(calibration_pattern_score(patch.data(), rect[2] * 4, rect[2], rect[3], DXGI_FORMAT_R8G8B8A8_UNORM,
            candidate, true, marker.code, 1) >= calibration_pattern_min_score,
            "Cyberpunk acquisition must hand off to the production tiny-patch tracker at its actual full-resolution location");
    }
}
void test_deferred_capture_close() {
    eye_calibration_stop();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context));
    D3D11_TEXTURE2D_DESC desc{128, 128, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
        {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
    ComPtr<ID3D11Texture2D> output;
    check(device->CreateTexture2D(&desc, nullptr, &output));
    eye_calibration_enable(true);
    eye_calibration_frame();
    std::vector<unsigned> black(128 * 128);
    context->UpdateSubresource(output.Get(), 0, nullptr, black.data(), 128 * 4, 0);
    eye_calibration_stamp(context.Get(), output.Get(), 7101, 0, 0, 128, 128);
    std::uint64_t ticket{1};
    std::thread xr([&] {
        ticket = eye_calibration_submit(output.Get(), 0, 0, 0, 1, 1);
        eye_calibration_frame();
    });
    xr.join();
    require(!ticket, "Unprotected cross-thread submission must be rejected");
    const auto snapshot = eye_calibration_json();
    require(snapshot.find("\"finish_wrong_thread\":1") != std::string::npos &&
        snapshot.find("\"submit_wrong_thread\":1") != std::string::npos &&
        snapshot.find("\"closed\":false") != std::string::npos &&
        snapshot.find("\"close_requested\":true") != std::string::npos,
        "Different XR thread must defer closure without touching the immediate context");
    eye_calibration_tick(); // Finish the deferred query on its render thread.
    context->Flush(); // Test-only GPU submission.
    const auto deadline = GetTickCount64() + 3000;
    while (eye_calibration_stats().in_flight && GetTickCount64() < deadline) {
        Sleep(1);
        eye_calibration_tick();
    }
    require(eye_calibration_stats().in_flight == 0 && !eye_calibration_stats().correction_active,
        "Render thread must retire deferred closure without publishing incomplete eye mapping");
    // More captures than the ring capacity must continue to drain. Each has
    // one evaluation and an XR-thread close, matching the reported pipeline.
    const auto previous_completed = eye_calibration_stats().completed;
    for (unsigned frame = 0; frame < 120; ++frame) {
        std::thread begin([] { eye_calibration_frame(); }); begin.join();
        eye_calibration_stamp(context.Get(), output.Get(), 7101, 0, 0, 128, 128);
        context->Flush();
        Sleep(1);
        eye_calibration_tick();
    }
    std::thread close([] { eye_calibration_enable(false); eye_calibration_frame(); }); close.join();
    eye_calibration_tick();
    context->Flush();
    const auto drain_deadline = GetTickCount64() + 3000;
    while (eye_calibration_stats().in_flight && GetTickCount64() < drain_deadline) {
        Sleep(1); eye_calibration_tick();
    }
    require(eye_calibration_stats().completed >= previous_completed + 12 &&
        eye_calibration_stats().in_flight == 0 && !eye_calibration_stats().correction_active,
        "Cross-thread frames must keep sampling beyond ring capacity and drain after disable");
    eye_calibration_stop();
}
// Execute the production DX11 stamp/read paths. Test-only CPU transport models
// a host crop/resize between those points, including packed bounds and origins.
void crop_calibration11() {
    for (unsigned c = 0; c < 2; ++c) {
        const auto plan = calibration_placement_plan(2000, 1500, c, 1500, 1500);
        require(plan.count == 16 && plan.at(1).x == 250 && plan.at(1).width == 1500,
            "Reported 2000x1500-to-1500x1500 case must propose the centered 250px crop");
        const auto points = calibration_marker_points(plan, 0, 0, 2000, c, 0);
        for (unsigned i = 0; i < points.count; ++i) {
            for (unsigned j = 0; j < i; ++j) {
                const auto& a = points.points[i]; const auto& b = points.points[j];
                require(a.x + 40 <= b.x || b.x + 40 <= a.x || a.y + 40 <= b.y || b.y + 40 <= a.y,
                    "Candidate marker writes must never partially overlap");
                require(a.code != b.code, "Different candidate locations must have distinguishable codes");
            }
        }
        const auto normal = calibration_sample_rect(plan.at(1), false, 3000, 1500, 0, 0, .5F, 1);
        const auto reversed = calibration_sample_rect(plan.at(1), false, 3000, 1500, .5F, 1, 0, 0);
        require(std::abs(int(normal[2]) - int(reversed[2])) <= 1 && std::abs(int(normal[3]) - int(reversed[3])) <= 1 &&
            std::abs(int(normal[0] + reversed[0] + normal[2]) - 1500) <= 1 &&
            std::abs(int(normal[1] + reversed[1] + normal[3]) - 1500) <= 1,
            "Reversed packed UV bounds must preserve the inferred sample footprint");
    }
    eye_calibration_stop(); eye_calibration_reset_stats();
    register_stereo_view(7101); register_stereo_view(7102);
    Settings settings{};
    (void)settings_for_view(settings, 7101); (void)settings_for_view(settings, 7102);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context));
    constexpr unsigned sw = 420, sh = 320, vw = 400, vh = 300, origin = 10;
    D3D11_TEXTURE2D_DESC desc{sw, sh, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
        {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
    std::array<ComPtr<ID3D11Texture2D>, 2> source;
    for (auto& r : source) check(device->CreateTexture2D(&desc, nullptr, &r));
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    check(device->CreateTexture2D(&desc, nullptr, &readback));
    const std::vector<unsigned> background(sw * sh, 0xff404040);
    eye_calibration_enable(true);
    auto attempt = [&](double crop_x, double crop_y, double crop_w, double crop_h,
                       unsigned size, bool flip = false, bool obscure = false, bool swapped = false, bool asymmetric = false,
                       bool delayed_completion = false) {
        auto support = request_calibration_images(true);
        for (unsigned n = 0; !eye_calibration_frame() && n < 20; ++n) {}
        auto target_desc = desc;
        target_desc.Width = size * 2; target_desc.Height = size;
        target_desc.Usage = D3D11_USAGE_DEFAULT; target_desc.CPUAccessFlags = 0;
        ComPtr<ID3D11Texture2D> target;
        check(device->CreateTexture2D(&target_desc, nullptr, &target));
        std::vector<unsigned> pixels(size * size * 2, 0xff404040);
        for (unsigned c = 0; c < 2; ++c) {
            context->UpdateSubresource(source[c].Get(), 0, nullptr, background.data(), sw * 4, 0);
            eye_calibration_stamp(context.Get(), source[c].Get(), 7101 + c, origin, origin, vw, vh);
            context->CopyResource(readback.Get(), source[c].Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)); // Test host only.
            for (unsigned y = 0; y < size; ++y) for (unsigned x = 0; x < size; ++x) {
                const double dx = asymmetric && c ? 7.3 : 0, dy = asymmetric && c ? 3.2 : 0;
                const double dw = asymmetric && c ? 13.1 : 0, dh = asymmetric && c ? 4.6 : 0;
                const auto sx = origin + unsigned(crop_x + dx + (x + .5) * (crop_w - dw) / size);
                const auto sy = origin + unsigned(crop_y + dy + ((flip ? size - 1 - y : y) + .5) * (crop_h - dh) / size);
                const auto* row = reinterpret_cast<const unsigned*>(static_cast<const unsigned char*>(mapped.pData) + sy * mapped.RowPitch);
                pixels[y * size * 2 + (swapped ? 1 - c : c) * size + x] = obscure ? 0xff404040 : row[sx];
            }
            context->Unmap(readback.Get(), 0);
        }
        context->UpdateSubresource(target.Get(), 0, nullptr, pixels.data(), size * 8, 0);
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto ticket = eye_calibration_submit(target.Get(), eye, eye * .5F, 0, (eye + 1) * .5F, 1);
            require(ticket != 0, "Cropped DX11 submission must enqueue readback");
            eye_calibration_result(ticket, 0);
        }
        // Age the actual production capture, rather than mocking the retry policy.
        if (delayed_completion) Sleep(1100);
        eye_calibration_frame(); context->Flush();
        const auto deadline = GetTickCount64() + 5000;
        while (eye_calibration_stats().in_flight && GetTickCount64() < deadline) { Sleep(1); eye_calibration_tick(); }
        require(!eye_calibration_stats().in_flight, "Cropped readbacks must drain without blocking production");
        const auto report = collect_calibration_images(support);
        require(report.files.size() == 5, "Search captures must include all production stamp/read evidence");
        require(support->images[0].info.markers.count <= 2 && support->images[1].info.markers.count <= 2,
            "Acquisition must never display the full marker bank");
        return support;
    };
    auto acquire = [&](auto render) {
        const auto before = eye_calibration_stats().valid;
        CalibrationImageRequestPtr support;
        for (unsigned i = 0; i < 40; ++i) {
            support = render();
            if (eye_calibration_stats().valid > before) return support;
            Sleep(100);
        }
        throw std::runtime_error("Sequential corner acquisition did not converge");
    };
    auto first = acquire([&] { return attempt(50, 0, 300, 300, 300); });
    require(eye_calibration_stats().valid == 1 && first->images[0].info.markers.count > 1,
        "Centered 4:3-to-square crop must find an inner marker");
    auto locked = attempt(50, 0, 300, 300, 300, false, false, true);
    require(eye_calibration_stats().valid == 2 && eye_calibration_stats().left_view == 7102 &&
        locked->images[0].info.markers.count == 1 && locked->images[2].info.sample_count == 4,
        "A locked crop must stamp one marker and read only its two identity/orientation probes per eye");
    const auto before = eye_calibration_stats().valid;
    const auto moved = attempt(100, 0, 300, 300, 300);
    require(eye_calibration_stats().valid == before + 1 && moved->images[0].info.markers.count == 1 &&
        eye_calibration_stats().left_view == 7101,
        "A 50px crop displacement must update the transform and eye identity without reopening acquisition");
    auto recovered = acquire([&] { return attempt(100, 0, 300, 300, 300); });
    require(eye_calibration_stats().valid > before && recovered->images[0].info.markers.count <= 2,
        "Search must recover an edge-aligned crop");
    // Changed scale, simultaneous horizontal/vertical crop and shader flip.
    acquire([&] { return attempt(87.5, 37.5, 225, 225, 192, true); });
    auto zoom = attempt(87.5, 37.5, 225, 225, 192, true);
    require(eye_calibration_stats().vertical_flip && zoom->images[0].info.markers.count == 1,
        "Crop-plus-resize and vertical flip must lock to one inner placement");
    const auto visible = eye_calibration_stats().valid;
    attempt(87.5, 37.5, 225, 225, 192, true, true);
    require(eye_calibration_stats().valid == visible, "Missing markers must never authenticate a crop guess");
    auto resumed = attempt(87.5, 37.5, 225, 225, 192, true);
    require(eye_calibration_stats().valid == visible + 1 && resumed->images[0].info.markers.count == 1,
        "A transient miss must recover without reopening acquisition");
    // Neither eye uses one of the predefined crop hypotheses, and their scales
    // differ. Acquisition must fit the actual pixels independently.
    const auto prior_acquisition = eye_calibration_stats().valid;
    acquire([&] { return attempt(34.3, 21.7, 330, 250, 300, false, false, false, true); });
    if (eye_calibration_stats().valid == prior_acquisition) std::cerr << eye_calibration_json() << '\n';
    require(eye_calibration_stats().valid > prior_acquisition && eye_calibration_json().find("\"per_eye\":true") != std::string::npos,
        "A wide search must acquire arbitrary, different crops in the two submitted eyes");
    const auto search_count = [&] {
        const auto json = eye_calibration_json(); const auto at = json.find("\"wide_searches\":");
        return std::stoull(json.substr(at + 16));
    };
    const auto searches = search_count(), acquired_valid = eye_calibration_stats().valid;
    for (unsigned i = 0; i < 3; ++i) {
        auto tracked = attempt(34.3, 21.7, 330, 250, 300, false, false, false, true);
        require(tracked->images[0].info.markers.count == 1 && tracked->images[2].info.sample_count == 4,
            "Acquired geometry must use one edge marker and tiny readbacks");
    }
    require(search_count() == searches && eye_calibration_stats().valid == acquired_valid + 3,
        "Stable acquired geometry must not schedule any further wide searches");
    for (unsigned i = 0; i < 3; ++i) attempt(34.3, 21.7, 330, 250, 300, false, true, false, true);
    require(eye_calibration_json().find("\"locked\":true") == std::string::npos,
        "Three consecutive marker misses must release placement locks");
    const auto candidate = [&] {
        const auto json = eye_calibration_json(); const auto key = std::string("\"search_candidate\":");
        return std::stoul(json.substr(json.find(key) + key.size()));
    };
    const auto failed_candidate = candidate();
    const auto failed_searches = search_count();
    const auto applied_before_retry = eye_calibration_stats().applied;
    Sleep(1100); // Make the next capture eligible for the throttled wide search.
    attempt(34.3, 21.7, 330, 250, 300, false, true, false, true, true);
    require(search_count() == failed_searches + 1 && candidate() == failed_candidate + 1,
        "A failed wide search older than one second must advance to the next corner");
    require(eye_calibration_stats().valid == acquired_valid + 3 &&
        eye_calibration_stats().applied == applied_before_retry &&
        eye_calibration_json().find("\"locked\":true") == std::string::npos,
        "A slow failed search must not publish eye identity or restore a placement lock");
    auto recovered_pair = acquire([&] { return attempt(34.3, 21.7, 330, 250, 300, false, false, false, true, true); });
    require(recovered_pair->images[0].info.markers.count <= 2 && eye_calibration_stats().valid == acquired_valid + 4,
        "Sustained marker loss must reacquire using only a local corner pair");
    require(eye_calibration_stats().applied == applied_before_retry,
        "A slow successful acquisition must not publish stale eye identity");
    const auto handoff_searches = search_count();
    const auto handoff = attempt(34.3, 21.7, 330, 250, 300, false, false, false, true);
    require(eye_calibration_stats().applied == applied_before_retry + 1 && search_count() == handoff_searches &&
        handoff->images[0].info.markers.count == 1,
        "Slow acquisition must hand off to a fresh one-marker mapping without another full search");
    eye_calibration_enable(false); eye_calibration_frame(); eye_calibration_stop();
    unregister_stereo_view(7101); unregister_stereo_view(7102);
    std::cout << "PASS DX11 crop search: centered/edge crop, packed bounds, source origin, resize/flip, lock/loss/recovery\n";
}
void test_support_archive_limits() {
    std::vector<SupportFile> report_files;
    for (unsigned i = 0; i < 194; ++i) report_files.push_back({"entry-" + std::to_string(i) + ".txt", "report"});
    for (unsigned i = 0; i < 6; ++i) report_files.push_back({"report-" + std::to_string(i) + ".txt", "report"});
    const auto report_path = std::filesystem::temp_directory_path() /
        ("Cheeky-support-archive-test-" + std::to_string(GetCurrentProcessId()) + ".zip");
    write_support_zip(report_path, report_files);
    std::ifstream archive(report_path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(archive)), std::istreambuf_iterator<char>());
    archive.close();
    require(contents.size() >= 22, "Support ZIP has an end record");
    const auto end = contents.size() - 22;
    require(contents.compare(end, 4, "PK\x05\x06", 4) == 0 &&
        static_cast<unsigned char>(contents[end + 10]) == 200 && contents[end + 11] == 0,
        "Support ZIP retains all 200 archive entries");
    const auto assert_rejected = [&](const std::vector<SupportFile>& invalid, const char* reason) {
        bool rejected{};
        try { write_support_zip(report_path, invalid); }
        catch (const std::runtime_error& error) { rejected = std::string(error.what()).find(reason) != std::string::npos; }
        require(rejected, "Invalid archive gets a specific validation error");
        require(std::filesystem::file_size(report_path) == contents.size(),
            "Validation failure must not truncate an existing archive");
    };
    assert_rejected(std::vector<SupportFile>(257, {"entry.txt", ""}), "256 files");
    assert_rejected({{"bad/name.txt", ""}}, "filename");
    assert_rejected({{"large.bin", std::string(20U * 1024U * 1024U + 1, 'x')}}, "20 MiB");
    std::filesystem::remove(report_path);
}
void run_calibration(bool hardware = false, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
                     bool obscure = false, bool duplicate_eye = false, bool support_images = false) {
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
    auto support = support_images ? request_calibration_images(true) : CalibrationImageRequestPtr{};
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
    if (support) {
        const auto report = collect_calibration_images(support);
        require(report.files.size() == 5, "DX11 support capture must retain both sources and submitted images even if recognition fails");
        require(report.diagnostics.find("\"original_width\":256") != std::string::npos &&
            report.diagnostics.find("\"submitted_uv_bounds\":[0.5,0,1,1]") != std::string::npos,
            "DX11 support diagnostics must preserve full allocation size and packed eye bounds");
        for (unsigned i = 0; i < 4; ++i) {
            const auto& bitmap = report.files[i].contents;
            require(bitmap.size() < 1000000 && bitmap.starts_with("BM"), "Support image must be a bounded BMP");
            const auto bright = std::count_if(bitmap.begin() + 54, bitmap.end(), [](char value) { return value != 0; });
            require(i >= 2 ? (!obscure || bright == 0) : bright > 0,
                "Capture must show source markers after stamping and actual obscured submitted pixels");
        }
        if (!obscure) {
            require(report.files[2].contents == report.files[3].contents,
                "Packed submissions must preserve the whole shared texture, with eye bounds in metadata");
            for (unsigned y = 0; y < 128; ++y)
                for (unsigned eye = 0; eye < 2; ++eye)
                    require(report.files[2].contents.compare(54 + y * 256 * 3 + eye * 128 * 3, 128 * 3,
                        report.files[1 - eye].contents, 54 + y * 128 * 3, 128 * 3) == 0,
                        "Packed submitted pixels must match the swapped source captured after stamping");
        }
        const auto root = std::filesystem::temp_directory_path() / "Cheeky-stereo-support-tests" /
            std::to_string(GetCurrentProcessId());
        std::filesystem::create_directories(root);
        write_support_zip(root / (obscure ? "dx11-obscured.zip" : "dx11-visible.zip"), report.files);
    }
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
int run_crop_calibration_tests() {
    try { test_wide_search(); test_hogwarts_corner_pair(); test_hogwarts_tracking(); test_cyberpunk_capture_search(); crop_calibration11(); return 0; }
    catch (const std::exception& e) { std::cerr << "Crop calibration: " << e.what() << '\n'; eye_calibration_stop(); return 1; }
}
int run_stereo_support_tests() {
    try {
        // A large noisy input proves the limit without depending on compression.
        auto request = request_calibration_images(true);
        auto claimed = claim_calibration_images(123, 456, {7, 9});
        require(claimed == request && !claim_calibration_images(124, 456, {}), "A request must select one interval only");
        CalibrationImageInfo info; info.width = 2048; info.height = 1024; info.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        std::vector<unsigned char> pixels(std::size_t(info.width) * info.height * 4);
        for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<unsigned char>((i * 73 + (i >> 11)) & 255);
        for (unsigned i = 0; i < 4; ++i) {
            require(begin_calibration_image(request, i, info), "Fresh capture slot available");
            complete_calibration_image(request, i, pixels.data(), info.width * 4);
        }
        const auto report = collect_calibration_images(request);
        require(report.files.size() == 5 && report.diagnostics.find("\"original_width\":2048") != std::string::npos,
            "Full dimensions must survive preview resizing");
        for (unsigned i = 0; i < 4; ++i) require(report.files[i].contents.size() < 1000000, "Noisy image exceeded 1 MB");
        require(!begin_calibration_image(request, 0, info), "Finished reports must reject late GPU results");
        require(collect_calibration_images(request_calibration_images(false)).files.size() == 1, "Disabled calibration exports its reason without images");
        const auto timed_out = request_calibration_images(true, std::chrono::milliseconds(1));
        require(collect_calibration_images(timed_out).diagnostics.find("unavailable") != std::string::npos,
            "Missing frames must time out rather than block ZIP creation");
        require(!reserve_calibration_image_memory(129ULL * 1024 * 1024), "Large readbacks must be bounded");
        {
            std::array<std::shared_ptr<CalibrationImageMemory>, 4> leases;
            for (auto& lease : leases) {
                lease = reserve_calibration_image_memory(128ULL * 1024 * 1024);
                require(bool(lease), "Readback budget must allow four maximum-size images");
            }
            require(!reserve_calibration_image_memory(1), "Concurrent requests must share the total readback limit");
        }
        require(calibration_image_bytes.load() == 0, "Readback reservations must be released");
        run_calibration(false, DXGI_FORMAT_R8G8B8A8_UNORM, false, false, true);
        run_calibration(false, DXGI_FORMAT_R8G8B8A8_UNORM, true, false, true);
        require(calibration_image_bytes.load() == 0, "DX11 capture buffers must drain");
        std::cout << "PASS stereo support: bounded previews, timeout, DX11 stamped/submitted pixels and failed recognition\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "Stereo support: " << e.what() << '\n'; eye_calibration_stop(); return 1; }
}
int run_eye_calibration_tests() {
    try {
        test_wide_search();
        test_hogwarts_corner_pair();
        test_hogwarts_tracking();
        test_cyberpunk_capture_search();
        crop_calibration11();
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
        test_deferred_capture_close();
        test_support_archive_limits();
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


int run_calibration_modes_tests() {
    try {
        require(!Settings{}.eye_calibration_continuous, "Default calibration must be one-shot");
        EyeCalibrationPolicy policy;
        policy.begin(EyeCalibrationMethod::automatic, 100);
        for (unsigned i=0; i<19; ++i) require(!policy.failed(200+i*100, false), "Sparse stage must tolerate nineteen misses");
        require(policy.failed(2200, false) && policy.active == EyeCalibrationMethod::timing, "Twentieth sparse failure must enter timing retry");
        for (unsigned i=0; i<30; ++i) require(!policy.failed(2201+i, false), "Timing retry needs a full observation window");
        require(policy.failed(3200, false) && policy.active == EyeCalibrationMethod::full, "Timing retry must escalate after twenty failures and one second");
        policy.begin(EyeCalibrationMethod::automatic, 100);
        require(!policy.failed(5200, false), "Budget expiry must not escalate a single miss");
        require(policy.failed(5300, false), "Five-second budget must bound usable startup failures");
        policy.begin(EyeCalibrationMethod::automatic, 100);
        for (unsigned i=0; i<50; ++i) require(!policy.failed(10000+i, true), "Motion-inconclusive observations must not escalate");
        for (const auto method : {EyeCalibrationMethod::standard, EyeCalibrationMethod::timing, EyeCalibrationMethod::full}) {
            policy.begin(method, 100);
            for (unsigned i=0; i<30; ++i) require(!policy.failed(10000+i, false), "Manual overrides must never escalate");
            policy.recover(20000); require(policy.active == method, "Recovery must respect manual override");
        }
        // Exercise actual GPU stamping/readback, persistence evidence and one-shot quiescence.
        set_eye_calibration_learning(0, 0, 0);
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context));
        constexpr unsigned width=640, height=480;
        const std::vector<unsigned> background(width*height, 0xff404040);
        D3D11_TEXTURE2D_DESC desc{width,height,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,0,0,0};
        std::array<ComPtr<ID3D11Texture2D>,2> sources, targets;
        for (auto& source : sources) check(device->CreateTexture2D(&desc,nullptr,&source));
        auto run = [&](bool cropped, EyeCalibrationMethod method, bool expect_success, bool expect_wide) {
            eye_calibration_stop(); eye_calibration_reset_stats();
            register_stereo_view(8101); register_stereo_view(8102);
            auto settings = configured_settings(); settings.eye_calibration_method=method;
            settings.eye_calibration_continuous=false; update_settings(settings);
            eye_calibration_enable(true);
            desc.Width=cropped ? 440 : width; desc.Height=cropped ? 360 : height;
            for (auto& target : targets) { target.Reset(); check(device->CreateTexture2D(&desc,nullptr,&target)); }
            auto frame = [&] {
                eye_calibration_frame();
                for (unsigned c=0;c<2;++c) {
                    context->UpdateSubresource(sources[c].Get(),0,nullptr,background.data(),width*4,0);
                    eye_calibration_stamp(context.Get(),sources[c].Get(),8101+c,0,0,width,height);
                    const D3D11_BOX box{cropped ? 100U : 0U,cropped ? 60U : 0U,0,
                        cropped ? 540U : width,cropped ? 420U : height,1};
                    context->CopySubresourceRegion(targets[c].Get(),0,0,0,0,sources[c].Get(),0,&box);
                }
                for (unsigned eye=0;eye<2;++eye) {
                    const auto ticket=eye_calibration_submit(targets[1-eye].Get(),eye,0,0,1,1);
                    if(ticket) eye_calibration_result(ticket,0);
                }
                context->Flush(); Sleep(10); eye_calibration_tick();
            };
            const auto deadline=GetTickCount64()+(expect_success ? 15000 : 3000);
            unsigned frames{}, first_wide_frame{};
            do {
                frame(); ++frames;
                if (!first_wide_frame && eye_calibration_stats().full_calibration_attempts) first_wide_frame=frames;
            } while (GetTickCount64()<deadline && eye_calibration_stats().acquisition_confirmations<1);
            const auto stats=eye_calibration_stats();
            if(expect_success) {
                if (!stats.crop_mapping_active || stats.acquisition_confirmations<1) std::cerr << eye_calibration_json() << '\n';
                require(stats.crop_mapping_active && stats.acquisition_confirmations>=1,"GPU calibration must acquire and confirm");
                require(stereo_eye_assignment(8101).eye_index==1,"GPU calibration must identify swapped eyes");
                for(unsigned i=0;i<25;++i) frame();
                require(eye_calibration_stats().captures==stats.captures,"One-shot mode must stop capture after the first accepted publication");
            } else require(!stats.crop_mapping_active,"Forced corners must not accept an invisible marker");
            require((stats.full_calibration_attempts>0)==expect_wide,"Full-image search must be reserved for the selected route");
            eye_calibration_stop(); unregister_stereo_view(8101); unregister_stereo_view(8102);
            return std::pair{stats, first_wide_frame};
        };
        run(false,EyeCalibrationMethod::automatic,true,false);
        auto learned=configured_settings();
        require(learned.eye_calibration_learned_method==1 && learned.eye_calibration_learned_signature,"Ordinary calibration must learn standard route");
        run(true,EyeCalibrationMethod::standard,false,false);
        const auto discovery=run(true,EyeCalibrationMethod::automatic,true,true);
        require(discovery.second>20,"Unlearned cropped output must get multiple corner attempts");
        learned=configured_settings();
        require(learned.eye_calibration_learned_method==3,"Cropped calibration must learn full route");
        const auto saved=learned.eye_calibration_learned_signature;
        const auto reuse=run(true,EyeCalibrationMethod::automatic,true,true);
        require(reuse.second<=20,"Matching learned crop route must bypass repeated corner discovery");
        require(configured_settings().eye_calibration_learned_signature==saved,"Matching launch geometry must reuse learned route");
        run(false,EyeCalibrationMethod::automatic,true,false);
        run(false,EyeCalibrationMethod::timing,true,false);
        run(false,EyeCalibrationMethod::full,true,true);
        const auto ordinary_signature=configured_settings().eye_calibration_learned_signature;
        set_eye_calibration_learning(2,ordinary_signature,1);
        require(run(false,EyeCalibrationMethod::automatic,true,false).first.active_method==EyeCalibrationMethod::standard,
            "A single timing success must not select the timing route on restart");
        set_eye_calibration_learning(2,ordinary_signature,2);
        require(run(false,EyeCalibrationMethod::automatic,true,false).first.active_method==EyeCalibrationMethod::timing,
            "Two timing successes must select the saved timing route");
        // A stale UI draft cannot overwrite newly learned evidence.
        const auto fresh=configured_settings(); update_settings(learned);
        require(configured_settings().eye_calibration_learned_signature==fresh.eye_calibration_learned_signature,"Editable settings must preserve learned evidence");
        set_eye_calibration_learning(0,0,0);
        std::cout << "PASS calibration modes: thresholds, overrides, GPU corners/crop escalation, learning, geometry changes and one-shot capture\n";
        return 0;
    } catch(const std::exception& e) {
        eye_calibration_stop(); unregister_stereo_view(8101); unregister_stereo_view(8102);
        std::cerr << "Calibration modes: " << e.what() << '\n'; return 1;
    }
}
