#pragma once
// TECHSCRIPT-SPECIAL-DISPATCH-1A — trace-only dispatcher shell.
// TECHSCRIPT-SPECIAL-DISPATCH-1B — adds MC2_BRAIN_DISPATCH_APPLY gate for first effect.
// TECHSCRIPT-SPECIAL-DISPATCH-1C — FSM-TODO surfacer: raw-scan specials.fit for
//   "; TODO: manual ABL line: <payload>" comments (stripped by FitIniFile) and emit
//   structured [BRAIN_DISPATCH_FSM_TODO] traces (information only, no behavior change).
// TECHSCRIPT-DISPATCH-1D — per-unit Var namespace + minimal store.
// TECHSCRIPT-CALL-CHAIN-1A — TechSpecial.Call chaining (parse + index + trace ONLY).
//   Gate: MC2_BRAIN_DISPATCH_CALL=1 (requires MC2_BRAIN_DISPATCH=1; warns + inert otherwise).
//   At mission load: parseBrainSpecialBody_RawScan populates a per-mission
//   specialIndex (std::vector<SpecialIndexEntry>) stored on MechBrainRuntime.
//   At dispatch time: "TechSpecial.Call <key>" verb resolves the target in the index,
//   recursively iterates its body's verbs emitting trace lines with depth prefix.
//   NO effects are executed within chained bodies in 1A (apply-mode chaining deferred to 1B).
//   Cycle guard: per-tick std::vector<std::string> visited set, cleared on dispatch entry.
//   Depth limit: 8 (matches recon recommendation; corpus max depth = 2, zero cycles found).
//   Gate: MC2_BRAIN_DISPATCH_VAR=1 (requires MC2_BRAIN_DISPATCH=1; default OFF).
//   Var.Set "<key>" <value> [scope=Mission]  — stores per-warrior or traces mission-scope.
//   Var.Get "<key>" [scope=Mission]          — reads per-warrior store, traces result.
//   Mission-scope writes are TRACE-ONLY in 1D (no shared store; nondeterminism deferred).
//   Gate-OFF: Var.* fall through to [BRAIN_DISPATCH_UNKNOWN] as before — byte-identical.
//
// Gate ladder:
//   MC2_BRAIN_DISPATCH=1                          → parse + trace ONLY (1A behavior, byte-identical)
//   MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_APPLY=1 → allow the ONE first effect (Brain.CorePower false → POWERDOWN)
//   MC2_BRAIN_DISPATCH_APPLY=1 alone              → warns + is inert (DISPATCH required)
//   MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_FSM_TODO=1 → additionally surface FSM TODO markers
//   MC2_BRAIN_DISPATCH_FSM_TODO=1 alone           → warns + is inert (DISPATCH required)
//   MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_VAR=1 → Var.Set/Get handled; per-unit store active
//   MC2_BRAIN_DISPATCH_VAR=1 alone                → warns + is inert (DISPATCH required)
//   MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_CALL=1 → TechSpecial.Call chaining; index + recurse + trace
//   MC2_BRAIN_DISPATCH_CALL=1 alone               → warns + is inert (DISPATCH required)
//   MC2_BRAIN_ALIAS=1 (BRAINSPECIAL-ALIAS-1)      → data-driven alias registry active:
//       built-in seeds + Aliases{} block entries + per-block alias= keys + case-insensitive
//       catalog shorthand. OFF (default): only the 5 hardcoded aliasToCanonical mappings
//       apply (pre-slice behavior, byte-identical).
//   MC2_BRAIN_SCOPE_GLOBAL=1 (BRAINSPECIAL-SCOPE-GLOBAL-1) → global_specials.fit (same
//       directory as the mission specials) merged into the special index as a shared
//       Call-target library. Mission-local keys win on collision. Globals never provide
//       the entry body. OFF (default): no global parse — byte-identical.
//   MC2_BRAIN_VARIANTOF=1 (BRAINSPECIAL-VARIANTOF-1) → variantOf= inheritance resolved
//       after parse + global merge: empty-body child inherits the parent chain's verbs;
//       a re-declared Body overrides (Body-level override; spec-delta documented in
//       .claude/TECHSCRIPT-GAP-CLOSURE-1.md #14). Depth<=8 + cycle guard.
//       OFF (default): variantOf fields parsed but ignored — byte-identical.
//   MC2_BRAIN_FLOW=1 (BRAINSPECIAL-FLOW-WAIT-1) → WAIT / WAIT_UNTIL / STOP flow verbs.
//       SPEC-DELTA: WAIT is a LATCHED SEQUENCE GATE, not VM blocking — the body
//       re-executes every deterministic brain tick; verbs after an unsatisfied
//       WAIT (sim-time deadline) / WAIT_UNTIL (Var == value condition) are skipped
//       until it latches open. STOP ends the tick's body execution. Root body only.
//       Flow-bearing bodies re-dispatch every tick (warrior.cpp skips the class-level
//       once-guard pre-set); per-verb-index flowFired guards prevent order re-emission.
//       GOTO/LABEL are NOT implemented (determinism ruling — see gap ledger #9).
//       OFF (default): scanner drops WAIT lines + STOP stays a skipped sentinel —
//       byte-identical.
//
// FORBIDDEN-CALL GUARD (1A — executeSpecialBody_TraceOnly / executeSpecialBody_TraceOnlyChained):
//   MUST NOT call ANY of: setGeneralTacOrder, setPlayerTacOrder, setAlarmTacOrder,
//   requestHelp, requestTarget, clearCurTacOrder, setMainGoal, calcTacOrder, coreMoveTo,
//   or ANY movement/attack/order function.
//   Calls ONLY: token parsing + fprintf + recursive dispatch (chained bodies).
//   CALL-CHAIN-1A: chaining is PURE VERB-STREAM COMPOSITION — no new order function calls,
//   no state writes, no effects in chained bodies (effects deferred to CALL-CHAIN-1B).
//   Verified by inspection.
//
// RELAXED-CALL GUARD (1B — executeSpecialBody_Apply + commitBrainIntents):
//   Permitted verbs (SIX total): Brain.CorePower false → POWERDOWN,
//   Unit.Eject (or coreEject alias) → EJECT, OPORD.CoreGuard (or coreGuard alias) → GUARD,
//   OPORD.CoreMoveTo x y z → MOVETO_POINT, OPORD.CoreAttack <wid> → ATTACK_OBJECT,
//   Unit.Retreat (or coreRetreat alias) → WITHDRAW.
//   BRAIN-DECISION-INTENT-QUEUE-1 (gate MC2_BRAIN_INTENT_QUEUE):
//     Gate OFF: executeSpecialBody_Apply calls warrior->setGeneralTacOrder() directly (6 sites).
//     Gate ON:  executeSpecialBody_Apply emits BrainOrderIntents; commitBrainIntents() is the
//               ONLY function that calls warrior->setGeneralTacOrder() (6 sites there).
//   STILL FORBIDDEN: orderAttackObject, setAttackTarget, setSituationOpenFire,
//   setPlayerTacOrder, setAlarmTacOrder, requestHelp, requestTarget,
//   calcTacOrder, coreMoveTo, setMainGoal, clearCurTacOrder, any movement/attack/
//   OPORD-advance/commander function NOT listed above.
//
// FSM-TODO SCANNER (1C — scanFsmTodosFromFile):
//   Calls ONLY std::ifstream + std::regex + fprintf. NO order functions, NO movement/attack/OPORD calls.
//   Verified by inspection: no warrior pointer, no MechWarrior type, no tac-order writes.

