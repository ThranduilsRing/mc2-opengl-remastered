//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//								WARRIOR.CPP

#include <atomic>
#include "warrior_internal.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "brain_execution_policy.h"
#include "mc2fmt.h"
#include <utility>
#include "sim_trace.h"

// ABL-VM-FACTS-1: TU-local definitions for the abl_trace externs.
namespace mc2_abl_trace {
	int g_peakCallDepth = 0;
	FrameCounters g_frame = {0, 0, 0, 0, 0.0};
}

// ABL-VM-FACTS-1: hook called from mclib/ablxstmt.cpp CallStackLevel++ site.
// Forwards to the gated inline tracker (byte-identical no-op when OFF).
void mc2AblNotePeakDepth(int level) { mc2_abl_trace::notePeakDepth(level); }

// Brain task queue: on under the TechBrain policy (brain_execution_policy.h); re-derived once per mission load.
static bool          s_brainTaskQEnabled     = false;
static bool          s_brainTaskQTraceEnabled = false;
static std::uint32_t s_brainTaskQGateGen     = 0;
static void initBrainTaskQGate() {
    if (s_brainTaskQGateGen == mc2::brainpolicy::generation()) return;
    s_brainTaskQGateGen      = mc2::brainpolicy::generation();
    s_brainTaskQEnabled     = mc2::brainpolicy::techBrain();
    s_brainTaskQTraceEnabled = mc2::gate::get<"MC2_BRAIN_TASKQ_TRACE">();
}

// Brain runtime (slot writes + short-circuit): on under the TechBrain policy; re-derived once per mission load.
static bool             s_brainRuntimeEnabled      = false;
static bool             s_brainRuntimeTraceEnabled  = false;
static bool             s_brainRuntimeApplyEnabled  = false;
static std::uint32_t    s_brainRuntimeGateGen       = 0;
static BrainRuntimeMode s_brainRuntimeForcedMode    = BrainRuntimeMode::Legacy;
static void initBrainRuntimeGate() {
    if (s_brainRuntimeGateGen == mc2::brainpolicy::generation()) return;
    s_brainRuntimeGateGen      = mc2::brainpolicy::generation();
    s_brainRuntimeEnabled      = mc2::brainpolicy::techBrain();
    s_brainRuntimeTraceEnabled = mc2::gate::get<"MC2_BRAIN_RUNTIME_TRACE">();
    s_brainRuntimeApplyEnabled = s_brainRuntimeEnabled;
    // Mode source: MC2_BRAIN_RUNTIME_FORCE_MODE env override only; per-unit FIT loader may override per-warrior.
    s_brainRuntimeForcedMode   = BrainRuntimeMode::Legacy;
    if (mc2::gate::isSet<"MC2_BRAIN_RUNTIME_FORCE_MODE">()) {
        if      (mc2::gate::is<"MC2_BRAIN_RUNTIME_FORCE_MODE", "hybrid">())   s_brainRuntimeForcedMode = BrainRuntimeMode::Hybrid;
        else if (mc2::gate::is<"MC2_BRAIN_RUNTIME_FORCE_MODE", "enhanced">()) s_brainRuntimeForcedMode = BrainRuntimeMode::Enhanced;
        else                                                                  s_brainRuntimeForcedMode = BrainRuntimeMode::Legacy;
    }
}

// MC2_BRAIN_FIXED_TICK gate — checked once at first brain-cadence check
// BRAIN-FIXED-TICK-1: fixed sim-time accumulator replaces per-warrior brainUpdate advance.
// Gate OFF (default): byte-identical to baseline (brainUpdate += BrainUpdateFrequency).
// Gate ON: global accumulator advanced by scenarioTime delta; integer tick index drives
//          which warriors fire; per-warrior brainUpdate still used as stagger offset only.
static bool  s_brainFixedTickEnabled     = false;
static bool  s_brainFixedTickTrace       = false;
static bool  s_brainFixedTickGateChecked = false;
static float s_brainFixedTickHz          = 10.0f;   // default 10 Hz = 0.1 s period
static float s_brainFixedTickPeriod      = 0.1f;    // 1.0 / hz, computed at init

// Per-frame accumulator: how many sim-seconds have accumulated toward next fixed tick.
// Advanced once per frame in updateBrainFixedTickAccumulator().
// Not per-warrior — this is a global sim-time counter.
static float    s_brainFixedTickAccum    = 0.0f;
static float    s_brainFixedTickSimLast  = -1.0f;   // last scenarioTime seen; -1 = uninit
static uint32_t s_brainTickIndex         = 0;       // monotonic tick counter, per-frame global
static uint32_t s_brainTicksThisFrame    = 0;       // ticks fired in current frame (reset each frame)

// BRAIN-DECISION-INTENT-QUEUE-1: accessor so brain_special_dispatch.cpp can stamp intents.
uint32_t getBrainTickIndex() { return s_brainTickIndex; }

// BRAINSPECIAL-FLOW-WAIT-1: sim-time accessor for WAIT deadlines (milliseconds).
// scenarioTime is the deterministic mission sim clock (same time base the fixed-tick
// accumulator above consumes); WAIT deadlines keyed on it hold under the determinism
// model (the queue key already carries sim_frame_ms). Harness stubs this symbol.
uint32_t getBrainTimeMs() {
    if (scenarioTime <= 0.0f) return 0;
    return static_cast<uint32_t>((scenarioTime * 1000.0f));
}

// Save v2 BRN1: the global fixed-tick accumulator travels with the per-warrior runtimes.
void getBrainFixedTickState(uint32_t& tickIndex, float& accum, float& simLast) {
    tickIndex = s_brainTickIndex; accum = s_brainFixedTickAccum; simLast = s_brainFixedTickSimLast;
}
void setBrainFixedTickState(uint32_t tickIndex, float accum, float simLast) {
    s_brainTickIndex = tickIndex; s_brainFixedTickAccum = accum; s_brainFixedTickSimLast = simLast;
}

static void initBrainFixedTickGate() {
    if (s_brainFixedTickGateChecked) return;
    s_brainFixedTickGateChecked = true;
    s_brainFixedTickEnabled = mc2::gate::get<"MC2_BRAIN_FIXED_TICK">();
    s_brainFixedTickTrace   = mc2::gate::get<"MC2_BRAIN_FIXED_TICK_TRACE">();
    if (s_brainFixedTickEnabled) {
        const float hz = mc2::gate::get<"MC2_BRAIN_FIXED_TICK_HZ">();   // the row clamps to [1, 30]
        s_brainFixedTickHz     = hz;
        s_brainFixedTickPeriod = 1.0f / hz;
        mc2::fmt::print_err("[BRAIN_FIXED_TICK] init: hz={:.1f} period={:.4f}s trace={:d}\n", s_brainFixedTickHz, s_brainFixedTickPeriod, mc2::fmt::as_i32(static_cast<int>(s_brainFixedTickTrace)));
        std::fflush(stderr);
    }
}

