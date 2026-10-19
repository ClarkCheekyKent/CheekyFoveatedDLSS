#include "../shared/fg_border.hpp"
#include <cassert>
#include <cmath>
static bool near(float a,float b){return std::fabs(a-b)<.0001f;}
int main(){
    using cheeky::fg::fg_border_rect;
    auto a=fg_border_rect(2000,1000,.55f,.45f,0,0,true);
    assert(a.visible&&near(a.x,.225f)&&near(a.y,.275f)&&near(a.w,.55f)&&near(a.h,.45f));
    auto b=fg_border_rect(2000,1000,.55f,.45f,1,-1,true);
    assert(b.visible&&near(b.x,.45f)&&near(b.y,0));
    assert(!fg_border_rect(2000,1000,1,1,0,0,true).visible);
    assert(!fg_border_rect(2000,1000,.55f,.45f,0,0,false).visible);
    assert(!fg_border_rect(0,1000,.55f,.45f,0,0,true).visible);
    assert(!fg_border_rect(2000,1000,NAN,.45f,0,0,true).visible);
}