#include <vector>
#include <string>
#include <cstring>   // strncmp/strncpy used in VarStore inline methods (pulled via mech_brain_runtime.h too)

class MechWarrior;        // forward decl — executeSpecialBody_Apply needs the warrior pointer
struct VarStore;          // forward decl — Var.Set/Get handlers take a VarStore* (defined in mech_brain_runtime.h)
struct MechBrainRuntime;  // forward decl — commitBrainIntents takes a MechBrainRuntime*

// ---------------------------------------------------------------------------
// TECHSCRIPT-SPECIAL-DISPATCH-1C: FSM TODO marker classification.
// These are surfaced from "; TODO: manual ABL line: <payload>" comments that
// the FitIniFile parser strips. They are INFORMATION ONLY — no execution, no state change.

enum class FsmTodoKind : uint8_t {
    STATE_DEF,   // state <name>;
    STATE_END,   // endstate;
    TRANS,       // trans <target>;
    TRANS_BACK,  // transBack;
    OTHER_TODO,  // anything else (variable decls, comments, etc.)
};

struct FsmTodoEntry {
    FsmTodoKind kind;
    std::string name;  // populated for STATE_DEF (state name) and TRANS (target name); empty otherwise
};

struct BrainSpecialBody {
    std::vector<std::string>   verbs;      // raw DO-verb tokens from BrainSpecial Body block
    bool                       loaded = false;

