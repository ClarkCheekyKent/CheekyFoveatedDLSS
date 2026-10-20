#include "../shared/fg_capture_pair_token.hpp"
#include <cassert>
#include <cstdint>

int main() {
    using cheeky::fg::pending_stereo_crop_token;
    assert(pending_stereo_crop_token(0,0,true)==0);
    assert(pending_stereo_crop_token(1,0,true)==2); // First eye predicts the completed pair.
    assert(pending_stereo_crop_token(2,0,true)==2); // Both eyes agree on the same token.
    assert(pending_stereo_crop_token(3,2,true)==4);
    assert(pending_stereo_crop_token(4,2,true)==4);
    assert(pending_stereo_crop_token(1,0,false)==0); // No prediction without hookless interval.
    assert(pending_stereo_crop_token(2,0,false)==2);
    assert(pending_stereo_crop_token(3,0,true)==0); // Extra eye: ambiguous, do not pack.
    assert(pending_stereo_crop_token(2,2,true)==0); // Already consumed.
    assert(pending_stereo_crop_token(1,2,true)==0); // Impossible order.
}
