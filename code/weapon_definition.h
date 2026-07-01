#ifndef WEAPON_DEFINITION_H
#define WEAPON_DEFINITION_H
// WEAPONDEF-OFFLINE-HARNESS-1 / WEAPONDEF-SCHEMA-1 (Phase 0 of the WEAPON-DEFINITION arc).
//
// PURE SCHEMA. Defines what a "weapon" IS as data, mirroring the proven UNIT
// arc Phase-0 shape (hardpoint.h / unit_action.h). This header wires NO engine
// behavior: nothing engine-side reads it yet (no gate needed). It is header-only,
// GL/IO-free (FNV-hashed ids / fixed char buffers / plain numbers / reused
// WeaponTag + DamageLocationId), so it compiles into the offline `mc2_tests`
// target and drags no engine/GL/IO code -- exactly like hardpoint.h.
//
// THE PROBLEM IT KILLS (WEAPON-DEFINITION-ARC-SCOPE-1.md):
// stock MC2 weapons are rows in compbas.csv loaded into MasterComponent::masterList,
// with the ROW ORDINAL as identity (masterID = the CSV line number), a hardcoded
// 255-component cap (mission.cpp:2451 / saveload.cpp:846), ammo linked by raw
// index + WEAPON_AMMO_OFFSET=65 arithmetic, and the "EnergyWeapon/BallisticWeapon/
// MissileWeapon" category matched to a FORM enum then DISCARDED. Insert or reorder
// a row and every loadout / ammo id / IDS string offset shifts. A modder cannot
// add a weapon without renumbering, and there is no surviving category a validator
// can compare against a hardpoint's acceptedTags.
//
// THE 255-CAP KILL (design property, not a runtime knob): identity here is a
// stable interned/hashed NAME (WeaponId), NOT a fixed ordinal into a [255] array.
// Two weapons are the same iff their names hash equal; a mod adds `myMod:heavy_ppc`
// without disturbing any other weapon's identity. The extensibility (no 255 cap)
// falls straight out of using WeaponId instead of an int index -- there is no
// array whose size caps the population.
//
// THE JOIN IS ALREADY HALF-BUILT (the single biggest reuse): the UNIT arc built
// WeaponTag {WTAG_ENERGY/BALLISTIC/MISSILE/ANTIPERSONNEL/AMS} + HardpointProfile.
// acceptedTags + hardpointAcceptsWeapon() (hardpoint.h). Today a hardpoint declares
// WHICH mount-classes it accepts, but NOTHING describes the weapon's class -- the
// weapon side of the join is missing. WeaponDefinition.category (a WeaponTag) is
// that missing half; weaponFitsHardpoint() below plugs into the existing accept
// test. Report-only for now; unlocks author-time HARDPOINT-ENFORCEMENT later.
//
// FUTURE CONSUMERS (documented, NOT built here): the engine's WEAPONDEF-TYPELOAD-
// READ slice will overlay a WeaponDefinition onto a MasterComponent at the ONE
// type-load seam (MasterComponent::loadMasterList, cmponent.cpp:630), and a
// string-id registry (WeaponId -> WeaponDefinition) will replace the masterList[255]
// ordinal space. This header is the shared CONTRACT those later slices implement
// against; it introduces no runtime read, no gate, no mc2srcdata change.

#include "hardpoint.h"       // WeaponTag, HardpointProfile, hardpointAcceptsWeapon
#include "damage_schema.h"   // DamageLocationId

// -------------------------------------------------------------------------
// WeaponId -- string-id identity (the 255-cap kill).
// -------------------------------------------------------------------------
// Same representation choice as HardpointId (hardpoint.h): an FNV-1a-hashed
// 32-bit id for O(1) VALUE EQUALITY, plus a fixed char buffer retaining the
// original namespaced name (`core:ac20`, `myMod:heavy_ppc`) for debug output and
// name round-trip. No engine types, no heap, trivially copyable -> safe in the
// offline test target and in any future POD facet. id == 0 means "unset".
//
// Equality is by hash only (the id IS the hash); the name buffer is carried for
// human readability and is NOT part of the equality contract. Names longer than
// the buffer are truncated in the stored NAME but hashed in FULL, so two distinct
// long names never collide merely because their stored prefixes match.
//
// Case-SENSITIVE by design, consistent with HardpointId: ids are authored
// namespaced strings, not free text.
enum { WEAPON_NAME_CAP = 64 };   // includes the NUL terminator