    // 1C: FSM TODO markers extracted from raw-text scan (populated when MC2_BRAIN_DISPATCH_FSM_TODO=1).
    // Max 64 entries enforced in scanFsmTodosFromFile.
    static constexpr int kMaxFsmTodos = 64;
    std::vector<FsmTodoEntry>  fsmTodos;
};

// ---------------------------------------------------------------------------
// TECHSCRIPT-CALL-CHAIN-1A: per-mission TechSpecial index.
//
// One entry per TechSpecial block found by parseBrainSpecialBody_RawScan.
// Stored as a flat vector (small: typically <10 entries; linear-scan is fine).
// No STL hash — intentional per spec (small, mission-ephemeral, no pointer ordering).
// Lifecycle: mission-ephemeral, same as specialBody. Cleared at mission teardown via
// MechBrainRuntime destructor (std::vector auto-destructs).
//
// Entry-body selection rule (for the existing `specialBody` member on MechBrainRuntime):
//   1. First TechSpecial block whose key contains "scenario_main" (case-sensitive).
//   2. Else first block with type="MissionSpecial".
//   3. Else the first block found.
//   Documented here and in brain_special_dispatch.cpp.
struct SpecialIndexEntry {
    std::string     key;   // from key= field in TechSpecial block
    // BRAINSPECIAL-ALIAS-1: per-block alias= field (carver: alias = "Scenario.Main").
    // Resolved by specialIndexFind as a fallback when MC2_BRAIN_ALIAS=1; unused otherwise.
    std::string     alias;
    // BRAINSPECIAL-VARIANTOF-1: per-block variantOf= field (parent Special key).
    // Resolved after parse+global-merge when MC2_BRAIN_VARIANTOF=1:
    //   child with EMPTY Body inherits the parent chain's verbs wholesale;
    //   child that re-declares a Body overrides it (spec-delta: Body-level override —
    //   engine blocks have exactly one overridable section).
    // Unused (parsed but ignored) when the gate is OFF.
    std::string     variantOf;
    BrainSpecialBody body; // verbs collected from that block's Body { DO ... } section
};

using SpecialIndex = std::vector<SpecialIndexEntry>;

// Looks up a key in the index (linear scan, case-sensitive).
// Returns pointer to entry if found, nullptr if not found.
// Does NOT take ownership; pointer is valid for index lifetime.
const SpecialIndexEntry* specialIndexFind(const SpecialIndex& idx, const std::string& key);

// Returns true if body contains the Brain.CorePower false (POWERDOWN) verb.
bool bodyHasPowerdown(const BrainSpecialBody& body);

// DISPATCH-EFFECT-UNITEJECT-1: Returns true if body contains Unit.Eject or coreEject alias.
bool bodyHasUnitEject(const BrainSpecialBody& body);

// DISPATCH-EFFECT-COREGUARD-1: Returns true if body contains OPORD.CoreGuard or coreGuard alias.
bool bodyHasCoreGuard(const BrainSpecialBody& body);

// DISPATCH-EFFECT-COREMOVETO-1: Returns true if body contains OPORD.CoreMoveTo (prefix match).
bool bodyHasCoreMoveTo(const BrainSpecialBody& body);

// DISPATCH-EFFECT-COREATTACK-1: Returns true if body contains OPORD.CoreAttack (prefix match).
bool bodyHasCoreAttack(const BrainSpecialBody& body);

// BRAIN-OPORD-COREPATROL-1: Returns true if body contains OPORD.CorePatrol (prefix match, 16 chars).
bool bodyHasCorePatrol(const BrainSpecialBody& body);

