#include "motion_region.hpp"
#include "ngx_abi.hpp"
#include <iostream>
#include <cstdlib>
using namespace cheeky::foveated_dlss;
struct Parameters : NgxParameters {
    ID3D12Resource* color{};
    unsigned reset{}; bool signed_read{}, unsigned_read{};
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
    NgxResult Get(const char*, unsigned* value) const override { *value = reset; return unsigned_read ? 1U : 0xBAD00005U; }
    NgxResult Get(const char*, int* value) const override { *value = static_cast<int>(reset); return signed_read ? 1U : 0xBAD00005U; }
    NgxResult Get(const char*, ID3D11Resource**) const override { return 0xBAD00005U; }
    NgxResult Get(const char*, ID3D12Resource** value) const override { *value = color; return 1U; }
    NgxResult Get(const char*, void**) const override { return 0xBAD00005U; }
    void Reset() override {}
};
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    Parameters p;
    std::uint32_t flags = 99;
    check(!try_get_ngx_integer_bits(nullptr, "flags", flags), "null flags");
    check(!try_get_ngx_integer_bits(&p, "flags", flags), "missing flags");
    for (bool signed_read : {false, true}) {
        p.signed_read = signed_read; p.unsigned_read = !signed_read;
        for (auto value : {0U, 2U, 0x80000002U}) {
            p.reset = value;
            check(try_get_ngx_integer_bits(&p, "flags", flags) && flags == value, "integer bits/zero lost");
        }
    }
    FoveationGeometry crop{100,304,1800,486,150,456,2700,729};
    auto resolve = [&](unsigned f, bool present = true, unsigned mx = 0, unsigned my = 0,
                       unsigned ox = 0, unsigned oy = 0, bool declared = true,
                       std::uint64_t tw = 4936, std::uint64_t th = 1189) {
        return resolve_motion_region(declared,f,present,tw,th,mx,my,crop,2468,1189,3702,1784,ox,oy);
    };
    for (unsigned y : {304U,312U}) {
        crop.input_base_y = y; crop.output_base_y = y * 3 / 2;
        auto low = resolve(2);
        check(low.valid() && low.space == DeclaredMotionSpace::input && low.rectangle.y == y,
            "4936x1189 gaze movement changed declared low MV mapping");
        auto high = resolve(0);
        check(high.valid() == (y == 304U) && high.space == DeclaredMotionSpace::output, "high declaration validation/reinterpretation");
    }
    crop = {0,0,100,100,0,0,100,100};
    check(resolve(0).valid() && resolve(2).valid(), "both interpretations fit");
    auto equal = resolve_motion_region(true,0,true,100,100,0,0,crop,100,100,100,100,0,0);
    check(equal.valid() && equal.space == DeclaredMotionSpace::output, "equal dimensions changed declaration");
    crop = {20,30,100,100,2040,80,150,150};
    auto packed = resolve(2,true,2468,10,2000,20);
    check(packed.valid() && packed.rectangle.x == 2488 && packed.rectangle.y == 40, "packed input base added twice");
    packed = resolve(0,true,2468,10,2000,20);
    check(packed.valid() && packed.rectangle.x == 2508 && packed.rectangle.y == 70, "packed output relative base");
    check(resolve(0,true,0,0,2041,20).status == MotionRegionStatus::output_underflow, "output underflow");
    check(resolve(2,true,0xffffffff).status == MotionRegionStatus::coordinate_overflow, "base overflow");
    check(resolve(2,true,0,0,0,0,true,0x100000000ULL).status == MotionRegionStatus::invalid_dimensions, "texture truncation");
    check(resolve(2,false).status == MotionRegionStatus::missing_resource, "missing resource");
    check(resolve(2,true,0,0,0,0,false).status == MotionRegionStatus::missing_declaration, "missing declaration");
    check(resolve(2,true,0,0,0,0,true,120,130).valid(), "boundary fit");
    check(resolve(2,true,0,0,0,0,true,119,130).status == MotionRegionStatus::outside_texture, "out of bounds");
    crop = {0,0,1,1,0,0,1,1};
    check(resolve(2,true,0xffffffff,0,0,0,true,0xffffffff,1).status == MotionRegionStatus::coordinate_overflow,
        "rectangle endpoint overflow");
    check(resolve(2,true,0xfffffffe,0,0,0,true,0xffffffff,1).valid(), "maximum boundary fit");
    crop = {20,30,100,100,2040,80,150,150};
    crop.input_base_x = 2468;
    check(resolve(2).status == MotionRegionStatus::outside_view, "outside view fits packed texture");
    crop.input_base_x = 0; crop.input_width = 0;
    check(resolve(2).status == MotionRegionStatus::invalid_dimensions, "zero dimensions");
    std::cout << "Motion region regressions passed\n";
}
