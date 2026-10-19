#include "../src/fg_sr_region.hpp"
#include "../shared/fg_border.hpp"
#include <cassert>
#include <cmath>
static bool near(float a,float b,float eps=.0005f){return std::fabs(a-b)<eps;}
int main(){
 using namespace cheeky::foveated_dlss;
 // Output is 3024x2836; SR used a pixel-rounded 1664x1276 crop.
 FoveationGeometry left{};
 left.output_base_x=1040;left.output_base_y=514;
 left.output_width=1664;left.output_height=1276;
 fg_sr_begin_evaluation(100);
 fg_sr_prepared(100,left,3024,2836,0,0);
 auto a=fg_sr_prepared_for(100,3024,2836);
 assert(a.valid&&near(a.region_width,1664.f/3024.f));
 assert(near(a.center_u,(1040.f+832.f)/3024.f));
 assert(!fg_sr_prepared_for(200,3024,2836).valid);
 assert(!fg_sr_prepared_for(100,2000,2836).valid);
 fg_sr_begin_evaluation(100);
 assert(!fg_sr_prepared_for(100,3024,2836).valid);
 // Manual FG alignment inherits the view's signed SR stereo offset.
 assert(near(fg_fallback_center(.55f,.587f),.632075f));
 assert(near(fg_fallback_center(.55f,-.587f),.367925f));
 // Independent dimensions retain the same binocular alignment.
 auto b=cheeky::fg::fg_border_rect(3024,2836,.65f,.50f,a.center_u-.5f,0,true);
 assert(b.visible);
 assert(b.x>0.18f);
 FoveationGeometry invalid=left;
 invalid.output_base_x=2000;
 assert(!fg_sr_placement(invalid,3024,2836,0,0,100).valid);
}