// DISPATCH-EFFECT-UNITRETREAT-1: Returns true if body contains Unit.Retreat or coreRetreat alias.
bool bodyHasUnitRetreat(const BrainSpecialBody& body);

// DISPATCH-EFFECT-UNITRETREAT-1: Returns true if body has ANY GENERAL-slot-claiming effect verb.
// (currently: POWERDOWN || EJECT || GUARD || MOVETO || ATTACK || RETREAT)
bool bodyHasEffect(const BrainSpecialBody& body);

// BRAINSPECIAL-FLOW-WAIT-1: Returns true if the body carries WAIT/WAIT_UNTIL/STOP verbs.
// (The scanner only emits these when MC2_BRAIN_FLOW=1.)
bool bodyHasFlowControl(const BrainSpecialBody& body);

// BRAINSPECIAL-FLOW-WAIT-1: gate + flow-verb check combined — warrior.cpp switches to
// every-tick re-dispatch (skipping the class-level once-guard pre-set) when true.
bool brainFlowActiveForBody(const BrainSpecialBody& body);

// TRACE ONLY. Zero effects. No orders. No state writes.
// Gate: MC2_BRAIN_DISPATCH=1.
// varStore: pass &brainRuntime->varStore when DISPATCH_VAR=1; nullptr is safe (gate-off path).
// index: pass &brainRuntime->specialIndex when DISPATCH_CALL=1; nullptr is safe (gate-off path).
// FORBIDDEN-CALL CONTRACT: calls ONLY fprintf + fflush + loop + store->set/get +
//   recursive executeSpecialBody_TraceOnlyChained (when DISPATCH_CALL=1).
//   No setGeneralTacOrder, no order/movement/attack function.
void executeSpecialBody_TraceOnly(const BrainSpecialBody& body, int wid,
                                   VarStore* varStore = nullptr,
                                   const SpecialIndex* index = nullptr,
                                   const char* callerKey = nullptr);

// Internal recursive call-chain dispatch (TECHSCRIPT-CALL-CHAIN-1A).
// Called from executeSpecialBody_TraceOnly / executeSpecialBody_Apply when a
// TechSpecial.Call verb is encountered and DISPATCH_CALL=1.
// depth: current recursion depth (starts at 1 for the first call).
// visited: per-tick cycle-guard set (caller creates, passes by reference).
// fromKey: key of the calling body (for trace), or "root" if called from root body.
// FORBIDDEN-CALL CONTRACT: pure trace + recursion. No order functions. No effects.
//   Chained effects (Brain.CorePower etc.) in called bodies are NOT applied in 1A.
//   Chained verbs trace as [BRAIN_DISPATCH depth=N] or [BRAIN_DISPATCH_UNKNOWN depth=N].
// DEPTH LIMIT: 8 (hardcoded; matches recon recommendation).
// CYCLE GUARD: key already in visited → [BRAIN_DISPATCH_CALL_CYCLE] + skip.
void executeSpecialBody_TraceOnlyChained(const BrainSpecialBody& body,
                                          int wid,
                                          const SpecialIndex& index,
                                          int depth,
                                          std::vector<std::string>& visited,
                                          const char* fromKey,
                                          VarStore* varStore = nullptr);

// 1B EFFECT DISPATCHER.
// Applies the ONE recognized effect when MC2_BRAIN_DISPATCH_APPLY=1:
//   Brain.CorePower false → warrior->setGeneralTacOrder(TACTICAL_ORDER_POWERDOWN)
// All other verbs: trace only, no effect (Var.* handled when DISPATCH_VAR=1).
// warrior must be non-null (caller guards).
// varStore: pass &brainRuntime->varStore when DISPATCH_VAR=1; nullptr is safe.
// index: pass &brainRuntime->specialIndex when DISPATCH_CALL=1; nullptr is safe.
// CALL-CHAIN-1A NOTE: TechSpecial.Call verbs in the ROOT body are dispatched for trace only
//   (chained bodies traced without effects). Chained-effect execution is CALL-CHAIN-1B.
// Returns true if a recognized effect was applied (caller uses to suppress synthetic HOLD).
bool executeSpecialBody_Apply(const BrainSpecialBody& body, MechWarrior* warrior, int wid,
                               VarStore* varStore = nullptr,
                               const SpecialIndex* index = nullptr,
                               const char* callerKey = nullptr);

