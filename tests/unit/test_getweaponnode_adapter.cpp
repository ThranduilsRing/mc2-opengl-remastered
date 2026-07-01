// tests/unit/test_getweaponnode_adapter.cpp
// GETWEAPONNODE-HARDPOINT-ADAPTER-1 (Phase 2, slice 4 of the UNIT-PROFILE arc).
//
// Pure-logic doctest coverage for chooseWeaponNode() in hardpoint.h: the
// authoritative-with-fallback weapon-node selection rule. This proves the ROUTE
// (declared hardpoint socket wins over legacy round-robin) WITHOUT a full data
// unit -- the in-mission visual proof of a declared hardpoint mounting to its
// socket is deferred to slice 9 (NON-MECH-DATA-UNIT-PROOF-1).
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
