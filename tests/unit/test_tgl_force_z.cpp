// forceZ (authored forward-Z) -> reverse-Z clip depth, for the vertex path and the ib_/vb_ matrix path.
#include "doctest.h"

#include "tgl_force_z.h"

TEST_CASE("forceZ matrix: post-divide depth is the mirrored forceZ for any input z and w") {
    const float forceZs[] = {0.00001f, 0.25f, 0.99999f};
    const float zs[] = {-3.0f, 0.0f, 7.5f};
    const float ws[] = {0.5f, 1.0f, 20.0f};
    for (float fz : forceZs) {
        const glm::mat4 m = mc2_tgl::forceZMatrix(fz);
        for (float zin : zs) {
            for (float win : ws) {
                const glm::vec4 out = m * glm::vec4(2.0f, 3.0f, zin, win);
                CHECK(out.w == doctest::Approx(win));
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
