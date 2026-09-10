#include "dlss_nr_input.hpp"
#include "gaze_foveation.hpp"
#include "mock_ngx_parameters.hpp"
#include <iostream>
#include <stdexcept>

namespace cheeky::foveated_dlss {
// Test executable substitutes the NVIDIA evaluator; production copy/orchestration
// still records real D3D12 commands in the WARP suite.
void note_dlss_nr_skipped(DlssNrRoute, const Settings&, const char*) noexcept {}
int nr_test_evaluations{};
bool nr_test_succeeds{true};
DlssNrFrame nr_test_frame{};
bool evaluate_dlss_nr(const DlssNrFrame& frame, const Settings&) noexcept {
    ++nr_test_evaluations;
    nr_test_frame = frame;
    return nr_test_succeeds && frame.color != nullptr;
}
}
namespace {
using namespace cheeky::foveated_dlss;
struct Resource { void* native{}; };
struct Tag { Resource* resource{}; };
struct Tags {
    void* seen{};
    unsigned calls{};
    bool fail{};
    static std::uint32_t submit(void* context, const Tag* tags, std::uint32_t count) {
        auto& state = *static_cast<Tags*>(context);
        state.seen = tags[0].resource->native;
        ++state.calls;
        return state.fail && count == 1U ? 1U : 0U;
    }
};
struct Parameters : NgxParameters {
    ID3D12Resource* color{};
    unsigned reset{};
    void Set(const char*, unsigned long long) override {}
    void Set(const char*, float) override {}
    void Set(const char*, double) override {}
    void Set(const char*, unsigned value) override { reset = value; }
    void Set(const char*, int) override {}
    void Set(const char*, ID3D11Resource*) override {}
    void Set(const char*, ID3D12Resource* value) override { color = value; }
    void Set(const char*, void*) override {}
    NgxResult Get(const char*, unsigned long long*) const override { return 0xBAD00005U; }
    NgxResult Get(const char*, float*) const override { return 0xBAD00005U; }
    NgxResult Get(const char*, double*) const override { return 0xBAD00005U; }
    NgxResult Get(const char*, unsigned* value) const override { *value = reset; return 1U; }
    NgxResult Get(const char*, int*) const override { return 0xBAD00005U; }
    NgxResult Get(const char*, ID3D11Resource**) const override { return 0xBAD00005U; }
    NgxResult Get(const char*, ID3D12Resource** value) const override { *value = color; return 1U; }
    NgxResult Get(const char*, void**) const override { return 0xBAD00005U; }
    void Reset() override {}
};
}
int run_nr_processing_tests() {
    using namespace cheeky::foveated_dlss;
    try {
        const auto require = [](bool value, const char* why) {
            if (!value) throw std::runtime_error(why);
        };
        {
            MockNgxParameters missing;
            { NgxNrInputSubstitution unchanged(&missing, nullptr, nullptr, true); }
            require(missing.values.empty(), "NR fallback invented missing host parameters");
        }
        {
            CropGeometry cached{};
            cached.input_width = 64U;
            cached.input_height = 64U;
            const FoveationCenter center{0.8F, 0.3F};
            ScopedCoordinatedCrop scope{987U, cached, true, &center};
            CropGeometry actual{};
            FoveationCenter actual_center{};
            bool reset{};
            require(calculate_coordinated_crop(Settings{}, 987U, nullptr,
                128U, 128U, 256U, 256U, 0U, 0U, actual, reset, nullptr, &actual_center),
                "Before NR cached crop rejected");
            require(reset && actual.input_width == 64U && actual_center.u == center.u &&
                actual_center.v == center.v, "Before NR cached crop lost upstream resolved center");
        }
        // Fake SR checks the production substitution through normal, fallback,
        // and exception exits. Pointer tokens are never dereferenced.
        Parameters parameters;
        auto* original = reinterpret_cast<ID3D12Resource*>(std::uintptr_t{1U});
        auto* processed = reinterpret_cast<ID3D12Resource*>(std::uintptr_t{2U});
        parameters.color = original;
        parameters.reset = 7U;
        for (int exit = 0; exit < 3; ++exit) {
            try {
                const auto sr = [&]() {
                    NgxNrInputSubstitution scope{&parameters, original, processed, true};
                    require(parameters.color == processed && parameters.reset == 1U,
                        "SR did not receive processed color and reset");
                    if (exit == 1) return;
                    if (exit == 2) throw 1;
                };
                sr();
            } catch (int) {}
            require(parameters.color == original && parameters.reset == 7U,
                "Parameter substitution leaked through an evaluation exit");
        }
        {
            NgxNrInputSubstitution fallback{&parameters, original, nullptr, false};
            require(parameters.color == original && parameters.reset == 7U, "Fallback changed original inputs");
        }
        for (bool failure : {false, true}) {
            std::array<Resource, 4U> resources{};
            std::array<Tag, 4U> tags{};
            resources[0].native = original;
            Tags submitted{original, 0U, failure};
            {
                NrTagSubstitution<Resource, Tag> scope{resources, tags, &Tags::submit, &submitted};
                require(scope.apply(processed) == !failure, "Tag failure did not select original input");
                require(submitted.seen == (failure ? original : processed), "Wrong color tag during evaluation");
                require(resources[0].native == original, "Host tag cache was mutated");
            }
            require(submitted.seen == original && submitted.calls == 2U,
                "Original tags not restored exactly once");
        }
        Settings settings;
        require(settings.nr_processing_order == NrProcessingOrder::after_upscaling,
            "Default order changed");
        require(nr_processing_order(2U) == NrProcessingOrder::after_upscaling &&
            nr_processing_order(~0U) == NrProcessingOrder::after_upscaling, "Invalid order accepted");
        settings.nr_working_scale = 0.37F;
        settings.nr_processing_order = NrProcessingOrder::before_upscaling;
        update_settings(settings);
        require(current_settings().nr_working_scale == 0.37F, "Order update changed saved scale");
        for (auto order : {NrProcessingOrder::before_upscaling, NrProcessingOrder::after_upscaling}) {
            const auto resolution = dlss_nr_processing_resolution(order, 1600U, 1200U, 3200U, 2400U);
            require(resolution.width == (order == NrProcessingOrder::before_upscaling ? 1600U : 3200U),
                "Incorrect processing resolution");
            settings.nr_foveated = false;
            for (float scale : {0.1F, 0.8F, 1.0F}) {
                settings.nr_working_scale = scale;
                DlssNrGeometry geometry;
                require(calculate_dlss_nr_geometry(settings, resolution.width, resolution.height, geometry),
                    "Valid dimensions rejected");
                require(geometry.working_width == static_cast<unsigned>(resolution.width * scale) &&
                    geometry.working_height == static_cast<unsigned>(resolution.height * scale),
                    "Working scale uses wrong resolution");
            }
        }
        const auto depth = scale_subrect(400U, 800U, 19U, 1600U, 1600U);
        const auto low_mv = scale_subrect(400U, 800U, 31U, 1600U, 1600U);
        const auto output_mv = scale_subrect(400U, 800U, 37U, 3200U, 1600U);
        require(depth.base == 419U && depth.extent == 800U && low_mv.base == 431U &&
            output_mv.base == 837U && output_mv.extent == 1600U,
            "Independent guide origins or output-resolution MV mapping lost");
        require(scale_subrect(0U, 1U, 0U, 0U, 0U).extent == 0U, "Empty guide mapping accepted");
        settings.nr_foveated = true;
        settings.nr_width = 0.5F; settings.nr_height = 0.5F;
        CropGeometry shared{};
        shared.input_base_x = 800U; shared.input_base_y = 0U;
        shared.input_width = 800U; shared.input_height = 600U;
        const auto region = calculate_region(settings, 1600U, 1200U, &shared, 1600U, 1200U);
        require(region.base_x == 800U && region.base_y == 0U && region.width == 800U,
            "NR lost coordinated crop center");
        require(dlss_nr_input_history_reset(100U, NrProcessingOrder::before_upscaling, true, 1600, 1200),
            "First processed input did not reset history");
        require(!dlss_nr_input_history_reset(100U, NrProcessingOrder::before_upscaling, true, 1600, 1200),
            "Stable input resets history");
        require(dlss_nr_input_history_reset(100U, NrProcessingOrder::before_upscaling, false, 1600, 1200),
            "Failure fallback did not reset history");
        require(!dlss_nr_input_history_compatible(100U, NrProcessingOrder::before_upscaling, true, 1600, 1200),
            "Fallback history reused for NR");
        require(dlss_nr_input_history_reset(101U, NrProcessingOrder::before_upscaling, true, 1600, 1200),
            "Second eye inherited first eye history");
        require(dlss_nr_input_history_compatible(100U, NrProcessingOrder::before_upscaling, false, 1600, 1200),
            "Second eye changed first eye history");
        require(!dlss_nr_input_history_reset(100U, NrProcessingOrder::after_upscaling, false, 1600, 1200),
            "Order switch after raw fallback reset SR history");
        require(dlss_nr_input_history_reset(101U, NrProcessingOrder::before_upscaling, true, 1500, 1200),
            "Dynamic resolution did not reset SR history");
        require(dlss_nr_input_history_reset(101U, NrProcessingOrder::after_upscaling, false, 1500, 1200),
            "Processed Before to After did not reset SR history");
        require(!dlss_nr_input_history_reset(101U, NrProcessingOrder::after_upscaling, false, 1500, 1200),
            "Processed Before to After reset more than once");
        for (const auto order : {NrProcessingOrder::after_upscaling, NrProcessingOrder::before_upscaling}) {
            require(dlss_nr_input_history_compatible(102U, order, false, 100, 100),
                "Unrecorded original input needs no additional reset");
        }
        for (unsigned i = 0; i < 4; ++i) {
            const auto order = i % 2 ? NrProcessingOrder::before_upscaling : NrProcessingOrder::after_upscaling;
            require(!dlss_nr_input_history_reset(102U, order, false, 100 + i, 100 + i),
                "Original input reset on first frame, order or dimension change");
        }
        require(dlss_nr_input_history_reset(102U, NrProcessingOrder::before_upscaling, true, 103, 103),
            "Original to processed Before input did not reset");
        require(!dlss_nr_input_history_reset(101U, NrProcessingOrder::before_upscaling, false, 200, 200),
            "Another processed view contaminated original input history");
        require(!dlss_nr_input_history_reset(102U, NrProcessingOrder::before_upscaling, true, 103, 103),
            "Another raw view contaminated processed input history");
        require(dlss_nr_input_history_reset(102U, NrProcessingOrder::before_upscaling, false, 103, 103),
            "Disabling NR or failed substitution did not reset processed input");
        require(!dlss_nr_input_history_reset(102U, NrProcessingOrder::before_upscaling, false, 104, 104),
            "Repeated disabled/failed frame reset original input");
        release_dlss_nr_inputs();
        settings = Settings{};
        update_settings(settings);
        const int calls = nr_test_evaluations;
        require(prepare_dlss_nr_input({}, settings) == nullptr && nr_test_evaluations == calls,
            "Disabled NR evaluated");
        settings.nr_enabled = true;
        require(prepare_dlss_nr_input({}, settings) == nullptr && nr_test_evaluations == calls,
            "After mode executed before SR");
        settings.nr_processing_order = NrProcessingOrder::before_upscaling;
        require(prepare_dlss_nr_input({}, settings) == nullptr && nr_test_evaluations == calls + 1,
            "Invalid before frame did not report exactly one failed attempt");
        std::cout << "NR processing contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NR contract: " << error.what() << '\n';
        return 1;
    }
}
