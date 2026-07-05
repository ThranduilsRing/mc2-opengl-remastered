// TECHSCRIPT-SPECIAL-DISPATCH-1A — trace-only dispatcher shell.
// TECHSCRIPT-SPECIAL-DISPATCH-1B — first real effect: Brain.CorePower false → POWERDOWN.
// TECHSCRIPT-SPECIAL-DISPATCH-1C — FSM-TODO surfacer: raw line scan for TODO comments.
// TECHSCRIPT-DISPATCH-1D — per-unit Var namespace + minimal store.
// DISPATCH-LOADER-RAW-1 — replace FitIniFile body reader with raw brace-block scanner
//   that correctly loads inline-quoted verb args (DO Var.Set "foo" 1).
//   FitIniFile fallback retained for legacy [BrainSpecial]/[Body]/DO0=... bracket form.
// TECHSCRIPT-CALL-CHAIN-1A — TechSpecial.Call chaining (parse + index + trace ONLY).
//   Gate: MC2_BRAIN_DISPATCH_CALL=1 (requires MC2_BRAIN_DISPATCH=1; warns + inert otherwise).
//   Index: parseBrainSpecialBody_RawScan populates a SpecialIndex (key→BrainSpecialBody*).
//   Dispatch: TechSpecial.Call "<key>" verb recurses into the named body (trace only).
//   Depth limit: 8 (kCallChainMaxDepth). Per-tick visited set (std::vector<std::string>).
//   NO effects in chained bodies: Brain.CorePower false in a CALLED body traces but does NOT
//   call setGeneralTacOrder. Chained-effect execution is deferred to CALL-CHAIN-1B.
//   FORBIDDEN-CALL CONTRACT (chained dispatch): pure trace + recursion. No order functions.
//   PURE VERB-STREAM COMPOSITION: chaining introduces NO new order function calls.
//
// Gates: MC2_BRAIN_DISPATCH (parse+trace), MC2_BRAIN_DISPATCH_APPLY (effect layer, requires DISPATCH),
//        MC2_BRAIN_DISPATCH_FSM_TODO (FSM TODO surfacer, requires DISPATCH),
//        MC2_BRAIN_DISPATCH_VAR (per-unit Var store, requires DISPATCH, default OFF),
//        MC2_BRAIN_DISPATCH_CALL (call chaining, requires DISPATCH, default OFF).
//
// FORBIDDEN-CALL GUARD — executeSpecialBody_TraceOnly / executeSpecialBody_TraceOnlyChained (1A):
// Calls ONLY fprintf + loop + recursive chained dispatch. DOES NOT call setGeneralTacOrder
// or any order/movement function. Verified: no warrior pointer, no MechWarrior type here.
//
// RELAXED-CALL GUARD — executeSpecialBody_Apply (1B + UNITEJECT + COREGUARD + COREMOVETO + COREATTACK + UNITRETREAT):
// Permitted order calls: warrior->setGeneralTacOrder() for:
//   Brain.CorePower false                 → TACTICAL_ORDER_POWERDOWN
//   Unit.Eject (or alias coreEject)       → TACTICAL_ORDER_EJECT
//   OPORD.CoreGuard                       → TACTICAL_ORDER_GUARD
//   OPORD.CoreMoveTo x y z [params=N]     → TACTICAL_ORDER_MOVETO_POINT (NaN-coord soft-fail guard)
//   OPORD.CoreAttack <target_wid>         → TACTICAL_ORDER_ATTACK_OBJECT (triple-guard: bad-WID/self/friendly)
//   Unit.Retreat (or alias coreRetreat)   → TACTICAL_ORDER_WITHDRAW (no-arg, engine owns withdraw nav)
// All fire IN THE ROOT BODY ONLY. Chained bodies (via TechSpecial.Call) are trace-only in 1A.
// BRAIN-FSM-1K-A NOTE: Unit.SetState / Unit.SetStatePrev / Unit.InState / Unit.NotInState are NOT
// order calls — they write currentState/prevState fields or set stateGateOpen only.
// FSM verbs NEVER call setGeneralTacOrder. The relaxed_guard_doc checker count = 6 (unchanged).
// STILL FORBIDDEN: orderAttackObject (forbidden side-effects), setAttackTarget, setSituationOpenFire,
// setPlayerTacOrder, setAlarmTacOrder, requestHelp, requestTarget, calcTacOrder,
// coreMoveTo (the C++ Mover method, distinct from the OPORD DSL verb), setMainGoal,
// clearCurTacOrder, any movement/attack/OPORD-advance/commander function NOT listed above.
// DISPATCH-EFFECT-UNITRETREAT-1: Unit.Retreat + coreRetreat alias added (no-arg, pure once-guard).
// DISPATCH-INTENT-CLEARMOVEORDERS-1: clearMoveOrders() is a movement function and therefore
// FORBIDDEN in this dispatch function. Unit.ClearMoveOrders emits a CLEAR_MOVE intent ONLY;
// the single permitted warrior->clearMoveOrders() call lives in commitBrainIntents. The
// check_brain_relaxed_guard_doc.py checker enforces that clearMoveOrders appears nowhere else.
// UNITQUERY-SETTARGETPRIORITY-1: same model for warrior->setTargetPriority() — the verb
// emits a TARGET_POLICY intent ONLY; the single permitted setTargetPriority() call lives in
// commitBrainIntents; the checker enforces it appears nowhere else in dispatch.
//
// FSM-TODO SCANNER (1C — scanFsmTodosFromFile):
// Calls ONLY std::ifstream + std::regex + fprintf. NO order functions, NO movement/attack/OPORD calls.
// Verified by inspection: no warrior pointer, no MechWarrior type, no tac-order writes.
//
// VAR HANDLER CONTRACT (1D — handleVarSet / handleVarGet):
// Calls ONLY store->set/store->get + fprintf/fflush. ZERO calls to setGeneralTacOrder
// or any order/movement/attack/OPORD function.
// Verified by inspection: no warrior pointer, no tac-order type, no slot writes.

#include "brain_special_dispatch.h"
#include "brain_symbol_resolve.h"    // UNITQUERY-SETTARGETPRIORITY-1: symbolic-arg resolver
#include "brain_mission_var_store.h" // TECHSCRIPT-DISPATCH-1D-M: MissionVarStore global
#include "brain_world_snapshot.h" // BRAIN-WORLD-SNAPSHOT-1: BrainWorldSnapshot POD + gate
#include "warrior.h"   // MechWarrior — needed for executeSpecialBody_Apply setGeneralTacOrder call
                       // warrior.h includes mech_brain_runtime.h which defines VarStore + VarScope
#include "tacordr.h"   // TacticalOrder, TACTICAL_ORDER_POWERDOWN, ORDER_ORIGIN_SELF
#include "gameobj.h"   // GameObjectPtr, GameObject::getWatchID/getTeam — DISPATCH-EFFECT-COREATTACK-1
#include "objmgr.h"    // ObjectManager (global) — DISPATCH-EFFECT-COREATTACK-1
#include "mover.h"     // Mover::getContacts / getVehicle MoverPtr — BRAIN-ENGAGE-1
#include "dcontact.h"  // CONTACT_SORT_DISTANCE, MAX_CONTACTS_PER_SENSOR — BRAIN-ENGAGE-1
#include "contact.h"   // SensorSystem::getEffectiveRange — BRAIN-ENGAGE-1 detection range
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include "inifile.h"   // FitIniFile — same header used by _ai.fit loader

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D-M: MissionVarStore global definition.
// Mission-ephemeral: reset via resetMissionVarStore() at each mission load.
// Not serialized.
MissionVarStore g_missionVarStore;

// Gate helper for MC2_BRAIN_VAR_MISSION (1D-M).
// Requires MC2_BRAIN_DISPATCH_VAR=1 to be meaningful, but has its own separate gate
// so it can be turned on independently. Warns if turned on without DISPATCH_VAR.
static bool s_missionVarGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_VAR_MISSION");
        if (v && std::atoi(v) != 0) {
            const char* dv = std::getenv("MC2_BRAIN_DISPATCH_VAR");
            if (!dv || std::atoi(dv) == 0) {
                std::fprintf(stderr, "[BRAIN_VAR_MISSION] WARNING: MC2_BRAIN_VAR_MISSION=1 requires MC2_BRAIN_DISPATCH_VAR=1 — mission-scope writes/reads are INERT\n");
                std::fflush(stderr);
                return false;
            }
            return true;
        }
        return false;
    })();
    return kGate;
}

// resetMissionVarStore — clears g_missionVarStore.
// Call at mission load before warriors execute their first tick.
// Safe to call at any time (no-op if store is already empty).
void resetMissionVarStore() {
    g_missionVarStore.reset();
    std::fprintf(stderr, "[BRAIN_VAR_MISSION_RESET] g_missionVarStore cleared\n");
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// TECHSCRIPT-CALL-CHAIN-1A: depth limit for TechSpecial.Call recursion.
// Corpus max observed depth = 2; limit set to 8 per recon recommendation.
static constexpr int kCallChainMaxDepth = 8;

// ---------------------------------------------------------------------------
// BRAINSPECIAL-ALIAS-1: gate + case-insensitive compare helper.
// Gate MC2_BRAIN_ALIAS (default OFF). When OFF every alias path below is a no-op
// and behavior is byte-identical to pre-slice (only the 5 hardcoded
// aliasToCanonical mappings apply, exactly as before).
static bool s_brainAliasGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_ALIAS");
        return v && std::atoi(v) != 0;
    })();
    return kGate;
}

// Forward declaration — defined after aliasToCanonical (used by the dispatch loops above it).
static const char* resolveVerbAliasToken(const char* verb, char out[256]);

// ASCII case-insensitive full-string compare (leaf TU — no engine str helpers).
static bool ciEquals(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

// specialIndexFind — linear-scan lookup by key (case-sensitive exact first).
// BRAINSPECIAL-ALIAS-1 (gate ON only): fallback passes resolve the per-block
// `alias = "..."` field (carver authors these, e.g. alias="Scenario.Main")
// and finally a case-insensitive key match (author shorthand).
// Returns pointer into idx for the found entry, or nullptr.
const SpecialIndexEntry* specialIndexFind(const SpecialIndex& idx, const std::string& key) {
    for (const SpecialIndexEntry& e : idx) {
        if (e.key == key)
            return &e;
    }
    if (s_brainAliasGate()) {
        for (const SpecialIndexEntry& e : idx) {
            if (!e.alias.empty() && ciEquals(e.alias.c_str(), key.c_str()))
                return &e;
        }
        for (const SpecialIndexEntry& e : idx) {
            if (ciEquals(e.key.c_str(), key.c_str()))
                return &e;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Gate helper for DISPATCH_CALL (TECHSCRIPT-CALL-CHAIN-1A).
static bool s_dispatchCallGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_DISPATCH_CALL");
        if (v && std::atoi(v) != 0) {
            const char* d = std::getenv("MC2_BRAIN_DISPATCH");
            if (!d || std::atoi(d) == 0) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL] WARNING: MC2_BRAIN_DISPATCH_CALL=1 requires MC2_BRAIN_DISPATCH=1 — call chaining is INERT\n");
                std::fflush(stderr);
                return false;
            }
            return true;
        }
        return false;
    })();
    return kGate;
}

// ---------------------------------------------------------------------------
// Forward declaration for gate helper (defined after helpers, used by execute functions).
static bool s_dispatchVarGate();

// ---------------------------------------------------------------------------
// Gate helper for MC2_BRAIN_FSM (BRAIN-FSM-1K-A).
// Default OFF. When ON, FSM verbs (SetState/SetStatePrev/InState/NotInState)
// are active.  When OFF, FSM verb strings fall through to the isRecognizedVerb
// path (trace-only [BRAIN_DISPATCH] line) — byte-identical for brains without FSM verbs.
static bool s_brainFsmGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_FSM");
        return v && std::atoi(v) != 0;
    })();
    return kGate;
}

// ---------------------------------------------------------------------------
// Gate helper for MC2_BRAIN_FLOW (BRAINSPECIAL-FLOW-WAIT-1).
// Default OFF. When ON:
//   - the raw scanner collects WAIT / WAIT_UNTIL lines as verbs and keeps STOP as a
//     verb (gate OFF: WAIT lines are not DO-prefixed and STOP is a skipped sentinel —
//     pre-slice parse behavior, byte-identical);
//   - executeSpecialBody_Apply gates verbs after an unsatisfied WAIT/WAIT_UNTIL and
//     terminates the tick's body execution at STOP.
static bool s_brainFlowGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_FLOW");
        return v && std::atoi(v) != 0;
    })();
    return kGate;
}

// ---------------------------------------------------------------------------
// Gate helper for MC2_BRAIN_PATROL (BRAIN-OPORD-COREPATROL-1).
// Default OFF. When ON, OPORD.CorePatrol parse + per-tick advance are active.
// NOTE: reads MC2_BRAIN_PATROL fresh per call (not cached) so harness _putenv works.
static bool s_brainPatrolGate() {
    const char* v = std::getenv("MC2_BRAIN_PATROL");
    return v && std::atoi(v) != 0;
}

// BRAIN-ENGAGE-1: MC2_BRAIN_ENGAGE gate (default OFF). When ON, Patrol/Guard runtimes with
// engageRadius>0 auto-acquire the nearest enemy in range and fire (tickEngageNearest).
// Read fresh per call (harness _putenv friendly), matching s_brainPatrolGate.
static bool s_brainEngageGate() {
    const char* v = std::getenv("MC2_BRAIN_ENGAGE");
    return v && std::atoi(v) != 0;
}

// BRAIN-COMMIT-PHASE-1 (GAP B): MC2_BRAIN_COMMIT_PHASE gate (default OFF).
// Mirrors warrior.cpp's s_brainCommitPhase / objmgr.cpp's s_commitPhaseGate.
// When ON, patrol-emitted intents must be left in pendingIntents for the deferred
// WID-ordered drain (objmgr.cpp) instead of being self-committed inline here.
static bool s_brainCommitPhaseEnabled() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_COMMIT_PHASE");
        return (v && std::atoi(v) != 0);
    })();
    return kGate;
}

// TACTIC-WEIGHTS-B: MC2_TACTIC_WEIGHTS_B gate (default OFF). When ON, the brain's
// already-selected tactic (runtime->selectedTactic, populated by selectTacticForWarrior
// under MC2_TACTIC_WEIGHTS) is written into the attack order's attackParams.tactic.
// Needs BOTH gates: WEIGHTS to populate selectedTactic, WEIGHTS_B to wire it. With B
// OFF the field is left at its init default (TACTIC_NONE) → byte-identical to pre-B.
static bool s_tacticWeightsBEnabled() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_TACTIC_WEIGHTS_B");
        return (v && std::atoi(v) != 0);
    })();
    return kGate;
}

// ---------------------------------------------------------------------------
// Recognized verb table for DISPATCH-1A.
// All other verbs produce [BRAIN_DISPATCH_UNKNOWN] trace.
static const char* const kRecognizedVerbs[] = {
    "Brain.CorePower",
    "Brain.CoreAttack",
    "Brain.CoreEject",   // BRAIN-ALIAS-COREVERBS-1: carver_v_enhanced alias → Unit.Eject
    "OPORD.CoreAttack",
    "OPORD.CoreGuard",
    "OPORD.CorePatrol",
    "OPORD.CoreMoveTo",
    "Unit.Retreat",
    "Unit.Eject",
    "Unit.ClearMoveOrders",   // DISPATCH-INTENT-CLEARMOVEORDERS-1: movement-queue scrub (intent-modelled)
    "UnitQuery.SetTargetpriority",  // UNITQUERY-SETTARGETPRIORITY-1: target-priority list write (intent)
    "HOLD",
    // BRAIN-FSM-1K-A: FSM state verbs (gate MC2_BRAIN_FSM, default OFF)
    "Unit.SetState",
    "Unit.SetStatePrev",
    "Unit.InState",
    "Unit.NotInState",
    // BRAIN-FSM-1K-B: conditional FSM transition (gate MC2_BRAIN_FSM, requires MC2_BRAIN_DISPATCH_VAR)
    "Unit.SetStateIf",
    // BRAINSPECIAL-FLOW-WAIT-1: flow-control verbs (gate MC2_BRAIN_FLOW; the scanner
    // only emits them when the gate is ON, so gate-OFF dispatch never sees them).
    "WAIT_UNTIL",
    "WAIT",
    "STOP",
    nullptr  // sentinel
};

