// LOADOUT-EQUIPMENT-ID-ALIAS-1 (equipment arc, engine slice E5) -- offline tests
// for the PURE loadout-ref resolution logic (resolveLoadoutEquipmentRef): the
// qualified-id branch (core:<slug> via the registry), the legacy numeric-ordinal
// branch, and the not-found / needs-widen sentinels. Reuses the pure
// EquipmentRegistry from equipment_registry.h -- no engine, no MasterComponent.

#include "doctest.h"
#include "equipment_registry.h"

static EquipmentRegistry makeReg() {
    EquipmentRegistry reg;
    reg.add("core:medium_laser", 10, EQ_WEAPON, true);
    reg.add("core:ppc", 145, EQ_WEAPON, true);          // known ordinal (recon)
    reg.add("core:comp200", 200, EQ_OTHER, false);      // default-off placeholder
    reg.add("core:big", 300, EQ_OTHER, true);           // needs-widen alias (>=255)
    return reg;
}

TEST_CASE("isQualifiedEquipmentRef distinguishes the two forms") {
    CHECK(isQualifiedEquipmentRef("core:ppc"));
    CHECK(isQualifiedEquipmentRef("myMod:clan_ppc"));
    CHECK_FALSE(isQualifiedEquipmentRef("145"));
    CHECK_FALSE(isQualifiedEquipmentRef(""));
    CHECK_FALSE(isQualifiedEquipmentRef(nullptr));
}

TEST_CASE("qualified-id branch resolves to the legacy ordinal") {
    EquipmentRegistry reg = makeReg();
    CHECK(resolveLoadoutEquipmentRef(reg, "core:medium_laser") == 10);
    CHECK(resolveLoadoutEquipmentRef(reg, "core:ppc") == 145);
    // disabled entry still resolves (surfaced, not filtered).
    CHECK(resolveLoadoutEquipmentRef(reg, "core:comp200") == 200);
}

TEST_CASE("qualified-id round-trip: ordinal -> id -> ordinal") {
    EquipmentRegistry reg = makeReg();
    std::string qid = reg.qualifiedIdFor(145);
    REQUIRE(qid == "core:ppc");
    CHECK(resolveLoadoutEquipmentRef(reg, qid.c_str()) == 145);
}

TEST_CASE("numeric branch parses the legacy ordinal") {
    EquipmentRegistry reg = makeReg();
    CHECK(resolveLoadoutEquipmentRef(reg, "0") == 0);
    CHECK(resolveLoadoutEquipmentRef(reg, "10") == 10);
    CHECK(resolveLoadoutEquipmentRef(reg, "254") == 254);
    // a numeric ref does NOT need to exist in the registry.
    CHECK(resolveLoadoutEquipmentRef(reg, "99") == 99);
}

TEST_CASE("not-found sentinel on unknown id / malformed / empty") {
    EquipmentRegistry reg = makeReg();
    CHECK(resolveLoadoutEquipmentRef(reg, "core:nope") == kEquipmentRefNotFound);
    CHECK(resolveLoadoutEquipmentRef(reg, "12abc")     == kEquipmentRefNotFound);
    CHECK(resolveLoadoutEquipmentRef(reg, "-3")        == kEquipmentRefNotFound);
    CHECK(resolveLoadoutEquipmentRef(reg, "")          == kEquipmentRefNotFound);
    CHECK(resolveLoadoutEquipmentRef(reg, nullptr)     == kEquipmentRefNotFound);
}

TEST_CASE("needs-widen sentinel: resolved id >= 255 is NOT truncated") {
    EquipmentRegistry reg = makeReg();
    // via qualified id (alias to ordinal 300).
    CHECK(resolveLoadoutEquipmentRef(reg, "core:big") == kEquipmentRefNeedsWiden);
    // via numeric form.
    CHECK(resolveLoadoutEquipmentRef(reg, "255") == kEquipmentRefNeedsWiden);
    CHECK(resolveLoadoutEquipmentRef(reg, "500") == kEquipmentRefNeedsWiden);
}
