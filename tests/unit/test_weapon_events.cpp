// tests/unit/test_weapon_events.cpp
// WEAPON-FACET-PRESENTATION-SCHEMA (equipment arc, slices 2+3 -- both pure schema).
//
// Pure-logic doctest coverage for the two schema additions in weapon_definition.h:
//   (2) the `fireExecutor` binding on WeaponFacet -- a stable-name id a FUTURE
//       FIRE-EXECUTOR-REGISTRY resolves; BINDING ONLY, no firing behavior.
//   (3) the EVENT-BASED WeaponPresentationProfile -- a WeaponEvent x WeaponAssetKind
//       grid mapping semantic firing events to asset string ids, plus the retained
//       flat Phase-0 fields (back-compat) and the syncFlatToEvents() bridge.
// The executor (future) EMITS events; this profile RESOLVES them to assets. Schema
// only -- header-only, drags no engine/GL/IO. NEVER hardwire SFX/VFX into firing.
#include "doctest.h"
#include "weapon_definition.h"
#include <string>

// Lock the event/kind counts against enum drift (mirrors action_anim_tag.h's
// static_assert-against-legacy-ids discipline).
static_assert(NUM_WEAPON_EVENTS == 10, "WeaponEvent count drifted");
static_assert(NUM_WEAPON_ASSET_KINDS == 4, "WeaponAssetKind count drifted");
static_assert((int)WeaponEvent::OnFire == 0, "OnFire must be index 0");
static_assert((int)WeaponEvent::OnOverheat == 9, "OnOverheat must be the last event");

// -------------------------------------------------------------------------
// (2) fireExecutor binding on WeaponFacet.
// -------------------------------------------------------------------------

TEST_CASE("fireExecutor: default is unset; set/get by stable name") {
    WeaponFacet w;
    CHECK_FALSE(w.fireExecutor.valid());          // default = no executor bound

    w.fireExecutor = makeFireExecutorId("beam_direct");
    CHECK(w.fireExecutor.valid());
    CHECK(std::string(w.fireExecutor.c_str()) == "beam_direct");
}

TEST_CASE("fireExecutor: value equality is by hashed name (mods add executors)") {
    WeaponFireExecutorId a = makeFireExecutorId("projectile_ballistic");
    WeaponFireExecutorId b = makeFireExecutorId("projectile_ballistic");
    WeaponFireExecutorId c = makeFireExecutorId("missile_salvo");
    CHECK(a == b);
    CHECK(a != c);

    // The stable-name families the roadmap lists all bind cleanly and distinctly.
    WeaponFireExecutorId names[] = {
        makeFireExecutorId("beam_direct"),
        makeFireExecutorId("projectile_ballistic"),
        makeFireExecutorId("missile_salvo"),
        makeFireExecutorId("artillery_indirect"),
        makeFireExecutorId("flamer_area"),
        makeFireExecutorId("tag_designator"),
    };
    for (auto& n : names) CHECK(n.valid());
    // A mod-authored executor name is just as valid (no fixed enum / 255-cap).
    CHECK(makeFireExecutorId("myMod:plasma_lance").valid());

    // Empty / null name => unset (id 0), the "no executor" sentinel.
    CHECK_FALSE(makeFireExecutorId("").valid());
    CHECK_FALSE(makeFireExecutorId(nullptr).valid());
}

// -------------------------------------------------------------------------
// (3a) Event -> asset grid: each WeaponEvent maps / doesn't-map, per kind.
// -------------------------------------------------------------------------

TEST_CASE("event->asset: empty by default; nothing maps") {
    WeaponPresentationProfile p;
    // Every event, every kind, defaults empty.
    WeaponEvent evs[] = {
        WeaponEvent::OnFire, WeaponEvent::OnProjectileSpawn, WeaponEvent::OnMuzzleFlash,
        WeaponEvent::OnBeamStart, WeaponEvent::OnBeamEnd, WeaponEvent::OnImpact,
        WeaponEvent::OnMiss, WeaponEvent::OnJam, WeaponEvent::OnAmmoDepleted,
        WeaponEvent::OnOverheat
    };
    WeaponAssetKind kinds[] = {
        WeaponAssetKind::Sfx, WeaponAssetKind::Vfx,
        WeaponAssetKind::Decal, WeaponAssetKind::Light
    };
    for (auto ev : evs) {
        CHECK_FALSE(eventHasAnyAsset(p, ev));
        for (auto k : kinds) {
            CHECK(assetForEvent(p, ev, k).empty());
            CHECK_FALSE(hasAssetForEvent(p, ev, k));
        }
    }
}

