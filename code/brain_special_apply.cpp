// Brain special dispatch: the effect handlers of executeSpecialBody_Apply -- power, eject, guard, move, attack,
// retreat, clear-move, target priority, patrol and the FSM verbs. The dispatch loop is in brain_special_dispatch.cpp.
#include "brain_special_internal.h"
#include "brain_special_frame.h"
#include "mc2fmt.h"


// Switch the warrior power state (Brain.CorePower false).
void applyCorePower(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect) {
    // BRAIN-DECISION-INTENT-QUEUE-1: gate ON → emit intent; gate OFF → direct call.
    // NOTE: This fires only for the ROOT body. Chained-body POWERDOWN = 1B.
    if (runtime) {
        emitBrainIntent(runtime, wid, TACTICAL_ORDER_POWERDOWN, -1, 0,0,0, 0);
        mc2::fmt::print_err("[BRAIN_INTENT_EMIT] verb=Brain.CorePower order=POWERDOWN tick={:d} wid={:d}\n", mc2::fmt::as_u32(getBrainTickIndex()), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    } else {
        TacticalOrder pdOrder;
        pdOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_POWERDOWN);
        warrior->setGeneralTacOrder(pdOrder);
        mc2::fmt::print_err("[BRAIN_DISPATCH_APPLY] verb=Brain.CorePower effect=POWERDOWN wid={:d}\n", mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    }
    appliedEffect = true;
}

// Eject the pilot (Unit.Eject).
void applyUnitEject(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect) {
    // DISPATCH-EFFECT-UNITEJECT-1: Unit.Eject (or alias coreEject) → TACTICAL_ORDER_EJECT.
    // ROOT BODY ONLY. Fires exactly once (ejectEffectApplied once-guard in warrior.cpp).
    if (runtime) {
        emitBrainIntent(runtime, wid, TACTICAL_ORDER_EJECT, -1, 0,0,0, 0);
        mc2::fmt::print_err("[BRAIN_INTENT_EMIT] verb=Unit.Eject order=EJECT tick={:d} wid={:d}\n", mc2::fmt::as_u32(getBrainTickIndex()), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    } else {
        TacticalOrder ejectOrder;
        ejectOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_EJECT);
        warrior->setGeneralTacOrder(ejectOrder);
        mc2::fmt::print_err("[BRAIN_DISPATCH_APPLY] verb=Unit.Eject effect=EJECT wid={:d}\n", mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    }
    appliedEffect = true;
}

// Order the unit to guard (OPORD.CoreGuard).
void applyCoreGuard(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect) {
    // DISPATCH-EFFECT-COREGUARD-1: OPORD.CoreGuard (or alias coreGuard) → TACTICAL_ORDER_GUARD.
    // ROOT BODY ONLY. Fires exactly once (guardEffectApplied once-guard in warrior.cpp).
    if (runtime) {
        emitBrainIntent(runtime, wid, TACTICAL_ORDER_GUARD, -1, 0,0,0, 0);
        mc2::fmt::print_err("[BRAIN_INTENT_EMIT] verb=OPORD.CoreGuard order=GUARD tick={:d} wid={:d}\n", mc2::fmt::as_u32(getBrainTickIndex()), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    } else {
        TacticalOrder guardOrder;
        guardOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_GUARD);
        warrior->setGeneralTacOrder(guardOrder);
        mc2::fmt::print_err("[BRAIN_DISPATCH_APPLY] verb=OPORD.CoreGuard effect=GUARD wid={:d}\n", mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    }
    appliedEffect = true;
}

// Order the unit to move to a point (OPORD.CoreMoveTo).
void applyCoreMoveTo(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect, BrainVerbFrame& f) {
    auto& vp = f.vp; auto& vpCanon = f.vpCanon;
    // DISPATCH-EFFECT-COREMOVETO-1: OPORD.CoreMoveTo x y z [params=N]
    //
    // Arg parser — bare-numeric form only:
    //   verb text after "OPORD.CoreMoveTo" is split by whitespace.
    //   First three numeric tokens are x y z (float via std::strtof).
    //   Optional trailing "params=N" token (int, default 0) is parsed but not
    //   used this slice (reserved for future movement-mode expansion).
    //   On any parse failure: soft-fail trace, skip order.
    //   NaN guard: std::isnan(x||y||z) → soft-fail trace, skip order.
    const char* args = vpCanon + 16; // points past "OPORD.CoreMoveTo"
    while (*args == ' ') ++args;     // skip leading spaces
    char* endp = nullptr;
    float mx = std::strtof(args, &endp);
    if (endp == args) {
        mc2::fmt::print_err("[BRAIN_DISPATCH_MOVETO_PARSE_FAIL] verb={} wid={:d}\n", mc2::fmt::safe(vp), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    } else {
        args = endp;
        while (*args == ' ') ++args;
        float my = std::strtof(args, &endp);
        if (endp == args) {
            mc2::fmt::print_err("[BRAIN_DISPATCH_MOVETO_PARSE_FAIL] verb={} wid={:d}\n", mc2::fmt::safe(vp), mc2::fmt::as_i32(wid));
            std::fflush(stderr);
        } else {
            args = endp;
            while (*args == ' ') ++args;
            float mz = std::strtof(args, &endp);
            if (endp == args) {
                mc2::fmt::print_err("[BRAIN_DISPATCH_MOVETO_PARSE_FAIL] verb={} wid={:d}\n", mc2::fmt::safe(vp), mc2::fmt::as_i32(wid));
                std::fflush(stderr);
            } else if (std::isnan(mx) || std::isnan(my) || std::isnan(mz)) {
                mc2::fmt::print_err("[BRAIN_DISPATCH_MOVETO_NAN] x={:g} y={:g} z={:g} wid={:d}\n", mx, my, mz, mc2::fmt::as_i32(wid));
                std::fflush(stderr);
            } else if (runtime) {
                // Gate ON: emit intent; commitBrainIntents rebuilds the TacticalOrder.
                emitBrainIntent(runtime, wid, TACTICAL_ORDER_MOVETO_POINT, -1, mx, my, mz, 0);
                mc2::fmt::print_err("[BRAIN_INTENT_EMIT] verb=OPORD.CoreMoveTo order=MOVETO_POINT pos=({:g} {:g} {:g}) tick={:d} wid={:d}\n", mx, my, mz, mc2::fmt::as_u32(getBrainTickIndex()), mc2::fmt::as_i32(wid));
                std::fflush(stderr);
                appliedEffect = true;
            } else {
                // Gate OFF: build TacticalOrder following missiongui.cpp:4161 exactly.
                LocationNode moveNode;
                moveNode.location.x = mx;
                moveNode.location.y = my;
                moveNode.location.z = mz;
                moveNode.run  = true;
                moveNode.next = nullptr;
                TacticalOrder moveOrder;
                moveOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_MOVETO_POINT, false);
                moveOrder.initWayPath(&moveNode);
                moveOrder.moveParams.wait = false;
                moveOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
                moveOrder.pack(nullptr, nullptr);
                warrior->setGeneralTacOrder(moveOrder);
                mc2::fmt::print_err("[BRAIN_DISPATCH_APPLY] verb=OPORD.CoreMoveTo effect=MOVETO_POINT pos=({:g} {:g} {:g}) wid={:d}\n", mx, my, mz, mc2::fmt::as_i32(wid));
                std::fflush(stderr);
                appliedEffect = true;
            }
        }
    }
}

// Order the unit to attack a target (OPORD.CoreAttack).
void applyCoreAttack(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect, BrainVerbFrame& f) {
    auto& vp = f.vp; auto& vpCanon = f.vpCanon;
    // DISPATCH-EFFECT-COREATTACK-1: OPORD.CoreAttack <wid>
    // BRAIN-ALIAS-COREVERBS-1: Brain.CoreAttack (carver_v_enhanced, 571 uses) shares
    // this handler. Both prefixes are exactly 16 chars, so the vpCanon+16 arg parser
    // below is identical for either spelling.
    //
    // Arg parser — bare-integer WID:
    //   Token after "OPORD.CoreAttack" is the decimal WID (strtol).
    //   Zero or negative WID → soft-fail [BRAIN_DISPATCH_ATTACK_BAD_WID].
    //   Non-numeric token → soft-fail [BRAIN_DISPATCH_ATTACK_PARSE_FAIL].
    // Triple guard (single lookup):
    //   1. target not found → [BRAIN_DISPATCH_ATTACK_BAD_WID] reason=not_found
    //   2. target WID == self WID → [BRAIN_DISPATCH_ATTACK_SELF]
    //   3. target same team → [BRAIN_DISPATCH_ATTACK_FRIENDLY]
    // ANTI-PATTERN: do NOT call orderAttackObject — it has forbidden side-effects.
    // Construct TacticalOrder by hand mirroring missiongui.cpp:2805.
    const char* aargs = vpCanon + 16;
    while (*aargs == ' ') ++aargs;
    char* aendp = nullptr;
    long parsedWID = std::strtol(aargs, &aendp, 10);
    if (aendp == aargs) {
        mc2::fmt::print_err("[BRAIN_DISPATCH_ATTACK_PARSE_FAIL] verb={} wid={:d}\n", mc2::fmt::safe(vp), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    } else if (parsedWID <= 0) {
        mc2::fmt::print_err("[BRAIN_DISPATCH_ATTACK_BAD_WID] wid={:d} wid={:d}\n", mc2::fmt::as_i32(parsedWID), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    } else {
        GameObjectPtr target = ObjectManager->getByWatchID(static_cast<long>(parsedWID));
        if (!target) {
            mc2::fmt::print_err("[BRAIN_DISPATCH_ATTACK_BAD_WID] wid={:d} reason=not_found wid={:d}\n", mc2::fmt::as_i32(parsedWID), mc2::fmt::as_i32(wid));
            std::fflush(stderr);
        } else if (target->getWatchID() == static_cast<long>(wid)) {
            mc2::fmt::print_err("[BRAIN_DISPATCH_ATTACK_SELF] wid={:d} wid={:d}\n", mc2::fmt::as_i32(parsedWID), mc2::fmt::as_i32(wid));
            std::fflush(stderr);
        } else if (target->getTeam() == warrior->getTeam()) {
            mc2::fmt::print_err("[BRAIN_DISPATCH_ATTACK_FRIENDLY] wid={:d} wid={:d}\n", mc2::fmt::as_i32(parsedWID), mc2::fmt::as_i32(wid));
            std::fflush(stderr);
        } else if (runtime) {
            // Gate ON: emit intent; commitBrainIntents rebuilds the TacticalOrder.
            emitBrainIntent(runtime, wid, TACTICAL_ORDER_ATTACK_OBJECT, static_cast<int>(parsedWID), 0,0,0, 0);
            mc2::fmt::print_err("[BRAIN_INTENT_EMIT] verb=OPORD.CoreAttack order=ATTACK_OBJECT targetWID={:d} tick={:d} wid={:d}\n", mc2::fmt::as_i32(parsedWID), mc2::fmt::as_u32(getBrainTickIndex()), mc2::fmt::as_i32(wid));
            std::fflush(stderr);
            appliedEffect = true;
        } else {
            // Gate OFF: construct TacticalOrder by hand — mirror missiongui.cpp:2805.
            // FORBIDDEN: orderAttackObject, setAttackTarget, setSituationOpenFire.
            TacticalOrder attackOrder;
            attackOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_ATTACK_OBJECT, false);
            attackOrder.targetWID = static_cast<long>(parsedWID);
            attackOrder.attackParams.type   = ATTACK_TO_DESTROY;
            attackOrder.attackParams.method = ATTACKMETHOD_RANGED;
            attackOrder.attackParams.range  = FIRERANGE_OPTIMAL;
            attackOrder.attackParams.pursue = true;
            // TACTIC-WEIGHTS-B: wire selected tactic. Local `runtime` is null on this
            // gate-OFF path, so read selectedTactic via warrior->getBrainRuntime().
            if (s_tacticWeightsBEnabled()) {
                MechBrainRuntime* rtB = warrior->getBrainRuntime();
                if (rtB) {
                    attackOrder.attackParams.tactic = static_cast<TacticType>(rtB->selectedTactic);
                    mc2::fmt::print_err("[BRAIN_TACTIC_B] applied tactic={:d} wid={:d}\n", mc2::fmt::as_i32(rtB->selectedTactic), mc2::fmt::as_i32(wid));
                    std::fflush(stderr);
                }
            }
            attackOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
            warrior->setGeneralTacOrder(attackOrder);
            mc2::fmt::print_err("[BRAIN_DISPATCH_APPLY] verb=OPORD.CoreAttack effect=ATTACK_OBJECT targetWID={:d} wid={:d}\n", mc2::fmt::as_i32(parsedWID), mc2::fmt::as_i32(wid));
            std::fflush(stderr);
            appliedEffect = true;
        }
    }
}

// Order the unit to retreat (Unit.Retreat).
void applyUnitRetreat(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect) {
    // DISPATCH-EFFECT-UNITRETREAT-1: Unit.Retreat (or alias coreRetreat) → TACTICAL_ORDER_WITHDRAW.
    // ROOT BODY ONLY. Fires exactly once (retreatEffectApplied once-guard in warrior.cpp).
    if (runtime) {
        emitBrainIntent(runtime, wid, TACTICAL_ORDER_WITHDRAW, -1, 0,0,0, 0);
        mc2::fmt::print_err("[BRAIN_INTENT_EMIT] verb=Unit.Retreat order=WITHDRAW tick={:d} wid={:d}\n", mc2::fmt::as_u32(getBrainTickIndex()), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    } else {
        TacticalOrder retreatOrder;
        retreatOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_WITHDRAW, false);
        warrior->setGeneralTacOrder(retreatOrder);
        mc2::fmt::print_err("[BRAIN_DISPATCH_APPLY] verb=Unit.Retreat effect=WITHDRAW wid={:d}\n", mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    }
    appliedEffect = true;
}

// Scrub the engine movement queue (Unit.ClearMoveOrders).
void applyClearMoveOrders(int wid, MechBrainRuntime* runtime) {
    // DISPATCH-INTENT-CLEARMOVEORDERS-1: scrub the engine movement queue. Intent-ONLY.
    // Prefix-match (20 chars): carver emits "Unit.ClearMoveOrders 1" — the trailing DO
    // arg is ABL syntax noise; clearMoveOrders() takes no parameter.
    // The warrior->clearMoveOrders() call lives in commitBrainIntents (the sole
    // contract-permitted mutator); calling it here would violate the FORBIDDEN-CALL
    // contract (unlisted movement function) and bypass the WID-ordered commit phase.
    // Does NOT set appliedEffect — this is a prologue scrub, not a GENERAL-slot order, so
    // it must not suppress the synthetic HOLD or claim the slot. patrolActive / FSM state /
    // Vars / target are intentionally left UNTOUCHED (frozen scope: movement-only).
    if (runtime) {
        emitBrainClearMoveIntent(runtime, wid);
        mc2::fmt::print_err("[BRAIN_INTENT_EMIT] verb=Unit.ClearMoveOrders order=CLEAR_MOVE tick={:d} wid={:d}\n", mc2::fmt::as_u32(getBrainTickIndex()), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    } else {
        // Intent queue OFF: cannot scrub here (direct call forbidden). Trace + no-op,
        // mirroring the patrol NO_QUEUE path. Live use requires the TechBrain policy.
        mc2::fmt::print_err("[BRAIN_CLEARMOVE_NO_QUEUE] wid={:d}: requires the TechBrain policy\n", mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    }
}

// Write a target-priority list entry (UnitQuery.SetTargetpriority).
void applySetTargetPriority(int wid, MechBrainRuntime* runtime, BrainVerbFrame& f) {
    auto& vp = f.vp; auto& vpCanon = f.vpCanon;
    // UNITQUERY-SETTARGETPRIORITY-1: per-warrior target-priority list write.
    // Format: UnitQuery.SetTargetpriority <slot> <type-sym> <p1> <p2> <criteria-expr>
    //   slot/p1/p2 = bare ints; type = TARGET_PRIORITY_* symbol; criteria = the rest
    //   of the line, a '+'-joined CONTACT_CRITERIA_* expression (or bare -1).
    // carver passes SYMBOLS (ABL pre-compiled them; our RawScan sees raw text), so the
    // args are resolved by brain_symbol_resolve. Intent-ONLY: setTargetPriority() runs
    // in commitBrainIntents (sole permitted mutator). Does NOT set appliedEffect — this
    // is target-selection config, not a GENERAL-slot order; must not suppress HOLD.
    const char* a = vpCanon + 27;
    long args5[5] = {0, 0, 0, 0, 0};
    bool parsed = true;
    for (int fi = 0; fi < 4 && parsed; ++fi) {
        while (*a == ' ' || *a == '\t') ++a;
        const char* ts = a;
        while (*a && *a != ' ' && *a != '\t') ++a;
        size_t tl = static_cast<size_t>((a - ts));
        char tok[64];
        if (tl == 0 || tl >= sizeof(tok)) { parsed = false; break; }
        std::memcpy(tok, ts, tl); tok[tl] = '\0';
        const auto v = brainResolveSymbolToken(tok);
        if (!v) { parsed = false; break; }
        args5[fi] = *v;
    }
    if (parsed) {
        while (*a == ' ' || *a == '\t') ++a;   // 5th field = rest-of-line expression
        const auto crit = brainResolveIntExpr(a);
        if (!crit) parsed = false;
        else args5[4] = *crit;
    }
    if (!parsed) {
        mc2::fmt::print_err("[BRAIN_DISPATCH_TARGETPRIORITY_PARSE_FAIL] verb={} wid={:d}\n", mc2::fmt::safe(vp), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    } else if (runtime) {
        emitBrainTargetPolicyIntent(runtime, wid, args5[0], args5[1], args5[2], args5[3], args5[4]);
        mc2::fmt::print_err("[BRAIN_INTENT_EMIT] verb=UnitQuery.SetTargetpriority order=TARGET_POLICY slot={:d} type={:d} p1={:d} p2={:d} crit={:d} tick={:d} wid={:d}\n", mc2::fmt::as_i32(args5[0]), mc2::fmt::as_i32(args5[1]), mc2::fmt::as_i32(args5[2]), mc2::fmt::as_i32(args5[3]), mc2::fmt::as_i32(args5[4]), mc2::fmt::as_u32(getBrainTickIndex()), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    } else {
        mc2::fmt::print_err("[BRAIN_TARGETPRIORITY_NO_QUEUE] wid={:d}: requires the TechBrain policy\n", mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    }
}

// Start a patrol over up to eight waypoints (OPORD.CorePatrol).
void applyCorePatrol(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect, BrainVerbFrame& f) {
    auto& vpCanon = f.vpCanon;
    // BRAIN-OPORD-COREPATROL-1: OPORD.CorePatrol x1 y1 z1 x2 y2 z2 ... [loop|once]
    //
    // FIRST-DISPATCH only (patrolActive==false guard):
    //   Parse up to 8 waypoint triples + optional trailing "loop"/"once" keyword.
    //   Set patrolWaypointCount, patrolWaypointIndex=0, patrolLoop, patrolActive=true.
    //   Emit MOVETO_POINT to waypoints[0] via emitBrainIntent (gate ON) or direct
    //   setGeneralTacOrder (gate OFF).
    //
    // RE-EMIT BYPASS: patrol does NOT set/check moveToEffectApplied.  patrolActive +
    // the per-tick tickPatrolAdvance cursor-advance are the re-emit guard.
    //
    // Emit path: emitBrainIntent (gate ON only).
    // NOTE: patrol does NOT call setGeneralTacOrder() directly here —
    //   this keeps the relaxed_guard_doc call-site count at 6 (unchanged).
    //   Without MC2_BRAIN_INTENT_QUEUE=1, patrol emits a trace-only warning on first
    //   dispatch.  Per-tick tickPatrolAdvance also uses emitBrainIntent exclusively.
    if (!warrior) {
        mc2::fmt::print_err("[BRAIN_PATROL_PARSE_FAIL] no warrior wid={:d}\n", mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    } else {
        MechBrainRuntime* prt = warrior->getBrainRuntime();
        if (!prt) {
            mc2::fmt::print_err("[BRAIN_PATROL_PARSE_FAIL] no runtime wid={:d}\n", mc2::fmt::as_i32(wid));
            std::fflush(stderr);
        } else if (prt->patrolActive) {
            // Already active — this is a re-tick of a body that has CorePatrol.
            // The per-tick advance is handled by tickPatrolAdvance() in warrior.cpp.
            // Do NOT re-parse; do NOT re-emit waypoint[0].  Trace only.
            mc2::fmt::print_err("[BRAIN_PATROL_ACTIVE] already running index={:d} count={:d} wid={:d}\n",
                         mc2::fmt::as_u32(static_cast<unsigned>(prt->patrolWaypointIndex)), mc2::fmt::as_u32(static_cast<unsigned>(prt->patrolWaypointCount)), mc2::fmt::as_i32(wid));
            std::fflush(stderr);
            appliedEffect = true;
        } else {
            // First dispatch: parse waypoints.
            const char* pargs = vpCanon + 16;
            while (*pargs == ' ') ++pargs;
            uint8_t wpCount = 0;
            float wpBuf[8][3];
            bool parseOk = true;
            while (wpCount < 8) {
                // Try to parse a float triplet.
                char* ep0 = nullptr;
                float x = std::strtof(pargs, &ep0);
                if (ep0 == pargs) break;  // no more numeric tokens
                pargs = ep0; while (*pargs == ' ') ++pargs;
                char* ep1 = nullptr;
                float y = std::strtof(pargs, &ep1);
                if (ep1 == pargs) { parseOk = false; break; }
                pargs = ep1; while (*pargs == ' ') ++pargs;
                char* ep2 = nullptr;
                float z = std::strtof(pargs, &ep2);
                if (ep2 == pargs) { parseOk = false; break; }
                pargs = ep2; while (*pargs == ' ') ++pargs;
                if (std::isnan(x) || std::isnan(y) || std::isnan(z)) {
                    mc2::fmt::print_err("[BRAIN_PATROL_NAN] wp={:d} x={:g} y={:g} z={:g} wid={:d} (skipped)\n", mc2::fmt::as_u32(static_cast<unsigned>(wpCount)), x, y, z, mc2::fmt::as_i32(wid));
                    std::fflush(stderr);
                    continue;
                }
                wpBuf[wpCount][0] = x;
                wpBuf[wpCount][1] = y;
                wpBuf[wpCount][2] = z;
                ++wpCount;
            }
            // Parse trailing loop/once keyword (default loop).
            bool loopMode = true;
            if (*pargs != '\0') {
                if (std::strncmp(pargs, "once", 4) == 0)
                    loopMode = false;
                // "loop" or any other trailing token → default loop=true.
            }
            if (!parseOk || wpCount < 2) {
                mc2::fmt::print_err("[BRAIN_PATROL_PARSE_FAIL] count={:d} parseOk={:d} wid={:d} (need >=2 waypoints)\n", mc2::fmt::as_u32(static_cast<unsigned>(wpCount)), mc2::fmt::as_i32(static_cast<int>(parseOk)), mc2::fmt::as_i32(wid));
                std::fflush(stderr);
            } else {
                // Commit parsed state to runtime.
                prt->patrolWaypointCount = wpCount;
                prt->patrolWaypointIndex = 0;
                for (uint8_t wi = 0; wi < wpCount; ++wi) {
                    prt->patrolWaypoints[wi][0] = wpBuf[wi][0];
                    prt->patrolWaypoints[wi][1] = wpBuf[wi][1];
                    prt->patrolWaypoints[wi][2] = wpBuf[wi][2];
                }
                prt->patrolLoop   = loopMode;
                prt->patrolActive = true;
                prt->patrolStarted = true;  // PATROL-DRIVE-1: this path emits waypoint[0] below; skip the tickPatrolAdvance kick.
                mc2::fmt::print_err("[BRAIN_PATROL_START] count={:d} loop={:d} wid={:d}\n",
                             mc2::fmt::as_u32(static_cast<unsigned>(wpCount)), mc2::fmt::as_i32(static_cast<int>(loopMode)), mc2::fmt::as_i32(wid));
                std::fflush(stderr);
                // Emit MOVETO_POINT to waypoints[0].
                float wx = prt->patrolWaypoints[0][0];
                float wy = prt->patrolWaypoints[0][1];
                float wz = prt->patrolWaypoints[0][2];
                if (runtime) {
                    // Gate ON: emit intent; commitBrainIntents builds the TacticalOrder.
                    emitBrainIntent(runtime, wid, TACTICAL_ORDER_MOVETO_POINT, -1, wx, wy, wz, 0);
                    mc2::fmt::print_err("[BRAIN_PATROL_EMIT] index=0 pos=({:g} {:g} {:g}) tick={:d} wid={:d}\n", wx, wy, wz, mc2::fmt::as_u32(getBrainTickIndex()), mc2::fmt::as_i32(wid));
                    std::fflush(stderr);
                } else {
                    // Gate OFF (no intent queue): patrol cannot issue MOVETO_POINT here
                    // without adding a 7th setGeneralTacOrder call-site (relaxed_guard_doc
                    // contract requires count=6).  Emit a one-time warning.
                    // Live-engine usage requires the TechBrain policy for patrol orders.
                    mc2::fmt::print_err("[BRAIN_PATROL_NO_QUEUE] wid={:d}: patrol requires the TechBrain policy for order emission\n", mc2::fmt::as_i32(wid));
                    std::fflush(stderr);
                }
                appliedEffect = true;
            }
        }
    }
}

// Transition the brain state and end the body (Unit.SetState).
bool applySetState(MechWarrior* warrior, int wid, BrainVerbFrame& f) {
    auto& vpCanon = f.vpCanon;
    // BRAIN-FSM-1K-A: Unit.SetState "x" — explicit FSM transition.
    // prevState←currentState; currentState←x; early return (no order call).
    // NOTE: NOT an order call — relaxed_guard_doc setGeneralTacOrder count = 6 (unchanged).
    MechBrainRuntime* fsm_rt = warrior ? warrior->getBrainRuntime() : nullptr;
    if (fsm_rt) {
        // Parse quoted state name: verb is "Unit.SetState \"stateName\""
        const char* sp = vpCanon + 13;
        while (*sp == ' ' || *sp == '\t') ++sp;
        if (*sp == '"') ++sp;
        char newState[32] = {};
        int ni = 0;
        while (*sp && *sp != '"' && ni < 31) newState[ni++] = *sp++;
        newState[ni] = '\0';
        std::strncpy(fsm_rt->prevState, fsm_rt->currentState, 31);
        fsm_rt->prevState[31] = '\0';
        std::strncpy(fsm_rt->currentState, newState, 31);
        fsm_rt->currentState[31] = '\0';
        mc2::fmt::print_err("[BRAIN_FSM_SETSTATE] from={} to={} wid={:d}\n", mc2::fmt::safe(fsm_rt->prevState), mc2::fmt::safe(fsm_rt->currentState), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    }
    return true;  // early exit — no subsequent verbs after SetState
    return false;
}

// Swap back to the previous brain state and end the body (Unit.SetStatePrev).
bool applySetStatePrev(MechWarrior* warrior, int wid) {
    // BRAIN-FSM-1K-A: Unit.SetStatePrev — transBack equivalent, swaps currentState↔prevState.
    // Early return (no order call).
    MechBrainRuntime* fsm_rt = warrior ? warrior->getBrainRuntime() : nullptr;
    if (fsm_rt) {
        char tmp[32];
        std::memcpy(tmp, fsm_rt->prevState, 32);
        std::strncpy(fsm_rt->prevState, fsm_rt->currentState, 31);
        fsm_rt->prevState[31] = '\0';
        std::memcpy(fsm_rt->currentState, tmp, 32);
        mc2::fmt::print_err("[BRAIN_FSM_SETSTATEPREV] now={} wid={:d}\n", mc2::fmt::safe(fsm_rt->currentState), mc2::fmt::as_i32(wid));
        std::fflush(stderr);
    }
    return true;  // early exit — no subsequent verbs after SetStatePrev
    return false;
}

// Close the state gate unless the brain is in the named state (Unit.InState).
void applyInState(MechWarrior* warrior, int wid, bool& stateGateOpen, BrainVerbFrame& f) {
    auto& vpCanon = f.vpCanon;
    // BRAIN-FSM-1K-A: Unit.InState "x" — guard verb; does NOT call any order function.
    // If currentState != x, stateGateOpen←false; subsequent verbs in this body are skipped.
    MechBrainRuntime* fsm_rt = warrior ? warrior->getBrainRuntime() : nullptr;
    const char* sp = vpCanon + 12;
    while (*sp == ' ' || *sp == '\t') ++sp;
    if (*sp == '"') ++sp;
    char testState[32] = {};
    int ni = 0;
    while (*sp && *sp != '"' && ni < 31) testState[ni++] = *sp++;
    testState[ni] = '\0';
    const char* cur = fsm_rt ? fsm_rt->currentState : "";
    int match = (std::strcmp(cur, testState) == 0) ? 1 : 0;
    if (!match) stateGateOpen = false;
    mc2::fmt::print_err("[BRAIN_FSM_INSTATE] state={} match={:d} wid={:d}\n", mc2::fmt::safe(testState), mc2::fmt::as_i32(match), mc2::fmt::as_i32(wid));
    std::fflush(stderr);
}

// Close the state gate when the brain is in the named state (Unit.NotInState).
void applyNotInState(MechWarrior* warrior, int wid, bool& stateGateOpen, BrainVerbFrame& f) {
    auto& vpCanon = f.vpCanon;
    // BRAIN-FSM-1K-A: Unit.NotInState "x" — inverse guard; does NOT call any order function.
    // If currentState == x, stateGateOpen←false; subsequent verbs in this body are skipped.
    MechBrainRuntime* fsm_rt = warrior ? warrior->getBrainRuntime() : nullptr;
    const char* sp = vpCanon + 15;
    while (*sp == ' ' || *sp == '\t') ++sp;
    if (*sp == '"') ++sp;
    char testState[32] = {};
    int ni = 0;
    while (*sp && *sp != '"' && ni < 31) testState[ni++] = *sp++;
    testState[ni] = '\0';
    const char* cur = fsm_rt ? fsm_rt->currentState : "";
    int match = (std::strcmp(cur, testState) == 0) ? 1 : 0;
    if (match) stateGateOpen = false;
    mc2::fmt::print_err("[BRAIN_FSM_NOTINSTATE] state={} match={:d} wid={:d}\n", mc2::fmt::safe(testState), mc2::fmt::as_i32(match), mc2::fmt::as_i32(wid));
    std::fflush(stderr);
}

// Transition the brain state when a unit variable matches (Unit.SetStateIf).
bool applySetStateIf(MechWarrior* warrior, int wid, VarStore* varStore, bool varGate, BrainVerbFrame& f) {
    auto& vpCanon = f.vpCanon;
    // BRAIN-FSM-1K-B: Unit.SetStateIf "stateName" "varKey" "varValue"
    // If Var store (unit scope) varKey == varValue → SetState transition (prevState←currentState; currentState←stateName); early return.
    // If no match → no-op; body continues.
    // Requires MC2_BRAIN_DISPATCH_VAR=1. If VAR gate OFF, emits one-time warning and is INERT.
    //
    // Parse 3 quoted args: stateName, varKey, varValue.
    MechBrainRuntime* fsm_rt = warrior ? warrior->getBrainRuntime() : nullptr;
    // Requires-VAR guard (mirror 1D-M pattern): FSM ON but VAR OFF → inert + one-time warning.
    if (!varGate) {
        static bool s_setstateif_var_warn = false;
        if (!s_setstateif_var_warn) {
            s_setstateif_var_warn = true;
            mc2::fmt::print_err("[BRAIN_FSM_SETSTATEIF] WARNING: Unit.SetStateIf requires the TechBrain policy — verb is INERT\n");
            std::fflush(stderr);
        }
        // INERT: no transition, body continues.
    } else {
        // Parse three quoted args: "stateName" "varKey" "varValue"
        const char* sp = vpCanon + 15;
        while (*sp == ' ' || *sp == '\t') ++sp;

        // Arg 1: stateName
        char stateName[32] = {};
        if (*sp == '"') ++sp;
        int ni = 0;
        while (*sp && *sp != '"' && ni < 31) stateName[ni++] = *sp++;
        stateName[ni] = '\0';
        if (*sp == '"') ++sp;
        while (*sp == ' ' || *sp == '\t') ++sp;

        // Arg 2: varKey
        char varKey[32] = {};
        if (*sp == '"') ++sp;
        ni = 0;
        while (*sp && *sp != '"' && ni < 31) varKey[ni++] = *sp++;
        varKey[ni] = '\0';
        if (*sp == '"') ++sp;
        while (*sp == ' ' || *sp == '\t') ++sp;

        // Arg 3: varValue (expected value)
        char varValue[32] = {};
        if (*sp == '"') ++sp;
        ni = 0;
        while (*sp && *sp != '"' && ni < 31) varValue[ni++] = *sp++;
        varValue[ni] = '\0';

        // Read unit-scope var (unit-scope only; mission-scope deferred to future slice).
        const char* curVal = (varStore && varKey[0] != '\0')
            ? varStore->get(varKey, VarScope::Unit)
            : "0";

        int match = (std::strcmp(curVal, varValue) == 0) ? 1 : 0;

        if (match) {
            // Perform SetState transition: prevState←currentState; currentState←stateName.
            if (fsm_rt) {
                std::strncpy(fsm_rt->prevState, fsm_rt->currentState, 31);
                fsm_rt->prevState[31] = '\0';
                std::strncpy(fsm_rt->currentState, stateName, 31);
                fsm_rt->currentState[31] = '\0';
                mc2::fmt::print_err("[BRAIN_FSM_SETSTATEIF] var={} cur={} want={} match=1 from={} to={} wid={:d}\n", mc2::fmt::safe(varKey), mc2::fmt::safe(curVal), mc2::fmt::safe(varValue), mc2::fmt::safe(fsm_rt->prevState), mc2::fmt::safe(fsm_rt->currentState), mc2::fmt::as_i32(wid));
            } else {
                mc2::fmt::print_err("[BRAIN_FSM_SETSTATEIF] var={} cur={} want={} match=1 from=(no-rt) to={} wid={:d}\n", mc2::fmt::safe(varKey), mc2::fmt::safe(curVal), mc2::fmt::safe(varValue), mc2::fmt::safe(stateName), mc2::fmt::as_i32(wid));
            }
            std::fflush(stderr);
            return true;  // early exit on match — same semantics as SetState
        } else {
            mc2::fmt::print_err("[BRAIN_FSM_SETSTATEIF] var={} cur={} want={} match=0 wid={:d}\n", mc2::fmt::safe(varKey), mc2::fmt::safe(curVal), mc2::fmt::safe(varValue), mc2::fmt::as_i32(wid));
            std::fflush(stderr);
            // No transition; body continues.
        }
    }
    return false;
}
