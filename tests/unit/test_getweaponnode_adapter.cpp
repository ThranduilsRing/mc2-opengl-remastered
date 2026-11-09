// tests/unit/test_getweaponnode_adapter.cpp
//
// Pure-logic doctest coverage for chooseWeaponNode() in hardpoint.h: the
// authoritative-with-fallback weapon-node selection rule. This proves the ROUTE
// (declared hardpoint socket wins over legacy round-robin) WITHOUT a full data
// unit -- the in-mission visual proof of a declared hardpoint mounting to its
// socket is covered by the non-mech data-unit proof.
//
// The whole point is the parity guarantee: stock weapons carry hardpointId == 0
// so hasHardpoint is false so the round-robin node is ALWAYS returned, i.e. the
// legacy mount is byte-identical. Header-only, drags no engine/GL/IO.
#include "doctest.h"
#include "hardpoint.h"

// -------------------------------------------------------------------------
// Fallback (parity) cases -- the stock-weapon path.
// -------------------------------------------------------------------------

TEST_CASE("no hardpoint declared => always the round-robin node (stock parity)") {
    // hardpointId == 0 -> hasHardpoint false. The socket-node argument is
    // irrelevant; the round-robin node is returned verbatim.
    CHECK(chooseWeaponNode(false, /*socketNode*/ 7, /*roundRobin*/ 3) == 3);
    CHECK(chooseWeaponNode(false, /*socketNode*/ -1, /*roundRobin*/ 5) == 5);
    // Even a valid-looking socket node is ignored when no join is present.
    CHECK(chooseWeaponNode(false, /*socketNode*/ 0, /*roundRobin*/ 9) == 9);
}

TEST_CASE("round-robin node passed through unchanged, including its -1 sentinel") {
    // getWeaponNode() itself returns -1 when the mech has no matching node.
    // The adapter must not paper over that on the fallback path.
    CHECK(chooseWeaponNode(false, /*socketNode*/ 4, /*roundRobin*/ -1) == -1);
}

// -------------------------------------------------------------------------
// Authoritative cases -- a declared, resolvable hardpoint wins.
// -------------------------------------------------------------------------

TEST_CASE("declared hardpoint with a resolvable socket => the socket node wins") {
    // hasHardpoint true AND socket resolved to a real node index (>= 0).
    CHECK(chooseWeaponNode(true, /*socketNode*/ 7, /*roundRobin*/ 3) == 7);
    CHECK(chooseWeaponNode(true, /*socketNode*/ 0, /*roundRobin*/ 9) == 0);
}

TEST_CASE("declared hardpoint but socket missing on model => fall back to round-robin") {
    // socketNode == -1 means the profile named a socket that does NOT resolve to
    // a node on this shape. The join is present but unusable -> legacy fallback,
    // so a bad/mismatched socket can never break the mount.
    CHECK(chooseWeaponNode(true, /*socketNode*/ -1, /*roundRobin*/ 3) == 3);
    CHECK(chooseWeaponNode(true, /*socketNode*/ -1, /*roundRobin*/ -1) == -1);
}

TEST_CASE("selection is a total function over the two integer inputs") {
    // Exhaustive small grid: authoritative iff hasHardpoint && socketNode >= 0.
    for (int hp = 0; hp < 2; ++hp) {
        for (int sock = -1; sock < 3; ++sock) {
            for (int rr = -1; rr < 3; ++rr) {
                const int got = chooseWeaponNode(hp != 0, sock, rr);
                const int expected = (hp != 0 && sock >= 0) ? sock : rr;
                CHECK(got == expected);
            }
        }
    }
}

// -------------------------------------------------------------------------
// FX-LOOP-AIM: the forced-VFX fixture aims every weapon at the middle of its range class, and every stock mech weapon
// resolves to a node through the getWeaponNode round-robin (the Zeus right arm is LRM-only, so a 30 m aim never showed it).
// -------------------------------------------------------------------------
#include "fx_fixture_aim.h"
#include "mclib/arm_node_remap.h"
#include "stock_data_scan.h"

namespace {
// gamesys.fit [WeaponRanges]: Short, Medium, Long.
constexpr float kRanges[3][2] = {{0.0f, 60.0f}, {30.0f, 120.0f}, {60.0f, 180.0f}};
// Mover::weaponInRange buffer (MapCellDiagonal) on the stock maps.
constexpr float kBuffer = 12.1f;
}