static bool isRecognizedVerb(const char* verb) {
    // Verbs are stored as full "verb arg..." tokens; match only the verb part
    // (up to the first whitespace) so "Brain.CorePower false" recognizes as
    // "Brain.CorePower" rather than falling through to UNKNOWN.
    for (int i = 0; kRecognizedVerbs[i] != nullptr; ++i) {
        size_t n = std::strlen(kRecognizedVerbs[i]);
        if (std::strncmp(verb, kRecognizedVerbs[i], n) == 0 &&
            (verb[n] == '\0' || verb[n] == ' ' || verb[n] == '\t'))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// parseCallVerbKey — parse the quoted key from a TechSpecial.Call verb string.
// Supports:  TechSpecial.Call "key"   (double-quoted)
//            TechSpecial.Call [key]   (bracket-quoted, FIT-safe)
// outKey must be at least 128 chars.
// Returns true on success, false on malformed input (missing delimiter, empty key).
static bool parseCallVerbKey(const char* verbStr, char outKey[128]) {
    outKey[0] = '\0';
    // Skip "TechSpecial.Call" prefix.
    static const char kPrefix[] = "TechSpecial.Call";
    const size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (std::strncmp(verbStr, kPrefix, kPrefixLen) != 0)
        return false;
    const char* p = verbStr + kPrefixLen;
    // Skip whitespace.
    while (*p == ' ' || *p == '\t') ++p;
    // Detect delimiter.
    char closeDelim = '\0';
    if (*p == '"')       { closeDelim = '"'; }
    else if (*p == '[')  { closeDelim = ']'; }
    else                  return false;
    ++p; // skip open delimiter
    const char* keyStart = p;
    const char* keyEnd   = std::strchr(p, closeDelim);
    if (!keyEnd) return false;
    size_t keyLen = (size_t)(keyEnd - keyStart);
    if (keyLen == 0) return false;
    if (keyLen > 127) keyLen = 127;
    std::strncpy(outKey, keyStart, keyLen);
    outKey[keyLen] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// executeSpecialBody_TraceOnlyChained
//
// TECHSCRIPT-CALL-CHAIN-1A: recursive verb-stream trace for called bodies.
//
// FORBIDDEN-CALL CONTRACT:
//   This function calls ONLY fprintf + fflush + loop + recursive calls to itself.
//   NO setGeneralTacOrder, NO order/movement/attack function.
//   NO effects executed on chained bodies (1A: trace only; 1B will add effect dispatch).
//   Brain.CorePower false in a CALLED body is traced but does NOT call setGeneralTacOrder.
//   This is PURE VERB-STREAM COMPOSITION.
//   Verified by inspection: no warrior pointer, no MechWarrior type, no TacticalOrder.
void executeSpecialBody_TraceOnlyChained(const BrainSpecialBody& body,
                                          int wid,
                                          const SpecialIndex& index,
                                          int depth,
                                          std::vector<std::string>& visited,
                                          const char* fromKey,
                                          VarStore* varStore)
{
    // Trace each verb in the called body with depth marker.
    for (const std::string& verb : body.verbs) {
        const char* vp = verb.c_str();
        // BRAINSPECIAL-ALIAS-1: resolve for MATCHING only; trace lines keep the
        // original author spelling (vp). No-op when MC2_BRAIN_ALIAS is OFF.
        char aliasBuf[256];
        const char* vpr = resolveVerbAliasToken(vp, aliasBuf);

        // Handle nested TechSpecial.Call within a called body.
        if (std::strncmp(vpr, "TechSpecial.Call", 16) == 0 &&
            (vpr[16] == ' ' || vpr[16] == '\t' || vpr[16] == '\0')) {
            char callKey[128];
            if (!parseCallVerbKey(vpr, callKey)) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=<parse-fail> depth=%d wid=%d\n",
                             fromKey, depth, wid);
                std::fflush(stderr);
                continue;
            }
            // Depth limit check.
            if (depth >= kCallChainMaxDepth) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_DEPTH] depth=%d max=%d wid=%d\n",
                             depth, kCallChainMaxDepth, wid);
                std::fflush(stderr);
                continue;
            }
            // Cycle guard.
            for (const std::string& vis : visited) {
                if (vis == callKey) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_CYCLE] from=%s to=%s depth=%d wid=%d\n",
                                 fromKey, callKey, depth, wid);
                    std::fflush(stderr);
                    goto next_verb_chained;
                }
            }
            {
                // Resolve key.
                const SpecialIndexEntry* target = specialIndexFind(index, callKey);
                if (!target) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=%s wid=%d\n",
                                 fromKey, callKey, wid);
                    std::fflush(stderr);
                    continue;
                }
                // Emit call trace, recurse.
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL] from=%s to=%s depth=%d wid=%d\n",
                             fromKey, callKey, depth + 1, wid);
                std::fflush(stderr);
                visited.push_back(callKey);
                executeSpecialBody_TraceOnlyChained(target->body, wid, index,
                                                    depth + 1, visited, callKey, varStore);
                visited.pop_back();
            }
            next_verb_chained:;
            continue;
        }

        // Var.* handling (when DISPATCH_VAR=1) — reuse the 1D handlers.
        if (s_dispatchVarGate() && varStore) {
            if (std::strncmp(vpr, "Var.Set", 7) == 0 || std::strncmp(vpr, "Var.Get", 7) == 0) {
                char key[32], value[32];
                uint8_t scope = 0;
                bool ok = parseVarVerb(vpr, key, value, &scope);
                if (!ok) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_PARSE_FAIL] verb=%s wid=%d depth=%d (malformed)\n",
                                 vp, wid, depth);
                    std::fflush(stderr);
                    continue;
                }
                bool isSet = (std::strncmp(vpr, "Var.Set", 7) == 0);
                if (isSet) handleVarSet(key, value, scope, varStore, wid);
                else       handleVarGet(key, scope, varStore, wid);
                continue;
            }
        }

        // Standard verb trace with depth.
        // NOTE: Brain.CorePower false in a CALLED body traces as recognized but
        // does NOT call setGeneralTacOrder (1A: no chained effects; deferred to 1B).
        if (isRecognizedVerb(vpr)) {
            std::fprintf(stderr, "[BRAIN_DISPATCH] depth=%d verb=%s wid=%d\n", depth, vp, wid);
        } else {
            std::fprintf(stderr, "[BRAIN_DISPATCH_UNKNOWN] depth=%d verb=%s wid=%d\n", depth, vp, wid);
        }
        std::fflush(stderr);
    }
}

// ---------------------------------------------------------------------------
// executeSpecialBody_TraceOnly
//
// FORBIDDEN-CALL CONTRACT: this function body contains ONLY fprintf + fflush
// + loop + string comparison + (when DISPATCH_VAR=1) store->set/get via handleVarSet/Get
// + (when DISPATCH_CALL=1) recursive executeSpecialBody_TraceOnlyChained.
// No tac-order writes. No movement/attack orders. No state writes to warrior or mission.
// Verified by inspection: no warrior pointer, no MechWarrior type, no
// setGeneralTacOrder / setPlayerTacOrder / setAlarmTacOrder / requestHelp /
// requestTarget / clearCurTacOrder / setMainGoal / calcTacOrder / coreMoveTo.
void executeSpecialBody_TraceOnly(const BrainSpecialBody& body, int wid, VarStore* varStore,
                                   const SpecialIndex* index, const char* callerKey) {
    const bool varGate  = s_dispatchVarGate();
    const bool callGate = s_dispatchCallGate() && (index != nullptr);

    // Per-tick visited set for cycle guard (CALL-CHAIN-1A).
    // Created here (root dispatch); passed into chained calls by reference.
    std::vector<std::string> visited;
    if (callerKey && callerKey[0] != '\0')
        visited.push_back(std::string(callerKey));

    for (const std::string& verb : body.verbs) {
        const char* vp = verb.c_str();
        // BRAINSPECIAL-ALIAS-1: resolve for MATCHING only; trace lines keep the
        // original author spelling (vp). No-op when MC2_BRAIN_ALIAS is OFF.
        char aliasBuf[256];
        const char* vpr = resolveVerbAliasToken(vp, aliasBuf);

        // CALL-CHAIN-1A: TechSpecial.Call "<key>" handling.
        if (callGate && std::strncmp(vpr, "TechSpecial.Call", 16) == 0 &&
            (vpr[16] == ' ' || vpr[16] == '\t' || vpr[16] == '\0')) {
            char callKey[128];
            if (!parseCallVerbKey(vpr, callKey)) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=<parse-fail> wid=%d\n",
                             callerKey ? callerKey : "root", wid);
                std::fflush(stderr);
                continue;
            }
            // Depth limit (root call = depth 1).
            if (kCallChainMaxDepth < 1) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_DEPTH] depth=1 max=%d wid=%d\n",
                             kCallChainMaxDepth, wid);
                std::fflush(stderr);
                continue;
            }
            // Cycle guard.
            bool cycle = false;
            for (const std::string& vis : visited) {
                if (vis == callKey) { cycle = true; break; }
            }
            if (cycle) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_CYCLE] from=%s to=%s depth=1 wid=%d\n",
                             callerKey ? callerKey : "root", callKey, wid);
                std::fflush(stderr);
                continue;
            }
            // Resolve key.
            const SpecialIndexEntry* target = specialIndexFind(*index, callKey);
            if (!target) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=%s wid=%d\n",
                             callerKey ? callerKey : "root", callKey, wid);
                std::fflush(stderr);
                continue;
            }
            // Emit call trace, then recurse.
            std::fprintf(stderr, "[BRAIN_DISPATCH_CALL] from=%s to=%s depth=1 wid=%d\n",
                         callerKey ? callerKey : "root", callKey, wid);
            std::fflush(stderr);
            visited.push_back(std::string(callKey));
            executeSpecialBody_TraceOnlyChained(target->body, wid, *index,
                                                 1, visited, callKey, varStore);
            visited.pop_back();
            continue;
        }

        // When DISPATCH_VAR=1, intercept Var.Set / Var.Get before the UNKNOWN fallthrough.
        if (varGate) {
            if (std::strncmp(vpr, "Var.Set", 7) == 0 || std::strncmp(vpr, "Var.Get", 7) == 0) {
                char key[32], value[32];
                uint8_t scope = 0;
                bool ok = parseVarVerb(vpr, key, value, &scope);
                if (!ok) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_PARSE_FAIL] verb=%s wid=%d (malformed)\n", vp, wid);
                    std::fflush(stderr);
                    continue;
                }
                bool isSet = (std::strncmp(vpr, "Var.Set", 7) == 0);
                if (isSet)
                    handleVarSet(key, value, scope, varStore, wid);
                else
                    handleVarGet(key, scope, varStore, wid);
                continue;
            }
        }
        if (isRecognizedVerb(vpr)) {
            std::fprintf(stderr, "[BRAIN_DISPATCH] verb=%s wid=%d\n", vp, wid);
        } else {
            std::fprintf(stderr, "[BRAIN_DISPATCH_UNKNOWN] verb=%s wid=%d\n", vp, wid);
        }
        std::fflush(stderr);
    }
}

// ---------------------------------------------------------------------------
// aliasToCanonical — DISPATCH-EFFECT-UNITEJECT-1
// Maps legacy ABL-style verb aliases to canonical dispatch verb names.
//   "coreEject"  → "Unit.Eject"
//   "corePower"  → "Brain.CorePower" (future; mapped here for completeness)
// Returns the canonical form if the input is a known alias; otherwise returns input unchanged.
static const char* aliasToCanonical(const char* verb) {
    if (std::strcmp(verb, "coreEject") == 0)
        return "Unit.Eject";
    // BRAIN-ALIAS-COREVERBS-1: carver_v_enhanced emits Brain.CoreEject (657 uses) — a
    // no-arg eject. Map it to the canonical Unit.Eject so the EJECT handler fires.
    // (Brain.CoreAttack carries a WID arg, so it is aliased at the handler prefix-check
    // instead — see executeSpecialBody_Apply, since aliasToCanonical is exact-strcmp only.)
    if (std::strcmp(verb, "Brain.CoreEject") == 0)
        return "Unit.Eject";
    if (std::strcmp(verb, "corePower") == 0)
        return "Brain.CorePower";
    if (std::strcmp(verb, "coreGuard") == 0)
        return "OPORD.CoreGuard";
    if (std::strcmp(verb, "coreRetreat") == 0)
        return "Unit.Retreat";
    return verb;
}

// ---------------------------------------------------------------------------
// BRAINSPECIAL-ALIAS-1: data-driven alias registry.
//
// Spec (discussion #18): "stable canonical keys + author-friendly scoped shorthand
// (Tactic.Standard -> tactic.standard)". Engine adaptation:
//   1. Registry seeded with the 5 built-in aliases (same set aliasToCanonical hardcodes).
//   2. Authorable `Aliases { <alias> = "<canonical>" }` block in the specials file
//      (parsed by the raw scanner; registered unconditionally, resolved gate-ON only —
//      same parse-unconditional / resolve-gated model as the CALL-CHAIN index).
//   3. Case-insensitive shorthand: a token matching a registry canonical or a
//      kRecognizedVerbs catalog entry case-insensitively is rewritten to the
//      canonical spelling (spec's `tactic.standard` form).
// Resolution is FIRST-TOKEN rewrite: verb args pass through untouched.
// Gate OFF: resolveVerbAliasToken returns its input pointer unchanged (no-op).
//
// FORBIDDEN-CALL CONTRACT (registry + resolver): string ops + fprintf only.
// No warrior pointer, no order functions.
struct AliasEntry { char alias[64]; char canonical[96]; };
static constexpr int kAliasRegistryCap = 32;
static AliasEntry g_aliasRegistry[kAliasRegistryCap];
static int       g_aliasRegistryCount = 0;

// Add (or overwrite by alias name). Returns false when the cap is hit.
static bool aliasRegistryAdd(const char* alias, const char* canonical) {
    if (!alias || !canonical || alias[0] == '\0' || canonical[0] == '\0')
        return false;
    for (int i = 0; i < g_aliasRegistryCount; ++i) {
        if (ciEquals(g_aliasRegistry[i].alias, alias)) {
            std::strncpy(g_aliasRegistry[i].canonical, canonical, 95);
            g_aliasRegistry[i].canonical[95] = '\0';
            return true;
        }
    }
    if (g_aliasRegistryCount >= kAliasRegistryCap) {
        std::fprintf(stderr, "[BRAIN_ALIAS_REG] WARN: registry cap (%d) reached — alias %s dropped\n",
                     kAliasRegistryCap, alias);
        std::fflush(stderr);
        return false;
    }
    AliasEntry& e = g_aliasRegistry[g_aliasRegistryCount++];
    std::strncpy(e.alias, alias, 63);         e.alias[63] = '\0';
    std::strncpy(e.canonical, canonical, 95); e.canonical[95] = '\0';
    return true;
}

// Re-seed the registry with the built-in aliases (mirrors aliasToCanonical).
// Called at the start of each specials parse (mission-load / harness path).
static void aliasRegistrySeed() {
    g_aliasRegistryCount = 0;
    aliasRegistryAdd("coreEject",       "Unit.Eject");
    aliasRegistryAdd("Brain.CoreEject", "Unit.Eject");
    aliasRegistryAdd("corePower",       "Brain.CorePower");
    aliasRegistryAdd("coreGuard",       "OPORD.CoreGuard");
    aliasRegistryAdd("coreRetreat",     "Unit.Retreat");
}

// resolveVerbAliasToken — first-token alias/shorthand resolution.
// Gate OFF (default): returns `verb` unchanged — byte-identical no-op.
// Gate ON: if the first token matches (1) a registry alias, (2) a registry
// canonical (case-insensitive), or (3) a kRecognizedVerbs entry (case-insensitive),
// rewrite the token to the canonical spelling into `out` and return `out`.
// Returns `verb` unchanged when no rewrite is needed.
static const char* resolveVerbAliasToken(const char* verb, char out[256]) {
    if (!s_brainAliasGate()) return verb;
    const char* te = verb;
    while (*te && *te != ' ' && *te != '\t') ++te;
    size_t tl = (size_t)(te - verb);
    if (tl == 0 || tl >= 96) return verb;
    char tok[96];
    std::memcpy(tok, verb, tl);
    tok[tl] = '\0';
    const char* canonical = nullptr;
    for (int i = 0; i < g_aliasRegistryCount && !canonical; ++i)
        if (ciEquals(g_aliasRegistry[i].alias, tok)) canonical = g_aliasRegistry[i].canonical;
    for (int i = 0; i < g_aliasRegistryCount && !canonical; ++i)
        if (ciEquals(g_aliasRegistry[i].canonical, tok)) canonical = g_aliasRegistry[i].canonical;
    for (int i = 0; kRecognizedVerbs[i] != nullptr && !canonical; ++i)
        if (ciEquals(kRecognizedVerbs[i], tok)) canonical = kRecognizedVerbs[i];
    if (!canonical) return verb;
    if (std::strcmp(canonical, tok) == 0) return verb;  // already canonical spelling
    std::snprintf(out, 256, "%s%s", canonical, te);     // te keeps the arg tail (incl. space)
    return out;
}