// Called once per frame (at start of first warrior's updateActions for this frame) to
// advance the accumulator by the sim-time delta and count whole ticks.
// Returns true if at least one tick fired this frame (i.e., warriors may run brain).
// All warriors within a single render frame share the same brainTickIndex window.
static bool advanceBrainFixedTickAccumulator(float scenarioTime) {
    if (s_brainFixedTickSimLast < 0.0f) {
        // First call: seed last-time, no ticks yet.
        s_brainFixedTickSimLast  = scenarioTime;
        s_brainTicksThisFrame    = 0;
        return false;
    }
    float dt = scenarioTime - s_brainFixedTickSimLast;
    if (dt < 0.0f) dt = 0.0f;  // guard against rewind (save/load)
    s_brainFixedTickSimLast = scenarioTime;
    s_brainFixedTickAccum  += dt;
    s_brainTicksThisFrame   = 0;
    while (s_brainFixedTickAccum >= s_brainFixedTickPeriod) {
        s_brainFixedTickAccum -= s_brainFixedTickPeriod;
        ++s_brainTickIndex;
        ++s_brainTicksThisFrame;
    }
    return (s_brainTicksThisFrame > 0);
}

// Track which frame we last advanced the accumulator for, so each render frame advances once.
static uint32_t s_brainAccumLastFrame = 0xFFFFFFFFu;

void MechWarrior::setBrainName (const char *brainName)
{
	strncpy(brainStr, brainName, MAXLEN_PILOT_BRAIN - 1);
	brainStr[MAXLEN_PILOT_BRAIN - 1] = '\0';
}

long MechWarrior::setBrain (long brainHandle) {

	long brainErr = NO_ERR;

	if (brain) {
		delete brain;
		brain = nullptr;
		for (long i = 0; i < NUM_PILOT_ALARMS; i++)
			brainAlarmCallback[i] = nullptr;
	}
	warriorBrainHandle = brainHandle;
	if (brainHandle > -1)
	{
		brain = new ABLModule;
		brainErr = brain->init(brainHandle);
		if (brainErr == NO_ERR) {
			char brainName[500];
			mc2::fmt::to_buf(brainName, "Pilot {}", mc2::fmt::safe(name));
			brain->setName(brainName);
			for (long i = 0; i < NUM_PILOT_ALARMS; i++)
				brainAlarmCallback[i] = brain->findFunction(pilotAlarmFunctionName[i], true);
		}
	}
	return(brainErr);
}

