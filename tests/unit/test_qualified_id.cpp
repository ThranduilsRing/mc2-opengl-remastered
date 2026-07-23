// tests/unit/test_qualified_id.cpp
// UNIT-ID-NAMESPACE-1 (Phase-3 slice 6): the `namespace:name` string-convention
// + deterministic override-precedence layer. These tests lock the charset rules,
// canonicalization (lowercasing + bare-name -> core:), core-reserved policy, and
// the deterministic override precedence (core < earlier-mod < later-mod) so the
// C++ util and the Python mirror (tools/unit_converter/qualified_id.py) agree.
// Pure, offline — std::string only.
#include "doctest.h"
#include "qualified_id.h"

using namespace mc2id;

TEST_CASE("parse: valid qualified ids canonicalize") {
    QualifiedId q = parseQualifiedId("core:hunchback_hbk4g");
    CHECK(q.valid);
    CHECK(q.ns == "core");
    CHECK(q.name == "hunchback_hbk4g");
    CHECK(q.str() == "core:hunchback_hbk4g");
    CHECK(q.isCore());
}

TEST_CASE("parse: mixed-case input is lowercased") {
    QualifiedId q = parseQualifiedId("MyMod:UrbanMech_LAM_Test");
    CHECK(q.valid);
    CHECK(q.ns == "mymod");
    CHECK(q.name == "urbanmech_lam_test");
    CHECK(q.str() == "mymod:urbanmech_lam_test");
    CHECK_FALSE(q.isCore());
}

TEST_CASE("parse: bare name defaults to reserved core namespace") {
    QualifiedId q = parseQualifiedId("atlas_prime");
    CHECK(q.valid);
    CHECK(q.ns == "core");
    CHECK(q.name == "atlas_prime");
    CHECK(q.str() == "core:atlas_prime");
    CHECK(q.isCore());
}

TEST_CASE("validate: invalid namespaces are rejected") {
    CHECK_FALSE(isValidQualifiedId(""));            // empty
    CHECK_FALSE(isValidQualifiedId(":name"));       // empty namespace
    CHECK_FALSE(isValidQualifiedId("ns:"));         // empty name
    CHECK_FALSE(isValidQualifiedId("a:b:c"));       // two separators
    CHECK_FALSE(isValidQualifiedId("1mod:name"));   // ns must start with a letter
    CHECK_FALSE(isValidQualifiedId("mod:9name"));   // name must start with a letter
    CHECK_FALSE(isValidQualifiedId("my-mod:name")); // '-' not in charset
    CHECK_FALSE(isValidQualifiedId("mod:na me"));   // space not in charset
    CHECK_FALSE(isValidQualifiedId("mod:na.me"));   // '.' not in charset
}

TEST_CASE("validate: charset / length boundaries") {
    // 32-char namespace ok, 33 rejected.
    std::string ns32(32, 'a');
    std::string ns33(33, 'a');
    CHECK(isValidNamespace(ns32));
    CHECK_FALSE(isValidNamespace(ns33));
    // 64-char name ok, 65 rejected.
    std::string nm64(64, 'a');
    std::string nm65(65, 'a');
    CHECK(isValidName(nm64));
    CHECK_FALSE(isValidName(nm65));
    // digits + underscore in body allowed.
    CHECK(isValidQualifiedId("mod_2:hbk_4g_v2"));
}

TEST_CASE("canonicalize: round-trips to stable string") {
    CHECK(canonicalizeQualifiedId("CORE:Hunchback") == "core:hunchback");
    CHECK(canonicalizeQualifiedId("hunchback") == "core:hunchback");
    CHECK(canonicalizeQualifiedId("bad id!").empty());
    // canonical id parses back to itself.
    std::string canon = canonicalizeQualifiedId("MyMod:X_1");
    CHECK(canonicalizeQualifiedId(canon) == canon);
}

TEST_CASE("core is reserved: mods may not author into core:") {
    CHECK_FALSE(isModAuthorable("core:anything"));
    CHECK_FALSE(isModAuthorable("bare_name"));       // bare -> core:, reserved
    CHECK(isModAuthorable("mymod:anything"));
    CHECK_FALSE(isModAuthorable("bad!id"));          // invalid isn't authorable
}

TEST_CASE("precedence: later load order overrides earlier; core is base") {
    // core (0) < earlier-mod (1) < later-mod (2)
    CHECK(overrides(/*cand*/1, /*incumbent*/coreLoadOrder())); // mod beats core
    CHECK(overrides(2, 1));                                    // later beats earlier
    CHECK_FALSE(overrides(1, 2));                              // earlier loses to later
    CHECK_FALSE(overrides(1, 1));                              // equal rank: no override
    CHECK_FALSE(overrides(coreLoadOrder(), 1));               // core never beats a mod
}

TEST_CASE("precedence: resolveWinningOrder picks the max (last-loaded)") {
    const int orders[] = {0, 3, 1, 2};
    CHECK(resolveWinningOrder(orders, 4) == 3);
    CHECK(resolveWinningOrder(nullptr, 0) == coreLoadOrder());
    const int one[] = {5};
    CHECK(resolveWinningOrder(one, 1) == 5);
}
