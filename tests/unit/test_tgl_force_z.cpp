// forceZ (authored forward-Z) -> reverse-Z clip depth, for the vertex path and the ib_/vb_ matrix path.
#include "doctest.h"

#include "tgl_force_z.h"

TEST_CASE("forceZ: front is near (reverse-Z 1), back is far, out-of-range is inactive") {
    CHECK(mc2_tgl::forceZClipDepth(0.0f) == doctest::Approx(1.0f));
    CHECK(mc2_tgl::forceZClipDepth(1.0f) == doctest::Approx(0.0f));
    CHECK(mc2_tgl::forceZActive(0.5f));
    CHECK_FALSE(mc2_tgl::forceZActive(-1.0f));
    CHECK_FALSE(mc2_tgl::forceZActive(1.0f));
}