// ---------------------------------------------------------------------------
// bodyHasPowerdown
// Returns true if the body contains the Brain.CorePower false verb token.
bool bodyHasPowerdown(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        char abuf[256];  // BRAINSPECIAL-ALIAS-1: registry resolve (no-op gate-OFF)
        const char* v = resolveVerbAliasToken(verb.c_str(), abuf);
        if (std::strcmp(v, "Brain.CorePower false") == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasUnitEject — DISPATCH-EFFECT-UNITEJECT-1
// Returns true if the body contains a Unit.Eject (or coreEject alias) verb token.
bool bodyHasUnitEject(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        char abuf[256];  // BRAINSPECIAL-ALIAS-1: registry resolve (no-op gate-OFF)
        const char* canonical = aliasToCanonical(resolveVerbAliasToken(verb.c_str(), abuf));
        if (std::strcmp(canonical, "Unit.Eject") == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasCoreGuard — DISPATCH-EFFECT-COREGUARD-1
// Returns true if the body contains an OPORD.CoreGuard (or coreGuard alias) verb token.
bool bodyHasCoreGuard(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        char abuf[256];  // BRAINSPECIAL-ALIAS-1: registry resolve (no-op gate-OFF)
        const char* canonical = aliasToCanonical(resolveVerbAliasToken(verb.c_str(), abuf));
        if (std::strcmp(canonical, "OPORD.CoreGuard") == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasCoreMoveTo — DISPATCH-EFFECT-COREMOVETO-1
// Returns true if the body contains an OPORD.CoreMoveTo verb token.
// Token-prefix match: first word of the stored verb string must equal "OPORD.CoreMoveTo".
bool bodyHasCoreMoveTo(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        char abuf[256];  // BRAINSPECIAL-ALIAS-1: registry resolve (no-op gate-OFF)
        const char* v = resolveVerbAliasToken(verb.c_str(), abuf);
        size_t vn = std::strlen(v);
        if (std::strncmp(v, "OPORD.CoreMoveTo", 16) == 0
            && (vn == 16 || v[16] == ' '))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasCoreAttack — DISPATCH-EFFECT-COREATTACK-1
// Returns true if the body contains an OPORD.CoreAttack verb token.
// Token-prefix match: first word of the stored verb string must equal "OPORD.CoreAttack".
bool bodyHasCoreAttack(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        char abuf[256];  // BRAINSPECIAL-ALIAS-1: registry resolve (no-op gate-OFF)
        const char* v = resolveVerbAliasToken(verb.c_str(), abuf);
        size_t vn = std::strlen(v);
        // BRAIN-ALIAS-COREVERBS-1: Brain.CoreAttack (carver_v_enhanced) shares the
        // OPORD.CoreAttack effect/slot; match either spelling (both 16-char prefixes).
        if ((std::strncmp(v, "OPORD.CoreAttack", 16) == 0
             || std::strncmp(v, "Brain.CoreAttack", 16) == 0)
            && (vn == 16 || v[16] == ' '))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasUnitRetreat — DISPATCH-EFFECT-UNITRETREAT-1
// Returns true if the body contains a Unit.Retreat (or coreRetreat alias) verb token.
bool bodyHasUnitRetreat(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        char abuf[256];  // BRAINSPECIAL-ALIAS-1: registry resolve (no-op gate-OFF)
        const char* canonical = aliasToCanonical(resolveVerbAliasToken(verb.c_str(), abuf));
        if (std::strcmp(canonical, "Unit.Retreat") == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasCorePatrol — BRAIN-OPORD-COREPATROL-1
// Returns true if the body contains an OPORD.CorePatrol verb token.
// Token-prefix match: first word of the stored verb string must equal "OPORD.CorePatrol".
bool bodyHasCorePatrol(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        char abuf[256];  // BRAINSPECIAL-ALIAS-1: registry resolve (no-op gate-OFF)
        const char* v = resolveVerbAliasToken(verb.c_str(), abuf);
        size_t vn = std::strlen(v);
        if (std::strncmp(v, "OPORD.CorePatrol", 16) == 0
            && (vn == 16 || v[16] == ' '))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasEffect — DISPATCH-EFFECT-UNITRETREAT-1 (extended from COREATTACK-1)
// Returns true if the body has ANY effect verb that claims the GENERAL slot
// (currently: POWERDOWN, EJECT, GUARD, MOVETO, ATTACK, or RETREAT).
bool bodyHasEffect(const BrainSpecialBody& body) {
    return bodyHasPowerdown(body) || bodyHasUnitEject(body) || bodyHasCoreGuard(body)
        || bodyHasCoreMoveTo(body) || bodyHasCoreAttack(body) || bodyHasUnitRetreat(body);
}

// ---------------------------------------------------------------------------
// BRAINSPECIAL-FLOW-WAIT-1 helpers.
//
// FORBIDDEN-CALL CONTRACT (all four helpers): string ops + runtime-field writes +
// fprintf only. NO order functions, NO warrior mutation beyond MechBrainRuntime
// wait/flow bookkeeping fields.

// True if the verb token is a flow-control verb (WAIT / WAIT_UNTIL / STOP).
static bool isFlowControlVerbToken(const char* v) {
    if (std::strncmp(v, "WAIT_UNTIL", 10) == 0 && (v[10] == ' ' || v[10] == '\t' || v[10] == '\0'))
        return true;
    if (std::strncmp(v, "WAIT", 4) == 0 && (v[4] == ' ' || v[4] == '\t' || v[4] == '\0'))
        return true;
    if (std::strcmp(v, "STOP") == 0)
        return true;
    return false;
}

// bodyHasFlowControl — true if the body carries any WAIT/WAIT_UNTIL/STOP verb.
// (The scanner only emits these when MC2_BRAIN_FLOW=1, so gate-OFF bodies never match.)
bool bodyHasFlowControl(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs)
        if (isFlowControlVerbToken(verb.c_str()))
            return true;
    return false;
}

// brainFlowActiveForBody — warrior.cpp's per-tick decision helper: flow gating applies
// to this body (gate ON + body carries flow verbs). Callers switch to every-tick
// re-dispatch and skip the class-level once-guard pre-set when this returns true.
bool brainFlowActiveForBody(const BrainSpecialBody& body) {
    return s_brainFlowGate() && bodyHasFlowControl(body);
}

// True if vpCanon is one of the GENERAL-slot effect verbs (the once-per-mission orders).
// Used for the per-verb-index refire guard while flow gating is active.
// OPORD.CorePatrol is intentionally excluded — patrolActive is its own re-emit guard.
static bool isGeneralEffectVerbToken(const char* v) {
    if (std::strcmp(v, "Brain.CorePower false") == 0) return true;
    if (std::strcmp(v, "Unit.Eject") == 0)            return true;
    if (std::strcmp(v, "OPORD.CoreGuard") == 0)       return true;
    if (std::strcmp(v, "Unit.Retreat") == 0)          return true;
    if ((std::strncmp(v, "OPORD.CoreMoveTo", 16) == 0
         || std::strncmp(v, "OPORD.CoreAttack", 16) == 0
         || std::strncmp(v, "Brain.CoreAttack", 16) == 0)
        && (v[16] == ' ' || v[16] == '\0'))
        return true;
    return false;
}

// Find (or create) the wait-state slot for a verb index. Returns nullptr on cap
// overflow (soft-fail: caller traces and does NOT gate — the WAIT is skipped so a
// runaway fixture cannot wedge the body forever).
static BrainWaitState* flowFindOrCreateWaitState(MechBrainRuntime* rt, uint16_t verbIdx) {
    for (int i = 0; i < rt->waitStateCount; ++i)
        if (rt->waitStates[i].verbIndex == verbIdx)
            return &rt->waitStates[i];
    if (rt->waitStateCount >= MechBrainRuntime::kBrainWaitCap)
        return nullptr;
    BrainWaitState& ws = rt->waitStates[rt->waitStateCount++];
    ws.verbIndex  = verbIdx;
    ws.armed      = 0;
    ws.satisfied  = 0;
    ws.deadlineMs = 0;
    return &ws;
}

// Per-verb-index effect refire guard (flow-active bodies re-dispatch every tick).
static bool flowVerbFired(const MechBrainRuntime* rt, uint16_t verbIdx) {
    for (int i = 0; i < rt->flowFiredCount; ++i)
        if (rt->flowFiredIdx[i] == verbIdx)
            return true;
    return false;
}

static void flowMarkFired(MechBrainRuntime* rt, uint16_t verbIdx) {
    if (flowVerbFired(rt, verbIdx)) return;
    if (rt->flowFiredCount >= MechBrainRuntime::kBrainFlowFiredCap) {
        std::fprintf(stderr, "[BRAIN_FLOW_FIRED_OVERFLOW] idx=%u cap=%d\n",
                     (unsigned)verbIdx, MechBrainRuntime::kBrainFlowFiredCap);
        std::fflush(stderr);
        return;
    }
    rt->flowFiredIdx[rt->flowFiredCount++] = verbIdx;
}

// Parse the WAIT_UNTIL condition:  WAIT_UNTIL Var "<key>" == <value> [scope=Mission]
// (key accepts "..." or [...] delimiters — same FIT-safe forms as Var.Set).
// Returns false on malformed input.
static bool parseWaitUntilCondition(const char* verbStr, char outKey[32], char outValue[32],
                                     bool* outMissionScope) {
    outKey[0] = '\0'; outValue[0] = '\0'; *outMissionScope = false;
    const char* p = verbStr + 10;  // past "WAIT_UNTIL"
    while (*p == ' ' || *p == '\t') ++p;
    if (std::strncmp(p, "Var", 3) != 0) return false;
    p += 3;
    while (*p == ' ' || *p == '\t') ++p;
    char close = '\0';
    if (*p == '"')      close = '"';
    else if (*p == '[') close = ']';
    else                 return false;
    ++p;
    const char* ke = std::strchr(p, close);
    if (!ke || ke == p) return false;
    size_t kl = (size_t)(ke - p);
    if (kl > 31) kl = 31;
    std::memcpy(outKey, p, kl); outKey[kl] = '\0';
    p = ke + 1;
    while (*p == ' ' || *p == '\t') ++p;
    if (p[0] != '=' || p[1] != '=') return false;
    p += 2;
    while (*p == ' ' || *p == '\t') ++p;
    int vi = 0;
    while (*p && *p != ' ' && *p != '\t' && vi < 31) outValue[vi++] = *p++;
    outValue[vi] = '\0';
    if (vi == 0) return false;
    if (std::strstr(p, "scope=Mission") != nullptr) *outMissionScope = true;
    return true;
}

// ---------------------------------------------------------------------------
// BRAIN-DECISION-INTENT-QUEUE-1: MC2_BRAIN_INTENT_QUEUE gate + emit helper.
//
// When gate ON, the 6 verb handlers in executeSpecialBody_Apply push a BrainOrderIntent
// to runtime->pendingIntents[] INSTEAD of calling warrior->setGeneralTacOrder directly.
// commitBrainIntents() (below) is the ONLY function that calls setGeneralTacOrder when
// gate ON.
//
// Gate OFF (default): each verb calls setGeneralTacOrder directly — byte-identical to
// the pre-BRAIN-DECISION-INTENT-QUEUE-1 behavior.

static bool s_intentQueueEnabled() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_INTENT_QUEUE");
        return (v && std::atoi(v) != 0);
    })();
    return kGate;
}

// ---------------------------------------------------------------------------
// BRAIN-WORLD-SNAPSHOT-1: MC2_BRAIN_SNAPSHOT gate (default OFF).
//
// Gate OFF (default): once-guards in warrior.cpp read live runtime->*EffectApplied
//   directly — byte-identical to pre-snapshot behavior.
// Gate ON: once-guards read snapshot.effectApplied[N] populated by
//   buildBrainWorldSnapshot() called right before the once-guard block.
//   The live runtime writes (*EffectApplied = 1) are NOT moved this rung.
//
// IDs-not-pointers: snapshot stores WatchIDs, not pointers. ATTACK target
//   team + existence still read live via ObjectManager::getByWatchID (deferred
//   to rung 7/8 when a target/contact table snapshot is added).
bool s_brainSnapshotEnabled() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_SNAPSHOT");
        return (v && std::atoi(v) != 0);
    })();
    return kGate;
}

// buildBrainWorldSnapshot — defined in warrior.cpp (static helper; needs protected access).

// Pushes one BrainOrderIntent onto runtime->pendingIntents[].
// Returns true if pushed, false if buffer full (soft-fail; rare — cap is 4).
// Caller must check gate before calling.
static bool emitBrainIntent(MechBrainRuntime* runtime, int warriorId,
                             TacticalOrderCode orderType, int targetWID,
                             float wx, float wy, float wz,
                             uint32_t sourceBodyId) {
    if (!runtime) return false;
    if (runtime->pendingIntentCount >= kBrainIntentCap) {
        std::fprintf(stderr, "[BRAIN_INTENT_EMIT_OVERFLOW] wid=%d order=%d cap=%d\n",
                     warriorId, (int)orderType, kBrainIntentCap);
        std::fflush(stderr);
        return false;
    }
    BrainOrderIntent& out = runtime->pendingIntents[runtime->pendingIntentCount++];
    out.warriorId   = warriorId;
    out.slot        = BrainIntentSlot::GENERAL;
    out.kind        = BrainIntentKind::ORDER;   // pendingIntents slots are reused — set kind explicitly
    out.orderType   = orderType;
    out.targetWID   = targetWID;
    out.waypoint[0] = wx;
    out.waypoint[1] = wy;
    out.waypoint[2] = wz;
    out.sourceBodyId = sourceBodyId;
    out.brainTick   = getBrainTickIndex();
    return true;
}

// DISPATCH-INTENT-CLEARMOVEORDERS-1: emit a CLEAR_MOVE intent (movement-queue scrub).
// Intent-only — the actual warrior->clearMoveOrders() runs in commitBrainIntents (the sole
// contract-permitted mutator). Mirrors emitBrainIntent's overflow + tick-stamp handling.
static bool emitBrainClearMoveIntent(MechBrainRuntime* runtime, int warriorId) {
    if (!runtime) return false;
    if (runtime->pendingIntentCount >= kBrainIntentCap) {
        std::fprintf(stderr, "[BRAIN_INTENT_EMIT_OVERFLOW] wid=%d order=CLEAR_MOVE cap=%d\n",
                     warriorId, kBrainIntentCap);
        std::fflush(stderr);
        return false;
    }
    BrainOrderIntent& out = runtime->pendingIntents[runtime->pendingIntentCount++];
    out.warriorId   = warriorId;
    out.slot        = BrainIntentSlot::GENERAL;
    out.kind        = BrainIntentKind::CLEAR_MOVE;
    out.orderType   = TACTICAL_ORDER_NONE;   // unused for CLEAR_MOVE
    out.targetWID   = -1;
    out.waypoint[0] = 0.0f;
    out.waypoint[1] = 0.0f;
    out.waypoint[2] = 0.0f;
    out.sourceBodyId = 0;
    out.brainTick   = getBrainTickIndex();
    return true;
}

// UNITQUERY-SETTARGETPRIORITY-1: emit a TARGET_POLICY intent (per-warrior target-priority
// list write). Applied via MechWarrior::setTargetPriority() in commitBrainIntents (the sole
// permitted mutator). policyArgs = {slot,type,param1,param2,param3} (already symbol-resolved).
static bool emitBrainTargetPolicyIntent(MechBrainRuntime* runtime, int warriorId,
                                        long slot, long type, long p1, long p2, long p3) {
    if (!runtime) return false;
    if (runtime->pendingIntentCount >= kBrainIntentCap) {
        std::fprintf(stderr, "[BRAIN_INTENT_EMIT_OVERFLOW] wid=%d order=TARGET_POLICY cap=%d\n",
                     warriorId, kBrainIntentCap);
        std::fflush(stderr);
        return false;
    }
    BrainOrderIntent& out = runtime->pendingIntents[runtime->pendingIntentCount++];
    out.warriorId   = warriorId;
    out.slot        = BrainIntentSlot::GENERAL;
    out.kind        = BrainIntentKind::TARGET_POLICY;
    out.orderType   = TACTICAL_ORDER_NONE;
    out.targetWID   = -1;
    out.waypoint[0] = 0.0f; out.waypoint[1] = 0.0f; out.waypoint[2] = 0.0f;
    out.sourceBodyId = 0;
    out.brainTick   = getBrainTickIndex();
    out.policyArgs[0] = (int32_t)slot;
    out.policyArgs[1] = (int32_t)type;
    out.policyArgs[2] = (int32_t)p1;
    out.policyArgs[3] = (int32_t)p2;
    out.policyArgs[4] = (int32_t)p3;
    return true;
}

// ---------------------------------------------------------------------------
// executeSpecialBody_Apply
//
// 1B EFFECT DISPATCHER — gate MC2_BRAIN_DISPATCH_APPLY=1 (requires MC2_BRAIN_DISPATCH=1).
//
// RELAXED-CALL GUARD CONTRACT:
//   Permitted verbs (SIX total): Brain.CorePower false → POWERDOWN,
//   Unit.Eject (or coreEject alias) → EJECT, OPORD.CoreGuard (or coreGuard alias) → GUARD,
//   OPORD.CoreMoveTo x y z → MOVETO_POINT, OPORD.CoreAttack <wid> → ATTACK_OBJECT,
//   Unit.Retreat (or coreRetreat alias) → WITHDRAW.
//   All other verbs produce [BRAIN_DISPATCH] or [BRAIN_DISPATCH_UNKNOWN] trace only.
//   FORBIDDEN in this function: orderAttackObject, setAttackTarget, setSituationOpenFire,
//   setPlayerTacOrder, setAlarmTacOrder, requestHelp,
//   requestTarget, calcTacOrder, coreMoveTo, setMainGoal, clearCurTacOrder,
//   any movement/attack/OPORD-advance/commander function NOT listed above.
//   BRAIN-DECISION-INTENT-QUEUE-1 (gate MC2_BRAIN_INTENT_QUEUE):
//   When gate ON:  the 6 verb handlers call emitBrainIntent() — NO setGeneralTacOrder here.
//                  commitBrainIntents() (below) is the sole caller of setGeneralTacOrder.
//   When gate OFF: the 6 verb handlers call warrior->setGeneralTacOrder() directly (unchanged).
//   The checker (check_brain_relaxed_guard_doc.py) counts call-sites across BOTH this function
//   AND commitBrainIntents to confirm the verb count matches the doc.
//   Verified by inspection: exactly six effect-verb branches (gate OFF callsites = 6;
//   gate ON emitBrainIntent() calls = 6 in commitBrainIntents).
//
// Returns true if a GENERAL-slot effect (POWERDOWN, EJECT, GUARD, MOVETO, ATTACK, or RETREAT) was applied.
// Caller uses the return value to suppress the synthetic HOLD_TASK (one GENERAL-slot write per tick).
//
// CALL-CHAIN-1A NOTE: TechSpecial.Call verbs in the ROOT body are dispatched trace-only
// (chained bodies traced without effects). Brain.CorePower false in a CALLED body is
// traced but does NOT call setGeneralTacOrder. Chained-effect execution = CALL-CHAIN-1B.
bool executeSpecialBody_Apply(const BrainSpecialBody& body, MechWarrior* warrior, int wid,
                               VarStore* varStore, const SpecialIndex* index,
                               const char* callerKey) {
    bool appliedEffect = false;
    // BRAIN-DECISION-INTENT-QUEUE-1: get runtime for intent emit (gate ON only).
    MechBrainRuntime* runtime = (warrior && s_intentQueueEnabled()) ? warrior->getBrainRuntime() : nullptr;
    const bool varGate  = s_dispatchVarGate();
    const bool callGate = s_dispatchCallGate() && (index != nullptr);
    // BRAIN-FSM-1K-A: stateGateOpen — stack-local, reset true at body entry.
    // Unit.InState / Unit.NotInState set this false on mismatch; subsequent verbs are skipped.
    // ONE continue check at top of loop; the 6 effect handlers are UNCHANGED.
    const bool fsmGate = s_brainFsmGate();
    bool stateGateOpen = true;

    // BRAINSPECIAL-FLOW-WAIT-1: flow gating locals.
    // flowGateOpen closes at an unsatisfied WAIT/WAIT_UNTIL (verbs after it skip this
    // tick) and re-opens on the next body dispatch (re-execution model). flowRt holds
    // the wait latches + per-verb-index refire guard. Gate OFF: all of this is inert
    // (the scanner never emits flow verbs, bodyHasFlowControl is false).
    MechBrainRuntime* flowRt = warrior ? warrior->getBrainRuntime() : nullptr;
    const bool flowActive = s_brainFlowGate() && (flowRt != nullptr) && bodyHasFlowControl(body);
    bool flowGateOpen = true;
    int  verbIdx = -1;

    // Per-tick visited set for cycle guard.
    std::vector<std::string> visited;
    if (callerKey && callerKey[0] != '\0')
        visited.push_back(std::string(callerKey));

    for (const std::string& verb : body.verbs) {
        const char* vp = verb.c_str();
        ++verbIdx;  // BRAINSPECIAL-FLOW-WAIT-1: root-body verb position (wait-state key)

        // BRAIN-FSM-1K-A: gate check — skip effect verbs after a failed InState/NotInState.
        if (!stateGateOpen) continue;

        // BRAINSPECIAL-FLOW-WAIT-1: sequence gate — skip verbs after an unsatisfied WAIT.
        if (!flowGateOpen) continue;

        // BRAINSPECIAL-ALIAS-1: registry/shorthand resolution for the whole handler
        // chain below (Call/Var/effects/FSM all see the canonical spelling).
        // No-op when MC2_BRAIN_ALIAS is OFF (returns vp unchanged).
        char aliasBuf[256];
        vp = resolveVerbAliasToken(vp, aliasBuf);

        // BRAINSPECIAL-FLOW-WAIT-1: per-verb-index refire guard — an effect verb that
        // already fired under flow gating is skipped on subsequent re-dispatches
        // (only effect indexes are ever marked).
        if (flowActive && flowVerbFired(flowRt, (uint16_t)verbIdx)) continue;

        // CALL-CHAIN-1A: TechSpecial.Call handling (trace only — no chained effects in 1A).
        if (callGate && std::strncmp(vp, "TechSpecial.Call", 16) == 0 &&
            (vp[16] == ' ' || vp[16] == '\t' || vp[16] == '\0')) {
            char callKey[128];
            if (!parseCallVerbKey(vp, callKey)) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=<parse-fail> wid=%d\n",
                             callerKey ? callerKey : "root", wid);
                std::fflush(stderr);
                continue;
            }
            if (kCallChainMaxDepth < 1) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_DEPTH] depth=1 max=%d wid=%d\n",
                             kCallChainMaxDepth, wid);
                std::fflush(stderr);
                continue;
            }
            bool cycle = false;
            for (const std::string& vis : visited) {
                if (vis == callKey) { cycle = true; break; }
            }
            if (cycle) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_CYCLE] from=%s to=%s depth=1 wid=%d\n",
                             callerKey ? callerKey : "root", callKey, wid);
                std::fflush(stderr);
                continue;
            }
            const SpecialIndexEntry* target = specialIndexFind(*index, callKey);
            if (!target) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=%s wid=%d\n",
                             callerKey ? callerKey : "root", callKey, wid);
                std::fflush(stderr);
                continue;
            }
            std::fprintf(stderr, "[BRAIN_DISPATCH_CALL] from=%s to=%s depth=1 wid=%d\n",
                         callerKey ? callerKey : "root", callKey, wid);
            std::fflush(stderr);
            // Recurse trace-only (1A: no effects in chained bodies).
            visited.push_back(std::string(callKey));
            executeSpecialBody_TraceOnlyChained(target->body, wid, *index,
                                                 1, visited, callKey, varStore);
            visited.pop_back();
            continue;
        }

        // When DISPATCH_VAR=1, intercept Var.Set / Var.Get before other dispatch.
        if (varGate) {
            if (std::strncmp(vp, "Var.Set", 7) == 0 || std::strncmp(vp, "Var.Get", 7) == 0) {
                char key[32], value[32];
                uint8_t scope = 0;
                bool ok = parseVarVerb(vp, key, value, &scope);
                if (!ok) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_PARSE_FAIL] verb=%s wid=%d (malformed)\n", vp, wid);
                    std::fflush(stderr);
                    continue;
                }
                bool isSet = (std::strncmp(vp, "Var.Set", 7) == 0);
                if (isSet)
                    handleVarSet(key, value, scope, varStore, wid);
                else
                    handleVarGet(key, scope, varStore, wid);
                continue;
            }
        }
        // DISPATCH-EFFECT-UNITEJECT-1: alias resolution before effect dispatch.
        const char* vpCanon = aliasToCanonical(vp);

        // BRAINSPECIAL-FLOW-WAIT-1: flow verbs (WAIT / WAIT_UNTIL / STOP).
        // SPEC-DELTA (documented in .claude/TECHSCRIPT-GAP-CLOSURE-1.md #7/#8): WAIT is
        // NOT VM-blocking — the body re-executes every deterministic brain tick; an
        // unsatisfied WAIT closes the sequence gate for the verbs after it, then latches
        // open. WAIT_UNTIL gates on a Var condition. STOP ends this tick's execution.
        // ROOT BODY ONLY (chained bodies via TechSpecial.Call are trace-only anyway).
        if (flowActive) {
            if (std::strncmp(vpCanon, "WAIT_UNTIL", 10) == 0 &&
                (vpCanon[10] == ' ' || vpCanon[10] == '\t' || vpCanon[10] == '\0')) {
                BrainWaitState* ws = flowFindOrCreateWaitState(flowRt, (uint16_t)verbIdx);
                if (!ws) {
                    std::fprintf(stderr, "[BRAIN_FLOW_WAIT_OVERFLOW] idx=%d cap=%d wid=%d (WAIT_UNTIL skipped)\n",
                                 verbIdx, MechBrainRuntime::kBrainWaitCap, wid);
                    std::fflush(stderr);
                    continue;
                }
                if (ws->satisfied) continue;   // latched open — sequence proceeds
                char wKey[32], wVal[32];
                bool missionScope = false;
                if (!parseWaitUntilCondition(vpCanon, wKey, wVal, &missionScope)) {
                    std::fprintf(stderr, "[BRAIN_FLOW_WAIT_UNTIL_PARSE_FAIL] verb=%s wid=%d (opens; malformed)\n",
                                 vp, wid);
                    std::fflush(stderr);
                    ws->satisfied = 1;   // soft-fail OPEN: a malformed WAIT_UNTIL must not wedge the body
                    continue;
                }
                const char* cur = missionScope
                    ? (s_missionVarGate() ? g_missionVarStore.get(wKey) : "0")
                    : (varStore ? varStore->get(wKey, VarScope::Unit) : "0");
                if (std::strcmp(cur, wVal) == 0) {
                    ws->satisfied = 1;
                    std::fprintf(stderr, "[BRAIN_FLOW_WAIT_UNTIL_DONE] idx=%d key=%s val=%s wid=%d\n",
                                 verbIdx, wKey, cur, wid);
                    std::fflush(stderr);
                    continue;            // gate stays open — verbs after run THIS tick
                }
                if (!ws->armed) {
                    ws->armed = 1;       // one-time gated trace
                    std::fprintf(stderr, "[BRAIN_FLOW_WAIT_UNTIL_GATED] idx=%d key=%s want=%s cur=%s wid=%d\n",
                                 verbIdx, wKey, wVal, cur, wid);
                    std::fflush(stderr);
                }
                flowGateOpen = false;
                continue;
            }
            if (std::strncmp(vpCanon, "WAIT", 4) == 0 &&
                (vpCanon[4] == ' ' || vpCanon[4] == '\t' || vpCanon[4] == '\0')) {
                BrainWaitState* ws = flowFindOrCreateWaitState(flowRt, (uint16_t)verbIdx);
                if (!ws) {
                    std::fprintf(stderr, "[BRAIN_FLOW_WAIT_OVERFLOW] idx=%d cap=%d wid=%d (WAIT skipped)\n",
                                 verbIdx, MechBrainRuntime::kBrainWaitCap, wid);
                    std::fflush(stderr);
                    continue;
                }
                if (ws->satisfied) continue;   // latched open
                if (!ws->armed) {
                    const char* wargs = vpCanon + 4;
                    while (*wargs == ' ' || *wargs == '\t') ++wargs;
                    char* wend = nullptr;
                    float sec = std::strtof(wargs, &wend);
                    if (wend == wargs || sec < 0.0f || std::isnan(sec)) {
                        std::fprintf(stderr, "[BRAIN_FLOW_WAIT_PARSE_FAIL] verb=%s wid=%d (opens; malformed)\n",
                                     vp, wid);
                        std::fflush(stderr);
                        ws->satisfied = 1;   // soft-fail OPEN
                        continue;
                    }
                    ws->armed      = 1;
                    ws->deadlineMs = getBrainTimeMs() + (uint32_t)(sec * 1000.0f);
                    std::fprintf(stderr, "[BRAIN_FLOW_WAIT_ARM] idx=%d sec=%g deadline=%u now=%u wid=%d\n",
                                 verbIdx, sec, ws->deadlineMs, getBrainTimeMs(), wid);
                    std::fflush(stderr);
                    flowGateOpen = false;
                    continue;
                }
                if (getBrainTimeMs() >= ws->deadlineMs) {
                    ws->satisfied = 1;
                    std::fprintf(stderr, "[BRAIN_FLOW_WAIT_DONE] idx=%d deadline=%u now=%u wid=%d\n",
                                 verbIdx, ws->deadlineMs, getBrainTimeMs(), wid);
                    std::fflush(stderr);
                    continue;            // gate stays open — verbs after run THIS tick
                }
                flowGateOpen = false;
                continue;
            }
            if (std::strcmp(vpCanon, "STOP") == 0) {
                std::fprintf(stderr, "[BRAIN_FLOW_STOP] idx=%d wid=%d\n", verbIdx, wid);
                std::fflush(stderr);
                break;                   // end this tick's body execution
            }
        }

        // BRAINSPECIAL-ALIAS-1: compare via vpCanon (== verb text when no alias
        // resolution occurred — behavior-identical to the previous `verb ==` form).
        if (std::strcmp(vpCanon, "Brain.CorePower false") == 0) {
            // BRAIN-DECISION-INTENT-QUEUE-1: gate ON → emit intent; gate OFF → direct call.
            // NOTE: This fires only for the ROOT body. Chained-body POWERDOWN = 1B.
            if (runtime) {
                emitBrainIntent(runtime, wid, TACTICAL_ORDER_POWERDOWN, -1, 0,0,0, 0);
                std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=Brain.CorePower order=POWERDOWN tick=%u wid=%d\n",
                             getBrainTickIndex(), wid);
                std::fflush(stderr);
            } else {
                TacticalOrder pdOrder;
                pdOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_POWERDOWN);
                warrior->setGeneralTacOrder(pdOrder);
                std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=Brain.CorePower effect=POWERDOWN wid=%d\n", wid);
                std::fflush(stderr);
            }
            appliedEffect = true;
        } else if (std::strcmp(vpCanon, "Unit.Eject") == 0) {
            // DISPATCH-EFFECT-UNITEJECT-1: Unit.Eject (or alias coreEject) → TACTICAL_ORDER_EJECT.
            // ROOT BODY ONLY. Fires exactly once (ejectEffectApplied once-guard in warrior.cpp).
            if (runtime) {
                emitBrainIntent(runtime, wid, TACTICAL_ORDER_EJECT, -1, 0,0,0, 0);
                std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=Unit.Eject order=EJECT tick=%u wid=%d\n",
                             getBrainTickIndex(), wid);
                std::fflush(stderr);
            } else {
                TacticalOrder ejectOrder;
                ejectOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_EJECT);
                warrior->setGeneralTacOrder(ejectOrder);
                std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=Unit.Eject effect=EJECT wid=%d\n", wid);
                std::fflush(stderr);
            }
            appliedEffect = true;
        } else if (std::strcmp(vpCanon, "OPORD.CoreGuard") == 0) {
            // DISPATCH-EFFECT-COREGUARD-1: OPORD.CoreGuard (or alias coreGuard) → TACTICAL_ORDER_GUARD.
            // ROOT BODY ONLY. Fires exactly once (guardEffectApplied once-guard in warrior.cpp).
            if (runtime) {
                emitBrainIntent(runtime, wid, TACTICAL_ORDER_GUARD, -1, 0,0,0, 0);
                std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=OPORD.CoreGuard order=GUARD tick=%u wid=%d\n",
                             getBrainTickIndex(), wid);
                std::fflush(stderr);
            } else {
                TacticalOrder guardOrder;
                guardOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_GUARD);
                warrior->setGeneralTacOrder(guardOrder);
                std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=OPORD.CoreGuard effect=GUARD wid=%d\n", wid);
                std::fflush(stderr);
            }
            appliedEffect = true;
        } else if (std::strncmp(vpCanon, "OPORD.CoreMoveTo", 16) == 0) {
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
                std::fprintf(stderr, "[BRAIN_DISPATCH_MOVETO_PARSE_FAIL] verb=%s wid=%d\n", vp, wid);
                std::fflush(stderr);
            } else {
                args = endp;
                while (*args == ' ') ++args;
                float my = std::strtof(args, &endp);
                if (endp == args) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_MOVETO_PARSE_FAIL] verb=%s wid=%d\n", vp, wid);
                    std::fflush(stderr);
                } else {
                    args = endp;
                    while (*args == ' ') ++args;
                    float mz = std::strtof(args, &endp);
                    if (endp == args) {
                        std::fprintf(stderr, "[BRAIN_DISPATCH_MOVETO_PARSE_FAIL] verb=%s wid=%d\n", vp, wid);
                        std::fflush(stderr);
                    } else if (std::isnan(mx) || std::isnan(my) || std::isnan(mz)) {
                        std::fprintf(stderr, "[BRAIN_DISPATCH_MOVETO_NAN] x=%g y=%g z=%g wid=%d\n", mx, my, mz, wid);
                        std::fflush(stderr);
                    } else if (runtime) {
                        // Gate ON: emit intent; commitBrainIntents rebuilds the TacticalOrder.
                        emitBrainIntent(runtime, wid, TACTICAL_ORDER_MOVETO_POINT, -1, mx, my, mz, 0);
                        std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=OPORD.CoreMoveTo order=MOVETO_POINT pos=(%g %g %g) tick=%u wid=%d\n",
                                     mx, my, mz, getBrainTickIndex(), wid);
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
                        std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=OPORD.CoreMoveTo effect=MOVETO_POINT pos=(%g %g %g) wid=%d\n",
                                     mx, my, mz, wid);
                        std::fflush(stderr);
                        appliedEffect = true;
                    }
                }
            }
        } else if ((std::strncmp(vpCanon, "OPORD.CoreAttack", 16) == 0
                    || std::strncmp(vpCanon, "Brain.CoreAttack", 16) == 0)
                   && (vpCanon[16] == ' ' || vpCanon[16] == '\0')) {
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
                std::fprintf(stderr, "[BRAIN_DISPATCH_ATTACK_PARSE_FAIL] verb=%s wid=%d\n", vp, wid);
                std::fflush(stderr);
            } else if (parsedWID <= 0) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_ATTACK_BAD_WID] wid=%ld wid=%d\n", parsedWID, wid);
                std::fflush(stderr);
            } else {
                GameObjectPtr target = ObjectManager->getByWatchID((long)parsedWID);
                if (!target) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_ATTACK_BAD_WID] wid=%ld reason=not_found wid=%d\n", parsedWID, wid);
                    std::fflush(stderr);
                } else if (target->getWatchID() == (long)wid) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_ATTACK_SELF] wid=%ld wid=%d\n", parsedWID, wid);
                    std::fflush(stderr);
                } else if (target->getTeam() == warrior->getTeam()) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_ATTACK_FRIENDLY] wid=%ld wid=%d\n", parsedWID, wid);
                    std::fflush(stderr);
                } else if (runtime) {
                    // Gate ON: emit intent; commitBrainIntents rebuilds the TacticalOrder.
                    emitBrainIntent(runtime, wid, TACTICAL_ORDER_ATTACK_OBJECT, (int)parsedWID, 0,0,0, 0);
                    std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=OPORD.CoreAttack order=ATTACK_OBJECT targetWID=%ld tick=%u wid=%d\n",
                                 parsedWID, getBrainTickIndex(), wid);
                    std::fflush(stderr);
                    appliedEffect = true;
                } else {
                    // Gate OFF: construct TacticalOrder by hand — mirror missiongui.cpp:2805.
                    // FORBIDDEN: orderAttackObject, setAttackTarget, setSituationOpenFire.
                    TacticalOrder attackOrder;
                    attackOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_ATTACK_OBJECT, false);
                    attackOrder.targetWID = (long)parsedWID;
                    attackOrder.attackParams.type   = ATTACK_TO_DESTROY;
                    attackOrder.attackParams.method = ATTACKMETHOD_RANGED;
                    attackOrder.attackParams.range  = FIRERANGE_OPTIMAL;
                    attackOrder.attackParams.pursue = true;
                    // TACTIC-WEIGHTS-B: wire selected tactic. Local `runtime` is null on this
                    // gate-OFF path, so read selectedTactic via warrior->getBrainRuntime().
                    if (s_tacticWeightsBEnabled()) {
                        MechBrainRuntime* rtB = warrior->getBrainRuntime();
                        if (rtB) {
                            attackOrder.attackParams.tactic = (TacticType)rtB->selectedTactic;
                            std::fprintf(stderr, "[BRAIN_TACTIC_B] applied tactic=%d wid=%d\n", rtB->selectedTactic, wid);
                            std::fflush(stderr);
                        }
                    }
                    attackOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
                    warrior->setGeneralTacOrder(attackOrder);
                    std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=OPORD.CoreAttack effect=ATTACK_OBJECT targetWID=%ld wid=%d\n",
                                 parsedWID, wid);
                    std::fflush(stderr);
                    appliedEffect = true;
                }
            }
        } else if (std::strcmp(vpCanon, "Unit.Retreat") == 0) {
            // DISPATCH-EFFECT-UNITRETREAT-1: Unit.Retreat (or alias coreRetreat) → TACTICAL_ORDER_WITHDRAW.
            // ROOT BODY ONLY. Fires exactly once (retreatEffectApplied once-guard in warrior.cpp).
            if (runtime) {
                emitBrainIntent(runtime, wid, TACTICAL_ORDER_WITHDRAW, -1, 0,0,0, 0);
                std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=Unit.Retreat order=WITHDRAW tick=%u wid=%d\n",
                             getBrainTickIndex(), wid);
                std::fflush(stderr);
            } else {
                TacticalOrder retreatOrder;
                retreatOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_WITHDRAW, false);
                warrior->setGeneralTacOrder(retreatOrder);
                std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=Unit.Retreat effect=WITHDRAW wid=%d\n", wid);
                std::fflush(stderr);
            }
            appliedEffect = true;
        } else if (std::strncmp(vpCanon, "Unit.ClearMoveOrders", 20) == 0 &&
                   (vpCanon[20] == ' ' || vpCanon[20] == '\0')) {
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
                std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=Unit.ClearMoveOrders order=CLEAR_MOVE tick=%u wid=%d\n",
                             getBrainTickIndex(), wid);
                std::fflush(stderr);
            } else {
                // Intent queue OFF: cannot scrub here (direct call forbidden). Trace + no-op,
                // mirroring the patrol NO_QUEUE path. Live use requires MC2_BRAIN_INTENT_QUEUE=1.
                std::fprintf(stderr, "[BRAIN_CLEARMOVE_NO_QUEUE] wid=%d: requires MC2_BRAIN_INTENT_QUEUE=1\n", wid);
                std::fflush(stderr);
            }
        } else if (std::strncmp(vpCanon, "UnitQuery.SetTargetpriority", 27) == 0 &&
                   (vpCanon[27] == ' ' || vpCanon[27] == '\0')) {
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
                size_t tl = (size_t)(a - ts);
                char tok[64];
                if (tl == 0 || tl >= sizeof(tok)) { parsed = false; break; }
                std::memcpy(tok, ts, tl); tok[tl] = '\0';
                long v = 0;
                if (!brainResolveSymbolToken(tok, &v)) { parsed = false; break; }
                args5[fi] = v;
            }
            if (parsed) {
                while (*a == ' ' || *a == '\t') ++a;   // 5th field = rest-of-line expression
                long crit = 0;
                if (*a == '\0' || !brainResolveIntExpr(a, &crit)) parsed = false;
                else args5[4] = crit;
            }
            if (!parsed) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_TARGETPRIORITY_PARSE_FAIL] verb=%s wid=%d\n", vp, wid);
                std::fflush(stderr);
            } else if (runtime) {
                emitBrainTargetPolicyIntent(runtime, wid, args5[0], args5[1], args5[2], args5[3], args5[4]);
                std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=UnitQuery.SetTargetpriority order=TARGET_POLICY slot=%ld type=%ld p1=%ld p2=%ld crit=%ld tick=%u wid=%d\n",
                             args5[0], args5[1], args5[2], args5[3], args5[4], getBrainTickIndex(), wid);
                std::fflush(stderr);
            } else {
                std::fprintf(stderr, "[BRAIN_TARGETPRIORITY_NO_QUEUE] wid=%d: requires MC2_BRAIN_INTENT_QUEUE=1\n", wid);
                std::fflush(stderr);
            }
        } else if (s_brainPatrolGate() && std::strncmp(vpCanon, "OPORD.CorePatrol", 16) == 0 &&
                   (vpCanon[16] == ' ' || vpCanon[16] == '\0')) {
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
                std::fprintf(stderr, "[BRAIN_PATROL_PARSE_FAIL] no warrior wid=%d\n", wid);
                std::fflush(stderr);
            } else {
                MechBrainRuntime* prt = warrior->getBrainRuntime();
                if (!prt) {
                    std::fprintf(stderr, "[BRAIN_PATROL_PARSE_FAIL] no runtime wid=%d\n", wid);
                    std::fflush(stderr);
                } else if (prt->patrolActive) {
                    // Already active — this is a re-tick of a body that has CorePatrol.
                    // The per-tick advance is handled by tickPatrolAdvance() in warrior.cpp.
                    // Do NOT re-parse; do NOT re-emit waypoint[0].  Trace only.
                    std::fprintf(stderr, "[BRAIN_PATROL_ACTIVE] already running index=%u count=%u wid=%d\n",
                                 (unsigned)prt->patrolWaypointIndex, (unsigned)prt->patrolWaypointCount, wid);
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
                            std::fprintf(stderr, "[BRAIN_PATROL_NAN] wp=%u x=%g y=%g z=%g wid=%d (skipped)\n",
                                         (unsigned)wpCount, x, y, z, wid);
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
                        std::fprintf(stderr, "[BRAIN_PATROL_PARSE_FAIL] count=%u parseOk=%d wid=%d (need >=2 waypoints)\n",
                                     (unsigned)wpCount, (int)parseOk, wid);
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
                        std::fprintf(stderr, "[BRAIN_PATROL_START] count=%u loop=%d wid=%d\n",
                                     (unsigned)wpCount, (int)loopMode, wid);
                        std::fflush(stderr);
                        // Emit MOVETO_POINT to waypoints[0].
                        float wx = prt->patrolWaypoints[0][0];
                        float wy = prt->patrolWaypoints[0][1];
                        float wz = prt->patrolWaypoints[0][2];
                        if (runtime) {
                            // Gate ON: emit intent; commitBrainIntents builds the TacticalOrder.
                            emitBrainIntent(runtime, wid, TACTICAL_ORDER_MOVETO_POINT, -1, wx, wy, wz, 0);
                            std::fprintf(stderr, "[BRAIN_PATROL_EMIT] index=0 pos=(%g %g %g) tick=%u wid=%d\n",
                                         wx, wy, wz, getBrainTickIndex(), wid);
                            std::fflush(stderr);
                        } else {
                            // Gate OFF (no intent queue): patrol cannot issue MOVETO_POINT here
                            // without adding a 7th setGeneralTacOrder call-site (relaxed_guard_doc
                            // contract requires count=6).  Emit a one-time warning.
                            // Live-engine usage requires MC2_BRAIN_INTENT_QUEUE=1 for patrol orders.
                            std::fprintf(stderr, "[BRAIN_PATROL_NO_QUEUE] wid=%d: patrol requires MC2_BRAIN_INTENT_QUEUE=1 for order emission\n", wid);
                            std::fflush(stderr);
                        }
                        appliedEffect = true;
                    }
                }
            }
        } else if (fsmGate && std::strncmp(vpCanon, "Unit.SetState", 13) == 0 &&
                   (vpCanon[13] == ' ' || vpCanon[13] == '\t' || vpCanon[13] == '\0')) {
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
                std::fprintf(stderr, "[BRAIN_FSM_SETSTATE] from=%s to=%s wid=%d\n",
                             fsm_rt->prevState, fsm_rt->currentState, wid);
                std::fflush(stderr);
            }
            return appliedEffect;  // early exit — no subsequent verbs after SetState
        } else if (fsmGate && std::strcmp(vpCanon, "Unit.SetStatePrev") == 0) {
            // BRAIN-FSM-1K-A: Unit.SetStatePrev — transBack equivalent, swaps currentState↔prevState.
            // Early return (no order call).
            MechBrainRuntime* fsm_rt = warrior ? warrior->getBrainRuntime() : nullptr;
            if (fsm_rt) {
                char tmp[32];
                std::memcpy(tmp, fsm_rt->prevState, 32);
                std::strncpy(fsm_rt->prevState, fsm_rt->currentState, 31);
                fsm_rt->prevState[31] = '\0';
                std::memcpy(fsm_rt->currentState, tmp, 32);
                std::fprintf(stderr, "[BRAIN_FSM_SETSTATEPREV] now=%s wid=%d\n",
                             fsm_rt->currentState, wid);
                std::fflush(stderr);
            }
            return appliedEffect;  // early exit — no subsequent verbs after SetStatePrev
        } else if (fsmGate && std::strncmp(vpCanon, "Unit.InState", 12) == 0 &&
                   (vpCanon[12] == ' ' || vpCanon[12] == '\t' || vpCanon[12] == '\0')) {
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
            std::fprintf(stderr, "[BRAIN_FSM_INSTATE] state=%s match=%d wid=%d\n", testState, match, wid);
            std::fflush(stderr);
        } else if (fsmGate && std::strncmp(vpCanon, "Unit.NotInState", 15) == 0 &&
                   (vpCanon[15] == ' ' || vpCanon[15] == '\t' || vpCanon[15] == '\0')) {
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
            std::fprintf(stderr, "[BRAIN_FSM_NOTINSTATE] state=%s match=%d wid=%d\n", testState, match, wid);
            std::fflush(stderr);
        } else if (fsmGate && std::strncmp(vpCanon, "Unit.SetStateIf", 15) == 0 &&
                   (vpCanon[15] == ' ' || vpCanon[15] == '\t' || vpCanon[15] == '\0')) {
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
                    std::fprintf(stderr, "[BRAIN_FSM_SETSTATEIF] WARNING: Unit.SetStateIf requires MC2_BRAIN_DISPATCH_VAR=1 — verb is INERT\n");
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
                        std::fprintf(stderr, "[BRAIN_FSM_SETSTATEIF] var=%s cur=%s want=%s match=1 from=%s to=%s wid=%d\n",
                                     varKey, curVal, varValue, fsm_rt->prevState, fsm_rt->currentState, wid);
                    } else {
                        std::fprintf(stderr, "[BRAIN_FSM_SETSTATEIF] var=%s cur=%s want=%s match=1 from=(no-rt) to=%s wid=%d\n",
                                     varKey, curVal, varValue, stateName, wid);
                    }
                    std::fflush(stderr);
                    return appliedEffect;  // early exit on match — same semantics as SetState
                } else {
                    std::fprintf(stderr, "[BRAIN_FSM_SETSTATEIF] var=%s cur=%s want=%s match=0 wid=%d\n",
                                 varKey, curVal, varValue, wid);
                    std::fflush(stderr);
                    // No transition; body continues.
                }
            }
        } else if (isRecognizedVerb(vpCanon)) {
            // Recognized but no effect implemented this slice — trace only.
            std::fprintf(stderr, "[BRAIN_DISPATCH] verb=%s wid=%d (apply-mode: no effect this verb)\n", vp, wid);
            std::fflush(stderr);
        } else {
            std::fprintf(stderr, "[BRAIN_DISPATCH_UNKNOWN] verb=%s wid=%d\n", vp, wid);
            std::fflush(stderr);
        }

        // BRAINSPECIAL-FLOW-WAIT-1: mark GENERAL-slot effect verbs fired (per-verb-index
        // refire guard for the every-tick re-dispatch under flow gating). Marked whether
        // or not the order soft-failed — matching the class-level once-guard semantics
        // (warrior.cpp pre-sets its flags before Apply on the non-flow path too).
        // Verbs that `continue` above (Call/Var/flow) never reach here — none are effects.
        if (flowActive && isGeneralEffectVerbToken(vpCanon))
            flowMarkFired(flowRt, (uint16_t)verbIdx);
    }

    return appliedEffect;
}

