// Portable regression test for the FG/primary SR mode transition key.
// On Windows: cl /std:c++20 /EHsc /I. tests\fg_sr_mode_lifecycle_test.cpp
#include "../src/fg_source_lifecycle.hpp"
#include <cassert>
using namespace cheeky::foveated_dlss;
int main() {
    constexpr auto config=0xA5123456ull;
    constexpr auto on=fg_sr_mode_configuration(config,true);
    constexpr auto off=fg_sr_mode_configuration(config,false);
    static_assert(on!=off, "primary SR foveation toggle must rebuild FG presenter");
    static_assert(on==fg_sr_mode_configuration(config,true), "stable SR mode must not restart FG");
    static_assert(fg_same_source_history(fg_source_configuration(on,2),fg_source_configuration(on,2)));
    static_assert(!fg_same_source_history(fg_source_configuration(on,2),fg_source_configuration(off,2)),
        "must not interpolate guides across primary SR ON/OFF boundary");
    static_assert(!fg_same_source_history(fg_source_configuration(on,2),fg_source_configuration(on,3)),
        "game NGX-SR feature recreation must still reset FG history");
    assert(on != off);
}