// FNV-1a over the whole (untruncated) name. Empty/null name -> 0 (the "unset" id).
inline unsigned int weaponIdHash(const char* name) {
    if (!name || name[0] == '\0') return 0u;
    unsigned int h = 2166136261u;   // FNV offset basis
    for (const char* p = name; *p; ++p) {
        h ^= (unsigned char)*p;
        h *= 16777619u;             // FNV prime
    }
    return h;
}

struct WeaponId {
    unsigned int id;               // FNV-1a hash of the full name (0 == unset)
    char name[WEAPON_NAME_CAP];    // retained (possibly truncated) debug name

    WeaponId() : id(0u) { name[0] = '\0'; }

    explicit WeaponId(const char* n) : id(weaponIdHash(n)) {
        int i = 0;
        if (n) {
            for (; n[i] && i < WEAPON_NAME_CAP - 1; ++i) name[i] = n[i];
        }
        name[i] = '\0';
    }

    bool valid() const { return id != 0u; }
    const char* debugName() const { return name; }
};

// Value equality is hash equality (see WeaponId note above).
inline bool operator==(const WeaponId& a, const WeaponId& b) { return a.id == b.id; }
inline bool operator!=(const WeaponId& a, const WeaponId& b) { return a.id != b.id; }

// Convenience factory so callers read `makeWeaponId("core:ac20")`.
inline WeaponId makeWeaponId(const char* name) { return WeaponId(name); }

// -------------------------------------------------------------------------
// String-id fields -- generic fixed, trivially-copyable id buffers.
// -------------------------------------------------------------------------
// Every cross-space join in a WeaponDefinition (ammo category, VFX/SFX cue, UI
// icon) is a stable string id, NEVER an index -- the same discipline HardpointId
// uses for sockets. A later slice resolves these against external index spaces
// (FX registry, atlas, string table) WITHOUT this header knowing about them.
// Empty means "not mapped" (a validator flags a missing/dangling id).
enum { WEAPON_STRID_CAP = 64 };

struct WeaponStringId {
    char v[WEAPON_STRID_CAP];
    WeaponStringId() { v[0] = '\0'; }
    explicit WeaponStringId(const char* s) {
        int i = 0;
        if (s) { for (; s[i] && i < WEAPON_STRID_CAP - 1; ++i) v[i] = s[i]; }
        v[i] = '\0';
    }
    const char* c_str() const { return v; }
    bool empty() const { return v[0] == '\0'; }
};

// -------------------------------------------------------------------------
// Facets. A WeaponDefinition AGGREGATES facets (it is not a flat blob), so each
// concern -- identity, category, combat, ammo, fit, presentation, UI -- can grow
// independently the way the unit arc's UnitDefinition aggregates its facets.
// -------------------------------------------------------------------------

// Ammo linkage by NAMED CATEGORY, not by index/offset. Replaces the stock
// `ammoMasterId` raw index + WEAPON_AMMO_OFFSET=65 arithmetic (cmponent.h:50).
// A weapon NAMES an ammo category (`core:ammo_ac5`); the ammo component declares
// the same category id (BT2018 AmmoCategory <-> ammoCategoryID). Empty category
// = self-powered / no ammo (energy weapons). ammoPerShot is a plain number.
struct WeaponAmmoFacet {
    WeaponStringId ammoCategory;   // named category id, NOT an index (empty = none)
    int            ammoPerShot;    // rounds consumed per shot (0 for energy)

    WeaponAmmoFacet() : ammoPerShot(0) {}
    bool usesAmmo() const { return !ammoCategory.empty(); }
};

// Plain combat numbers. Range is the stock short/med/long BUCKET distances kept
// as plain numbers now (a later slice can widen to real RangeSplit thresholds).
// CV = combat/battle value (the stock BR column). No engine types.
struct WeaponCombatStats {
    float damage;
    float heat;
    float recycleTime;   // refire delay (stock Recycle column)
    float rangeShort;
    float rangeMed;
    float rangeLong;
    float combatValue;   // CV / BR

    WeaponCombatStats()
        : damage(0.f), heat(0.f), recycleTime(0.f),
          rangeShort(0.f), rangeMed(0.f), rangeLong(0.f), combatValue(0.f) {}
};

