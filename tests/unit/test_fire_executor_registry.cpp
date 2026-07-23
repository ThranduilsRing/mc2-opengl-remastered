// FIRE-EXECUTOR-REGISTRY-1 (equipment arc, engine slice E7) -- offline tests for
// the PURE fire-executor registry: bind/isBound/entryFor/resolve, the stock
// executor vocabulary (must match the converter's assigned fireExecutor names),
// resolve(WeaponFacet) by category-derived id, and unset/unbound classification.
// No engine, no MasterComponent -- the singleton glue that populates the engine
// instance lives in fire_executor_registry.cpp (not built here).

#include "doctest.h"
#include "fire_executor_registry.h"

TEST_CASE("bind / isBound / entryFor round-trip") {
    FireExecutorRegistry reg;
    WeaponFireExecutorId beam = makeFireExecutorId("beam_direct");

    CHECK_FALSE(reg.isBound(beam));            // nothing bound yet
    CHECK(reg.entryFor(beam) == nullptr);

    reg.bind("beam_direct", FEK_BEAM, /*ammo*/false, /*beam*/true, /*indirect*/false);

    CHECK(reg.isBound(beam));
    const FireExecutorEntry* e = reg.entryFor(beam);
    REQUIRE(e != nullptr);
    CHECK(e->kind == FEK_BEAM);
    CHECK(e->isBeam);
    CHECK_FALSE(e->needsAmmo);
    CHECK_FALSE(e->isIndirect);
    CHECK(e->id == beam);
}

TEST_CASE("unset id never binds and never resolves") {
    FireExecutorRegistry reg;
    WeaponFireExecutorId unset;                // default -> id==0
    CHECK_FALSE(unset.valid());
    CHECK_FALSE(reg.isBound(unset));
    CHECK(reg.entryFor(unset) == nullptr);

    // Attempting to bind an unset id is a no-op (0 must never be a hit).
    reg.bind(FireExecutorEntry(unset, FEK_BEAM, false, true, false));
    CHECK(reg.count() == 0);
}

TEST_CASE("rebind overwrites descriptor") {
    FireExecutorRegistry reg;
    reg.bind("projectile_ballistic", FEK_PROJECTILE, true, false, false);
    reg.bind("projectile_ballistic", FEK_ARTILLERY, true, false, true);
    const FireExecutorEntry* e = reg.entryFor(makeFireExecutorId("projectile_ballistic"));
    REQUIRE(e != nullptr);
    CHECK(e->kind == FEK_ARTILLERY);          // last write wins
    CHECK(e->isIndirect);
    CHECK(reg.count() == 1);                   // still one binding for the id
}

TEST_CASE("stock executor set: all converter ids bound") {
    FireExecutorRegistry reg;
    populateStockFireExecutors(reg);
    CHECK(reg.count() == NUM_STOCK_FIRE_EXECUTORS);
    CHECK(NUM_STOCK_FIRE_EXECUTORS == 8);

    // Every name the converter (equipment_extract._fire_executor / equipment_model
    // FX_*) can assign is bound, plus the two schema-docstring names.
    const char* stock[] = {
        "beam_direct", "projectile_ballistic", "lbx_spread",
        "missile_salvo", "missile_streak", "artillery_indirect",
        "flamer_area", "tag_designator"
    };
    for (const char* n : stock)
        CHECK(reg.isBound(makeFireExecutorId(n)));

    // Trait spot-checks that mirror the converter's FORM/field mapping.
    CHECK(reg.entryFor(makeFireExecutorId("beam_direct"))->isBeam);
    CHECK_FALSE(reg.entryFor(makeFireExecutorId("beam_direct"))->needsAmmo);
    CHECK(reg.entryFor(makeFireExecutorId("artillery_indirect"))->isIndirect);
    CHECK(reg.entryFor(makeFireExecutorId("missile_salvo"))->kind == FEK_MISSILE);
    CHECK(reg.entryFor(makeFireExecutorId("lbx_spread"))->kind == FEK_PROJECTILE);
    CHECK_FALSE(reg.entryFor(makeFireExecutorId("tag_designator"))->needsAmmo);
}

TEST_CASE("resolve(WeaponFacet) by declared fireExecutor") {
    FireExecutorRegistry reg;
    populateStockFireExecutors(reg);

    // Energy weapon -> beam_direct (converter WT_ENERGY branch).
    WeaponFacet energy;
    energy.category     = WTAG_ENERGY;
    energy.fireExecutor = makeFireExecutorId("beam_direct");
    const FireExecutorEntry* e = reg.resolve(energy);
    REQUIRE(e != nullptr);
    CHECK(e->kind == FEK_BEAM);

    // Ballistic weapon -> projectile_ballistic.
    WeaponFacet ballistic;
    ballistic.category     = WTAG_BALLISTIC;
    ballistic.fireExecutor = makeFireExecutorId("projectile_ballistic");
    REQUIRE(reg.resolve(ballistic) != nullptr);
    CHECK(reg.resolve(ballistic)->kind == FEK_PROJECTILE);

    // Missile weapon -> missile_salvo.
    WeaponFacet missile;
    missile.category     = WTAG_MISSILE;
    missile.fireExecutor = makeFireExecutorId("missile_salvo");
    REQUIRE(reg.resolve(missile) != nullptr);
    CHECK(reg.resolve(missile)->kind == FEK_MISSILE);
}

TEST_CASE("classify: OK / UNSET / UNBOUND") {
    FireExecutorRegistry reg;
    populateStockFireExecutors(reg);

    // OK: a bound, named executor.
    WeaponFacet ok;
    ok.fireExecutor = makeFireExecutorId("beam_direct");
    CHECK(classifyFireExecutor(reg, ok) == FE_RES_OK);
    CHECK(reg.resolve(ok) != nullptr);

    // UNSET: weapon carries no fireExecutor (default id==0) -> validator would
    // report EQV_WEAPON_NO_FIRE_EXECUTOR.
    WeaponFacet unset;                         // fireExecutor defaults to unset
    CHECK(classifyFireExecutor(reg, unset) == FE_RES_UNSET);
    CHECK(reg.resolve(unset) == nullptr);

    // UNBOUND: names an executor that is not registered -> validator would
    // report EQV_FIRE_EXECUTOR_UNBOUND.
    WeaponFacet unbound;
    unbound.fireExecutor = makeFireExecutorId("myMod:plasma_lance");
    CHECK(classifyFireExecutor(reg, unbound) == FE_RES_UNBOUND);
    CHECK(reg.resolve(unbound) == nullptr);
}