// ---------------------------------------------------------------------------
// BRAIN-DECISION-INTENT-QUEUE-1: commitBrainIntents
//
// COMMIT PHASE — the ONLY function that calls warrior->setGeneralTacOrder() when
// MC2_BRAIN_INTENT_QUEUE=1.  Called right after executeSpecialBody_Apply returns
// (inline, same thread, this rung).  Drains runtime->pendingIntents[] and for each
// intent reconstructs a TacticalOrder and calls warrior->setGeneralTacOrder().
// DISPATCH-INTENT-CLEARMOVEORDERS-1: also the ONLY function that may call
// warrior->clearMoveOrders() (CLEAR_MOVE intents) — a movement scrub, not a slot order,
// so it is handled before the orderType switch and does not count toward the 6 verbs.
// UNITQUERY-SETTARGETPRIORITY-1: likewise the ONLY function that may call
// warrior->setTargetPriority() (TARGET_POLICY intents); also handled before the switch.
//
// FORBIDDEN-CALL GUARD (commit phase): this function and only this function may call
// warrior->setGeneralTacOrder() when gate ON.  The verb handlers in
// executeSpecialBody_Apply must NOT call setGeneralTacOrder when gate ON.
//
// MOVETO reconstruction mirrors missiongui.cpp:4161 (same as the gate-OFF path above).
// ATTACK reconstruction mirrors missiongui.cpp:2805.
// All other verbs: simple init(ORDER_ORIGIN_SELF, orderType).
//
// Emits: [BRAIN_INTENT_COMMIT] verb=<v> order=<type> tick=<brainTick> wid=<W> per intent.
// After draining, resets pendingIntentCount to 0.
// warrior and runtime must be non-null (caller guards).
void commitBrainIntents(MechWarrior* warrior, MechBrainRuntime* runtime) {
    if (!warrior || !runtime) return;
    const int n = runtime->pendingIntentCount;
    for (int i = 0; i < n; ++i) {
        const BrainOrderIntent& intent = runtime->pendingIntents[i];
        // DISPATCH-INTENT-CLEARMOVEORDERS-1: CLEAR_MOVE is a movement-queue scrub, not a
        // GENERAL-slot order. commitBrainIntents is the ONLY function the FORBIDDEN-CALL
        // contract permits to mutate warrior order/movement state, so the clearMoveOrders()
        // call lives here. Movement-only: does not touch patrol/FSM/Var/target state.
        if (intent.kind == BrainIntentKind::CLEAR_MOVE) {
            warrior->clearMoveOrders();
            std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=Unit.ClearMoveOrders order=CLEAR_MOVE tick=%u wid=%d\n",
                         intent.brainTick, intent.warriorId);
            std::fflush(stderr);
            continue;
        }
        // UNITQUERY-SETTARGETPRIORITY-1: TARGET_POLICY writes one target-priority list slot.
        // commitBrainIntents is the ONLY function permitted to call setTargetPriority (a clean
        // single-field setter — no order/movement side effects). WID-ordered like all commits.
        if (intent.kind == BrainIntentKind::TARGET_POLICY) {
            warrior->setTargetPriority(intent.policyArgs[0], intent.policyArgs[1],
                                       intent.policyArgs[2], intent.policyArgs[3], intent.policyArgs[4]);
            std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=UnitQuery.SetTargetpriority order=TARGET_POLICY slot=%d type=%d tick=%u wid=%d\n",
                         intent.policyArgs[0], intent.policyArgs[1], intent.brainTick, intent.warriorId);
            std::fflush(stderr);
            continue;
        }
        switch (intent.orderType) {
            case TACTICAL_ORDER_POWERDOWN: {
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_POWERDOWN);
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=Brain.CorePower order=POWERDOWN tick=%u wid=%d\n",
                             intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            case TACTICAL_ORDER_EJECT: {
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_EJECT);
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=Unit.Eject order=EJECT tick=%u wid=%d\n",
                             intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            case TACTICAL_ORDER_GUARD: {
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_GUARD);
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=OPORD.CoreGuard order=GUARD tick=%u wid=%d\n",
                             intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            case TACTICAL_ORDER_MOVETO_POINT: {
                LocationNode moveNode;
                moveNode.location.x = intent.waypoint[0];
                moveNode.location.y = intent.waypoint[1];
                moveNode.location.z = intent.waypoint[2];
                moveNode.run  = true;
                moveNode.next = nullptr;
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_MOVETO_POINT, false);
                o.initWayPath(&moveNode);
                o.moveParams.wait = false;
                o.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
                o.pack(nullptr, nullptr);
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=OPORD.CoreMoveTo order=MOVETO_POINT pos=(%g %g %g) tick=%u wid=%d\n",
                             intent.waypoint[0], intent.waypoint[1], intent.waypoint[2],
                             intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            case TACTICAL_ORDER_ATTACK_OBJECT: {
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_ATTACK_OBJECT, false);
                o.targetWID = (long)intent.targetWID;
                o.attackParams.type   = ATTACK_TO_DESTROY;
                o.attackParams.method = ATTACKMETHOD_RANGED;
                o.attackParams.range  = FIRERANGE_OPTIMAL;
                o.attackParams.pursue = true;
                // TACTIC-WEIGHTS-B: wire selected tactic (runtime non-null per caller guard).
                if (s_tacticWeightsBEnabled()) {
                    o.attackParams.tactic = (TacticType)runtime->selectedTactic;
                    std::fprintf(stderr, "[BRAIN_TACTIC_B] applied tactic=%d wid=%d\n", runtime->selectedTactic, intent.warriorId);
                    std::fflush(stderr);
                }
                o.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=OPORD.CoreAttack order=ATTACK_OBJECT targetWID=%d tick=%u wid=%d\n",
                             intent.targetWID, intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            case TACTICAL_ORDER_WITHDRAW: {
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_WITHDRAW, false);
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=Unit.Retreat order=WITHDRAW tick=%u wid=%d\n",
                             intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            default:
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT_UNKNOWN] order=%d tick=%u wid=%d\n",
                             (int)intent.orderType, intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
        }
    }
    runtime->pendingIntentCount = 0;
}

