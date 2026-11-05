#pragma once
// BRAIN-ARCHETYPE-1: declarative Brain archetype presets (discussion #19 "BrainArchetype").
//
// An archetype is a named collection of default brain-switch values. A Brain{} block names one
// via `archetype = "Archetype.Faction.Variant"`; the unit inherits the preset's switch defaults,
// and any switch the Brain{} block sets explicitly OVERRIDES the preset. This is the resolution
// the engine consumer applies (mission.cpp): preset first, then per-Brain overrides.
//
// Pure + header-only (no engine headers) so it is unit-testable and usable from the consumer.
// The four presets below are the ones the carver_v_enhanced corpus references
// (InnerSphere.Standard, Clan.Standard, Mercenary.Standard, PlayerControlled); the table is
// substring-matched so faction variants (e.g. "Archetype.InnerSphere.Davion") fall back to the
// faction Standard. Adding presets = one row.

#include "mc2_czstring.h"
#include <cstring>

struct BrainArchetypeDefaults {
    float engageRadius;        // detection/engagement willingness radius
    float attackerHelpRadius;  // call for help when attacking within this radius
    float defenderHelpRadius;  // call for help when defending within this radius
    int   requestHelp;         // 0/1 — will this unit request help
    int   returnToPost;        // 0/1 — return to guard post after pursuing (vs press the attack)
    int   playerControlled;    // 0/1 — unit is player-driven (skip AI activation)
};

// Fills `out` with the named archetype's defaults. Returns true if a preset matched (false →
// out left at a neutral Inner-Sphere-ish default, still usable).
inline bool brainArchetypeLookup(mc2::czstring name, BrainArchetypeDefaults& out) {
    // Neutral default (used when name is empty/unknown).
    out = BrainArchetypeDefaults{ 300.0f, 100.0f, 125.0f, 1, 1, 0 };
    if (!name || !name[0]) return false;

    // PlayerControlled — unit is driven by the player; AI switches inert.
    if (std::strstr(name, "PlayerControlled")) {
        out = BrainArchetypeDefaults{ 0.0f, 0.0f, 0.0f, 0, 0, 1 };
        return true;
    }
    // Clan — honor doctrine: tighter help (fight your own duels), hold post.
    if (std::strstr(name, "Clan")) {
        out = BrainArchetypeDefaults{ 350.0f, 80.0f, 100.0f, 0, 1, 0 };
        return true;
    }
    // Mercenary — aggressive: press the attack (do not return to post), readily call/answer help.
    if (std::strstr(name, "Mercenary")) {
        out = BrainArchetypeDefaults{ 300.0f, 120.0f, 120.0f, 1, 0, 0 };
        return true;
    }
    // Inner Sphere — balanced doctrine (also the default for unrecognised faction variants).
    if (std::strstr(name, "InnerSphere")) {
        out = BrainArchetypeDefaults{ 300.0f, 100.0f, 125.0f, 1, 1, 0 };
        return true;
    }
    return false;
}

// Tactic name (from a Brain Tactics block) -> TacticType index (tacordr.h enum, NUM_TACTICS=12),
// -1 = unknown. Maps the discussion #19 tactic aliases onto the engine's tactic enum.
inline int brainTacticNameToIndex(mc2::czstring name) {
    if (!name || !name[0]) return -1;
    if (!std::strcmp(name, "FlankLeft"))                                  return 1;  // TACTIC_FLANK_LEFT
    if (!std::strcmp(name, "FlankRight") || !std::strcmp(name, "Flank"))  return 2;  // TACTIC_FLANK_RIGHT
    if (!std::strcmp(name, "FlankRear")  || !std::strcmp(name, "Rear"))   return 3;  // TACTIC_FLANK_REAR
    if (!std::strcmp(name, "Standard")   || !std::strcmp(name, "StopAndFire")) return 4; // TACTIC_STOP_AND_FIRE
    if (!std::strcmp(name, "Turret"))                                     return 5;  // TACTIC_TURRET
    if (!std::strcmp(name, "Joust"))                                      return 6;  // TACTIC_JOUST
    if (!std::strcmp(name, "IndirectFire") || !std::strcmp(name, "Suppress")) return 7; // TACTIC_INDIRECT_FIRE
    if (!std::strcmp(name, "HullDown"))                                   return 8;  // TACTIC_HULL_DOWN
    if (!std::strcmp(name, "FightingWithdraw") || !std::strcmp(name, "Rearguard")) return 9; // TACTIC_FIGHTING_WITHDRAW
    if (!std::strcmp(name, "Pursue"))                                     return 10; // TACTIC_PURSUE
    if (!std::strcmp(name, "HitAndRun"))                                  return 11; // TACTIC_HIT_AND_RUN
    return -1;
}

// OPORD type token -> id (0..9), 255 = unknown/none. Shared by parser-consumer + runtime.
inline uint8_t brainOpordTypeId(mc2::czstring type) {
    if (!type || !type[0]) return 255;
    if (!std::strcmp(type, "Patrol"))           return 0;
    if (!std::strcmp(type, "Guard"))            return 1;
    if (!std::strcmp(type, "MoveTo"))           return 2;
    if (!std::strcmp(type, "Sentry"))           return 3;
    if (!std::strcmp(type, "Escort"))           return 4;
    if (!std::strcmp(type, "Ambush"))           return 5;
    if (!std::strcmp(type, "Scout"))            return 6;
    if (!std::strcmp(type, "Attack"))           return 7;
    if (!std::strcmp(type, "Withdraw"))         return 8;
    if (!std::strcmp(type, "PlayerControlled")) return 9;
    return 255;
}
