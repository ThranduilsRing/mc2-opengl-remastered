#ifndef HARDPOINT_H
#define HARDPOINT_H
// HARDPOINT-ID-SCHEMA-1 (Phase 2, slice 1 of the UNIT-PROFILE arc).
//
// PURE SCHEMA. Defines the logical-hardpoint join key that Phase 2 is built
// around. The recon's central finding: logical hardpoints DO NOT EXIST today --
// the loadout slot, damage location, model socket, muzzle/FX socket, UI slot,
// and animation event each invent their own index language, joined by nothing
// (PRESENTATION-LOADOUT-STRANGLER-RECON-1.md "THE CRUX"). HardpointId is the one
// stable NAME every later Phase-2 slice maps to, so the validator can say useful
// modder-readable things: "AC20 on RightTorso_Energy_1: invalid weapon class",
// "loadout references LeftArm_Energy_2 but damage schema has no LeftArm".
//
// This header wires NO engine behavior. Nothing engine-side reads it yet (no
// gate needed). It is header-only, GL/IO-free (plain ints / char buffers /
// reused DamageLocationId), so it compiles into the offline `mc2_tests` target
// and drags no engine/GL/IO code -- exactly like damage_schema.h / locomotion.
//
// Reuse rule (roadmap redline): the damage-location link uses the EXISTING
// DamageLocationId vocabulary from damage_schema.h (DLOC_MECH_*, DLOC_VEH_*,
// DLOC_BODY). Do NOT invent a parallel location vocabulary. Link resolution
// reuses schemaHasLocation() -- a hardpoint is "location-coherent" against a
// unit's DamageSchema iff its damageLocation is a member of that schema.

#include "damage_schema.h"   // DamageLocationId, DamageSchemaId, schemaHasLocation

// -------------------------------------------------------------------------
// WeaponTag -- the mount-type tags a hardpoint accepts.
// -------------------------------------------------------------------------
// A hardpoint declares WHICH weapon mount-classes it will legally accept, as a
// bitset of these tags. This replaces the coarse Energy/Ballistic/Missile "form"
// buckets that getWeaponNode() round-robins over today (mech3d.cpp:1045) with a
// typed, name-addressable accept-set. Values are explicit bit positions so a
// WeaponTagMask can OR several together; keep NUM_WEAPON_TAGS <= 32 so the mask
// fits a uint32.
enum WeaponTag {
    WTAG_ENERGY        = 0,   // lasers, PPC, flamer
    WTAG_BALLISTIC     = 1,   // autocannon, gauss, MG
    WTAG_MISSILE       = 2,   // LRM / SRM racks
    WTAG_ANTIPERSONNEL = 3,   // infantry small-arms / AP
    WTAG_AMS           = 4,   // anti-missile system point-defense
    NUM_WEAPON_TAGS           // keep last; must stay <= 32
};

// Bitset of accepted WeaponTags. std-layout unsigned; OR tags to build an
// accept-set, test with hardpointAcceptsWeapon().
typedef unsigned int WeaponTagMask;

// Turn a WeaponTag into its single-bit mask.
inline WeaponTagMask weaponTagBit(WeaponTag t) {
    return (WeaponTagMask)1u << (unsigned)t;
}

// -------------------------------------------------------------------------
// HardpointId -- the stable join key (a hashed name + retained debug name).
// -------------------------------------------------------------------------
// Representation choice: an FNV-1a-hashed 32-bit id for O(1) VALUE EQUALITY,
// plus a small fixed char buffer that retains the original logical name for
// debug output and name round-trip. No engine types, no heap, trivially
// copyable -> safe in the offline test target and in any future POD facet.
// Logical names it must represent: RightTorso_Energy_1, LeftArm_Ballistic_1,
// CenterTorso_Missile_2, Infantry_PrimaryWeapon, Vehicle_Turret_Heavy_1.
//
// Equality is by hash only (the id IS the hash); the name buffer is carried for
// human readability and is not part of the equality contract. Names longer than
// the buffer are truncated in the stored NAME but still hashed in FULL, so two
// distinct long names never collide merely because their prefixes match.
enum { HARDPOINT_NAME_CAP = 48 };   // includes the NUL terminator

// FNV-1a over the whole (untruncated) name. Empty/null name -> 0 (the "unset"
// id). Case-SENSITIVE by design: RightTorso and righttorso are different
// hardpoints; presentation sockets are case-sensitive node-name strings.
inline unsigned int hardpointHash(const char* name) {
    if (!name || name[0] == '\0') return 0u;
    unsigned int h = 2166136261u;   // FNV offset basis
    for (const char* p = name; *p; ++p) {
        h ^= (unsigned char)*p;
        h *= 16777619u;             // FNV prime
    }
    return h;
}