// ---------------------------------------------------------------------------
// BRAIN-ENGAGE-1: tickEngageNearest
//
// Per-tick autonomous threat engagement for Patrol/Guard OPORDs (discussion #19: Patrol/Guard
// "engage threats within radius"). Acquires the nearest live enemy within runtime->engageRadius
// and issues a contract-blessed TACTICAL_ORDER_ATTACK_OBJECT via the intent queue — the same
// path CoreAttack uses, with an auto-selected target instead of a hardcoded WID. Re-emits only
// when the target changes (a persistent target keeps its pursue-attack order). Returns true while
// actively engaging so the caller suppresses patrol advance (fight in place, don't walk off).
// Gate: MC2_BRAIN_ENGAGE (default OFF).
bool tickEngageNearest(MechWarrior* warrior, MechBrainRuntime* runtime, int wid) {
    if (!s_brainEngageGate()) return false;
    if (!warrior || !runtime) return false;
    // Scout observes only — it moves its route and reports contacts but never engages.
    if (runtime->scoutObserveOnly) return false;
    // Armed when the unit has an active combatant OPORD (Patrol walks+engages; Guard/Sentry/Escort
    // hold/move+engage).
    if (!runtime->patrolActive && !runtime->guardHold && !runtime->escortMoving) return false;

    MoverPtr veh = warrior->getVehicle();
    if (!veh || veh->isDisabled()) return false;

    // A sleeping Sentry only engages once a detected enemy has woken it (WakeOnAttack).
    if (runtime->sentryAsleep && !runtime->sentryWoken) {
        int wcl[8]; long wn = veh->getContacts(wcl, (1 | 64), 0);
        if (wn <= 0) return false;                  // still asleep, nothing detected
        runtime->sentryWoken = true;
        std::fprintf(stderr, "[BRAIN_SENTRY_WAKE] wid=%d\n", wid);
        std::fflush(stderr);
    }

    // Detection = the engine's TEAM CONTACT list: enemies this unit's team has actually detected.
    // getContacts() already incorporates per-unit sensor-tier ranges (basic/intermediate/advanced),
    // team sensor sharing, visual sighting and ECM — the engine's full visibility model. So we
    // engage ONLY enemies the team can genuinely perceive, never enemies the unit has no way of
    // knowing exist. Patrol/scripted movement closes the distance until enemies enter detection;
    // we do NOT preempt that movement except to fight a genuinely-detected threat.
    unsigned long tgtWID = 0;
    long teamContacts = 0;
    float tcNear = -1.0f;
    {
        int cl[MAX_CONTACTS_PER_SENSOR];
        teamContacts = veh->getContacts(cl, (1 | 64), CONTACT_SORT_DISTANCE);  // ENEMY|NOT_DISABLED, nearest first
        if (teamContacts > 0) {
            GameObjectPtr c0 = ObjectManager->get(cl[0]);                       // contact list holds handles
            if (c0) { tgtWID = c0->getWatchID(); tcNear = veh->distanceFrom(c0->getPosition()); }
        }
    }

    // EngageRadius localization: the team shares the contact (the unit KNOWS the enemy exists),
    // but it only commits to attacking once the threat is within its own EngageRadius — distant
    // units keep patrolling/holding instead of the whole map swarming a single sighting.
    // engageRadius<=0 means "no commitment limit" (engage any detected contact).
    if (tgtWID && runtime->engageRadius > 0.0f && tcNear > runtime->engageRadius)
        tgtWID = 0;

    // RequestHelp (answering side): if an ally summoned us, engage the assigned target regardless
    // of our own EngageRadius — we were called to help. Clear the assignment once it is dead/gone.
    if (runtime->helpTargetWID != 0) {
        GameObjectPtr h = ObjectManager->getByWatchID(runtime->helpTargetWID);
        if (h && !h->isDisabled() && veh->isEnemy(h->getTeam()))
            tgtWID = runtime->helpTargetWID;            // answer the call (overrides EngageRadius)
        else
            runtime->helpTargetWID = 0;                 // target gone; stop helping
    }

    // Throttled diagnostic (MC2_BRAIN_ENGAGE_TRACE=1): detected-contact count + nearest distance.
    if (std::getenv("MC2_BRAIN_ENGAGE_TRACE") && (getBrainTickIndex() % 64u) == 0u) {
        std::fprintf(stderr, "[BRAIN_ENGAGE_EVAL] wid=%d teamContacts=%ld tcNear=%.0f tgt=%lu\n",
                     wid, teamContacts, tcNear, tgtWID);
        std::fflush(stderr);
    }

    if (tgtWID == 0) {
        // No enemy in range — disengage so patrol/idle resumes next tick.
        if (runtime->engageTargetWID != 0) {
            runtime->engageTargetWID = 0;
            std::fprintf(stderr, "[BRAIN_ENGAGE_CLEAR] wid=%d\n", wid);
            std::fflush(stderr);
        }
        return false;
    }

    // Enemy in range. Issue/refresh the attack order only on target change (pursue-attack
    // persists across ticks for the same target).
    if (tgtWID != runtime->engageTargetWID) {
        runtime->engageTargetWID = tgtWID;
        if (s_intentQueueEnabled()) {
            emitBrainIntent(runtime, wid, TACTICAL_ORDER_ATTACK_OBJECT, (int)tgtWID, 0, 0, 0, 0);
            std::fprintf(stderr, "[BRAIN_ENGAGE] target=%lu tick=%u wid=%d\n",
                         tgtWID, getBrainTickIndex(), wid);
            std::fflush(stderr);
            if (!s_brainCommitPhaseEnabled())
                commitBrainIntents(warrior, runtime);
            // RequestHelp (calling side): summon allies within AttackerHelpRadius to this target.
            // Localized reinforcement (carver AttackerHelpRadius=100) — NOT a map-wide swarm. A
            // summoned ally engages the target via its helpTargetWID override; nearby sentries wake.
            if (runtime->swRequestHelp == 1 && runtime->swAttackerHelpRadius > 0.0f) {
                long nm = ObjectManager->getNumMovers();
                int called = 0;
                for (long i = 0; i < nm; ++i) {
                    MoverPtr m = ObjectManager->getMover(i);
                    if (!m || m == veh || m->isDisabled()) continue;
                    if (veh->isEnemy(m->getTeam())) continue;                 // allies only
                    if (veh->distanceFrom(m->getPosition()) > runtime->swAttackerHelpRadius) continue;
                    MechWarriorPtr ally = m->getPilot();
                    if (!ally) continue;
                    MechBrainRuntime* art = ally->getBrainRuntime();
                    if (!art) continue;
                    art->helpTargetWID = tgtWID;
                    if (art->sentryAsleep) art->sentryWoken = true;           // a nearby fight wakes a sentry
                    ++called;
                }
                if (called > 0) {
                    std::fprintf(stderr, "[BRAIN_REQUEST_HELP] wid=%d called=%d radius=%.0f target=%lu\n",
                                 wid, called, runtime->swAttackerHelpRadius, tgtWID);
                    std::fflush(stderr);
                }
            }
        } else {
            std::fprintf(stderr, "[BRAIN_ENGAGE_NO_QUEUE] wid=%d requires MC2_BRAIN_INTENT_QUEUE=1\n", wid);
            std::fflush(stderr);
        }
    }
    return true;  // actively engaging — caller suppresses patrol advance
}

