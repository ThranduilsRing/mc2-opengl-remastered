#pragma once
// BRAIN-DECISION-INTENT-QUEUE-1 — rung 5 of Brain decoupling ladder.
// Gate: MC2_BRAIN_INTENT_QUEUE (default OFF).
//
// BrainOrderIntent: a deferred order record emitted by an effect verb handler instead of
// calling setGeneralTacOrder directly.  The commit function (commitBrainIntents) drains
// pendingIntents and calls setGeneralTacOrder once per intent — inline, same thread,
// this rung only.  No threading, no snapshot, no separated commit phase.
//
// Design:
//   EMIT phase  — verb handler builds a BrainOrderIntent + pushes to pendingIntents[];
//                 the forbidden-call guard (no setGeneralTacOrder here) moves to COMMIT.
//   COMMIT phase— commitBrainIntents() drains the buffer and calls setGeneralTacOrder.
//                 commitBrainIntents() is the ONLY path that calls setGeneralTacOrder
//                 when gate ON.
//
// Stub-linkable: POD + stdint only; no TacticalOrder/MechWarrior includes needed.
// The harness (tools/brain_dispatch_harness) does not exercise Apply, so it does not
// need to link brain_order_intent.h beyond the struct definition.

#include <cstdint>
#include "tacordr.h"   // TacticalOrderCode — we store the code by value

// ---------------------------------------------------------------------------
// Order-slot tag: all 6 verbs write to the GENERAL slot (this rung).
// PLAYER / ALARM slots are out of scope until rung 7.
enum class BrainIntentSlot : uint8_t {
    GENERAL = 0,
};

// ---------------------------------------------------------------------------
// DISPATCH-INTENT-CLEARMOVEORDERS-1: intent kind discriminator.
// ORDER (default): orderType is a real TACTICAL_ORDER_* issued via setGeneralTacOrder.
// CLEAR_MOVE: a movement-queue scrub committed via warrior->clearMoveOrders() — NOT a
//   GENERAL-slot order. orderType is unused (TACTICAL_ORDER_NONE). Modelled as an intent
//   (not a direct dispatch call) so the mutation stays inside commitBrainIntents, the sole
//   contract-permitted order/movement mutator.
enum class BrainIntentKind : uint8_t {
    ORDER      = 0,
    CLEAR_MOVE = 1,
    // UNITQUERY-SETTARGETPRIORITY-1: a per-warrior target-priority list write,
    // committed via MechWarrior::setTargetPriority(). policyArgs = {slot,type,p1,p2,p3}.
    TARGET_POLICY = 2,
};

// ---------------------------------------------------------------------------
// BrainOrderIntent: per-emit intent record.
// Emitted by each of the 6 verb handlers when MC2_BRAIN_INTENT_QUEUE=1.
// Committed by commitBrainIntents() immediately after executeSpecialBody_Apply returns.
//
// Fields used per-verb:
//   POWERDOWN / EJECT / GUARD / WITHDRAW  — warriorId, slot, orderType, brainTick, sourceBodyId
//   MOVETO_POINT                           — + waypoint[3]
//   ATTACK_OBJECT                          — + targetWID
//
// Unused fields are zeroed at construction time.
struct BrainOrderIntent {
    int               warriorId;    // vehicleWID of the emitting warrior
    BrainIntentSlot   slot;         // always GENERAL for the 6 current verbs
    BrainIntentKind   kind = BrainIntentKind::ORDER;  // ORDER vs CLEAR_MOVE (DISPATCH-INTENT-CLEARMOVEORDERS-1)
    TacticalOrderCode orderType;    // which TACTICAL_ORDER_* to issue on commit (unused when kind==CLEAR_MOVE)
    int               targetWID;    // ATTACK_OBJECT: target watch-ID; -1 otherwise
    float             waypoint[3];  // MOVETO_POINT: x/y/z destination; zeroed otherwise
    uint32_t          sourceBodyId; // 0 = root body; future: index into SpecialIndex
    uint32_t          brainTick;    // s_brainTickIndex at time of emission
    // UNITQUERY-SETTARGETPRIORITY-1: TARGET_POLICY payload = {slot,type,p1,p2,p3};
    // zeroed otherwise. Passed verbatim to MechWarrior::setTargetPriority() on commit.
    int32_t           policyArgs[5];
};

// ---------------------------------------------------------------------------
// Per-warrior pending intent buffer capacity.
// Fixed cap 4: at most one intent per effect per mission due to once-guards;
// the buffer is sized to hold all 6 verbs if they somehow all fired together
// (defensive), plus 2 spare.  Inline in MechBrainRuntime (no heap).
// UNITQUERY-SETTARGETPRIORITY-1: raised 4→8. carver issues up to ~3-4 TARGET_POLICY
// writes (priority-list slots) per body, possibly alongside another order intent.
static constexpr int kBrainIntentCap = 8;

// ---------------------------------------------------------------------------
// Accessor for the monotonic brain tick counter defined in warrior.cpp.
// Used by the emit path to stamp each intent with the current tick index.
// Declared here so brain_special_dispatch.cpp can call it without #include warrior.cpp.
uint32_t getBrainTickIndex();

// BRAINSPECIAL-FLOW-WAIT-1: deterministic sim-time in milliseconds (scenarioTime*1000).
// Used for WAIT deadline arming/expiry. Defined in warrior.cpp (engine) and in
// tools/brain_dispatch_harness/stubs/brain_tick_stub.cpp (harness, settable).
uint32_t getBrainTimeMs();