long MechWarrior::runBrain (void) {

	ZoneScopedN("GameLogic.AI.BrainRun");

//	if (teamId  > -1)
//		if (teamId != Team::home->getId())
//		return(0);

	// PATROL-DRIVE-1 / pure-Enhanced: inline-Brain warriors (TechScript/Enhanced) carry NO
	// legacy ABL brain (brain==NULL), but their behavior is driven by the Enhanced runtime
	// further down this function (patrol tick, dispatch). The legacy ABL path (brain->execute)
	// is the only consumer of `brain`. So bail on NULL brain ONLY when there is also no
	// Enhanced runtime to run — otherwise fall through and let the runtime drive. Stock
	// warriors always have a brain, so this is byte-identical for stock.
	if (!brain) {
		const bool enhancedRuntimeActive =
			s_brainRuntimeEnabled && s_brainRuntimeApplyEnabled &&
			brainRuntime && (brainRuntime->mode == BrainRuntimeMode::Enhanced);
		if (!enhancedRuntimeActive)
			return(0);
	}

	// MOD-HEALTH-1: tools/mod_smoke.py asserts AI is alive ("dead AI on POAR") from these
	// milestone lines; gated by MC2_MOD_HEALTH_DIAG (mod_smoke sets it), three lines per process.
	if (static const bool s_modHealth = mc2::gate::get<"MC2_MOD_HEALTH_DIAG">(); s_modHealth) {
		static unsigned long s_brainRuns = 0;
		++s_brainRuns;
		if (s_brainRuns == 1 || s_brainRuns == 100 || s_brainRuns == 1000) {
			mc2::fmt::print_out("[MOD_HEALTH v1] event=brain_runs count={:d} scenarioTime={:.2f}\n",
				mc2::fmt::as_u64(s_brainRuns), static_cast<double>(scenarioTime));
			fflush(stdout);
		}
	}

	// Param 1 is the ID of this mech...
	//ABLi_setIntegerParam(brainParams, 0, ((BattleMechPtr)owner)->ID);

	// Param 2 is the current game clock...
	//ABLi_setRealParam(brainParams, 1, scenarioTime);

	// Param 3 is the type of AI event (0 is
	// standard sequential update)...
	//ABLi_setIntegerParam(brainParams, 2, 0);

	// Now, call the brain...
	CurGroup = getGroup();
	CurObject = static_cast<GameObjectPtr>(getVehicle());
	CurObjectClass = getVehicle()->getObjectClass();
	CurWarrior = this;
	CurContact = nullptr;
	curEventID = 0;
	curEventTrigger = 0;
	ModuleInfo moduleInfo;
	if (brain)                       // PATROL-DRIVE-1: inline-Brain warriors have no ABL brain.
		brain->getInfo(&moduleInfo); // moduleInfo is only consumed by the legacy ABL path below.

	// BRAIN-RUNTIME-1B: check if this warrior is under Enhanced runtime control.
	// If so, skip brain->execute() and the ABL-derived tac order, push+drain HOLD once.
	// CurGroup/CurWarrior clear and clearAlarmsHistory() run UNCONDITIONALLY below.
	bool enhancedApply = s_brainRuntimeEnabled && s_brainRuntimeApplyEnabled
	                     && brainRuntime
	                     && (brainRuntime->mode == BrainRuntimeMode::Enhanced);

	// TACTIC-WEIGHTS-B: selection MUST run BEFORE dispatch so the attack order
	// (once-guarded) picks up selectedTactic this tick. Relocated above the brain
	// legacy/enhanced branch for that reason.
	// TACTIC-WEIGHTS-A: deterministic weighted tactic selection (TRACE-ONLY).
	// Gate: MC2_TACTIC_WEIGHTS=1 (default OFF).
	// Runs for BOTH legacy+enhanced paths; NO behavior write (deferred to TACTIC-WEIGHTS-B).
	// missionSeed: MC2_BRAIN_MISSION_SEED env override, else 0xDEADBEEF placeholder.
	{
		static bool   s_tacticWeightsEnabled  = false;
		static bool   s_tacticWeightsChecked  = false;
		static uint32_t s_missionSeed         = 0xDEADBEEFu;
		if (!s_tacticWeightsChecked) {
			s_tacticWeightsChecked = true;
			s_tacticWeightsEnabled = mc2::gate::get<"MC2_TACTIC_WEIGHTS">();
			const std::string seedText = mc2::gate::str<"MC2_BRAIN_MISSION_SEED">();   // "" when unset or empty
			if (!seedText.empty())
				s_missionSeed = static_cast<uint32_t>(std::strtoul(seedText.c_str(), nullptr, 0));
		}
		if (s_tacticWeightsEnabled && brainRuntime) {
			// Collect pilot stats for modulation
			int gunnery       = static_cast<int>(static_cast<unsigned char>(skills[MWS_GUNNERY]));
			int pilotAggr     = static_cast<int>(static_cast<unsigned char>(aggressiveness));
			int pilotCourage  = static_cast<int>(static_cast<unsigned char>(courage));

			// Wang hash: deterministic per (wid, brainTick, missionSeed)
			uint32_t h = tacticWang(static_cast<int>(vehicleWID), s_brainTickIndex, s_missionSeed);

			// Apply pilot modulation to FIT-loaded base weights
			float wEff[NUM_TACTICS] = {};
			applyPilotModulation(brainRuntime->tacticWeights, wEff, NUM_TACTICS,
			                     gunnery, pilotAggr, pilotCourage);

			// Weighted pick — pure, deterministic
			int chosen = selectTactic(wEff, NUM_TACTICS, h);
			brainRuntime->selectedTactic = chosen;

			// Emit trace (no behavior write)
			mc2::fmt::print_err("[BRAIN_TACTIC_SELECT] wid={:d} tick={:d} tactic={}({:d})\n", mc2::fmt::as_i32(vehicleWID), mc2::fmt::as_u32(s_brainTickIndex), mc2::fmt::safe(tacticName(chosen)), mc2::fmt::as_i32(chosen));
			std::fflush(stderr);
		}
	}

	long brainErr = 0;
	if (!enhancedApply) {
		// ── Legacy ABL path ──────────────────────────────────────────────────────────
		// ABL-VM-FACTS-1: time the fire, read stmts (execute() return) + state
		// transitions at exit. Byte-identical when the ABL diag tag is OFF.
		mc2_abl_trace::FireScope ablFire("warrior", static_cast<int>(vehicleWID));
		long ablStmts;
		{ ZoneScopedN("GameLogic.AI.UnitBrainExecute"); ablStmts = brain->execute(); }
		ablFire.record(ablStmts, NumStateTransitions);
		// Well, we'll just set it every frame so it doesn't screw up :)
		setUseGoalPlan(!MPlayer && (getCommander() != Commander::home));

		if (useGoalPlan) {
			TacticalOrder tacOrder;
			long result = calcTacOrder(mainGoalAction, mainGoalObjectWID, mainGoalLocation, mainGoalControlRadius, 0, 1, 150.0, 300, tacOrder);
			if (result == 0) {
				if (tacOrder.code != TACTICAL_ORDER_NONE) {
					if (getCommander() == Commander::home)
						setPlayerTacOrder(tacOrder);
					else
						setGeneralTacOrder(tacOrder);
				}
				}
			else if (result > 0) {
				setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
				clearCurTacOrder();
			}
		}

		brainErr = brain->getInteger();
		switch (brainErr) {
			case 0:
				// No error...
				break;
			case 1:
				// General purpose error, for now, so bomb out...
				break;
		}
	} else {
		// ── APPLY=1 + Enhanced: skip ABL execute; push HOLD once, drain+apply ───────
		if (brainTaskQueue && !brainRuntime->initialHoldPushed) {
			brainRuntime->initialHoldPushed = 1;
			brainTaskQueue->push(/*tier=*/0, /*frame_ms=*/0, vehicleWID, BrainTaskType::BRAIN_TASK_HOLD);
		}
		// TECHSCRIPT-SPECIAL-DISPATCH-1B: DISPATCH_APPLY gate check.
		// When MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_APPLY=1 and the body contains a
		// recognized effect verb (Brain.CorePower false → POWERDOWN), the dispatcher owns
		// the GENERAL-slot write this tick. The synthetic HOLD_TASK is suppressed to ensure
		// EXACTLY ONE GENERAL-slot write per tick.
		// When DISPATCH_APPLY=0, synthetic HOLD fires as 1B-runtime default (unchanged).
		const bool s_dispatchApply = mc2::brainpolicy::techBrain();   // policy, read live (was a cached gate)
		// BRAIN-DECISION-INTENT-QUEUE-1: gate check (default OFF).
		// When ON: executeSpecialBody_Apply emits intents; commitBrainIntents drains them.
		const bool s_intentQueue = mc2::brainpolicy::techBrain();
		// BRAIN-COMMIT-PHASE-1: deferred commit gate (default OFF).
		// Requires MC2_BRAIN_INTENT_QUEUE=1 to be meaningful.
		// Gate ON → inline commitBrainIntents calls are SKIPPED here; intents persist in
		//   runtime->pendingIntents[] until commitAllBrainIntents() drains them in WID order
		//   after the full warrior update loop in objmgr.cpp (GameObjectManager::update).
		// Gate OFF → commitBrainIntents runs inline per-warrior (rung-5 behavior, unchanged).
		const bool s_brainCommitPhase = mc2::brainpolicy::techBrain();

		// BRAIN-WORLD-SNAPSHOT-1: static populate helper (main-thread only).
		// Accesses protected MechWarrior members — must remain in warrior.cpp.
		// Called below when s_brainSnapshotEnabled() is true (gate ON).
		// FORBIDDEN: must not call setGeneralTacOrder or any order/movement/attack function.
		auto buildBrainWorldSnapshotLocal = [&](BrainWorldSnapshot& out) {
			// Warrior identity: vehicleWID is in scope and is the WID passed to dispatch.
			// MechWarrior itself is not a GameObject; vehicleWID == warrior's vehicle WatchID.
			out.warriorId = static_cast<int32_t>(vehicleWID);

			// Team ID: deferred to rung 7/8 — ATTACK friendly-fire still reads live.
			out.teamId = 0;

			// Liveness: query the vehicle (MoverPtr from getVehicle() — may be null; guard it).
			{
				MoverPtr veh = getVehicle();
				out.isDisabled = (veh && veh->isDisabled()) ? 1u : 0u;
			}
			out.commanderIsHome = 0u; // deferred — commander identity not queried this rung

			// Main goal (protected MechWarrior fields; directly accessible in warrior.cpp)
			out.mainGoalObjectWID     = static_cast<int32_t>(mainGoalObjectWID);
			out.mainGoalLocation[0]   = mainGoalLocation.x;
			out.mainGoalLocation[1]   = mainGoalLocation.y;
			out.mainGoalLocation[2]   = mainGoalLocation.z;
			out.mainGoalControlRadius = mainGoalControlRadius;

			// Attack orders target WID (protected field, directly accessible in warrior.cpp)
			out.attackOrderTargetWID = static_cast<int32_t>(attackOrders.targetWID);

			// Once-guard flags — the primary snapshot surface for rung 6
			out.effectApplied[std::to_underlying(BSNAPFX_POWERDOWN)] = brainRuntime->dispatchEffectApplied;
			out.effectApplied[std::to_underlying(BSNAPFX_EJECT)]     = brainRuntime->ejectEffectApplied;
			out.effectApplied[std::to_underlying(BSNAPFX_GUARD)]     = brainRuntime->guardEffectApplied;
			out.effectApplied[std::to_underlying(BSNAPFX_MOVETO)]    = brainRuntime->moveToEffectApplied;
			out.effectApplied[std::to_underlying(BSNAPFX_ATTACK)]    = brainRuntime->attackEffectApplied;
			out.effectApplied[std::to_underlying(BSNAPFX_RETREAT)]   = brainRuntime->retreatEffectApplied;

			// Debug tick stamp
			out.brainTick = getBrainTickIndex();
		};

		bool dispatcherAppliedEffect = false;
		if (s_dispatchApply && brainRuntime && brainRuntime->specialBody.loaded) {
			// DISPATCH-EFFECT-UNITRETREAT-1: bodyHasEffect() now covers POWERDOWN, EJECT, GUARD, MOVETO, ATTACK, RETREAT.
			// CALL-CHAIN-1A: dispatch ALL loaded bodies (not just effect bodies).
			// executeSpecialBody_Apply traces all verbs; applies effect ONCE if present.
			// The HOLD suppression only triggers when an effect verb was actually applied
			// (dispatcherAppliedEffect=true ← Apply return value true).
			const SpecialIndex* idx = brainRuntime->specialIndex.empty()
			    ? nullptr : &brainRuntime->specialIndex;

			const bool hasPowerdown = bodyHasPowerdown(brainRuntime->specialBody);
			const bool hasEject     = bodyHasUnitEject(brainRuntime->specialBody);
			const bool hasGuard     = bodyHasCoreGuard(brainRuntime->specialBody);
			const bool hasMoveTo    = bodyHasCoreMoveTo(brainRuntime->specialBody);
			const bool hasAttack    = bodyHasCoreAttack(brainRuntime->specialBody);
			const bool hasRetreat   = bodyHasUnitRetreat(brainRuntime->specialBody);
			const bool hasEffect    = hasPowerdown || hasEject || hasGuard || hasMoveTo || hasAttack || hasRetreat;

			// BRAINSPECIAL-FLOW-WAIT-1: flow-bearing bodies (WAIT/WAIT_UNTIL/STOP present,
			// gate MC2_BRAIN_FLOW) re-dispatch EVERY tick — the class-level once-guard
			// pre-set below would permanently suppress an effect a WAIT still gates.
			// Apply's per-verb-index flowFired guards prevent order re-emission instead.
			// flowFiredCount>0 means the dispatcher has claimed the GENERAL slot at some
			// point — keep suppressing the synthetic HOLD exactly like alreadyDone does.
			// Gate OFF: brainFlowActiveForBody is false (scanner emits no flow verbs) —
			// this branch is untouched dead code, byte-identical behavior.
			const bool flowActive = brainFlowActiveForBody(brainRuntime->specialBody);
			if (flowActive) {
				bool applied = executeSpecialBody_Apply(brainRuntime->specialBody, this, vehicleWID,
				                                        &brainRuntime->varStore, idx, "");
				if (s_intentQueue && !s_brainCommitPhase) commitBrainIntents(this, brainRuntime);
				dispatcherAppliedEffect = applied || (brainRuntime->flowFiredCount > 0);
				// (falls through to the shared dispatcherAppliedEffect drain below)
			} else {
			// (non-flow path below is the pre-slice code, unchanged)

			// BRAIN-WORLD-SNAPSHOT-1: capture world state before once-guard reads (gate ON only).
			// Gate OFF: snap is zero-initialised and unused — reads fall through to live runtime.
			BrainWorldSnapshot snap = {};
			const bool snapshotGate = s_brainSnapshotEnabled();
			if (snapshotGate)
				buildBrainWorldSnapshotLocal(snap);

			// Once-guard: if the effect was already applied, suppress HOLD without re-applying.
			// Gate ON:  read from snapshot.effectApplied[N] (the seam for future off-thread emit).
			// Gate OFF: read live from runtime (byte-identical to pre-snapshot behavior).
			const bool powerdownDone = hasPowerdown && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_POWERDOWN)]
			    : brainRuntime->dispatchEffectApplied);
			const bool ejectDone     = hasEject     && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_EJECT)]
			    : brainRuntime->ejectEffectApplied);
			const bool guardDone     = hasGuard     && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_GUARD)]
			    : brainRuntime->guardEffectApplied);
			const bool moveToDone    = hasMoveTo    && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_MOVETO)]
			    : brainRuntime->moveToEffectApplied);
			const bool attackDone    = hasAttack    && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_ATTACK)]
			    : brainRuntime->attackEffectApplied);
			const bool retreatDone   = hasRetreat   && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_RETREAT)]
			    : brainRuntime->retreatEffectApplied);
			const bool alreadyDone   = hasEffect && ((!hasPowerdown || powerdownDone) && (!hasEject || ejectDone) && (!hasGuard || guardDone) && (!hasMoveTo || moveToDone) && (!hasAttack || attackDone) && (!hasRetreat || retreatDone));

			if (!alreadyDone) {
				if (hasEffect) {
					// Effect body: apply once, suppress HOLD slot write thereafter.
					// Set the appropriate once-guard flag BEFORE calling Apply so a re-entrant
					// tick (edge case) can't fire a second order.
					if (hasPowerdown && !brainRuntime->dispatchEffectApplied)
						brainRuntime->dispatchEffectApplied = 1;
					if (hasEject && !brainRuntime->ejectEffectApplied)
						brainRuntime->ejectEffectApplied = 1;
					if (hasGuard && !brainRuntime->guardEffectApplied)
						brainRuntime->guardEffectApplied = 1;
					if (hasMoveTo && !brainRuntime->moveToEffectApplied)
						brainRuntime->moveToEffectApplied = 1;
					if (hasAttack && !brainRuntime->attackEffectApplied)
						brainRuntime->attackEffectApplied = 1;
					if (hasRetreat && !brainRuntime->retreatEffectApplied)
						brainRuntime->retreatEffectApplied = 1;
					bool applied = executeSpecialBody_Apply(brainRuntime->specialBody, this, vehicleWID,
					                                        &brainRuntime->varStore, idx, "");
					// BRAIN-DECISION-INTENT-QUEUE-1: drain pending intents inline (gate ON only).
					// BRAIN-COMMIT-PHASE-1: when s_brainCommitPhase is ON, skip inline commit —
					//   intents stay in runtime->pendingIntents[] for commitAllBrainIntents().
					// commitBrainIntents is the sole caller of setGeneralTacOrder when gate ON.
					if (s_intentQueue && !s_brainCommitPhase) commitBrainIntents(this, brainRuntime);
					if (applied)
						dispatcherAppliedEffect = true;
				} else {
					// No effect verb — trace-dispatch every tick (no slot ownership; HOLD fires normally).
					executeSpecialBody_Apply(brainRuntime->specialBody, this, vehicleWID,
					                        &brainRuntime->varStore, idx, "");
					// Gate ON + commit-phase OFF: drain inline (no-op since no effect verbs; defensive).
					// Gate ON + commit-phase ON: leave empty pendingIntents for the phase (no-op there too).
					if (s_intentQueue && !s_brainCommitPhase) commitBrainIntents(this, brainRuntime);
					// dispatcherAppliedEffect stays false → HOLD fires as normal.
				}
			} else {
				// Effect already applied — suppress HOLD without re-applying.
				dispatcherAppliedEffect = true;
			}
			}  // BRAINSPECIAL-FLOW-WAIT-1: end of non-flow (pre-slice) path
			if (dispatcherAppliedEffect) {
				if (brainTaskQueue) {
					BrainTaskEntry task;
					while (brainTaskQueue->drainWithTask(task)) {
						// Discard queued tasks — dispatcher owns GENERAL slot this body.
						// 1B-supersede: HOLD_TASK silenced (no setGeneralTacOrder STOP write).
					}
				}
			}
		}

		// BRAIN-OPORD-COREPATROL-1: per-tick patrol advance.
		// Gate: MC2_BRAIN_PATROL (default OFF).  No-op when gate OFF or patrolActive==false.
		// Called unconditionally (outside alreadyDone guard) so arrival is polled every tick.
		// tickPatrolAdvance handles arrival poll, cursor advance, and MOVETO_POINT re-emit.
		// moveToEffectApplied is NOT touched — patrol manages its own cursor guard.
		if (brainRuntime) {
			// BRAIN-ENGAGE-1: engagement takes priority over patrol. If a threat is in range the
			// unit attacks (and we suppress patrol advance so it fights instead of walking off);
			// otherwise patrol/idle proceeds as before. Both emit via the intent queue.
			bool engaging = tickEngageNearest(this, brainRuntime, vehicleWID);
			if (engaging) {
				dispatcherAppliedEffect = true;
			} else if (tickPatrolAdvance(this, brainRuntime, vehicleWID)) {
				// Patrol re-emitted a MOVETO_POINT — suppress HOLD so the order isn't stomped.
				dispatcherAppliedEffect = true;
			} else if (tickOpordMove(this, brainRuntime, vehicleWID)) {
				// BRAIN-OPORD-MOVE-1: Escort follow / ReturnToPost leash issued a move.
				dispatcherAppliedEffect = true;
			}
		}

		if (!dispatcherAppliedEffect) {
			// Synthetic HOLD path (1B-runtime default) — fires when dispatcher did NOT take slot.
			if (brainTaskQueue) {
				BrainTaskEntry task;
				while (brainTaskQueue->drainWithTask(task)) {
					if (task.type == BrainTaskType::BRAIN_TASK_HOLD) {
						TacticalOrder holdOrder;
						holdOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_STOP);
						setGeneralTacOrder(holdOrder);
						mc2::fmt::print_err("[BRAIN_RT] HOLD_TASK applied wid={:d}\n", mc2::fmt::as_i32(vehicleWID));
						std::fflush(stderr);
					}
					// Other task types: discard (no handler this slice).
				}
			}
		}

		// TECHSCRIPT-SPECIAL-DISPATCH trace path (1A behavior preserved when APPLY=0).
		// CALL-CHAIN-1A: pass index for TechSpecial.Call resolution.
		if (!s_dispatchApply && brainRuntime && brainRuntime->specialBody.loaded) {
			const SpecialIndex* idx = brainRuntime->specialIndex.empty()
			    ? nullptr : &brainRuntime->specialIndex;
			executeSpecialBody_TraceOnly(brainRuntime->specialBody, vehicleWID,
			                             &brainRuntime->varStore, idx, "");
		}
		brainErr = 0;
	}

	// UNCONDITIONAL housekeeping — runs for BOTH paths (preserves Legacy invariant).
	CurGroup = nullptr;
	CurObject = nullptr;
	CurObjectClass = 0;
	CurWarrior = nullptr;
	CurContact = nullptr;

	clearAlarmsHistory();

	// SAVE/LOAD-AIFREEZE-TRACE-1: gated one-line diagnostic (default OFF ->
	// byte-identical) for the save/load AI-freeze investigation. Emits the
	// post-execution decision state so a quick-save/quick-load repro of idle
	// enemies can be classified:
	//   curTacOrder.code==NONE(0) && movePath.numSteps==0 -> standing-order not
	//     re-emitted after load (pursue standing-order re-emit).
	//   movePath.numSteps>0 with no actual movement       -> pivot to
	//     PathManager / reservation state.
	// NOTE: the recon's "initCalled" field does not exist on MechWarrior;
	// brainState + moveState are the real available signals and substituted.
	// ISSUE-10 extension (scripted units never engaging / permanent stalls):
	// added fields classify every stuck warrior without a debugger —
	//   orderState        ORDERSTATE_GENERAL(0)/PLAYER(1)/ALARM(2): pinned in
	//                     ALARM = escape-tile alarm loop (general orders never
	//                     consumed while the alarm slot is held).
	//   mainGoal/goalWID  goal-plan divert target (turret/gate/wall capture-
	//                     divert that never completes).
	//   genOrder          tacOrder[ORDERSTATE_GENERAL].code: brain/FSM emitted
	//                     an order vs no-order-from-brain (ABL stall).
	//   pos               position delta over ticks = machine-checkable
	//                     "path-but-not-moving" (reservation/collision) class.
	//   lastMoveErr       LastMoveCalcErr: no-path (LR path fail / MOVE-data
	//                     area-graph misread) class.
	{
		static const bool s_aiFreezeTrace = mc2::gate::get<"MC2_BRAIN_FREEZE_TRACE">();
		if (s_aiFreezeTrace) {
			MovePathPtr mp = getMovePath();
			MoverPtr traceVehicle = getVehicle();
			glm::vec3 tracePos;
			tracePos = glm::vec3(0.0f);
			if (traceVehicle)
				tracePos = traceVehicle->getPosition();
			mc2::fmt::print_err("[BRAIN_FREEZE_TRACE] wid={:d} brainState={:d} curTacOrder.code={:d} movePath.numSteps={:d} moveState={:d} hasBrain={:d} orderState={:d} mainGoal={:d} goalWID={:d} genOrder={:d} pos=({:.0f},{:.0f}) lastMoveErr={:d}\n", mc2::fmt::as_i32(static_cast<int>(vehicleWID)), mc2::fmt::as_i32(static_cast<int>(brainState)), mc2::fmt::as_i32(static_cast<int>(curTacOrder.code)), mc2::fmt::as_i32(mp ? static_cast<int>(mp->numSteps) : -1), mc2::fmt::as_i32(moveOrders.moveState), mc2::fmt::as_i32(brain ? 1 : 0), mc2::fmt::as_i32(static_cast<int>(orderState)), mc2::fmt::as_i32(static_cast<int>(mainGoalAction)), mc2::fmt::as_u32(static_cast<unsigned long>(mainGoalObjectWID)), mc2::fmt::as_i32(static_cast<int>(tacOrder[ORDERSTATE_GENERAL].code)), static_cast<double>(tracePos.x), static_cast<double>(tracePos.y), mc2::fmt::as_i32(LastMoveCalcErr));
			std::fflush(stderr);
		}
	}

	return(brainErr);
}

