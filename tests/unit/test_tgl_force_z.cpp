// forceZ (authored forward-Z) -> reverse-Z clip depth, for the vertex path and the ib_/vb_ matrix path.
#include "doctest.h"

#include "tgl_force_z.h"

TEST_CASE("forceZ matrix: post-divide depth is the mirrored forceZ for any input z and w") {
    for (float fz : {0.00001f, 0.25f, 0.99999f}) {
        const glm::mat4 m = mc2_tgl::forceZMatrix(fz);
        for (float z : {-3.0f, 0.0f, 7.5f}) {
            for (float w : {0.5f, 1.0f, 20.0f}) {
                const glm::vec4 out = m * glm::vec4(2.0f, 3.0f, z, w);
                CHECK(out.w == doctest::Approx(w));
                CHECK(out.x == doctest::Approx(2.0f));
                CHECK(out.z / out.w == doctest::Approx(mc2_tgl::forceZClipDepth(fz)).epsilon(1e-5));
            }
        }
    }
}

TEST_CASE("forceZ: front is near (reverse-Z 1), back is far, out-of-range is inactive") {
    CHECK(mc2_tgl::forceZClipDepth(0.0f) == doctest::Approx(1.0f));
    CHECK(mc2_tgl::forceZClipDepth(1.0f) == doctest::Approx(0.0f));
    CHECK(mc2_tgl::forceZActive(0.5f));
    CHECK_FALSE(mc2_tgl::forceZActive(-1.0f));
    CHECK_FALSE(mc2_tgl::forceZActive(1.0f));
}
