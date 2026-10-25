#include "doctest.h"
#include "searchlight_aim.h"
#include "math/mc2_xform.h"

TEST_CASE("searchlight/rate cap, wrap and operator lag")
{
    using mc2::searchlight::approachAngle;
    CHECK(approachAngle(0, 150, 18, 0.1f) == doctest::Approx(1.8f));
    CHECK(approachAngle(179, -179, 18, 0.1f) > 179);
    CHECK(approachAngle(-179, 179, 18, 0.1f) < -179);
    CHECK(approachAngle(0, 5, 18, 0.1f) < 1.0f);
    CHECK(approachAngle(0, 150, 18, 0) == 0);
    float a = 0, b = 0;
    for (int i = 0; i < 30; ++i) a = approachAngle(a, 10, 18, 1.0f / 30);
    for (int i = 0; i < 120; ++i) b = approachAngle(b, 10, 18, 1.0f / 120);
    CHECK(a == doctest::Approx(b).epsilon(0.0001));
}

TEST_CASE("searchlight/yoke yaw and drum pitch follow the lens axis")
{
    const auto pitch = mc2::math::rotationMatrix(mc2::math::Euler(0.5f, 0, 0));
    const auto yaw = mc2::math::rotationMatrix(mc2::math::Euler(0, 1.0f, 0));
    const auto drum = mc2::math::transformPoint(glm::vec3(0, 0, 1), pitch * yaw);
    const auto yoke = mc2::math::transformPoint(glm::vec3(0, 1, 0), yaw);
    CHECK(drum.y < 0); // positive pitch points the lamp down
    CHECK(drum.x > 0); // positive yaw turns toward negative MC2 X
    CHECK(yoke.y == doctest::Approx(1)); // fork remains upright
}