// Fit / size facet: how much of a location the weapon occupies and where it may
// legally mount. allowedLocations is a small fixed list of DamageLocationIds
// (the EXISTING vocabulary from damage_schema.h -- do NOT invent a parallel one);
// a later validator checks a chosen mount location is a member. allowedCount == 0
// is read as "unconstrained" (BT2018 AllowedLocations:"All").
enum { WEAPON_MAX_ALLOWED_LOCATIONS = NUM_DAMAGE_LOCATION_IDS };

struct WeaponFitFacet {
    int              critSlots;    // InventorySize -- crit spaces required
    float            tonnage;      // weight
    DamageLocationId allowedLocations[WEAPON_MAX_ALLOWED_LOCATIONS];
    int              allowedCount; // 0 == unconstrained ("All")

    WeaponFitFacet() : critSlots(0), tonnage(0.f), allowedCount(0) {}

    // Add an allowed damage-location (no dedupe -- authoring-side concern).
    void allowLocation(DamageLocationId loc) {
        if (allowedCount < WEAPON_MAX_ALLOWED_LOCATIONS)
            allowedLocations[allowedCount++] = loc;
    }
    // Is `loc` a legal mount location? Unconstrained (count 0) accepts anything.
    bool allowsLocation(DamageLocationId loc) const {
        if (allowedCount == 0) return true;
        for (int i = 0; i < allowedCount; ++i)
            if (allowedLocations[i] == loc) return true;
        return false;
    }
};

// -------------------------------------------------------------------------
// WeaponPresentationProfile -- VFX/SFX as a FIRST-CLASS facet (the user's
// "defs -> VFX/SFX" vision). Every cue is a string id, resolved by a later slice
// against the FX/audio registries. This is the half stock MC2 crushed into a
// single raw `specialEffect` char id (cmponent.cpp:237); here muzzle vs impact
// and visual vs audio are distinct, named, and moddable.
// -------------------------------------------------------------------------
struct WeaponPresentationProfile {
    WeaponStringId muzzleVfxId;    // muzzle-flash / firing VFX cue
    WeaponStringId impactVfxId;    // on-hit VFX cue
    WeaponStringId fireSfxId;      // firing sound cue
    WeaponStringId impactSfxId;    // on-hit sound cue

    WeaponPresentationProfile() {}
};

// UI facet: icon + display name by string id, not by IDS-number/atlas index.
struct WeaponUIProfile {
    WeaponStringId iconId;         // UI icon id (BT2018 Description.Icon)
    WeaponStringId uiName;         // localized display name (Description.UIName)

    WeaponUIProfile() {}
};

// -------------------------------------------------------------------------
// WeaponDefinition -- the facet aggregator. Modder-facing "what is a weapon",
// keyed by the stable WeaponId. Category is a WeaponTag (reused from hardpoint.h)
// -- the same value hardpointAcceptsWeapon() has been waiting for.
// -------------------------------------------------------------------------
struct WeaponDefinition {
    // Identity facet.
    WeaponId  weaponId;            // stable string-id identity (the 255-cap kill)
    WeaponStringId debugName;      // author/debug name (may differ from UI name)

    // Category / mount-tag facet (the missing half of the hardpoint join).
    WeaponTag category;            // WTAG_ENERGY/BALLISTIC/MISSILE/ANTIPERSONNEL/AMS

    // Composed facets.
    WeaponCombatStats         combat;
    WeaponAmmoFacet           ammo;
    WeaponFitFacet            fit;
    WeaponPresentationProfile presentation;   // VFX/SFX -- first-class
    WeaponUIProfile           ui;

    WeaponDefinition() : category(WTAG_ENERGY) {}

    bool valid() const { return weaponId.valid(); }
};

// -------------------------------------------------------------------------
// Pure helpers -- the contract later weapon slices / validators call.
// -------------------------------------------------------------------------

// The weapon HALF of the hardpoint join. Delegates to the EXISTING
// hardpointAcceptsWeapon() (hardpoint.h) using the weapon's own category tag.
// Report-only: a hardpoint declares acceptedTags; this asks "does this weapon's
// class fit that hardpoint?". Energy weapon on an energy hardpoint -> true; an
// AC (WTAG_BALLISTIC) on an energy-only hardpoint -> false. Unlocks author-time
// HARDPOINT-ENFORCEMENT (mechlab rejects the mount) in a later slice. Pure; no
// engine types, no side effects.
inline bool weaponFitsHardpoint(const WeaponDefinition& w, const HardpointProfile& hp) {
    return hardpointAcceptsWeapon(hp, w.category);
}

#endif // WEAPON_DEFINITION_H
