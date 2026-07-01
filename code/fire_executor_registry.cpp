// code/fire_executor_registry.cpp
// FIRE-EXECUTOR-REGISTRY-1 (equipment arc, engine slice E7).
//
// Home of the ONE engine-side FireExecutorRegistry instance -- the firing
// analogue of theActionExecutorRegistry() (action_executor_registry.cpp). The
// registry itself is pure/header-only and doctest-tested
// (tests/unit/test_fire_executor_registry.cpp); this TU is the thin engine glue
// that populates it once, at first access, with the stock executor vocabulary.
//
// It establishes the LOOKUP that replaces "if (masterId==PPC || class==LASER)"
// firing dispatch with "resolve the weapon's fireExecutor name to a registered
// executor". ★ SCHEMA/DISPATCH ONLY -- nothing here fires a weapon; the legacy
// MasterComponent / weaponFire path stays authoritative. No caller resolves()
// to fire in this slice (E-source-flip does that later).
//
// No GL, no IO, no engine headers -- just the pure registry contract. Populated
// lazily via a function-local static so init order is irrelevant.
#include "fire_executor_registry.h"

FireExecutorRegistry& theFireExecutorRegistry() {
    static FireExecutorRegistry reg = [] {
        FireExecutorRegistry r;
        // The stock executor set matching the converter's assigned fireExecutor
        // ids (plus flamer_area / tag_designator from the schema docstring).
        // Pure population shared with the offline doctest -- see the header.
        populateStockFireExecutors(r);
        return r;
    }();
    return reg;
}

// =========================================================================
// Gated proof (additive; MC2_UNIT_PROFILE_DATA). Proves the E7 lookup works
// END-TO-END on real loaded weapon components WITHOUT touching firing: for each
// weapon-form MasterComponent, DERIVE the same default fireExecutor the offline
// converter assigns (tools/unit_converter/equipment_extract._fire_executor --
// form/category + streak/LBX/artillery flags), then RESOLVE that id against the
// stock FireExecutorRegistry and tally bound/unbound. NOTHING here fires a
// weapon; the legacy MasterComponent / weaponFire path is untouched and
// authoritative. Gate-OFF: no-op -> byte-identical parity.
//
// NOTE: stock masterList weapons do NOT carry a fireExecutor field yet
// (E-source-flip is where the def's fireExecutor becomes authoritative). The
// proof derives the converter's default here so the lookup can be exercised.
// This whole block is engine-side (includes cmponent.h); it is NOT built into
// the pure offline mc2_tests target.
// =========================================================================
#include "unitprofile.h"   // mc2UnitProfileDataEnabled()
#include "cmponent.h"      // MasterComponent + masterList + weapon flag accessors
#include <cstdio>

// Mirror of equipment_extract._fire_executor: derive the default fireExecutor
// NAME from a weapon MasterComponent's form + missile/streak/LBX/artillery flags.
// Returns an empty id for non-weapon forms.
static WeaponFireExecutorId deriveDefaultFireExecutorId(MasterComponent& mc) {
    switch (mc.getForm()) {
        case COMPONENT_FORM_WEAPON_ENERGY:
            return makeFireExecutorId("beam_direct");
        case COMPONENT_FORM_WEAPON_MISSILE:
            return makeFireExecutorId(mc.getWeaponStreak() ? "missile_streak"
                                                           : "missile_salvo");
        case COMPONENT_FORM_WEAPON_BALLISTIC:
            if (mc.getWeaponArtillery()) return makeFireExecutorId("artillery_indirect");
            if (mc.getWeaponLBX())       return makeFireExecutorId("lbx_spread");
            return makeFireExecutorId("projectile_ballistic");
        case COMPONENT_FORM_WEAPON:
            // Undetermined weapon form -> converter's safe default.
            return makeFireExecutorId("projectile_ballistic");
        default:
            return WeaponFireExecutorId();   // non-weapon -> unset
    }
}

void dumpFireExecutorProofIfEnabled() {
    if (!mc2UnitProfileDataEnabled())
        return;
    if (!MasterComponent::masterList)
        return;

    const FireExecutorRegistry& reg = theFireExecutorRegistry();

    size_t weapons = 0, bound = 0, unbound = 0;
    int shown = 0;
    printf("[FIRE-EXECUTOR-REGISTRY-1] stock executors registered: %zu\n", reg.count());

    for (long i = 0; i < MasterComponent::numComponents; ++i) {
        MasterComponent& mc = MasterComponent::masterList[i];
        WeaponFireExecutorId fx = deriveDefaultFireExecutorId(mc);
        if (!fx.valid())
            continue;                          // not a weapon form
        ++weapons;

        bool isBound = reg.isBound(fx);
        if (isBound) ++bound; else ++unbound;

        const char* nm = mc.getName();
        if (shown < 8) {
            printf("[FIRE-EXECUTOR-REGISTRY-1]   sample %s fireExecutor=%s bound=%s\n",
                   (nm && nm[0]) ? nm : "<unnamed>", fx.c_str(),
                   isBound ? "YES" : "NO");
            ++shown;
        }
    }

    printf("[FIRE-EXECUTOR-REGISTRY-1] resolved: weapons=%zu  bound=%zu  unbound=%zu\n",
           weapons, bound, unbound);
    if (weapons == 0)
        printf("[FIRE-EXECUTOR-REGISTRY-1]   (no weapon-form components -- masterList not loaded?)\n");
}
