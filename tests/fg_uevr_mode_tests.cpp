#include "../shared/fg_uevr_mode.hpp"
#include <cassert>
int main(){
    using namespace cheeky::fg;
    static_assert(uevr_mode_supports_fg(no_uevr_host));
    static_assert(!uevr_mode_supports_fg(unknown_uevr_mode));
    static_assert(uevr_mode_supports_fg(uevr_mode_value(0)));
    for(unsigned mode=1;mode<=3;++mode){
        assert(!uevr_mode_supports_fg(uevr_mode_value(mode)));
        assert(uevr_mode_pauses_fg(uevr_mode_value(mode)));
    }
    assert(!uevr_mode_supports_fg(uevr_mode_value(0xffffffffu)));
    assert(uevr_mode_supports_fg(uevr_mode_value(0)));
}