// ---------------------------------------------------------------------------
// BRAIN-OPORD-MOVE-1: tickOpordMove — non-patrol OPORD movement when NOT engaging.
//
//   Escort      : move toward the escortee unit (follow + defend).
//   ReturnToPost: a Guard/Sentry unit that pursued a threat walks back to its post once clear.
//
// Called from runBrain AFTER tickEngageNearest returns false (no current threat) and only when
// the unit is not patrolling. Emits MOVETO_POINT via the intent queue, throttled so we re-order
// only when meaningfully off-target (avoids per-tick order spam). Returns true if it issued a move.
// Gate: MC2_BRAIN_ENGAGE (shares the OPORD-runtime gate set).
static bool advanceOpordSlot(MechBrainRuntime* runtime, int wid);  // BRAIN-OPORD-PROGRESS-1 (defined below)
bool tickOpordMove(MechWarrior* warrior, MechBrainRuntime* runtime, int wid) {
    if (!s_brainEngageGate()) return false;
    if (!warrior || !runtime) return false;
    MoverPtr veh = warrior->getVehicle();
    if (!veh || veh->isDisabled()) return false;
    if (!s_intentQueueEnabled()) return false;
    // Throttle: only re-evaluate the move every 16 brain ticks.
    if ((getBrainTickIndex() % 16u) != 0u) return false;

    // Escort — follow the escortee (or hand off to the next slot if the escortee is gone).
    if (runtime->escortMoving && runtime->escortTargetWID != 0) {
        GameObjectPtr e = ObjectManager->getByWatchID(runtime->escortTargetWID);
        if (!e || e->isDisabled()) {                 // escortee dead/gone — Escort complete
            runtime->escortMoving = false;
            advanceOpordSlot(runtime, wid);
            return false;
        }
        if (e && !e->isDisabled()) {
            Stuff::Vector3D ep = e->getPosition();
            float d = veh->distanceFrom(ep);
            if (d > 120.0f) {                            // stay within ~120 of the escortee
                emitBrainIntent(runtime, wid, TACTICAL_ORDER_MOVETO_POINT, -1, ep.x, ep.y, ep.z, 0);
                if (!s_brainCommitPhaseEnabled()) commitBrainIntents(warrior, runtime);
                std::fprintf(stderr, "[BRAIN_ESCORT_MOVE] wid=%d to=(%g %g) d=%.0f\n", wid, ep.x, ep.y, d);
                std::fflush(stderr);
                return true;
            }
        }
        return false;
    }

    // ReturnToPost — a guard that wandered (pursuing a now-cleared threat) walks back to its post.
    if (runtime->guardHold && runtime->swReturnToPost == 1 && runtime->postSet) {
        Stuff::Vector3D post; post.x = runtime->postPos[0]; post.y = runtime->postPos[1]; post.z = runtime->postPos[2];
        float d = veh->distanceFrom(post);
        if (d > 100.0f) {                                // returned-to-post tolerance
            emitBrainIntent(runtime, wid, TACTICAL_ORDER_MOVETO_POINT, -1, post.x, post.y, post.z, 0);
            if (!s_brainCommitPhaseEnabled()) commitBrainIntents(warrior, runtime);
            std::fprintf(stderr, "[BRAIN_RETURN_POST] wid=%d d=%.0f\n", wid, d);
            std::fflush(stderr);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// BRAIN-OPORD-PROGRESS-1: advance Primary -> Secondary -> Tertiary when the active slot completes.
// A finite OPORD (a non-looping Patrol/MoveTo/Withdraw reaching its destination, or an Escort that
// lost its target) hands off to the next slot. The successor's hold/engage behavior is activated by
// TYPE — carver's Secondary is almost always Guard, and secondary/tertiary route waypoints are not
// stored (finite-move successors with their own routes are not a carver pattern). Terminal OPORDs
// (looping Patrol, Guard, Sentry) never reach here. Returns true if it advanced a slot.
static bool advanceOpordSlot(MechBrainRuntime* runtime, int wid) {
    if (!runtime || runtime->opordCursor >= 2) return false;
    uint8_t next = runtime->opordType[runtime->opordCursor + 1];
    if (next == 255 || next == 9) return false;          // no further slot / PlayerControlled
    runtime->opordCursor++;
    if (next == 7) { runtime->guardHold = true; runtime->engageRadius = 0.0f; }  // Attack: aggressive
    else           { runtime->guardHold = true; }                                // Guard/Sentry/...: hold+engage
    std::fprintf(stderr, "[BRAIN_OPORD_PROGRESS] wid=%d -> slot=%u type=%u\n",
                 wid, (unsigned)runtime->opordCursor, (unsigned)next);
    std::fflush(stderr);
    return true;
}

// ---------------------------------------------------------------------------
// BRAIN-OPORD-COREPATROL-1: tickPatrolAdvance
//
// Called every brain tick from warrior.cpp AFTER executeSpecialBody_Apply + commitBrainIntents.
// Gate: MC2_BRAIN_PATROL (default OFF).  No-op when gate OFF or patrolActive==false.
//
// Per-tick arrival poll:
//   Reads warrior->curTacOrder.status(warrior) == TACORDER_SUCCESS.
//   On arrival:
//     - Advance cursor: loop → (index+1)%count; once → index+1 clamped to count-1.
//     - If once and index was already at count-1: set patrolActive=false, emit
//       [BRAIN_PATROL_DONE], stop.
//     - Emit MOVETO_POINT to waypoints[newIndex] via emitBrainIntent (gate ON) or
//       direct setGeneralTacOrder (gate OFF).
//     - Emit [BRAIN_PATROL_ADVANCE] from=<old> to=<new> wid=<W>.
//
// moveToEffectApplied is intentionally NOT touched — patrol manages its own cursor guard.
// Returns true if an advance was performed this tick.
bool tickPatrolAdvance(MechWarrior* warrior, MechBrainRuntime* runtime, int wid) {
    if (!s_brainPatrolGate()) return false;
    if (!warrior || !runtime) return false;
    if (!runtime->patrolActive) return false;
    if (runtime->patrolWaypointCount == 0) return false;

    // PATROL-DRIVE-1: initial kick. A freshly-activated patrol (declarative mission.fit OPORD
    // populate) has no outstanding MOVETO order, so the arrival-poll below would never fire and
    // the unit would sit idle. Drive to the CURRENT waypoint once, then let the arrival-poll
    // advance cursor on every subsequent arrival. (The CorePatrol special begin-path emits
    // waypoint[0] itself and sets patrolStarted=true, so it skips this kick.)
    if (!runtime->patrolStarted) {
        runtime->patrolStarted = true;
        uint8_t idx = runtime->patrolWaypointIndex;
        float kx = runtime->patrolWaypoints[idx][0];
        float ky = runtime->patrolWaypoints[idx][1];
        float kz = runtime->patrolWaypoints[idx][2];
        if (s_intentQueueEnabled()) {
            emitBrainIntent(runtime, wid, TACTICAL_ORDER_MOVETO_POINT, -1, kx, ky, kz, 0);
            std::fprintf(stderr, "[BRAIN_PATROL_KICK] index=%u pos=(%g %g %g) tick=%u wid=%d\n",
                         (unsigned)idx, kx, ky, kz, getBrainTickIndex(), wid);
            std::fflush(stderr);
            if (!s_brainCommitPhaseEnabled()) {
                commitBrainIntents(warrior, runtime);
            }
        } else {
            std::fprintf(stderr, "[BRAIN_PATROL_NO_QUEUE] kick index=%u wid=%d: requires MC2_BRAIN_INTENT_QUEUE=1\n",
                         (unsigned)idx, wid);
            std::fflush(stderr);
        }
        return true;
    }

    // Poll arrival via public getCurTacOrder() accessor (curTacOrder is protected).
    TacticalOrder* cur = warrior->getCurTacOrder();
    if (!cur || cur->status(warrior) != TACORDER_SUCCESS) return false;

    uint8_t oldIndex = runtime->patrolWaypointIndex;
    uint8_t count    = runtime->patrolWaypointCount;

    // once-mode stop check: already at last waypoint.
    if (!runtime->patrolLoop && oldIndex >= (count - 1)) {
        runtime->patrolActive = false;
        std::fprintf(stderr, "[BRAIN_PATROL_DONE] wid=%d\n", wid);
        std::fflush(stderr);
        advanceOpordSlot(runtime, wid);   // BRAIN-OPORD-PROGRESS-1: hand off to the next OPORD slot
        return false;
    }

    // Advance cursor.
    uint8_t newIndex;
    if (runtime->patrolLoop) {
        newIndex = (uint8_t)((oldIndex + 1) % count);
    } else {
        newIndex = (uint8_t)(oldIndex + 1);
        if (newIndex >= count) {
            newIndex = count - 1;
            runtime->patrolActive = false;
        }
    }
    runtime->patrolWaypointIndex = newIndex;

    std::fprintf(stderr, "[BRAIN_PATROL_ADVANCE] from=%u to=%u wid=%d\n",
                 (unsigned)oldIndex, (unsigned)newIndex, wid);
    std::fflush(stderr);

    if (!runtime->patrolActive) {
        // once-mode: just advanced to last, now done.
        std::fprintf(stderr, "[BRAIN_PATROL_DONE] wid=%d\n", wid);
        std::fflush(stderr);
        advanceOpordSlot(runtime, wid);   // BRAIN-OPORD-PROGRESS-1: hand off to the next OPORD slot
        return true;
    }

    float wx = runtime->patrolWaypoints[newIndex][0];
    float wy = runtime->patrolWaypoints[newIndex][1];
    float wz = runtime->patrolWaypoints[newIndex][2];

    // Emit MOVETO_POINT to new waypoint.
    // Emit MOVETO_POINT via intent queue only.
    // (No direct setGeneralTacOrder here — relaxed_guard_doc call-site count must stay 6.)
    // Live usage requires MC2_BRAIN_INTENT_QUEUE=1; without it a NO_QUEUE trace fires.
    if (s_intentQueueEnabled()) {
        emitBrainIntent(runtime, wid, TACTICAL_ORDER_MOVETO_POINT, -1, wx, wy, wz, 0);
        std::fprintf(stderr, "[BRAIN_PATROL_EMIT] index=%u pos=(%g %g %g) tick=%u wid=%d\n",
                     (unsigned)newIndex, wx, wy, wz, getBrainTickIndex(), wid);
        std::fflush(stderr);
        // GAP B: tickPatrolAdvance runs outside the normal apply+commit block, so with
        // commit-phase OFF it must self-commit inline. With commit-phase ON, leave the
        // intent in pendingIntents so the deferred WID-ordered drain (objmgr.cpp) picks
        // it up — self-committing here would drain pendingIntentCount→0 and the deferred
        // phase would report committed=0.
        if (!s_brainCommitPhaseEnabled()) {
            commitBrainIntents(warrior, runtime);
        }
    } else {
        std::fprintf(stderr, "[BRAIN_PATROL_NO_QUEUE] advance index=%u wid=%d: requires MC2_BRAIN_INTENT_QUEUE=1\n",
                     (unsigned)newIndex, wid);
        std::fflush(stderr);
    }
    return true;
}

// ---------------------------------------------------------------------------
// DISPATCH-LOADER-RAW-1: parseBrainSpecialBody_RawScan
//
// Raw line-scanner for the new brace-block TechSpecial format used by
// carver_v_enhanced (and any other tool that emits inline-quoted DO args).
//
// Format parsed:
//   TechSpecial {
//       key  = "..."
//       alias = "..."
//       type = "..."
//       sourceABLFunction = "..."
//       Body {
//           DO <rest-of-line>
//           STOP
//       }
//   }
//
// Multiple TechSpecial blocks are parsed; ALL verbs from ALL bodies are
// appended to outBody.verbs in file order.
//
// Safety caps (DISPATCH-LOADER-RAW-1):
//   kMaxLineLen     = 512 bytes — lines longer than this are skipped with a trace.
//   kMaxVerbsPerBody = 256 — excess DO lines within one Body block are skipped.
//   kMaxBodies      = 256 — excess TechSpecial blocks are skipped entirely.
//
// FORBIDDEN-CALL CONTRACT:
//   Calls ONLY std::ifstream + string ops + fprintf/fflush. No order functions.
//   No warrior pointer. No tac-order writes. Verified by inspection.
//
// Returns true if at least one verb was loaded.
static const int kMaxLineLen      = 512;
static const int kMaxVerbsPerBody = 256;
static const int kMaxBodies       = 256;

// parseBrainSpecialBody_RawScan
//
// TECHSCRIPT-CALL-CHAIN-1A: extended to also populate outIndex (if non-null).
// For each TechSpecial block, the key= field is captured into a SpecialIndexEntry
// alongside the block's Body verbs. This is unconditional (not gated) — the index
// is always populated when the raw scanner runs, regardless of DISPATCH_CALL state.
// The gate controls runtime call resolution only.
//
// Entry-body selection rule (for outBody — the existing single-body member):
//   1. First TechSpecial block whose key contains "scenario_main" (case-sensitive).
//   2. Else first block whose type field = "MissionSpecial".
//   3. Else the first block found.
// If no blocks are found, outBody remains empty.
//
// Helper: parse a quoted string value from "key = \"...\""  or  "key = [...]".
// Returns the value token or "" on failure.
static std::string parseQuotedField(const char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    char closeDelim = '\0';
    if (*p == '"')      closeDelim = '"';
    else if (*p == '[') closeDelim = ']';
    else                return "";
    ++p;
    const char* end = std::strchr(p, closeDelim);
    if (!end) return "";
    return std::string(p, end);
}

static bool parseBrainSpecialBody_RawScan(const char* fitPath, BrainSpecialBody& outBody,
                                           SpecialIndex* outIndex,
                                           std::string* outChosenKey = nullptr) {
    std::ifstream inFile(fitPath);
    if (!inFile.is_open())
        return false;

    // State machine:
    //   OUTER      — scanning for "TechSpecial {" or "Aliases {"
    //   IN_SPECIAL — inside TechSpecial block, scanning for "Body {" or key=/type=/alias= fields
    //   IN_BODY    — inside Body block, collecting DO lines
    //   IN_ALIASES — inside Aliases block (BRAINSPECIAL-ALIAS-1), collecting <alias> = "<canonical>"
    enum { OUTER, IN_SPECIAL, IN_BODY, IN_ALIASES } state = OUTER;

    int bodiesFound = 0;
    int verbsThisBody = 0;
    std::string line;

    // Per-block state (reset on each TechSpecial {).
    std::string currentKey;
    std::string currentType;
    std::string currentAlias;      // BRAINSPECIAL-ALIAS-1: per-block alias= field
    std::string currentVariantOf;  // BRAINSPECIAL-VARIANTOF-1: per-block variantOf= field
    std::vector<std::string> currentVerbs;
    // BRAINSPECIAL-SCOPE-GLOBAL-1 (quirk fix): pre-slice, a TechSpecial WITH a Body was
    // committed twice — once (with verbs) at the Body's closing brace, and once more
    // (empty duplicate) at the TechSpecial's own closing brace. The duplicates were
    // harmless for entry-body selection / Call lookup (first match wins) but inflate the
    // index and double-count global merges. Track whether this TechSpecial already
    // committed its Body so the closing brace only commits truly body-less blocks.
    bool bodyCommittedThisSpecial = false;

    // Entry-body selection tracking.
    // We collect ALL blocks into the index, then choose outBody post-scan.
    // indexEntries built in parallel.
    // (outBody is set from the selected block at the end.)
    struct RawBlock {
        std::string key;
        std::string type;
        std::string alias;      // BRAINSPECIAL-ALIAS-1
        std::string variantOf;  // BRAINSPECIAL-VARIANTOF-1
        std::vector<std::string> verbs;
    };
    std::vector<RawBlock> rawBlocks;

    while (std::getline(inFile, line)) {
        // Guard line length.
        if ((int)line.size() > kMaxLineLen) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] WARN: line too long (%d bytes) in %s — skipped\n",
                         (int)line.size(), fitPath);
            std::fflush(stderr);
            continue;
        }

        // Strip trailing CR/LF/whitespace.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                  line.back() == ' '  || line.back() == '\t'))
            line.pop_back();

        // Find first non-whitespace.
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        const char* p = line.c_str() + s;

        // Skip comment lines.
        if (*p == ';')
            continue;

        if (state == OUTER) {
            // Look for "TechSpecial {".
            if (std::strncmp(p, "TechSpecial", 11) == 0) {
                const char* q = p + 11;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '{') {
                    if ((int)rawBlocks.size() >= kMaxBodies) {
                        std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] WARN: TechSpecial block cap (%d) reached in %s — skipping rest\n",
                                     kMaxBodies, fitPath);
                        std::fflush(stderr);
                        break;
                    }
                    state = IN_SPECIAL;
                    verbsThisBody = 0;
                    currentKey.clear();
                    currentType.clear();
                    currentAlias.clear();
                    currentVariantOf.clear();
                    currentVerbs.clear();
                    bodyCommittedThisSpecial = false;
                }
            } else if (std::strncmp(p, "Aliases", 7) == 0) {
                // BRAINSPECIAL-ALIAS-1: top-level Aliases { <alias> = "<canonical>" } block.
                // Registered unconditionally (parse-unconditional / resolve-gated model);
                // the [BRAIN_ALIAS_REG] trace fires only when MC2_BRAIN_ALIAS=1 so gate-OFF
                // stderr output is byte-identical.
                const char* q = p + 7;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '{')
                    state = IN_ALIASES;
            }
        } else if (state == IN_ALIASES) {
            // BRAINSPECIAL-ALIAS-1: collect alias lines until the closing brace.
            if (*p == '}') {
                state = OUTER;
            } else {
                const char* eq = std::strchr(p, '=');
                if (eq) {
                    size_t nl = (size_t)(eq - p);
                    while (nl > 0 && (p[nl - 1] == ' ' || p[nl - 1] == '\t')) --nl;
                    char aname[64];
                    if (nl > 0 && nl < sizeof(aname)) {
                        std::memcpy(aname, p, nl);
                        aname[nl] = '\0';
                        std::string canon = parseQuotedField(eq + 1);
                        if (!canon.empty()) {
                            aliasRegistryAdd(aname, canon.c_str());
                            if (s_brainAliasGate()) {
                                std::fprintf(stderr, "[BRAIN_ALIAS_REG] alias=%s canonical=%s\n",
                                             aname, canon.c_str());
                                std::fflush(stderr);
                            }
                        }
                    }
                }
            }
        } else if (state == IN_SPECIAL) {
            // Look for key=, type=, alias=, "Body {", or closing "}".
            if (std::strncmp(p, "key", 3) == 0) {
                const char* q = p + 3;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '=') {
                    ++q;
                    currentKey = parseQuotedField(q);
                }
            } else if (std::strncmp(p, "type", 4) == 0) {
                const char* q = p + 4;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '=') {
                    ++q;
                    currentType = parseQuotedField(q);
                }
            } else if (std::strncmp(p, "alias", 5) == 0) {
                // BRAINSPECIAL-ALIAS-1: per-block alias= field (carver authors these,
                // e.g. alias = "Scenario.Main"). Captured into the index entry;
                // specialIndexFind resolves it when MC2_BRAIN_ALIAS=1.
                const char* q = p + 5;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '=') {
                    ++q;
                    currentAlias = parseQuotedField(q);
                }
            } else if (std::strncmp(p, "variantOf", 9) == 0) {
                // BRAINSPECIAL-VARIANTOF-1: parent Special key. Parsed unconditionally;
                // inheritance resolved only when MC2_BRAIN_VARIANTOF=1 (parse-unconditional /
                // resolve-gated model, same as the CALL-CHAIN index and the alias registry).
                const char* q = p + 9;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '=') {
                    ++q;
                    currentVariantOf = parseQuotedField(q);
                }
            } else if (std::strncmp(p, "Body", 4) == 0) {
                const char* q = p + 4;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '{') {
                    state = IN_BODY;
                    ++bodiesFound;
                }
            } else if (*p == '}') {
                // Closing brace of TechSpecial. Commit ONLY if no Body block was already
                // committed for this TechSpecial (body-less block); see quirk-fix note above.
                if (!bodyCommittedThisSpecial) {
                    RawBlock rb;
                    rb.key       = currentKey;
                    rb.type      = currentType;
                    rb.alias     = currentAlias;
                    rb.variantOf = currentVariantOf;
                    // no verbs
                    rawBlocks.push_back(std::move(rb));
                }
                state = OUTER;
            }
        } else { // IN_BODY
            if (*p == '}') {
                // End of Body block — commit the block.
                {
                    RawBlock rb;
                    rb.key       = currentKey;
                    rb.type      = currentType;
                    rb.alias     = currentAlias;
                    rb.variantOf = currentVariantOf;
                    rb.verbs     = currentVerbs;
                    rawBlocks.push_back(std::move(rb));
                    bodyCommittedThisSpecial = true;  // suppress the empty duplicate commit
                }
                state = IN_SPECIAL;   // still inside TechSpecial; look for its closing '}'
            } else if (std::strncmp(p, "STOP", 4) == 0 &&
                       (p[4] == '\0' || p[4] == ' ' || p[4] == '\t' || p[4] == ';')) {
                // STOP — BRAINSPECIAL-FLOW-WAIT-1: with MC2_BRAIN_FLOW=1 STOP is a real
                // verb (this tick's execution terminator). Gate OFF: skipped sentinel —
                // pre-slice behavior, byte-identical.
                if (s_brainFlowGate()) {
                    if (verbsThisBody < kMaxVerbsPerBody) {
                        currentVerbs.push_back("STOP");
                        ++verbsThisBody;
                    }
                }
            } else if (s_brainFlowGate() &&
                       (std::strncmp(p, "WAIT_UNTIL", 10) == 0 || std::strncmp(p, "WAIT", 4) == 0) &&
                       (std::strncmp(p, "WAIT_UNTIL", 10) == 0
                            ? (p[10] == '\0' || p[10] == ' ' || p[10] == '\t')
                            : (p[4]  == '\0' || p[4]  == ' ' || p[4]  == '\t'))) {
                // BRAINSPECIAL-FLOW-WAIT-1: WAIT / WAIT_UNTIL flow lines (bare — not
                // DO-prefixed, per the spec's flow-control grammar). Only collected when
                // the flow gate is ON; gate OFF these lines never matched anything and
                // were dropped — pre-slice behavior, byte-identical.
                if (verbsThisBody < kMaxVerbsPerBody) {
                    std::string fv(p);
                    while (!fv.empty() && (fv.back() == ' ' || fv.back() == '\t' || fv.back() == ';'))
                        fv.pop_back();
                    if (!fv.empty()) {
                        currentVerbs.push_back(std::move(fv));
                        ++verbsThisBody;
                    }
                }
            } else if (std::strncmp(p, "DO ", 3) == 0 || std::strncmp(p, "DO\t", 3) == 0) {
                if (verbsThisBody >= kMaxVerbsPerBody) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] WARN: verb cap (%d) per-body reached in %s — skipping rest of body\n",
                                 kMaxVerbsPerBody, fitPath);
                    std::fflush(stderr);
                    continue;
                }
                const char* verbStart = p + 3;
                while (*verbStart == ' ' || *verbStart == '\t') ++verbStart;

                std::string verb(verbStart);
                while (!verb.empty() && (verb.back() == ' ' || verb.back() == '\t'))
                    verb.pop_back();
                while (!verb.empty() && verb.back() == ';')
                    verb.pop_back();
                while (!verb.empty() && (verb.back() == ' ' || verb.back() == '\t'))
                    verb.pop_back();

                if (!verb.empty()) {
                    currentVerbs.push_back(std::move(verb));
                    ++verbsThisBody;
                }
            }
        }
    }

    if (rawBlocks.empty())
        return false;

    // Populate outIndex (all blocks, unconditionally).
    if (outIndex) {
        outIndex->clear();
        for (const RawBlock& rb : rawBlocks) {
            SpecialIndexEntry entry;
            entry.key        = rb.key;
            entry.alias      = rb.alias;       // BRAINSPECIAL-ALIAS-1
            entry.variantOf  = rb.variantOf;   // BRAINSPECIAL-VARIANTOF-1
            entry.body.verbs = rb.verbs;
            entry.body.loaded = !rb.verbs.empty();
            outIndex->push_back(std::move(entry));
        }
    }

    // Entry-body selection: populate outBody from chosen block.
    //   Rule 1: first block whose key contains "scenario_main".
    //   Rule 2: else first block with type == "MissionSpecial".
    //   Rule 3: else first block found.
    const RawBlock* chosen = nullptr;
    for (const RawBlock& rb : rawBlocks) {
        if (rb.key.find("scenario_main") != std::string::npos) {
            chosen = &rb; break;
        }
    }
    if (!chosen) {
        for (const RawBlock& rb : rawBlocks) {
            if (rb.type == "MissionSpecial") { chosen = &rb; break; }
        }
    }
    if (!chosen)
        chosen = &rawBlocks[0];

    // Migrate chosen block's verbs into outBody.
    // (verbs from ALL blocks were previously all merged; now only the entry-body is outBody.
    //  Callers that want all verbs should iterate the index.)
    outBody.verbs = chosen->verbs;

    // BRAINSPECIAL-VARIANTOF-1: report the chosen entry key so the caller can re-sync
    // outBody after variantOf inheritance resolves (the chosen block may be an
    // empty-body variant whose verbs only exist post-resolution).
    if (outChosenKey)
        *outChosenKey = chosen->key;

    return !outBody.verbs.empty() || !rawBlocks.empty();
}