struct HardpointId {
    unsigned int id;                 // FNV-1a hash of the full name (0 == unset)
    char name[HARDPOINT_NAME_CAP];   // retained (possibly truncated) debug name

    HardpointId() : id(0u) { name[0] = '\0'; }

    explicit HardpointId(const char* n) : id(hardpointHash(n)) {
        int i = 0;
        if (n) {
            for (; n[i] && i < HARDPOINT_NAME_CAP - 1; ++i) name[i] = n[i];
        }
        name[i] = '\0';
    }

    bool valid() const { return id != 0u; }
    const char* debugName() const { return name; }
};

// Value equality is hash equality (see HardpointId note above).
inline bool operator==(const HardpointId& a, const HardpointId& b) {
    return a.id == b.id;
}
inline bool operator!=(const HardpointId& a, const HardpointId& b) {
    return a.id != b.id;
}

// Convenience factory so callers read `makeHardpointId("RightTorso_Energy_1")`.
inline HardpointId makeHardpointId(const char* name) { return HardpointId(name); }

// -------------------------------------------------------------------------
// HardpointProfile -- the per-hardpoint schema row.
// -------------------------------------------------------------------------
// One logical hardpoint: its stable id, the weapon mount-classes it accepts,
// the damage location it lives on (in the EXISTING DamageLocationId vocabulary),
// and three small string-id fields that later slices resolve against external
// index spaces WITHOUT this header knowing about them:
//   * socket   -> model node-name (PresentationProfile / GetTransformedNodePosition)
//   * muzzle   -> muzzle / FX event socket (weapon-node FX, jumpJetEffect)
//   * uiSlot   -> mechlab / HUD slot name (UIProfile, decoupled from atlas index)
// All three are plain char buffers -> pure, no engine types. Empty means "not
// mapped yet" (a later slice / validator flags a missing socket, etc.).
enum { HARDPOINT_SOCKET_CAP = 48 };

// Fixed, trivially-copyable string field.
struct HardpointSocketName {
    char v[HARDPOINT_SOCKET_CAP];
    HardpointSocketName() { v[0] = '\0'; }
    explicit HardpointSocketName(const char* s) {
        int i = 0;
        if (s) { for (; s[i] && i < HARDPOINT_SOCKET_CAP - 1; ++i) v[i] = s[i]; }
        v[i] = '\0';
    }
    const char* c_str() const { return v; }
    bool empty() const { return v[0] == '\0'; }
};

struct HardpointProfile {
    HardpointId        id;              // the join key
    WeaponTagMask      acceptedTags;    // OR of weaponTagBit(...) it will mount
    DamageLocationId   damageLocation;  // reused vocabulary (damage_schema.h)
    HardpointSocketName socket;         // model node-name (presentation)
    HardpointSocketName muzzle;         // muzzle / FX event socket
    HardpointSocketName uiSlot;         // mechlab / HUD slot name

    HardpointProfile()
        : acceptedTags(0u), damageLocation(DLOC_BODY) {}
};

// -------------------------------------------------------------------------
// Pure helpers -- the contract every Phase-2 validator/adapter must honor.
// -------------------------------------------------------------------------

// Does this hardpoint accept a weapon of the given mount-class? Pure bit test
// over acceptedTags. Phase-2 loadout-join / validator calls this to reject
// "AC20 (Ballistic) on an Energy-only hardpoint".
inline bool hardpointAcceptsWeapon(const HardpointProfile& hp, WeaponTag tag) {
    return (hp.acceptedTags & weaponTagBit(tag)) != 0u;
}

// Add / test individual tags on a mask (small ergonomics for builders & tests).
inline void addWeaponTag(HardpointProfile& hp, WeaponTag tag) {
    hp.acceptedTags |= weaponTagBit(tag);
}

// Link-resolution contract: does this hardpoint's damageLocation exist in the
// given DamageSchema? Reuses schemaHasLocation() (damage_schema.h) -- the SAME
// membership test the DAMAGE-SCHEMA-FACET-1 slice already ships. A mech hardpoint
// on DLOC_MECH_RARM is coherent under DMG_MECH_8PART but NOT under
// DMG_VEHICLE_FACINGS. This is the join every later Phase-2 validator leans on:
// "loadout references LeftArm but the unit's DamageSchema has no LeftArm".
inline bool hardpointResolvesInSchema(const HardpointProfile& hp,
                                      DamageSchemaId schema) {
    return schemaHasLocation(schema, hp.damageLocation);
}

#endif // HARDPOINT_H
