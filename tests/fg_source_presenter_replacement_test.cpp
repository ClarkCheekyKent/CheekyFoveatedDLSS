// Portable policy test. Windows: cl /std:c++20 /EHsc /I. tests\fg_source_presenter_replacement_test.cpp
#include "../openxr_layer/fg_restart_policy.hpp"
#include "../src/fg_source_lifecycle.hpp"
#include <cassert>

using cheeky::fg::replace_presenter_for_configuration;
using cheeky::foveated_dlss::fg_sr_mode_configuration;

int main() {
    constexpr std::uint64_t settings = 0x4567abcd;
    constexpr auto foveation_on  = fg_sr_mode_configuration(settings,true);
    constexpr auto foveation_off = fg_sr_mode_configuration(settings,false);
    static_assert(foveation_on != foveation_off);
    static_assert(replace_presenter_for_configuration(true,foveation_on,foveation_off));
    static_assert(replace_presenter_for_configuration(true,foveation_off,foveation_on));
    static_assert(!replace_presenter_for_configuration(false,foveation_on,foveation_off));
    static_assert(!replace_presenter_for_configuration(true,foveation_on,foveation_on));
    static_assert(!replace_presenter_for_configuration(true,0,foveation_on));
    static_assert(!replace_presenter_for_configuration(true,foveation_on,0));
    assert(replace_presenter_for_configuration(true,foveation_on,foveation_off));
}