// ---------------------------------------------------------------------------
// parseBrainSpecialBody_FitIni
//
// Legacy FitIniFile-based loader.  Retained as fallback for bracket-form fixtures
// ([BrainSpecial] / [Body] / DO0=...) that don't use the brace-block TechSpecial format.
// This is the original parseBrainSpecialBody implementation, renamed.
//
// FORBIDDEN-CALL CONTRACT: calls FitIniFile API only. No order functions.
static bool parseBrainSpecialBody_FitIni(const char* fitPath, BrainSpecialBody& outBody) {
    FitIniFile* fit = new FitIniFile;
    if (!fit)
        return false;

    if (fit->open(fitPath) != NO_ERR) {
        delete fit;
        return false;
    }

    if (fit->seekBlock("BrainSpecial") != NO_ERR) {
        fit->close();
        delete fit;
        return false;
    }

    if (fit->seekBlock("Body") != NO_ERR) {
        fit->close();
        delete fit;
        return false;
    }

    char keyBuf[16];
    // UNITQUERY-SETTARGETPRIORITY-1: 64→256. Symbolic verbs (e.g. UnitQuery.SetTargetpriority
    // with a '+'-joined CONTACT_CRITERIA_* expression) exceed 63 chars and were silently
    // truncated. The RawScan/brace-block path uses std::string (no cap); only this legacy
    // FitIni reader had the fixed buffer.
    char verbBuf[256];
    for (int i = 0; i < 64; ++i) {
        std::snprintf(keyBuf, sizeof(keyBuf), "DO%d", i);
        verbBuf[0] = '\0';
        long result = fit->readIdString(keyBuf, verbBuf, (unsigned long)(sizeof(verbBuf) - 1));
        if (result != NO_ERR)
            break;
        if (verbBuf[0] != '\0')
            outBody.verbs.push_back(std::string(verbBuf));
    }

    fit->close();
    delete fit;
    return !outBody.verbs.empty();
}

// ---------------------------------------------------------------------------
// BRAINSPECIAL-SCOPE-GLOBAL-1 — GlobalSpecial scope (gate MC2_BRAIN_SCOPE_GLOBAL).
//
// Spec (discussion #18): four Special scopes; GlobalSpecial = reusable mod/game
// scripts. Engine adaptation: a `global_specials.fit` file in the same directory
// as the mission specials file is parsed at mission load and its TechSpecial
// blocks are merged into the mission's special index as a shared LIBRARY:
//   - global blocks are TechSpecial.Call targets (and, later, variantOf parents);
//   - they NEVER provide a mission's root/entry body;
//   - mission-local keys win on collision ([BRAIN_SCOPE_GLOBAL_SHADOWED]);
//   - global Aliases{} blocks register FIRST, so mission aliases override them.
// Gate OFF (default): no global parse, no merge — byte-identical.
//
// FORBIDDEN-CALL CONTRACT: file scan + string ops + fprintf only. No order functions.
static bool s_brainScopeGlobalGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_SCOPE_GLOBAL");
        return v && std::atoi(v) != 0;
    })();
    return kGate;
}

// Derive "<dir-of-missionFitPath>/global_specials.fit" and raw-scan it into gIndex.
// Returns true if the file existed and contained at least one TechSpecial block.
// No-op (false) when the gate is OFF.
static bool parseGlobalSpecials(const char* missionFitPath, SpecialIndex& gIndex) {
    if (!s_brainScopeGlobalGate()) return false;
    char gpath[256];
    const char* lastSlash = nullptr;
    for (const char* c = missionFitPath; *c; ++c)
        if (*c == '/' || *c == '\\') lastSlash = c;
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - missionFitPath) + 1;
        if (dirLen >= sizeof(gpath) - 24) return false;
        std::memcpy(gpath, missionFitPath, dirLen);
        std::snprintf(gpath + dirLen, sizeof(gpath) - dirLen, "global_specials.fit");
    } else {
        std::snprintf(gpath, sizeof(gpath), "global_specials.fit");
    }
    BrainSpecialBody gBody;   // discarded — globals never provide the entry body
    if (!parseBrainSpecialBody_RawScan(gpath, gBody, &gIndex))
        return false;
    std::fprintf(stderr, "[BRAIN_SCOPE_GLOBAL] parsed %d global special block(s) from %s\n",
                 (int)gIndex.size(), gpath);
    std::fflush(stderr);
    return !gIndex.empty();
}

// ---------------------------------------------------------------------------
// BRAINSPECIAL-VARIANTOF-1 — variantOf inheritance (gate MC2_BRAIN_VARIANTOF).
//
// Spec (discussion #18): "A variantOf Special inherits the full body of its parent
// and can override specific behavior by re-declaring sections." Engine adaptation
// (SPEC-DELTA, documented in .claude/TECHSCRIPT-GAP-CLOSURE-1.md #14):
//   - child with EMPTY Body inherits the parent chain's verbs wholesale;
//   - child that re-declares a Body overrides it entirely (engine blocks have
//     exactly one overridable section — Body — so override granularity = whole Body);
//   - resolution runs AFTER the global merge, so a mission variant can inherit a
//     GlobalSpecial parent ("protected core Specials": extend without overwriting);
//   - parent chain depth <= 8, cycle-guarded (mirrors the Call-chain guards).
// Gate OFF (default): variantOf fields are parsed but ignored — byte-identical.
//
// FORBIDDEN-CALL CONTRACT: index walk + string copies + fprintf only. No order functions.
static bool s_brainVariantOfGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_VARIANTOF");
        return v && std::atoi(v) != 0;
    })();
    return kGate;
}

static void resolveVariantInheritance(SpecialIndex* index) {
    if (!index || !s_brainVariantOfGate()) return;
    static constexpr int kVariantMaxDepth = 8;
    for (SpecialIndexEntry& e : *index) {
        if (e.variantOf.empty()) continue;
        if (!e.body.verbs.empty()) {
            std::fprintf(stderr, "[BRAIN_VARIANTOF_OVERRIDE] child=%s parent=%s (body re-declared)\n",
                         e.key.c_str(), e.variantOf.c_str());
            std::fflush(stderr);
            continue;
        }
        // Walk the parent chain until a body-bearing ancestor (or guard trip).
        std::vector<std::string> visited;
        visited.push_back(e.key);
        std::string parentKey = e.variantOf;
        const SpecialIndexEntry* src = nullptr;
        bool cycle = false, unknown = false;
        int depth = 0;
        while (depth < kVariantMaxDepth) {
            const SpecialIndexEntry* p = specialIndexFind(*index, parentKey);
            if (!p) { unknown = true; break; }
            for (const std::string& v : visited) {
                if (v == p->key) { cycle = true; break; }
            }
            if (cycle) break;
            visited.push_back(p->key);
            if (!p->body.verbs.empty() || p->variantOf.empty()) { src = p; break; }
            parentKey = p->variantOf;
            ++depth;
        }
        if (unknown) {
            std::fprintf(stderr, "[BRAIN_VARIANTOF_UNKNOWN] child=%s parent=%s (not in index)\n",
                         e.key.c_str(), parentKey.c_str());
            std::fflush(stderr);
        } else if (cycle) {
            std::fprintf(stderr, "[BRAIN_VARIANTOF_CYCLE] child=%s parent=%s (chain cycles; left as-authored)\n",
                         e.key.c_str(), parentKey.c_str());
            std::fflush(stderr);
        } else if (depth >= kVariantMaxDepth) {
            std::fprintf(stderr, "[BRAIN_VARIANTOF_DEPTH] child=%s depth=%d max=%d (left as-authored)\n",
                         e.key.c_str(), depth, kVariantMaxDepth);
            std::fflush(stderr);
        } else if (src && !src->body.verbs.empty()) {
            e.body.verbs  = src->body.verbs;   // same-vector element copy: no push, no realloc
            e.body.loaded = true;
            std::fprintf(stderr, "[BRAIN_VARIANTOF] child=%s parent=%s inherited=%d verb(s)\n",
                         e.key.c_str(), src->key.c_str(), (int)src->body.verbs.size());
            std::fflush(stderr);
        } else {
            std::fprintf(stderr, "[BRAIN_VARIANTOF_EMPTY] child=%s parent=%s (parent chain has no body)\n",
                         e.key.c_str(), e.variantOf.c_str());
            std::fflush(stderr);
        }
    }
}

// Re-sync the entry body after variantOf resolution: the chosen block may have been an
// empty-body variant. Fill outBody from the (now-resolved) chosen index entry.
// Gate-guarded no-op; only fills an EMPTY outBody (override bodies are already correct).
static void resyncEntryBodyAfterVariants(BrainSpecialBody& outBody, const SpecialIndex* index,
                                          const std::string& chosenKey) {
    if (!index || chosenKey.empty() || !s_brainVariantOfGate()) return;
    if (!outBody.verbs.empty()) return;
    const SpecialIndexEntry* ce = specialIndexFind(*index, chosenKey);
    if (ce && !ce->body.verbs.empty()) {
        outBody.verbs = ce->body.verbs;
        std::fprintf(stderr, "[BRAIN_VARIANTOF] entry body %s resolved to %d verb(s)\n",
                     chosenKey.c_str(), (int)outBody.verbs.size());
        std::fflush(stderr);
    }
}

// Append non-colliding global entries into the mission index (mission-local wins).
static void appendGlobalSpecials(SpecialIndex* outIndex, SpecialIndex& gIndex) {
    if (!outIndex || gIndex.empty()) return;
    int merged = 0, shadowed = 0;
    for (SpecialIndexEntry& ge : gIndex) {
        if (ge.key.empty()) continue;
        if (specialIndexFind(*outIndex, ge.key)) {
            std::fprintf(stderr, "[BRAIN_SCOPE_GLOBAL_SHADOWED] key=%s (mission-local wins)\n",
                         ge.key.c_str());
            std::fflush(stderr);
            ++shadowed;
            continue;
        }
        outIndex->push_back(std::move(ge));
        ++merged;
    }
    std::fprintf(stderr, "[BRAIN_SCOPE_GLOBAL] merged=%d shadowed=%d (index=%d blocks)\n",
                 merged, shadowed, (int)outIndex->size());
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// parseBrainSpecialBody — public entry point (DISPATCH-LOADER-RAW-1)
//
// Strategy:
//   1. Try raw brace-block scanner first (handles inline-quoted DO args).
//   2. If no TechSpecial blocks found (raw scanner returned empty), fall back
//      to the legacy FitIniFile-based [BrainSpecial]/[Body]/DO0= form.
//   Both forms emit the same [BRAIN_DISPATCH] trace on success.
//   BRAINSPECIAL-SCOPE-GLOBAL-1: when MC2_BRAIN_SCOPE_GLOBAL=1, global_specials.fit
//   (same directory) is parsed FIRST (alias precedence: mission overrides global)
//   and its blocks are appended to the index AFTER the mission parse succeeds.
//
// Caller passes missionName like "mc2_01" (no path prefix, no extension).
// Returns true if at least one verb was loaded.
bool parseBrainSpecialBody(const char* missionName, BrainSpecialBody& outBody,
                            SpecialIndex* outIndex) {
    outBody.verbs.clear();
    outBody.loaded = false;

    // BRAINSPECIAL-ALIAS-1: reset the alias registry to its built-in seeds before
    // each specials parse (mission-ephemeral, like the special cache).
    aliasRegistrySeed();

    char fitPath[256];
    std::snprintf(fitPath, sizeof(fitPath), "data/missions/%s_specials.fit", missionName);

    // BRAINSPECIAL-SCOPE-GLOBAL-1: parse the global library first (gate-guarded no-op).
    SpecialIndex gIndex;
    bool haveGlobal = parseGlobalSpecials(fitPath, gIndex);

    // Try raw brace-block scanner first.
    // CALL-CHAIN-1A: outIndex forwarded (may be nullptr if caller doesn't need it).
    std::string chosenKey;  // BRAINSPECIAL-VARIANTOF-1: entry-body key for post-resolve re-sync
    bool ok = parseBrainSpecialBody_RawScan(fitPath, outBody, outIndex, &chosenKey);

    if (!ok) {
        // No TechSpecial brace blocks found — try legacy bracket-form fallback.
        // NOTE: legacy FitIni form does NOT populate the index (no TechSpecial blocks).
        std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] no TechSpecial blocks in %s — trying FitIni fallback\n",
                     fitPath);
        std::fflush(stderr);
        ok = parseBrainSpecialBody_FitIni(fitPath, outBody);
        if (ok) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] FitIni fallback loaded %s: %d verbs\n",
                         fitPath, (int)outBody.verbs.size());
            std::fflush(stderr);
        }
    }

    // BRAINSPECIAL-SCOPE-GLOBAL-1: merge global library blocks (mission-local wins).
    // Only when the mission itself loaded — globals never provide the entry body.
    if (ok && haveGlobal)
        appendGlobalSpecials(outIndex, gIndex);

    // BRAINSPECIAL-VARIANTOF-1: resolve inheritance AFTER the global merge (a mission
    // variant may inherit a GlobalSpecial parent), then re-sync the entry body.
    if (ok) {
        resolveVariantInheritance(outIndex);
        resyncEntryBodyAfterVariants(outBody, outIndex, chosenKey);
    }

    if (ok) {
        outBody.loaded = true;
        std::fprintf(stderr, "[BRAIN_DISPATCH] parsed %s_specials.fit: %d verbs (index=%d blocks)\n",
                     missionName, (int)outBody.verbs.size(),
                     outIndex ? (int)outIndex->size() : 0);
        std::fflush(stderr);
    }
    return outBody.loaded;
}

