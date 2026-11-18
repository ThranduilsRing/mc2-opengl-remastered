#include "doctest.h"
#include "armor_alloc.h"

using namespace mc2::armor;
static constexpr Allocation atlas{9,11,9,9,8,8,9,9,4,2,2};
TEST_CASE("armor allocation preserves legacy arithmetic") {
    for (int n : {0,1,5,11}) {
        auto a = legacyDistribution(atlas, n * 32);
        for (int i = 0; i < 11; ++i) CHECK(a[i] == atlas[i] + static_cast<int>(shares[i] * (n * 32)));
    }
    CHECK(total(atlas) == 80);
    CHECK(total(legacyDistribution(atlas,32)) == 107);
}
TEST_CASE("armor allocation limits shared torsos bytes and pool") {
    const auto cap = maxPerLocation(atlas,80,432);
    auto a = atlas;
    CHECK_FALSE(step(a,0,1,cap,432));
    CHECK_FALSE(step(a,1,1,cap,80));
    a[8] = 0; a[1] = cap[1];
    CHECK(step(a,1,1,cap,432)); // rear capacity transfers to front
    a[1] = 255;
    CHECK_FALSE(step(a,1,1,cap,1000));
    a[1] = cap[1]; a[8] = cap[8];
    CHECK_FALSE(step(a,8,1,cap,1000));
    a[0] = 0;
    CHECK_FALSE(step(a,0,-1,cap,80));
    a.fill(255);
    CHECK(available(a,1,cap,80) == 0);
}
TEST_CASE("armor normalisation is deterministic after removing plating") {
    const auto cap = maxPerLocation(atlas,80,432);
    for (int plating : {0,32,64,352}) {
        auto a = legacyDistribution(atlas,plating);
        const int pool = 80 + std::max(0,plating-32);
        auto b = normalise(a,atlas,cap,pool);
        CHECK(b == normalise(a,atlas,cap,pool));
        CHECK(b == normalise(b,atlas,cap,pool));
        CHECK(total(b) <= pool);
        for (int i=0;i<11;++i) { CHECK(b[i] >= 0); CHECK(b[i] <= 255); CHECK(available(b,i,cap,pool) >= 0); }
        for (int i=1;i<=3;++i) CHECK(b[i]+b[i+7] <= cap[i]+cap[i+7]);
    }
    Allocation saturated; saturated.fill(999);
    auto b = normalise(saturated,atlas,cap,432);
    CHECK(total(b) <= 432);
}