// BRAIN-RUNTIME-1B: public accessor used by mission.cpp FIT loader.

void MechWarrior::setBrainRuntimeMode (BrainRuntimeMode fitMode) {
	if (!brainRuntime) {
		brainRuntime = new MechBrainRuntime();
	}
	brainRuntime->mode = fitMode;
}

long MechWarrior::mainDecisionTree (void) {

    // sebi: WTF??? path is an array of2 elements => comparison always true
	//Assert(moveOrders.path != NULL, 0, " bad warrior path ");
	Assert(moveOrders.path[0] != nullptr, 0, " bad warrior path ");

	if ((teamId == -1) || !brainsEnabled[teamId]) {
		clearCurTacOrder(false);
		tacOrder[ORDERSTATE_GENERAL].init();
		tacOrder[ORDERSTATE_PLAYER].init();
		tacOrder[ORDERSTATE_ALARM].init();
		setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
	}

	//If we currently have no order, check if we have any player orders queued...
	if (curTacOrder.code == TACTICAL_ORDER_NONE) {
		if (tacOrderQueueExecuting && (numTacOrdersQueued > 0)) {
			if (!newTacOrderReceived[ORDERSTATE_PLAYER]) {
				// First queued order just given, so begin executing it immediately...
				executeTacOrderQueue();
			}
		}
	}

	// Is the current tac order accomplished?
	if ((curTacOrder.code != TACTICAL_ORDER_NONE) && (curTacOrder.status(this) == TACORDER_SUCCESS)) {
		bool playerOrderQueued = false;
		if ((orderState == ORDERSTATE_PLAYER) && tacOrderQueueExecuting) {
			executeTacOrderQueue();
			playerOrderQueued = newTacOrderReceived[ORDERSTATE_PLAYER];
		}
		clearCurTacOrder();
		newTacOrderReceived[ORDERSTATE_PLAYER] = playerOrderQueued;
	}

	// If we're without orders now, let's record the time...
	if (underHomeCommand() && (curTacOrder.code == TACTICAL_ORDER_NONE) && (timeOfLastOrders < 0.0))
		timeOfLastOrders = scenarioTime;

	// Update Weapons Status, if needed this frame...
	if ((brainUpdate <= scenarioTime) || (combatUpdate <= scenarioTime) || (movementUpdate <= scenarioTime)) {
		GameObjectPtr target = getCurrentTarget(); //getAttackTarget(ORDER_CURRENT);
		if (target)
			weaponsStatusResult = calcWeaponsStatus(target, weaponsStatus);
		else if (curTacOrder.code == TACTICAL_ORDER_ATTACK_POINT) {
			glm::vec3 pos = getAttackTargetPoint();
			weaponsStatusResult = calcWeaponsStatus(target, weaponsStatus, &pos);
		}
	}

	// Make sure, if I have a target, it's still legit.
	// Now, when we go into runBrain, we know the current target is up-to-date with current
	// game conditions...
	GameObjectPtr tacOrderTarget = nullptr;
	if (curTacOrder.code != TACTICAL_ORDER_NONE)
		tacOrderTarget = ObjectManager->getByWatchID(curTacOrder.targetWID);
	GameObjectPtr target = getLastTarget();
	if (target) {
		bool clearTarget = false;
		if (target->isDestroyed())
			clearTarget = true;
		else if (target->isDisabled() && !lastTargetObliterate)
			clearTarget = true;
		if (clearTarget) {
			setLastTarget(nullptr);
			if ((curTacOrder.code == TACTICAL_ORDER_ATTACK_OBJECT) && (target == tacOrderTarget)) {
				clearCurTacOrder();
				target = nullptr;
				tacOrderTarget = nullptr;
			}
		}
	}
	if ((curTacOrder.code == TACTICAL_ORDER_ATTACK_OBJECT) && (target != tacOrderTarget))
		setLastTarget(tacOrderTarget);

	// Update pilot brain...
	// BRAIN-FIXED-TICK-1: advance global sim-time accumulator once per render frame,
	// before the per-warrior gate so even staggered-out warriors contribute to the advance.
	initBrainFixedTickGate();
	if (s_brainFixedTickEnabled && s_brainAccumLastFrame != g_mc2FrameCounter.load(std::memory_order_relaxed)) {
		s_brainAccumLastFrame = g_mc2FrameCounter.load(std::memory_order_relaxed);
		bool ticked = advanceBrainFixedTickAccumulator(scenarioTime);
		if (s_brainFixedTickTrace && ticked) {
			mc2::fmt::print_err("[BRAIN_FIXED_TICK] tick={:d} simTime={:.4f} ranThisFrame={:d}\n", mc2::fmt::as_u32(s_brainTickIndex), static_cast<double>(scenarioTime), mc2::fmt::as_u32(s_brainTicksThisFrame));
			std::fflush(stderr);
		}
	}
	if ((brainUpdate <= scenarioTime) && ((teamId == -1) || brainsEnabled[teamId])) {
		// MC2_BRAIN_TASKQ: drain one task per brain cadence (inert stub — no tacOrder writes this slice)
		initBrainTaskQGate();
		if (s_brainTaskQEnabled) {
			if (!brainTaskQueue) {
				brainTaskQueue = new BrainTaskQueue();
			}
			if (s_brainTaskQTraceEnabled) {
				brainTaskQueue->dumpTrace(vehicleWID);
			}
			// Task queue allocated here; consumed inside runBrain() per BRAIN-RUNTIME-1B.
		}

		// MC2_BRAIN_RUNTIME: init struct and set mode from global forced mode (per-unit FIT override applied at mission load).
		initBrainRuntimeGate();
		if (s_brainRuntimeEnabled && s_brainTaskQEnabled) {
			if (!brainRuntime) {
				brainRuntime = new MechBrainRuntime();
				// Set mode: per-unit FIT loader (mission_ai.fit) may have already set brainRuntime->mode
				// at mission load via loadBrainRuntimeFromFit(). If not yet created, use global forced mode.
				brainRuntime->mode = s_brainRuntimeForcedMode;
				// GAP-A MULTI-WARRIOR SPECIALS: this runtime was allocated lazily — AFTER the
				// mission.cpp specials-load loop — so its specialBody is empty. Copy the parsed
				// body from the mission-level cache so this warrior dispatches like the
				// _ai.fit-recorded ones. No-op when the cache is empty (gate-OFF / no file).
				applyCachedSpecialBodyToRuntime(brainRuntime);
			} else {
				// GAP-A-ROBUSTNESS-1: a brainRuntime can be eager-allocated BEFORE this point by
				// the [Tactics] loader (mission.cpp setBrainRuntimeMode(Legacy), so tactic weights
				// can attach). That pre-empts the lazy-alloc branch above — so without this, the
				// mission-level specials cache is never applied AND the runtime stays Legacy →
				// enhancedApply==false → no Enhanced dispatch (observed: TACTIC_WEIGHTS=1 collapses
				// multi-warrior patrol 70→1). When a forced mode is set (FORCE_MODE != Legacy),
				// honor it over the eager Legacy default; then apply the cached specials. Both are
				// no-ops if already satisfied (idempotent — safe to run every tick).
				if (s_brainRuntimeForcedMode != BrainRuntimeMode::Legacy &&
				    brainRuntime->mode != s_brainRuntimeForcedMode) {
					brainRuntime->mode = s_brainRuntimeForcedMode;
				}
				applyCachedSpecialBodyToRuntime(brainRuntime);
			}
		}

		// BRAIN-RUNTIME-1B seam: short-circuit logic lives INSIDE runBrain().
		// APPLY=0: trace-log the would-own mask (1A behavior, byte-identical to baseline).
		// APPLY=1+Enhanced: runBrain() skips brain->execute(), pushes/drains HOLD_TASK.
		// All paths: CurGroup/CurWarrior clear + clearAlarmsHistory() run inside runBrain().
		if (s_brainRuntimeEnabled && s_brainTaskQEnabled && brainRuntime) {
			if (s_brainRuntimeTraceEnabled && !s_brainRuntimeApplyEnabled) {
				uint8_t wouldOwn = brainRuntime->computeWouldOwnMask();
				// Build human-readable slot list
				char slotsBuf[32] = "(none)";
				if (wouldOwn) {
					slotsBuf[0] = '\0';
					if (wouldOwn & kBrainOwnsGeneral) { if (slotsBuf[0]) std::strcat(slotsBuf, "|"); std::strcat(slotsBuf, "GENERAL"); }
					if (wouldOwn & kBrainOwnsPlayer)  { if (slotsBuf[0]) std::strcat(slotsBuf, "|"); std::strcat(slotsBuf, "PLAYER");  }
					if (wouldOwn & kBrainOwnsAlarm)   { if (slotsBuf[0]) std::strcat(slotsBuf, "|"); std::strcat(slotsBuf, "ALARM");   }
				}
				mc2::fmt::print_err("[BRAIN_RT] wid={:d} mode={} would-own {} (NOT applied)\n", mc2::fmt::as_i32(vehicleWID), mc2::fmt::safe(MechBrainRuntime::modeString(brainRuntime->mode)), mc2::fmt::safe(slotsBuf));
			}
		}
		// runBrain() always called — short-circuit is gated inside it.
		runBrain();
		// BRAIN-FIXED-TICK-1: advance brainUpdate by fixed tick period (gate ON)
		// or legacy BrainUpdateFrequency (gate OFF — byte-identical to baseline).
		if (s_brainFixedTickEnabled) {
			brainUpdate += s_brainFixedTickPeriod;
		} else {
			brainUpdate += BrainUpdateFrequency;
		}
	}

	// In case the brain set a new target, let's recalc weaponsStatus...
	target = getLastTarget(); //getAttackTarget(ORDER_CURRENT);
	if (target && (lastTargetTime/*getLastTargetTime()*/ == scenarioTime))
		weaponsStatusResult = calcWeaponsStatus(target, weaponsStatus);

	// Any Alarms?
	checkAlarms();

	// Now, check if we have a new current tac order...
	TacticalOrder newTacOrder;
	switch (orderState) {
		case ORDERSTATE_GENERAL:
			if (newTacOrderReceived[ORDERSTATE_ALARM]) {
                suspendAuthoredRoute(this);
				clearCurTacOrder(false);
				newTacOrder = tacOrder[ORDERSTATE_ALARM];
				orderState = ORDERSTATE_ALARM;
				}
			else if (newTacOrderReceived[ORDERSTATE_PLAYER]) {
				// If we're currently executing a general (brain) order,
				// purge it permanently (NOW, we DON'T want them to
				// fall back to the general order--the brain must re-order
				// it)...
				tacOrder[ORDERSTATE_GENERAL].init();
				// Now grab the player order...
				newTacOrder = tacOrder[ORDERSTATE_PLAYER];
				orderState = ORDERSTATE_PLAYER;
				}
			else if (newTacOrderReceived[ORDERSTATE_GENERAL])
				newTacOrder = tacOrder[ORDERSTATE_GENERAL];
			break;
		case ORDERSTATE_PLAYER:
			if (newTacOrderReceived[ORDERSTATE_ALARM]) {
				newTacOrder = tacOrder[ORDERSTATE_ALARM];
				orderState = ORDERSTATE_ALARM;
				}
			else if (newTacOrderReceived[ORDERSTATE_PLAYER]) {
				// Make sure we have no general order we fall back to when
				// we finish this, per Tim's request...
				tacOrder[ORDERSTATE_GENERAL].init();
				newTacOrder = tacOrder[ORDERSTATE_PLAYER];
			}
			break;
		case ORDERSTATE_ALARM:
			// If the player issues a new order, let it override any current
			// alarm (self-issued) order...
			if (newTacOrderReceived[ORDERSTATE_PLAYER]) {
				alarmPriority = 0;
				tacOrder[ORDERSTATE_ALARM].init();
				newTacOrder = tacOrder[ORDERSTATE_PLAYER];
				}
			else if (newTacOrderReceived[ORDERSTATE_ALARM])
				newTacOrder = tacOrder[ORDERSTATE_ALARM];
			break;
	}
	if (newTacOrder.code != TACTICAL_ORDER_NONE) {
		// Note how we need to copy the move path over, since we
		// allocate it only once now...
		MovePathPtr tempPath[2];
		if ((newTacOrder.code == TACTICAL_ORDER_MOVETO_POINT) || (newTacOrder.code == TACTICAL_ORDER_MOVETO_OBJECT)) {
			for (long i = 0; i < 2; i++) {
				tempPath[i] = moveOrders.path[i];
				if (i > 0)
					tempPath[i]->numSteps = 0;
			}
			}
		else {
			for (long i = 0; i < 2; i++) {
				tempPath[i] = moveOrders.path[i];
				tempPath[i]->numSteps = 0;
			}
		}
		long tempMoveState = moveOrders.moveState;
		long tempMoveStateGoal = moveOrders.moveStateGoal;
		bool running = moveOrders.run;
		moveOrders.init();
		moveOrders.run = running;
		PathManager->remove(this);
		moveOrders.moveState = tempMoveState;
		moveOrders.moveStateGoal = tempMoveStateGoal;
		moveOrders.path[0] = tempPath[0];
		moveOrders.path[1] = tempPath[1];
		attackOrders.init();
		long message = -1;
		newTacOrder.execute(this, message);
		if (orderState == ORDERSTATE_PLAYER)
			radioMessage(message, true);
		setCurTacOrder(newTacOrder);
		if (useGoalPlan) {
			glm::vec3 movePoint = newTacOrder.getWayPoint(0);
			if (!newTacOrder.subOrder)
				switch (newTacOrder.code) {
					case TACTICAL_ORDER_MOVETO_POINT:
						setMainGoal(GOAL_ACTION_MOVE, nullptr, &movePoint, -1.0);
						break;
					case TACTICAL_ORDER_ATTACK_OBJECT:
						setMainGoal(GOAL_ACTION_ATTACK, newTacOrder.getTarget(), nullptr, -1.0);
						break;
					case TACTICAL_ORDER_CAPTURE:
						setMainGoal(GOAL_ACTION_CAPTURE, newTacOrder.getTarget(), nullptr, 400.0);
						break;
					default:
						setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
				}
		}
		timeOfLastOrders = -1.0;
	}

	// These must be cleared every frame...
	newTacOrderReceived[ORDERSTATE_GENERAL] = false;
	newTacOrderReceived[ORDERSTATE_PLAYER] = false;
	newTacOrderReceived[ORDERSTATE_ALARM] = false;

	// Now that we know what we're doing, do it...
	updateActions();

	return(NO_ERR);
}

