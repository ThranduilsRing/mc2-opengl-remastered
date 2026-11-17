#include "doctest.h"
#include "battle_value.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>

namespace {
mc2bv::Profile profile() {
    std::ifstream file(MC2_REPO_ROOT + "/data/defs/rules/battle_value.fit");
    const auto p = mc2bv::readProfile(file);
    REQUIRE(p.has_value());
    return *p;
}
struct Fixture {
    const char* name;
    mc2bv::Layout layout;
    mc2bv::Variant variant;
    mc2bv::Components components;
    int expected;
};
std::vector<Fixture> fixtures() {
    // Stock misc.fst Prime loadouts: lab total armor (base + Bulk), structure,
    // speed class, sink budget minus jump heat; compbas BR/heat/ammo shots.
    // Marauder uses the in-repo bt2018 recipe and balance profile v1.
    using W = mc2bv::Weapon;
    const auto energy = [](double br, double heat) { return W{br, heat, std::nullopt}; };
    std::vector<Fixture> f{
        {"Flea / Fire Ant Prime (light)", {80, 51}, {27, 0, 22}, {}, 809},
        {"Bushwacker Prime (medium)", {144, 91}, {23, 0, 27}, {}, 2279},
        {"Hunchback Prime", {144, 83}, {19, 1, 17}, {}, 1355},
        {"Mad Cat Prime (heavy, Clan)", {226, 114}, {23, 0, 61}, {}, 5536},
        {"Atlas Prime (assault)", {304, 152}, {15, 0, 43}, {}, 3763},
        {"bt2018 Marauder MAD-3R", {110, 114}, {19, 0, 64}, {}, 1953}
    };
    auto add = [&](int index, W w, int count) {
        for (int i = 0; i < count; ++i) f[index].components.weapons.push_back(w);
    };
    add(0, energy(104.72, 7), 3); add(0, energy(44.77, 0), 3);
    add(1, {419.13, 2, 39}, 1); add(1, energy(441.04, 10), 1);
    add(1, {174.46, 2, 24}, 3); add(1, energy(44.77, 0), 3);
    add(2, energy(91.82, 3), 2); add(2, {545.11, 7, 38}, 1);
    add(3, energy(952.79, 15), 2); add(3, energy(222.04, 5), 3);
    add(3, {348.92, 4, 47}, 4); add(3, energy(44.77, 0), 4);
    add(4, energy(174.46, 5), 4); add(4, {174.46, 2, 24}, 4);
    add(4, {103.76, 2, 68}, 4); add(4, {545.11, 7, 38}, 1);
    add(4, energy(44.77, 0), 3);
    add(5, energy(441.04, 10), 2); add(5, energy(91.82, 3), 2);
    add(5, {266.32, 1, 75}, 1);
    return f;
}
}

TEST_CASE("battle_value/golden stock classes, Clan and bt2018") {
    const auto p = profile();
    const auto f = fixtures();
    for (const auto& v : f) {
        INFO(v.name);
        CHECK(mc2bv::compute(v.layout, v.variant, v.components, p) == v.expected);
    }
    CHECK(f[4].expected > f[2].expected);
    CHECK(f[2].expected > f[0].expected);
}

TEST_CASE("battle_value/armor and usable weapons monotonic even when overheated") {
    const auto p = profile();
    for (const auto& v : fixtures()) {
        const auto base = mc2bv::compute(v.layout, v.variant, v.components, p);
        CHECK(mc2bv::compute(v.layout, v.variant, {}, p) > 0);
        CHECK(mc2bv::compute(v.layout, v.variant, {}, p) < base);
        for (int amount = 1; amount <= 100; ++amount) {
            auto l = v.layout; l.armor += amount;
            CHECK(mc2bv::compute(l, v.variant, v.components, p) >= base);
            auto c = v.components;
            c.weapons.push_back({double(amount), double(amount * 7), std::nullopt});
            CHECK(mc2bv::compute(v.layout, v.variant, c, p) >= base);
            std::reverse(c.weapons.begin(), c.weapons.end());
            CHECK(mc2bv::compute(v.layout, v.variant, c, p) >= base);
        }
    }
}

TEST_CASE("battle_value/heat discount, ammo usability and per weapon cap") {
    const auto p = profile();
    const mc2bv::Layout l{100, 50};
    const mc2bv::Components c{{{100, 10, std::nullopt}}, 0};
    CHECK(mc2bv::compute(l, {19, 0, 0}, c, p) < mc2bv::compute(l, {19, 0, 10}, c, p));
    CHECK(mc2bv::compute(l, {}, {{{100, 0, 0}}, 0}, p) == mc2bv::compute(l, {}, {}, p));
    CHECK(mc2bv::compute(l, {}, {{{100, 0, 50}}, 0}, p) ==
          mc2bv::compute(l, {}, {{{100, 0, 5000}}, 0}, p));
    CHECK(mc2bv::compute(l, {}, {{{100, 0, 1}}, 0}, p) <
          mc2bv::compute(l, {}, {{{100, 0, 50}}, 0}, p));
    CHECK(mc2bv::compute(l, {}, {{{100, 0, 5000}, {100, 0, 0}}, 0}, p) ==
          mc2bv::compute(l, {}, {{{100, 0, 50}}, 0}, p));
}

TEST_CASE("battle_value/profile and invalid input validation") {
    const auto p = profile();
    CHECK(p.version == 1);
    for (const char* text : {"", "l BvProfile = 2", "l BvProfile = 1\nl BvProfile = 1", "f Armor = nan"}) {
        std::istringstream in(text);
        CHECK_FALSE(mc2bv::readProfile(in).has_value());
    }
    CHECK_THROWS_AS(mc2bv::compute({-1, 0}, {}, {}, p), std::invalid_argument);
    CHECK_THROWS_AS(mc2bv::compute({}, {}, {{{1, 0, -1}}, 0}, p), std::invalid_argument);
    CHECK_THROWS_AS(mc2bv::compute({std::numeric_limits<double>::infinity(), 0}, {}, {}, p), std::invalid_argument);
    CHECK(mc2bv::compute({1e300, 0}, {}, {}, p) == std::numeric_limits<int>::max());
}
