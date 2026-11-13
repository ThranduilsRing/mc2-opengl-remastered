#include "doctest.h"
#include "../../code/soft_splash.h"
#include <vector>

// Trigger paths: code/soft_splash.h, code/weaponbolt*.cpp.
TEST_CASE("soft splash/eligibility falloff and deterministic candidates")
{
    using namespace soft_splash;
    CHECK(eligible(true, false, 0, 0));
    CHECK_FALSE(eligible(false, false, 0, 0)); // Energy and ballistic.
    CHECK_FALSE(eligible(true, true, 0, 0));
    CHECK_FALSE(eligible(true, false, 3, 120));
    CHECK(damage(4, 0) == 2);
    CHECK(damage(4, 32 * 32) == 1);
    CHECK(damage(4, 64 * 64) == 0);
    CHECK(damage(4, 65 * 65) == 0);
    CHECK(damage(4, 56 * 56) == 0.25f);
    CHECK(damage(4, 57 * 57) == 0);
    CHECK_FALSE(candidate(false, false, false, false));
    CHECK_FALSE(candidate(true, true, false, false));
    CHECK_FALSE(candidate(true, false, true, false));
    CHECK_FALSE(candidate(true, false, false, true));
    std::vector<int> selected;
    for (int i = 0; i < 5; ++i)
        if (candidate(true, false, i == 1, i == 3)) selected.push_back(i);
    CHECK(selected == std::vector<int>{0, 2, 4});
}