long MechWarrior::loadBrainParameters (FitIniFile* brainFile, long warriorId) {

	if (!brain)
		Fatal(0, " Warrior.loadBrainParameters: NULL brain ");

	// Main Info section
	char warriorName[32];
	mc2::fmt::to_buf(warriorName, "Warrior{:d}", mc2::fmt::as_i32(warriorId));
	long result = brainFile->seekBlock(warriorName);
	if (result != NO_ERR)
		return(result);

	long numCells = 0;
	result = brainFile->readIdLong("NumCells", numCells);
	if (result != NO_ERR)
		return(result);

	long numStaticVars = 0;
	result = brainFile->readIdLong("NumStaticVars", numStaticVars);
	if (result != NO_ERR)
		return(result);

	for (long i = 0; i < numCells; i++) {
		char blockName[64];
		mc2::fmt::to_buf(blockName, "{}Cell{:d}", mc2::fmt::safe(warriorName), mc2::fmt::as_i32(i));
		long result = brainFile->seekBlock(blockName);
		if (result != NO_ERR)
			return(result);
		long cell = 0;
		result = brainFile->readIdLong("Cell", cell);
		if (result != NO_ERR)
			return(result);
		long memType = 0;
		result = brainFile->readIdLong("MemType", memType);
		if (result != NO_ERR)
			return(result);
		switch (memType) {
			case 0: {
				// Integer Memory
				long value = 0;
				result = brainFile->readIdLong("Value", value);
				if (result != NO_ERR)
					return(result);
				setIntegerMemory(cell, value);
				}
				break;
			case 1: {
				// Real Memory
				float value = 0.0;
				result = brainFile->readIdFloat("Value", value);
				if (result != NO_ERR)
					return(result);
				setRealMemory(cell, value);
				}
				break;
			default:
				result = 666;
				return(result);
		}
	}

	for (int i = 0; i < numStaticVars; i++) {
		char blockName[64];
		mc2::fmt::to_buf(blockName, "{}Static{:d}", mc2::fmt::safe(warriorName), mc2::fmt::as_i32(i));
		long result = brainFile->seekBlock(blockName);
		if (result != NO_ERR)
			return(result);
		long type = 0;
		result = brainFile->readIdLong("type", type);
		if (result != NO_ERR)
			return(result);
		char varName[256];
		result = brainFile->readIdString("Name", varName, 255);
		if (result != NO_ERR)
			return(result);
		switch (type) {
			case 0: {
				// Integer
				long value = 0;
				result = brainFile->readIdLong("Value", value);
				if (result != NO_ERR)
					return(result);

				brain->setStaticInteger(varName, value);
				}
				break;
			case 1: {
				// Real
				float value = 0;
				result = brainFile->readIdFloat("Value", value);
				if (result != NO_ERR)
					return(result);

				brain->setStaticReal(varName, value);
				}
				break;
			case 2: {
				// Integer Array
				long numValues = 0;
				result = brainFile->readIdLong("NumValues", numValues);
				if (result != NO_ERR)
					return(result);

				int values[1024];
				result = brainFile->readIdIntArray("Values", values, numValues);
				if (result != NO_ERR)
					return(result);

				brain->setStaticIntegerArray(varName, numValues, values);
				}
				break;
			case 3: {
				// Real Array
				long numValues = 0;
				result = brainFile->readIdLong("NumValues", numValues);
				if (result != NO_ERR)
					return(result);

				float values[1024];
				result = brainFile->readIdFloatArray("Values", values, numValues);
				if (result != NO_ERR)
					return(result);

				brain->setStaticRealArray(varName, numValues, values);
				}
				break;
			default:
				result = 667;
				return(result);
		}
	}

	return(NO_ERR);
}