// ---------------------------------------------------------------------------
// GAP-A MULTI-WARRIOR SPECIALS: mission-level specials cache.
//
// The _specials.fit body is mission-level — identical for every warrior. At mission
// load (mission.cpp), only warriors whose brainRuntime is already allocated (named in
// _ai.fit) get their specialBody parsed. Warriors whose brainRuntime is allocated
// LAZILY inside runBrain (warrior.cpp) miss that parse window, so their specialBody
// stays loaded=false and they never dispatch. This cache parses the file ONCE at
// mission load and lets lazily-allocated runtimes copy the parsed body in.
//
// Lifecycle: mission-ephemeral. Reset + populated once per mission load when
// MC2_BRAIN_DISPATCH=1. Copy is by value — BrainSpecialBody and SpecialIndex are pure
// value types (no internal pointers), so the per-runtime copy is self-contained.
static BrainSpecialBody g_missionSpecialBody;
static SpecialIndex     g_missionSpecialIndex;
static bool             g_missionSpecialCached = false;

void resetMissionSpecialCache() {
    g_missionSpecialBody = BrainSpecialBody();
    g_missionSpecialIndex.clear();
    g_missionSpecialCached = false;
}

void cacheMissionSpecialBody(const char* specialFitName) {
    resetMissionSpecialCache();
    if (parseBrainSpecialBody(specialFitName, g_missionSpecialBody, &g_missionSpecialIndex)) {
        g_missionSpecialCached = g_missionSpecialBody.loaded;
    }
}

bool applyCachedSpecialBodyToRuntime(MechBrainRuntime* rt) {
    if (!rt) return false;
    if (!g_missionSpecialCached) return false;
    if (rt->specialBody.loaded) return false;  // already parsed its own copy at load
    rt->specialBody  = g_missionSpecialBody;
    rt->specialIndex = g_missionSpecialIndex;
    std::fprintf(stderr, "[BRAIN_DISPATCH] GAP-A: applied cached specials to lazily-alloc'd runtime\n");
    std::fflush(stderr);
    return true;
}

// ---------------------------------------------------------------------------
// BRAIN-DISPATCH-HARNESS-1: parseBrainSpecialBodyFromPath
//
// Explicit-path variant of parseBrainSpecialBody for offline harness use.
// Directly opens fitPath without the "data/missions/<name>_specials.fit" construction.
// Same scanner logic (raw brace-block first, FitIni fallback).
bool parseBrainSpecialBodyFromPath(const char* fitPath, BrainSpecialBody& outBody,
                                    SpecialIndex* outIndex) {
    outBody.verbs.clear();
    outBody.loaded = false;

    // BRAINSPECIAL-ALIAS-1: reset the alias registry to its built-in seeds (harness path).
    aliasRegistrySeed();

    // BRAINSPECIAL-SCOPE-GLOBAL-1: parse the global library first (gate-guarded no-op).
    SpecialIndex gIndex;
    bool haveGlobal = parseGlobalSpecials(fitPath, gIndex);

    std::string chosenKey;  // BRAINSPECIAL-VARIANTOF-1
    bool ok = parseBrainSpecialBody_RawScan(fitPath, outBody, outIndex, &chosenKey);
    if (!ok) {
        std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] no TechSpecial blocks in %s — trying FitIni fallback\n", fitPath);
        std::fflush(stderr);
        ok = parseBrainSpecialBody_FitIni(fitPath, outBody);
        if (ok) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] FitIni fallback loaded %s: %d verbs\n",
                         fitPath, (int)outBody.verbs.size());
            std::fflush(stderr);
        }
    }
    if (ok && haveGlobal)
        appendGlobalSpecials(outIndex, gIndex);
    // BRAINSPECIAL-VARIANTOF-1: resolve inheritance after global merge; re-sync entry body.
    if (ok) {
        resolveVariantInheritance(outIndex);
        resyncEntryBodyAfterVariants(outBody, outIndex, chosenKey);
    }
    if (ok) {
        outBody.loaded = true;
        std::fprintf(stderr, "[BRAIN_DISPATCH] parsed %s: %d verbs (index=%d blocks)\n",
                     fitPath, (int)outBody.verbs.size(),
                     outIndex ? (int)outIndex->size() : 0);
        std::fflush(stderr);
    }
    return outBody.loaded;
}

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D: parseVarVerb
//
// Parses "Var.Set \"<key>\" <value> [scope=Mission]" or
//         "Var.Get \"<key>\" [scope=Mission]"
// into outKey, outValue, outScope.
// outValue is only meaningful for Var.Set; for Var.Get it is set to "".
// outScope: 0=Unit (VarScope::Unit), 1=Mission (VarScope::Mission).
// Returns true on success; false on malformed input (empty key; missing value for Set).
//
// FORBIDDEN-CALL CONTRACT: calls ONLY std::strchr/strstr/strncpy/snprintf. No orders.
bool parseVarVerb(const char* verbStr, char outKey[32], char outValue[32], uint8_t* outScope) {
    outKey[0] = '\0';
    outValue[0] = '\0';
    *outScope = 0;  // default Unit

    // Determine whether this is Var.Set or Var.Get.
    bool isSet = (std::strncmp(verbStr, "Var.Set", 7) == 0 && (verbStr[7] == ' ' || verbStr[7] == '\t' || verbStr[7] == '\0'));
    bool isGet = (std::strncmp(verbStr, "Var.Get", 7) == 0 && (verbStr[7] == ' ' || verbStr[7] == '\t' || verbStr[7] == '\0'));
    if (!isSet && !isGet)
        return false;

    // Skip verb token + whitespace.
    const char* p = verbStr + 7;
    while (*p == ' ' || *p == '\t') ++p;

    // Parse key token — supports two forms:
    //   "key"   — double-quoted (canonical spec form; used in non-FIT contexts)
    //   [key]   — bracket-quoted (FIT-safe form; FIT outer-quotes can't embed '"')
    char closeDelim = '\0';
    if (*p == '"') {
        closeDelim = '"';
    } else if (*p == '[') {
        closeDelim = ']';
    } else {
        return false;  // unrecognized key delimiter
    }
    ++p;  // skip opening delimiter
    const char* keyStart = p;
    const char* keyEnd = std::strchr(p, closeDelim);
    if (!keyEnd)
        return false;
    size_t keyLen = (size_t)(keyEnd - keyStart);
    if (keyLen == 0)
        return false;
    if (keyLen > 31) keyLen = 31;
    std::strncpy(outKey, keyStart, keyLen);
    outKey[keyLen] = '\0';

    p = keyEnd + 1;  // skip closing delimiter

    // Skip whitespace between key and value/scope.
    while (*p == ' ' || *p == '\t') ++p;

    if (isSet) {
        // Parse value token (whitespace-delimited; may be quoted or unquoted).
        if (*p == '\0')
            return false;  // missing value for Set

        if (*p == '"') {
            // Quoted value.
            ++p;
            const char* vStart = p;
            const char* vEnd = std::strchr(p, '"');
            if (!vEnd) return false;
            size_t vLen = (size_t)(vEnd - vStart);
            if (vLen > 31) vLen = 31;
            std::strncpy(outValue, vStart, vLen);
            outValue[vLen] = '\0';
            p = vEnd + 1;
        } else {
            // Unquoted value — read until whitespace.
            const char* vStart = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
            size_t vLen = (size_t)(p - vStart);
            if (vLen > 31) vLen = 31;
            std::strncpy(outValue, vStart, vLen);
            outValue[vLen] = '\0';
        }

        // Skip whitespace after value.
        while (*p == ' ' || *p == '\t') ++p;
    }

    // Detect optional "scope=Mission" trailing token.
    if (std::strncmp(p, "scope=Mission", 13) == 0)
        *outScope = 1;  // VarScope::Mission

    return true;
}

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D: handleVarSet
//
// FORBIDDEN-CALL CONTRACT: calls ONLY store->set + fprintf/fflush. No order functions.
// Verified by inspection: no warrior pointer, no TacticalOrder, no slot writes.
void handleVarSet(const char* key, const char* value, uint8_t scope, VarStore* store, int wid) {
    if (scope == 1 /* Mission */) {
        // TECHSCRIPT-DISPATCH-1D-M: MC2_BRAIN_VAR_MISSION gate ON → real write to g_missionVarStore.
        // Gate OFF → trace-only (1D deferred behavior preserved).
        if (!s_missionVarGate()) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET_MISSION_TRACE] key=%s value=%s wid=%d (write deferred -- MC2_BRAIN_VAR_MISSION not set)\n",
                         key, value, wid);
            std::fflush(stderr);
            return;
        }
        // Guarded-single-writer: WID-order-wins within same tick.
        // brainTick via getBrainTickIndex() — available in dispatch.cpp context.
        uint32_t tick = getBrainTickIndex();
        bool conflict = false;
        bool ok = g_missionVarStore.trySet(key, value, wid, tick, &conflict);
        if (conflict) {
            // A higher-WID already wrote this key this tick — lower WID suppressed.
            // Retrieve the winning value and prevWID for the conflict trace.
            const MissionVarEntry* e = g_missionVarStore.find(key);
            int prevWid = e ? e->lastWriterWID : -1;
            std::fprintf(stderr, "[BRAIN_VAR_MISSION_CONFLICT] key=%s wid=%d prevWid=%d tick=%u (higher-WID wins; this write suppressed)\n",
                         key, wid, prevWid, tick);
            std::fflush(stderr);
            return;
        }
        if (!ok) {
            std::fprintf(stderr, "[BRAIN_VAR_MISSION_SET_FULL] key=%s wid=%d (cap=%d reached -- skipped)\n",
                         key, wid, MissionVarStore::kMissionVarStoreCap);
            std::fflush(stderr);
            return;
        }
        std::fprintf(stderr, "[BRAIN_VAR_MISSION_SET] key=%s val=%s wid=%d tick=%u\n", key, value, wid, tick);
        std::fflush(stderr);
        return;
    }
    // Unit scope (unchanged from 1D).
    if (!store) {
        std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET] WARN: no varStore for wid=%d key=%s (store null)\n", wid, key);
        std::fflush(stderr);
        return;
    }
    bool ok = store->set(key, value, VarScope::Unit);
    if (ok) {
        std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET] key=%s value=%s scope=Unit wid=%d\n", key, value, wid);
    } else {
        std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET_FULL] wid=%d key=%s (cap=%d reached -- skipped)\n",
                     wid, key, VarStore::kVarStoreCap);
    }
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D: handleVarGet
//
// FORBIDDEN-CALL CONTRACT: calls ONLY store->get + fprintf/fflush. No order functions.
void handleVarGet(const char* key, uint8_t scope, VarStore* store, int wid) {
    if (scope == 1 /* Mission */) {
        // TECHSCRIPT-DISPATCH-1D-M: MC2_BRAIN_VAR_MISSION gate ON → real read from g_missionVarStore.
        // Gate OFF → trace-only default-only (1D deferred behavior preserved).
        if (!s_missionVarGate()) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_GET_MISSION_TRACE] key=%s wid=%d (default -- MC2_BRAIN_VAR_MISSION not set)\n",
                         key, wid);
            std::fflush(stderr);
            return;
        }
        const char* val = g_missionVarStore.get(key);
        bool isHit = (g_missionVarStore.find(key) != nullptr);
        std::fprintf(stderr, "[BRAIN_VAR_MISSION_GET] key=%s val=%s wid=%d (%s)\n",
                     key, val, wid, isHit ? "hit" : "default");
        std::fflush(stderr);
        return;
    }
    // Unit scope (unchanged from 1D).
    const char* val = (store) ? store->get(key, VarScope::Unit) : "0";
    bool isHit = store && (store->find(key, VarScope::Unit) != nullptr);
    std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_GET] key=%s value=%s scope=Unit wid=%d (%s)\n",
                 key, val, wid, isHit ? "hit" : "default");
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// Gate init helper for DISPATCH_VAR (used by both execute functions).
static bool s_dispatchVarGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_DISPATCH_VAR");
        if (v && std::atoi(v) != 0) {
            const char* d = std::getenv("MC2_BRAIN_DISPATCH");
            if (!d || std::atoi(d) == 0) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_VAR] WARNING: MC2_BRAIN_DISPATCH_VAR=1 requires MC2_BRAIN_DISPATCH=1 -- var handling is INERT\n");
                std::fflush(stderr);
                return false;
            }
            return true;
        }
        return false;
    })();
    return kGate;
}

// ---------------------------------------------------------------------------
// scanFsmTodosFromFile — TECHSCRIPT-SPECIAL-DISPATCH-1C
//
// FORBIDDEN-CALL CONTRACT:
//   This function calls ONLY std::ifstream + std::regex (classify) + fprintf/fflush.
//   It does NOT call setGeneralTacOrder, setPlayerTacOrder, setAlarmTacOrder,
//   requestHelp, requestTarget, clearCurTacOrder, setMainGoal, calcTacOrder,
//   coreMoveTo, or ANY movement/attack/order function.
//   No warrior pointer. No MechWarrior type. No tac-order writes.
//   Verified by inspection.
//
// Context: auto-converted mission_specials.fit files produced by the modder's tool
// contain ABL state machine constructs ONLY as FIT comments of the form:
//   "; TODO: manual ABL line: <text>"
// FitIniFile strips comments, so parseBrainSpecialBody never sees them.
// This function does a second raw-text pass to surface those markers as structured
// trace data, giving the modder an inventory of dropped FSM logic to hand-port.
//
// Classifier rules (applied to <payload> after stripping the TODO prefix):
//   "^\s*state\s+(\w+);"  → STATE_DEF(name)
//   "^\s*endstate\s*;?"   → STATE_END
//   "^\s*trans\s+(\w+);"  → TRANS(target)
//   "^\s*transBack\s*;"   → TRANS_BACK
//   else                  → OTHER_TODO (variable decls, misc — counted but not detail-traced)
int scanFsmTodosFromFile(const char* missionName, BrainSpecialBody& outBody) {
    outBody.fsmTodos.clear();

    char fitPath[256];
    std::snprintf(fitPath, sizeof(fitPath), "data/missions/%s_specials.fit", missionName);

    std::ifstream inFile(fitPath);
    if (!inFile.is_open()) {
        return 0;
    }

    // Regex to match: optional-whitespace ; optional-whitespace TODO: optional-whitespace
    // "manual ABL line:" optional-whitespace <payload>
    // We use a simple string search rather than a full regex for the outer match
    // to avoid locale/platform regex engine differences in the prefix, then classify
    // the payload with targeted regexes.
    static const std::regex reStateDef(R"(^\s*state\s+(\w+)\s*;)",
                                        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex reStateEnd(R"(^\s*endstate\s*;?)",
                                        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex reTrans   (R"(^\s*trans\s+(\w+)\s*;)",
                                        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex reTransBack(R"(^\s*transBack\s*;)",
                                        std::regex::ECMAScript | std::regex::optimize);

    // Needle we look for (case-sensitive, exactly as produced by the converter tool).
    static const char kTodoPrefix[] = "; TODO: manual ABL line:";
    const size_t kTodoPrefixLen = sizeof(kTodoPrefix) - 1;

    int found = 0;
    std::string line;
    while (std::getline(inFile, line)) {
        // Trim leading whitespace to find the semicolon prefix.
        size_t pos = 0;
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
            ++pos;

        // Check for "; TODO: manual ABL line:" prefix.
        if (line.size() - pos < kTodoPrefixLen)
            continue;
        if (std::strncmp(line.c_str() + pos, kTodoPrefix, kTodoPrefixLen) != 0)
            continue;

        // Extract payload (everything after the prefix, trimmed).
        std::string payload = line.substr(pos + kTodoPrefixLen);
        // Trim leading whitespace from payload.
        size_t pstart = 0;
        while (pstart < payload.size() && (payload[pstart] == ' ' || payload[pstart] == '\t'))
            ++pstart;
        payload = payload.substr(pstart);
        // Trim trailing whitespace/CR.
        while (!payload.empty() && (payload.back() == '\r' || payload.back() == '\n' ||
                                    payload.back() == ' '  || payload.back() == '\t'))
            payload.pop_back();

        if (payload.empty())
            continue;

        // Cap at kMaxFsmTodos.
        if (found >= BrainSpecialBody::kMaxFsmTodos) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_FSM_TODO] WARN: FSM TODO cap (%d) reached in %s_specials.fit — truncating\n",
                         BrainSpecialBody::kMaxFsmTodos, missionName);
            std::fflush(stderr);
            break;
        }

        // Classify payload.
        FsmTodoEntry entry;
        std::smatch m;
        if (std::regex_search(payload, m, reStateDef)) {
            entry.kind = FsmTodoKind::STATE_DEF;
            entry.name = m[1].str();
        } else if (std::regex_search(payload, m, reStateEnd)) {
            entry.kind = FsmTodoKind::STATE_END;
        } else if (std::regex_search(payload, m, reTrans)) {
            entry.kind = FsmTodoKind::TRANS;
            entry.name = m[1].str();
        } else if (std::regex_search(payload, m, reTransBack)) {
            entry.kind = FsmTodoKind::TRANS_BACK;
        } else {
            entry.kind = FsmTodoKind::OTHER_TODO;
            entry.name = payload;  // store full payload for OTHER_TODO (for potential future use)
        }

        outBody.fsmTodos.push_back(std::move(entry));
        ++found;
    }

    return found;
}