TEST_CASE("fixture aim: the middle of each range class is inside its weapon window; the old 30 m aim refuses long range") {
    CHECK(mc2_fx_aim::fixtureAimDistance(kRanges, 3, 0) == doctest::Approx(30.0f));
    CHECK(mc2_fx_aim::fixtureAimDistance(kRanges, 3, 1) == doctest::Approx(75.0f));
    CHECK(mc2_fx_aim::fixtureAimDistance(kRanges, 3, 2) == doctest::Approx(120.0f));
    for (long c = 0; c < 3; ++c)
        CHECK(mc2_fx_aim::weaponRangeAccepts(kRanges[c][0], kRanges[c][1], mc2_fx_aim::fixtureAimDistance(kRanges, 3, c), kBuffer));
    // The old aim was pos.x + 150 world units = 30 m: medium accepts it (30 - 12.1), long (60 - 12.1 = 47.9) refuses it.
    CHECK(mc2_fx_aim::weaponRangeAccepts(kRanges[1][0], kRanges[1][1], 150.0f / 5.0f, kBuffer));
    CHECK_FALSE(mc2_fx_aim::weaponRangeAccepts(kRanges[2][0], kRanges[2][1], 150.0f / 5.0f, kBuffer));
    CHECK(mc2_fx_aim::fixtureAimDistance(kRanges, 3, 99) == doctest::Approx(30.0f));   // unknown class -> class 0
}

TEST_CASE("round-robin: Zeus nodes (right arm LRM-only) give every Zeus weapon a node") {
    using namespace mc2_arm_remap;
    // zeus.ini [WeaponNode0..4] types: rightarm 1, leftarm 5, righttorso 5, centertorso 5, lefttorso 1.
    std::vector<Node> nodes = {{kMissile, true, false, 0}, {kDirect, false, true, 0}, {kDirect, false, false, 0},
                               {kDirect, false, false, 0}, {kMissile, false, false, 0}};
    // 2x PPC (energy), then 3x LRM5 (missile), in inventory order.
    const long weapons[] = {kEnergy, kEnergy, kMissile, kMissile, kMissile};
    const long expected[] = {1, 2, 0, 4, 0};   // Direct nodes take the PPCs; missiles alternate between the two Missile nodes
    for (int i = 0; i < 5; ++i) {
        const long n = roundRobinWeaponNode({nodes.data(), nodes.size()}, weapons[i], false, false);
        REQUIRE(n >= 0);
        CHECK(n == expected[i]);
        nodes[static_cast<size_t>(n)].used++;
    }
}

TEST_CASE("round-robin: a weapon no node accepts resolves to -1; lost arms are skipped") {
    using namespace mc2_arm_remap;
    const Node onlyMissile[] = {{kMissile, false, false, 0}};
    CHECK(roundRobinWeaponNode(onlyMissile, kEnergy, false, false) == -1);
    const Node arms[] = {{kAny, true, false, 0}, {kAny, false, true, 0}, {kAny, false, false, 5}};
    CHECK(roundRobinWeaponNode(arms, kEnergy, false, false) == 0);
    CHECK(roundRobinWeaponNode(arms, kEnergy, true, false) == 1);
    CHECK(roundRobinWeaponNode(arms, kEnergy, true, true) == 2);
}

TEST_CASE("node coverage: every weapon of every stock mech variant resolves to a node (needs the stock data)") {
    using namespace stockscan;
    const fs::path root = srcDataRoot();
    if (root.empty()) { MESSAGE("FXTEST_SRCDATA not found: stock data scan skipped"); return; }
    const auto weaponOf = parseCompbasWeaponTypes(readAll(root / "objects" / "compbas.csv"));
    REQUIRE_FALSE(weaponOf.empty());
    int mechs = 0, variants = 0;
    for (const auto& e : fs::directory_iterator(root / "objects")) {
        if (e.path().extension() != ".csv" || e.path().stem() == "compbas") continue;
        std::string mechName;
        const auto vars = parseCsvVariants(readAll(e.path()), weaponOf, &mechName);
        if (vars.empty()) continue;
        fs::path ini = root / "tgl" / (e.path().stem().string() + ".ini");
        std::string spaced;
        for (char c : mechName) if (c != ' ') spaced.push_back(c);
        if (!fs::exists(ini)) ini = root / "tgl" / (lower(spaced) + ".ini");
        if (!fs::exists(ini)) continue;
        const IniNodes nodesIni = parseIniNodes(readAll(ini));
        if (nodesIni.declared <= 0) continue;
        ++mechs;
        for (const Variant& v : vars) {
            ++variants;
            std::vector<mc2_arm_remap::Node> nodes;
            for (long t : nodesIni.types) nodes.push_back({t, false, false, 0});
            for (size_t w = 0; w < v.weaponTypes.size(); ++w) {
                const long n = mc2_arm_remap::roundRobinWeaponNode({nodes.data(), nodes.size()}, v.weaponTypes[w], false, false);
                INFO(e.path().filename().string() << " variant '" << v.name << "' weapon #" << w << " type " << v.weaponTypes[w]);
                REQUIRE(n >= 0);
                nodes[static_cast<size_t>(n)].used++;
            }
        }
    }
    MESSAGE("scanned " << mechs << " mechs, " << variants << " variants");
    CHECK(mechs >= 20);
}
