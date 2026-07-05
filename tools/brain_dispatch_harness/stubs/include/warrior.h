#pragma once
#ifndef WARRIOR_H
#define WARRIOR_H
// BRAIN-DISPATCH-HARNESS-1: warrior.h stub
// Provides MechWarrior — only what executeSpecialBody_Apply calls.
// Does NOT include mclib.h or any heavy engine headers.

#include "tacordr.h"
#include "gameobj.h"
#include "mover.h"                // HARNESS-STUB-REPAIR-1: MoverPtr for getVehicle()
#include "mech_brain_runtime.h"   // VarStore, MechBrainRuntime (LEAF header)

class MechWarrior {
public:
    virtual ~MechWarrior() = default;

    // Called by executeSpecialBody_Apply (all 6 effect verbs, gate OFF)
    virtual void setGeneralTacOrder(const TacticalOrder& order) {}

    // DISPATCH-INTENT-CLEARMOVEORDERS-1: called by commitBrainIntents on a CLEAR_MOVE intent.
    virtual void clearMoveOrders() {}

    // UNITQUERY-SETTARGETPRIORITY-1: called by commitBrainIntents on a TARGET_POLICY intent.
    virtual long setTargetPriority(long index, long type, long p1, long p2, long p3) { return 0; }

    // Called by executeSpecialBody_Apply (friendly-fire check)
    virtual TeamPtr getTeam() { return nullptr; }

    // Called by executeSpecialBody_Apply (self-attack check via warrior->getTeam())
    virtual long getWatchID(bool assign = true) { return 0; }

    // BRAIN-DECISION-INTENT-QUEUE-1: called by executeSpecialBody_Apply when
    // MC2_BRAIN_INTENT_QUEUE=1 to get the runtime's intent buffer.
    // The harness exercises TraceOnly + Apply gate-OFF; stub returns nullptr so
    // s_intentQueueEnabled() path is always taken via the (runtime == nullptr) check.
    virtual MechBrainRuntime* getBrainRuntime() { return nullptr; }

    // HARNESS-STUB-REPAIR-1: BRAIN-ENGAGE-1 vehicle accessor. Stub returns nullptr —
    // tickEngageNearest/tickOpordMove early-return on the null vehicle, so the engage
    // path never executes in the harness (and MC2_BRAIN_ENGAGE is never set anyway).
    virtual MoverPtr getVehicle() { return nullptr; }

    // BRAIN-OPORD-COREPATROL-1: patrol arrival poll accessor.
    // Returns a stub TacticalOrder whose status() always returns TACORDER_RUNNING,
    // so the harness never triggers cursor advance (arrival is smoke-only).
    TacticalOrder stubCurOrder;
    virtual TacticalOrder* getCurTacOrder() { return &stubCurOrder; }

    // BRAIN-FSM-1K-A: FSM-capable warrior stub.
    // Subclass FsmMechWarrior (below) provides a real runtime for FSM state tests.
};

// FsmMechWarrior — warrior stub that provides a real MechBrainRuntime for FSM harness tests.
// Used by the fsm-state-transitions fixture via the recording driver.
// BRAIN-FSM-1K-A.
class FsmMechWarrior : public MechWarrior {
public:
    MechBrainRuntime fsmRuntime;
    MechBrainRuntime* getBrainRuntime() override { return &fsmRuntime; }
    // setGeneralTacOrder sink — count calls for gated-verb verification.
    int orderCount = 0;
    void setGeneralTacOrder(const TacticalOrder& order) override { ++orderCount; }
    // DISPATCH-INTENT-CLEARMOVEORDERS-1: count clearMoveOrders() commits for verification.
    int clearMoveCount = 0;
    void clearMoveOrders() override { ++clearMoveCount; }
    // UNITQUERY-SETTARGETPRIORITY-1: record setTargetPriority() commits (count + last args).
    int  targetPriorityCount = 0;
    long lastTargetPriority[5] = {0,0,0,0,0};
    long setTargetPriority(long index, long type, long p1, long p2, long p3) override {
        ++targetPriorityCount;
        lastTargetPriority[0] = index; lastTargetPriority[1] = type;
        lastTargetPriority[2] = p1; lastTargetPriority[3] = p2; lastTargetPriority[4] = p3;
        return 0;
    }
};

#endif // WARRIOR_H
