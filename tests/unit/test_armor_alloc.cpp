#include "doctest.h"
#include "armor_alloc.h"

using namespace mc2::armor;
static constexpr Allocation atlas{9,11,9,9,8,8,9,9,4,2,2};
static constexpr std::array<int,8> atlasStructure{3,31,21,21,17,17,21,21};
struct Stock { Allocation base; std::array<int,8> structure; int baseArmor, maxArmor; };
static const Stock stockPool[] = {   // base/structure/max from the stock chassis csv rows 12, 17, 18
    {atlas, atlasStructure, 80, 432},
    {{9,16,14,14,11,11,13,13,5,3,3}, {5,18,13,13,9,9,13,13}, 112, 240},   // Bushwacker
    {{9,11,5,5,10,10,7,7,2,2,2}, {3,10,7,7,5,5,7,7}, 63, 185},   // Razorback-like (arm 10 < stock 11)
};
TEST_CASE("armor capacity: no stock loadout exceeds its displayed MAX") {
    for (const auto& c : stockPool) {
        const auto cap = maxPerLocation(c.structure, c.base, c.maxArmor - c.baseArmor);
        CHECK(cap[0] >= 9);
        for (int plating = 0; plating <= c.maxArmor - c.baseArmor; ++plating) {
            const auto a = legacyDistribution(c.base, plating);
            for (int i = 0; i < 8; ++i) {
                const int p = partner(i);
                CHECK(a[i] + (p >= 0 ? a[p] : 0) <= cap[i]);
                if (i > 0) CHECK(cap[i] >= 2 * c.structure[i]);
            }
        }
    }
    const auto a = maxPerLocation(atlasStructure, atlas, 352);   // Atlas: arm 50, CT 84, LT/RT 62, head 9
    CHECK(a[0] == 9); CHECK(a[4] == 50); CHECK(a[1] == 84); CHECK(a[2] == 62); CHECK(a[8] == 0);
    // stock Atlas LT 37 front + 6 rear and Shootist arms 24 sit under the displayed MAX
    CHECK(37 + 6 <= a[2]);
    // Hunchback-like: structure alone (arm 8 -> 16) never lowers the limit below 2 x structure
    CHECK(maxPerLocation({3,20,15,15,8,8,15,15}, {9,0,0,0,0,0,0,0,0,0,0}, 0)[4] == 16);
    CHECK(maxTotal(a, 200) == 200);
    Allocation x = atlas; x[1] = a[1]; x[8] = 0;
    CHECK(available(x,1,a,1000) == 0);   // front+rear share the torso limit
    x[1] = 40; x[8] = 20; CHECK(available(x,8,a,1000) == 24);
}
TEST_CASE("armor allocation preserves legacy arithmetic") {
    for (int n : {0,1,5,11}) {
        auto a = legacyDistribution(atlas, n * 32);
        for (int i = 0; i < 11; ++i) CHECK(a[i] == atlas[i] + static_cast<int>(shares[i] * (n * 32)));
    }
    CHECK(legacyDistribution(atlas,0) == atlas);
    CHECK(legacyDistribution(atlas,32) == Allocation{9,16,13,13,11,11,13,13,4,2,2});
    CHECK(legacyDistribution(atlas,160) == Allocation{9,38,29,29,27,27,29,29,8,5,5});
    CHECK(legacyDistribution(atlas,352) == Allocation{9,70,53,53,50,50,54,54,14,9,9});
    CHECK(total(atlas) == 80);
    CHECK(total(legacyDistribution(atlas,32)) == 107);
}
TEST_CASE("armor allocation limits shared torsos bytes and pool") {
    const auto cap = maxPerLocation(atlasStructure,atlas,352);
    auto a = atlas;
    CHECK_FALSE(step(a,0,1,cap,432));
    CHECK_FALSE(step(a,1,1,cap,80));
    a[8] = 0; a[1] = cap[1] - 1;
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
    const auto cap = maxPerLocation(atlasStructure,atlas,352);
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

TEST_CASE("armor production baseline adapter preserves reset transitions") {
    struct Armor { int maxArmor = 0, curArmor = 0; } armor[11];
    FactoryBaseline baseline;
    for (int i=0;i<11;++i) armor[i].maxArmor = atlas[i];
    baseline.capture(armor);
    int count = 0;
    for (int points : {0,32,64}) {
        const auto legacy = legacyDistribution(atlas,points);
        for (int i=0;i<11;++i) armor[i].curArmor = armor[i].maxArmor = legacy[i];
        count = points/32;
        baseline.restore(armor,count); // legacy -> legacy: untouched subtraction inputs
        CHECK(count == points/32);
        for (int i=0;i<11;++i) CHECK(armor[i].maxArmor == legacy[i]);
        Allocation explicitValues{}; explicitValues[1] = 40;
        baseline.apply(armor,explicitValues.data()); // legacy -> explicit
        CHECK(baseline.explicitApplied);
        baseline.restore(armor,count); // explicit -> explicit or legacy
        CHECK(count == 0);
        for (int i=0;i<11;++i) CHECK(armor[i].maxArmor == atlas[i]);
        baseline.apply(armor,explicitValues.data());
        baseline.restore(armor,count);
        for (int i=0;i<11;++i) CHECK(armor[i].maxArmor == atlas[i]);
        Allocation zero{}; baseline.apply(armor,zero.data());
        CHECK(armor[1].maxArmor == 0);
        baseline.restore(armor,count);
    }
}