// BRAIN-DECISION-INTENT-QUEUE-1: commit all pending intents for this warrior.
// Called right after executeSpecialBody_Apply returns, when MC2_BRAIN_INTENT_QUEUE=1.
// Drains runtime->pendingIntents[], reconstructs the TacticalOrder for each, and calls
// warrior->setGeneralTacOrder().  This is the ONLY path that calls setGeneralTacOrder
// when gate ON — the verb handlers no longer call it directly.
// Emits [BRAIN_INTENT_COMMIT] verb=<v> order=<type> tick=<brainTick> wid=<W> per intent.
// After draining, sets pendingIntentCount to 0.
// warrior and runtime must be non-null (caller guards).
void commitBrainIntents(MechWarrior* warrior, struct MechBrainRuntime* runtime);

// DISPATCH-LOADER-RAW-1: Parse BrainSpecial/TechSpecial Body DO-verbs.
// Primary: raw brace-block scanner (handles inline-quoted DO args).
// Fallback: legacy FitIniFile [BrainSpecial]/[Body]/DO0=... bracket form.
// Returns false if file absent or no verbs found.
//
// TECHSCRIPT-CALL-CHAIN-1A: if outIndex is non-null, the raw brace-block scanner also
// populates it with one SpecialIndexEntry per TechSpecial block found.
// The entry-body selection rule (for outBody) is documented in brain_special_dispatch.h
// under SpecialIndexEntry. The index is populated regardless of DISPATCH_CALL gate state
// (build-time unconditional) — the gate controls runtime call resolution only.
bool parseBrainSpecialBody(const char* missionName, BrainSpecialBody& outBody,
                            SpecialIndex* outIndex = nullptr);

// BRAIN-DISPATCH-HARNESS-1: explicit-path variant for offline harness use.
// Directly opens fitPath (absolute or CWD-relative) without the "data/missions/" prefix logic.
// Same parser logic as parseBrainSpecialBody; index populated if outIndex is non-null.
// Intended for test/harness use only — game code uses parseBrainSpecialBody(missionName).
bool parseBrainSpecialBodyFromPath(const char* fitPath, BrainSpecialBody& outBody,
                                    SpecialIndex* outIndex = nullptr);

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D: Var.Set / Var.Get parser + handler.
//
// parseVarVerb — minimal tokenizer for "Var.Set/Var.Get" verb strings.
//   verbStr  — the full DO-verb string (e.g. `Var.Set "foo" 1 scope=Mission`).
//   outKey   — output: key string (up to 31 chars + '\0').
//   outValue — output (Set only; unused for Get): value token (up to 31 chars + '\0').
//   outScope — output: VarScope::Unit (default) or VarScope::Mission.
//   Returns true and populates outputs on success.
//   Returns false (soft-fail) on malformed input (empty key, missing value for Set).
//
// handleVarSet / handleVarGet — called from executeSpecialBody_TraceOnly +
//   executeSpecialBody_Apply when MC2_BRAIN_DISPATCH_VAR=1.
//   FORBIDDEN-CALL CONTRACT (VAR handlers): call ONLY store->set/get + fprintf/fflush.
//   ZERO calls to setGeneralTacOrder or any order/movement/attack function.
//   Verified by inspection: no warrior pointer, no tac-order type.
//
// NOTE: VarScope enum is defined in mech_brain_runtime.h (included after this header by callers).
// To avoid a circular include, handleVarSet/Get take VarScope as uint8_t cast internally.
// The free function prototypes use a forward-declared uint8_t scope parameter convention;
// see brain_special_dispatch.cpp for the actual VarScope-aware implementation.

bool parseVarVerb(const char* verbStr, char outKey[32], char outValue[32], uint8_t* outScope);

// Handles "Var.Set" when DISPATCH_VAR=1.
// store may be nullptr if gate-off or runtime unavailable — soft-fails gracefully.
void handleVarSet(const char* key, const char* value, uint8_t scope, VarStore* store, int wid);

// Handles "Var.Get" when DISPATCH_VAR=1.
void handleVarGet(const char* key, uint8_t scope, VarStore* store, int wid);