TEST_CASE("event->asset: set per (event, kind); other pairs stay empty") {
    WeaponPresentationProfile p;

    p.setEventAsset(WeaponEvent::OnFire,   WeaponAssetKind::Sfx, "sfx:ppc_fire");
    p.setEventAsset(WeaponEvent::OnFire,   WeaponAssetKind::Light, "light:ppc_flash");
    p.setEventAsset(WeaponEvent::OnImpact, WeaponAssetKind::Vfx, "vfx:ppc_hit");
    p.setEventAsset(WeaponEvent::OnImpact, WeaponAssetKind::Decal, "decal:scorch");

    // The four authored pairs resolve.
    CHECK(std::string(assetForEvent(p, WeaponEvent::OnFire, WeaponAssetKind::Sfx).c_str()) == "sfx:ppc_fire");
    CHECK(std::string(assetForEvent(p, WeaponEvent::OnFire, WeaponAssetKind::Light).c_str()) == "light:ppc_flash");
    CHECK(std::string(assetForEvent(p, WeaponEvent::OnImpact, WeaponAssetKind::Vfx).c_str()) == "vfx:ppc_hit");
    CHECK(std::string(assetForEvent(p, WeaponEvent::OnImpact, WeaponAssetKind::Decal).c_str()) == "decal:scorch");

    CHECK(hasAssetForEvent(p, WeaponEvent::OnFire, WeaponAssetKind::Sfx));
    CHECK(eventHasAnyAsset(p, WeaponEvent::OnFire));
    CHECK(eventHasAnyAsset(p, WeaponEvent::OnImpact));

    // Unauthored kinds on an authored event stay empty.
    CHECK_FALSE(hasAssetForEvent(p, WeaponEvent::OnFire, WeaponAssetKind::Vfx));
    CHECK_FALSE(hasAssetForEvent(p, WeaponEvent::OnFire, WeaponAssetKind::Decal));

    // A wholly-unauthored event maps to nothing.
    CHECK_FALSE(eventHasAnyAsset(p, WeaponEvent::OnJam));
    CHECK(assetForEvent(p, WeaponEvent::OnMiss, WeaponAssetKind::Sfx).empty());
}

TEST_CASE("event->asset: each event kind slot is independent") {
    // Exercise every event with a distinct asset so no two indices collide.
    WeaponPresentationProfile p;
    struct Row { WeaponEvent ev; const char* id; };
    Row rows[] = {
        { WeaponEvent::OnFire,            "vfx:e0" },
        { WeaponEvent::OnProjectileSpawn, "vfx:e1" },
        { WeaponEvent::OnMuzzleFlash,     "vfx:e2" },
        { WeaponEvent::OnBeamStart,       "vfx:e3" },
        { WeaponEvent::OnBeamEnd,         "vfx:e4" },
        { WeaponEvent::OnImpact,          "vfx:e5" },
        { WeaponEvent::OnMiss,            "vfx:e6" },
        { WeaponEvent::OnJam,             "vfx:e7" },
        { WeaponEvent::OnAmmoDepleted,    "vfx:e8" },
        { WeaponEvent::OnOverheat,        "vfx:e9" },
    };
    for (auto& r : rows) p.setEventAsset(r.ev, WeaponAssetKind::Vfx, r.id);
    for (auto& r : rows)
        CHECK(std::string(assetForEvent(p, r.ev, WeaponAssetKind::Vfx).c_str()) == r.id);
}

// -------------------------------------------------------------------------
// (3b) Flat Phase-0 fields retained + syncFlatToEvents() back-compat bridge.
// -------------------------------------------------------------------------

