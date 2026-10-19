#pragma once
// BRAIN-WORLD-SNAPSHOT-1 — rung 6 of the Brain decoupling ladder.
//
// BrainWorldSnapshot is a per-warrior POD snapshot of the world state read
// by the emit phase (executeSpecialBody_Apply + once-guards in warrior.cpp).
// Captured on the main thread right before the once-guard check block runs.
//
// Gate: MC2_BRAIN_SNAPSHOT (default OFF).
//   Gate OFF: once-guards read live runtime->*EffectApplied directly (byte-identical).
//   Gate ON:  once-guards read snapshot.effectApplied[N]; live runtime writes are unchanged.
//
// IDs-not-pointers discipline: all references to game objects are WatchIDs (int32_t),
// not pointers. Pointers are unsafe to hold across ticks; WIDs are stable references
// resolved at use time via ObjectManager::getByWatchID.
//
// Deferred to rung 7/8:
//   - target/contact table snapshot (ATTACK target team + existence check still reads live)
//   - PLAYER/ALARM slot reads
//   - mission-global shared state (commander, team table)
//
// POD + stdint only — no engine headers, stub-linkable.

#include <stdint.h>

struct BrainWorldSnapshot {
    int32_t  warriorId;              // WatchID of this warrior (the attacker side)
    int32_t  teamId;                 // warrior->getTeam() token (opaque int; 0 = none)
    uint8_t  isDisabled;             // warrior->isDisabled() (1 = disabled)
    uint8_t  commanderIsHome;        // warrior->getCommander() == homeCommander (1 = yes)
    int32_t  mainGoalObjectWID;      // mainGoal object WatchID (0 if none)
    float    mainGoalLocation[3];    // mainGoal location XYZ
    float    mainGoalControlRadius;  // mainGoal controlRadius
    int32_t  attackOrderTargetWID;   // attackOrders.targetWID (0 if none)
    uint8_t  effectApplied[6];       // per-effect once-guard flags (copied from runtime)
                                     //   [0] = POWERDOWN  (dispatchEffectApplied)
                                     //   [1] = EJECT      (ejectEffectApplied)
                                     //   [2] = GUARD      (guardEffectApplied)
                                     //   [3] = MOVETO     (moveToEffectApplied)
                                     //   [4] = ATTACK     (attackEffectApplied)
                                     //   [5] = RETREAT    (retreatEffectApplied)
    uint32_t brainTick;              // frame/tick counter for debugging (getBrainTickIndex())
};

// effectApplied[] index constants — use these instead of bare integers.
enum class BrainSnapshotEffectIdx {
    BSNAPFX_POWERDOWN = 0,
    BSNAPFX_EJECT     = 1,
    BSNAPFX_GUARD     = 2,
    BSNAPFX_MOVETO    = 3,
    BSNAPFX_ATTACK    = 4,
    BSNAPFX_RETREAT   = 5,
    BSNAPFX_COUNT     = 6
};
using enum BrainSnapshotEffectIdx;