// TECHSCRIPT-SPECIAL-DISPATCH-1C: Raw-text scan of the specials file for FSM TODO markers.
// Gate: MC2_BRAIN_DISPATCH_FSM_TODO=1 (requires MC2_BRAIN_DISPATCH=1; callee warns if invoked without).
// Populates outBody.fsmTodos (capped at BrainSpecialBody::kMaxFsmTodos).
// Returns count of FSM TODO lines found (0 if file absent or no TODO lines).
// FORBIDDEN-CALL CONTRACT: calls ONLY std::ifstream + classify + fprintf. No order functions.
int scanFsmTodosFromFile(const char* missionName, BrainSpecialBody& outBody);

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D-M: mission-scope Var store management.
//
// resetMissionVarStore() — clears the global g_missionVarStore.
//   Call at mission load (before warriors execute their first specials tick).
//   Gate: no gate guard; always safe to call (zero-cost when store count=0).
//   Defined in brain_special_dispatch.cpp.
//
// See code/brain_mission_var_store.h for MissionVarStore struct + policy doc.
#include "brain_mission_var_store.h"
void resetMissionVarStore();

// ---------------------------------------------------------------------------
// GAP-A MULTI-WARRIOR SPECIALS: mission-level specials cache.
//   cacheMissionSpecialBody(name) — parse <name>_specials.fit ONCE at mission load.
//   resetMissionSpecialCache()    — clear the cache (call at mission load before caching).
//   applyCachedSpecialBodyToRuntime(rt) — copy cached body+index into a runtime that
//     has none yet (lazily-allocated warriors). No-op if cache empty or rt already loaded.
//   All defined in brain_special_dispatch.cpp. Mission-ephemeral; gate MC2_BRAIN_DISPATCH.
struct MechBrainRuntime;
void resetMissionSpecialCache();
void cacheMissionSpecialBody(const char* specialFitName);
bool applyCachedSpecialBodyToRuntime(struct MechBrainRuntime* rt);

// ---------------------------------------------------------------------------
// BRAIN-OPORD-COREPATROL-1: per-tick patrol advance.
// Called every brain tick when MC2_BRAIN_PATROL=1 and brainRuntime->patrolActive.
// Polls curTacOrder.status(warrior)==TACORDER_SUCCESS for arrival, advances the patrol
// cursor, emits a MOVETO_POINT intent via emitBrainIntent (or direct setGeneralTacOrder
// when MC2_BRAIN_INTENT_QUEUE=0).  Gate default OFF.
// If patrolActive==false or gate OFF: no-op.
// Returns true if an advance (re-emit) was performed this tick.
bool tickPatrolAdvance(MechWarrior* warrior, MechBrainRuntime* runtime, int wid);

// BRAIN-ENGAGE-1: per-tick autonomous threat engagement. Acquires the nearest live enemy within
// runtime->engageRadius and issues an ATTACK_OBJECT order via the intent queue. Returns true while
// actively engaging (caller suppresses patrol advance). Gate MC2_BRAIN_ENGAGE (default OFF).
bool tickEngageNearest(MechWarrior* warrior, MechBrainRuntime* runtime, int wid);

// BRAIN-OPORD-MOVE-1: non-patrol OPORD movement (Escort follow, ReturnToPost leash) when the unit
// is not engaging or patrolling. Returns true if it issued a MOVETO. Gate MC2_BRAIN_ENGAGE.
bool tickOpordMove(MechWarrior* warrior, MechBrainRuntime* runtime, int wid);

// BRAIN-WORLD-SNAPSHOT-1: snapshot gate.
//
// s_brainSnapshotEnabled() — returns true when MC2_BRAIN_SNAPSHOT=1.
//   Gate default OFF.  Gate OFF = byte-identical to pre-snapshot behavior.
//
// BrainWorldSnapshot struct is defined in brain_world_snapshot.h (POD, no engine headers).
// buildBrainWorldSnapshot() is a static helper defined in warrior.cpp (needs protected
// member access to MechWarrior fields).  Callers outside warrior.cpp use only the gate query.
#include "brain_world_snapshot.h"
bool s_brainSnapshotEnabled();