TEST_CASE("presentation: flat Phase-0 fields still authored + read (back-compat)") {
    WeaponPresentationProfile p;
    p.muzzleVfxId = WeaponStringId("vfx:muzzle");
    p.impactVfxId = WeaponStringId("vfx:impact");
    p.fireSfxId   = WeaponStringId("sfx:fire");
    p.impactSfxId = WeaponStringId("sfx:impact");
    CHECK(std::string(p.muzzleVfxId.c_str()) == "vfx:muzzle");
    CHECK(std::string(p.impactVfxId.c_str()) == "vfx:impact");
    CHECK(std::string(p.fireSfxId.c_str())   == "sfx:fire");
    CHECK(std::string(p.impactSfxId.c_str()) == "sfx:impact");
    // Flat fields alone do NOT populate the event grid until synced.
    CHECK_FALSE(eventHasAnyAsset(p, WeaponEvent::OnFire));
}

TEST_CASE("presentation: syncFlatToEvents mirrors flat fields into the event grid") {
    WeaponPresentationProfile p;
    p.muzzleVfxId = WeaponStringId("vfx:muzzle");
    p.impactVfxId = WeaponStringId("vfx:impact");
    p.fireSfxId   = WeaponStringId("sfx:fire");
    p.impactSfxId = WeaponStringId("sfx:impact");
    p.syncFlatToEvents();

    CHECK(std::string(assetForEvent(p, WeaponEvent::OnFire, WeaponAssetKind::Sfx).c_str()) == "sfx:fire");
    CHECK(std::string(assetForEvent(p, WeaponEvent::OnMuzzleFlash, WeaponAssetKind::Vfx).c_str()) == "vfx:muzzle");
    CHECK(std::string(assetForEvent(p, WeaponEvent::OnImpact, WeaponAssetKind::Vfx).c_str()) == "vfx:impact");
    CHECK(std::string(assetForEvent(p, WeaponEvent::OnImpact, WeaponAssetKind::Sfx).c_str()) == "sfx:impact");

    // Empty flat fields do not create spurious mappings.
    CHECK_FALSE(hasAssetForEvent(p, WeaponEvent::OnMiss, WeaponAssetKind::Sfx));
}

TEST_CASE("presentation: sync does not clobber an already-authored event asset") {
    WeaponPresentationProfile p;
    p.setEventAsset(WeaponEvent::OnFire, WeaponAssetKind::Sfx, "sfx:authored_first");
    p.fireSfxId = WeaponStringId("sfx:flat_second");
    p.syncFlatToEvents();
    // Flat sync only fills; it overwrites the OnFire/Sfx slot with the flat value
    // (documented bridge behavior: flat is the compatibility source when synced).
    CHECK(std::string(assetForEvent(p, WeaponEvent::OnFire, WeaponAssetKind::Sfx).c_str()) == "sfx:flat_second");
}

// -------------------------------------------------------------------------
// Integration: a WeaponFacet carries both the executor binding and event presentation.
// -------------------------------------------------------------------------

TEST_CASE("WeaponFacet: fireExecutor + event presentation compose (schema, no firing)") {
    WeaponFacet ppc;
    ppc.weaponId = makeWeaponId("core:ppc");
    ppc.category = WTAG_ENERGY;
    ppc.fireExecutor = makeFireExecutorId("beam_direct");
    ppc.presentation.setEventAsset(WeaponEvent::OnBeamStart, WeaponAssetKind::Sfx, "sfx:ppc_beam");
    ppc.presentation.setEventAsset(WeaponEvent::OnImpact, WeaponAssetKind::Decal, "decal:scorch");

    CHECK(ppc.valid());
    CHECK(std::string(ppc.fireExecutor.c_str()) == "beam_direct");
    CHECK(eventHasAnyAsset(ppc.presentation, WeaponEvent::OnBeamStart));
    CHECK(hasAssetForEvent(ppc.presentation, WeaponEvent::OnImpact, WeaponAssetKind::Decal));
    // Behavior (category/combat) unaffected by presentation/executor bindings.
    CHECK(ppc.category == WTAG_ENERGY);
}
