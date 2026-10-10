// Mission: the per-frame Mission::update with its MC2_MISSION_SPLIT timing, mover bridge, soak hooks and coherence audit.
// Declared in the class headers; shared names in mission_internal.h.
#include "mission_internal.h"


// FREEZE-MOVERS-1: ONE predicate for "the simulation tick is suspended".
//
// Every site below was previously written `!missionInterface->isPaused()`. If
// the freeze only widened the ObjectManager branch and left these alone, the
// sim would be half-frozen, which is worse than either extreme:
//   * mcTextureManager->update() -- its own comment says it must not run while
//     paused because it "uncaches things which only objectManager->update can
//     cache back in", and under a freeze that call is exactly what is skipped.
//     Half-freezing would uncache textures with nothing to restore them, i.e.
//     manufacture missing/black texture artifacts -- the very defect class this
//     harness exists to REPRODUCE, which would make it a confound.
//   * SensorManager::update / updateCollisions -- keep mutating contact and
//     drawFlags state, so two "frozen" arms could still render differently.
//   * the mission ABL brain -- keeps issuing orders and rewriting mission state.
//   * scenarioTime -- drives gosFX ExecuteInfo, so effects keep animating.
// Returns false unless the verb / MC2_FREEZE_MOVERS_AT armed it, so the stock
// path is byte-identical.
static inline bool mc2SimTickFrozen(MissionInterfaceManager* mim)
{
	return (mim && mim->isPaused()) || (mc2FreezeMoversActive() != 0);
}

static void simClockTraceTick(float scenarioTimeNow)
{
	static const bool s_traceEnabled = mc2::gate::get<"MC2_SIM_CLOCK_TRACE">();
	if (!s_traceEnabled)
		return;

	static bool s_seeded = false;
	static uint64_t s_wallStartMs = 0;
	static float s_scenarioStart = 0.0f;
	static uint64_t s_frameCount = 0;
	static uint64_t s_lastEmitWallMs = 0;

	uint64_t nowMs = (uint64_t)timeGetTime();
	if (!s_seeded)
	{
		s_wallStartMs = nowMs;
		s_scenarioStart = scenarioTimeNow;
		s_lastEmitWallMs = nowMs;
		s_seeded = true;
	}
	++s_frameCount;

	if (nowMs - s_lastEmitWallMs >= 1000)
	{
		double wallElapsedSec = (double)(nowMs - s_wallStartMs) / 1000.0;
		double simElapsedSec = (double)(scenarioTimeNow - s_scenarioStart);
		double ratio = (wallElapsedSec > 0.0) ? (simElapsedSec / wallElapsedSec) : 0.0;
		fprintf(stderr,
			"[SIM-CLOCK-TRACE] wall=%.3fs sim=%.3fs ratio(sim/wall)=%.3f frames=%llu frameLength=%.5f\n",
			wallElapsedSec, simElapsedSec, ratio,
			(unsigned long long)s_frameCount, (double)frameLength);
		fflush(stderr);
		s_lastEmitWallMs = nowMs;
	}
}
namespace {
	static const bool s_misSplit = mc2::gate::get<"MC2_MISSION_SPLIT">();
	enum { MS_LAND_UPDATE=0, MS_PATHMGR, MS_CLEAR_BLOCKS, MS_CLEAR_VERTS,
	       MS_TERRAIN_TEX, MS_GEOMETRY, MS_OBJMGR,
	       MS_TOTAL, MS_CLEARARRAYS, MS_INTERFACE, MS_CAMERA, MS_VTOL, MS_WEATHER, MS_WAYPOINTS,
	       MS_COUNT };
	static const char* s_misNames[MS_COUNT] = {
		"land_update","pathmgr","clearBlocks","clearVerts","terrainTex","geometry","objmgr",
		"TOTAL","clearArrays","interface","camera","vtol","weather","waypoints" };
	static unsigned long long s_misNs[MS_COUNT]  = {0};
	static unsigned long long s_misMax[MS_COUNT] = {0};
	static unsigned long long s_misFrames = 0;
	static bool s_misAtexit = false;
	static void misEmit() {
		if (!s_misSplit) return;
		std::printf("[MISSION_SPLIT v1] event=shutdown frames=%llu", s_misFrames);
		for (int i = 0; i < MS_COUNT; i++)
			std::printf(" %s={avg_us:%.1f,max_us:%.1f}", s_misNames[i],
				s_misFrames ? (double)s_misNs[i]/s_misFrames/1000.0 : 0.0,
				(double)s_misMax[i]/1000.0);
		std::printf("\n"); std::fflush(stdout);
	}
	struct MisScope {
		int idx; std::chrono::steady_clock::time_point t0;
		explicit MisScope(int i) : idx(i) {
			if (s_misSplit) t0 = std::chrono::steady_clock::now();
		}
		~MisScope() {
			if (!s_misSplit) return;
			unsigned long long ns = (unsigned long long)
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - t0).count();
			s_misNs[idx] += ns;
			if (ns > s_misMax[idx]) s_misMax[idx] = ns;
		}
	};
}

//----------------------------------------------------------------------------------
// MC2_BRIDGE_MOVER_STATE -- runtime-bridge v0 (mover.state over stdout).
//
// When env MC2_BRIDGE_MOVER_STATE=1, emit one [MOVER v2] line per live mover
// every MC2_BRIDGE_MOVER_PERIOD_SEC seconds (default 2). Strictly READ-ONLY:
// only getters, every deref null-guarded. Zero cost when the env is unset
// (single getenv at startup; the per-frame check is one bool + one float
// compare). Consumed by the editor's GameplayDebugger live table; see
// docs/superpowers/strategy/runtime-bridge-architecture.md Phase 3 sec 11.
//----------------------------------------------------------------------------------
namespace {
	static const bool  s_bridgeMoverState = mc2::gate::get<"MC2_BRIDGE_MOVER_STATE">();
	static float s_bridgeNextEmitTime = 0.0f;   // scenarioTime of next burst
	static float bridgeMoverPeriodSec() {
		return mc2::gate::get<"MC2_BRIDGE_MOVER_PERIOD_SEC">();   // the row clamps to >= 0.25
	}

	static const char* bridgeWarriorStatusName(long s) {
		switch (s) {
			case WARRIOR_STATUS_NORMAL:      return "normal";
			case WARRIOR_STATUS_WITHDRAWING: return "withdrawing";
			case WARRIOR_STATUS_WITHDRAWN:   return "withdrawn";
			case WARRIOR_STATUS_EJECTED:     return "ejected";
			case WARRIOR_STATUS_DEAD:        return "dead";
			case WARRIOR_STATUS_MIA:         return "mia";
			case WARRIOR_STATUS_CAPTURED:    return "captured";
			case WARRIOR_STATUS_BASECAMP:    return "basecamp";
			default:                         return "unk";
		}
	}

	// Copy src into dst (cap chars) replacing spaces/'='/control with '_' so the
	// key=value [MOVER v2] line stays parseable even for names with spaces. Always
	// NUL-terminates; emits "?" for null/empty input.
	static void bridgeSanitize(char* dst, size_t cap, const char* src) {
		if (cap == 0) return;
		if (!src || !src[0]) { strncpy(dst, "?", cap - 1); dst[cap - 1] = 0; return; }
		size_t j = 0;
		for (size_t i = 0; src[i] && j + 1 < cap; ++i) {
			char c = src[i];
			if (c == ' ' || c == '=' || c == '\t' || c == '\r' || c == '\n')
				c = '_';
			dst[j++] = c;
		}
		dst[j] = 0;
	}

	// Emit [MOVER v2] lines for up to kCap live movers. Read-only; defensive.
	static void bridgeEmitMoverState(float scenarioTimeNow) {
		if (!s_bridgeMoverState || !ObjectManager)
			return;
		if (scenarioTimeNow < s_bridgeNextEmitTime)
			return;
		s_bridgeNextEmitTime = scenarioTimeNow + bridgeMoverPeriodSec();

		const long kCap = 64;
		long total = ObjectManager->getNumMovers();
		long emitted = 0;
		printf("[MOVER v2 begin] t=%.1f count=%ld\n", scenarioTimeNow, total);
		for (long i = 0; i < total && emitted < kCap; ++i) {
			MoverPtr mover = ObjectManager->getMover(i);
			if (!mover)
				continue;

			long              partId = mover->getPartId();
			char              name[64];
			bridgeSanitize(name, sizeof(name), mover->getName());
			long              team   = mover->getTeamId();
			Stuff::Vector3D   pos    = mover->getPosition();

			// HP as a 0..1 damage-derived fraction (getDamageLevel is the only
			// HP signal reachable without poking per-location body arrays).
			float dmg = mover->getDamageLevel();
			float hpFrac = 1.0f - dmg;
			if (hpFrac < 0.0f) hpFrac = 0.0f;
			if (hpFrac > 1.0f) hpFrac = 1.0f;

			char              pilotName[64];
			bridgeSanitize(pilotName, sizeof(pilotName), 0);   // default "?"
			long              orderVal  = -1;
			const char*       orderName = "none";
			long              targetId  = -1;

			// v2 brain/path/target detail. Defaults are emitted even when the
			// pilot or path is absent so the [MOVER v2] grammar stays fixed-arity
			// and the editor parser never has to handle optional fields.
			long              brainState = -1;   // ABL brain FSM state index (opaque)
			long              moveState  = -1;   // moveOrders.moveState
			Stuff::Vector3D   pgoal;             pgoal.Zero();   // world goal of active path
			long              pCurStep   = -1;   // step index being headed for
			long              pNumSteps  = 0;    // total steps in active path
			long              pCost      = -1;   // A* total path cost
			Stuff::Vector3D   tgtPos;            tgtPos.Zero();  // current target world pos

			MechWarriorPtr    pilot     = mover->getPilot();
			if (pilot) {
				bridgeSanitize(pilotName, sizeof(pilotName), pilot->getName());
				orderVal  = pilot->getStatus();
				orderName = bridgeWarriorStatusName(orderVal);
				brainState = pilot->getBrainState();
				moveState  = pilot->getMoveState();
				// getMovePath(long) is a PURE accessor (returns moveOrders.path[which]);
				// the zero-arg getMovePath() MUTATES (swaps path slots) and is never
				// called here. Null-guarded defensively though the path pair is
				// normally pre-allocated.
				MovePathPtr mp = pilot->getMovePath(0);
				if (mp) {
					pgoal     = mp->goal;
					pCurStep  = mp->curStep;
					pNumSteps = mp->numStepsWhenNotPaused;
					pCost     = mp->cost;
				}
				GameObjectPtr tgt = pilot->getCurrentTarget();
				if (tgt) {
					targetId = tgt->getPartId();
					tgtPos   = tgt->getPosition();
				}
			}

			printf("[MOVER v2] id=%ld name=%s team=%ld pos=%.1f,%.1f,%.1f "
				"hp=%.2f pilot=%s order=%ld/%s target=%ld "
				"brain=%ld mstate=%ld pgoal=%.1f,%.1f,%.1f pstep=%ld/%ld pcost=%ld "
				"tgtpos=%.1f,%.1f,%.1f\n",
				partId, name, team, pos.x, pos.y, pos.z,
				hpFrac, pilotName, orderVal, orderName, targetId,
				brainState, moveState, pgoal.x, pgoal.y, pgoal.z,
				pCurStep, pNumSteps, pCost,
				tgtPos.x, tgtPos.y, tgtPos.z);
			++emitted;
		}
		fflush(stdout);
	}

	// --- CRASH-SOAK harness (MC2_SOAK_AUTOWIN) -------------------------------
	// Auto-win the running mission after MC2_SOAK_WIN_AFTER_SEC of scenario
	// time so a campaign booted via MC2_BOOT_TO_BAY walks itself end-to-end
	// unattended. Gated entirely on the env flag; default OFF = byte-identical.
	static const bool s_soakAutoWin = mc2::gate::get<"MC2_SOAK_AUTOWIN">();
	static float soakWinAfterSec() {
		return mc2::gate::get<"MC2_SOAK_WIN_AFTER_SEC">();   // the row clamps to >= 0
	}

	// --- MC2_SOAK_KILL_ENEMY -------------------------------------------------
	// When set, kills one random live enemy BattleMech before autowin fires so
	// the salvage screen has a candidate, and then programmatically salvages it
	// into LogisticsData inventory.  Default OFF = byte-identical when unset.
	static const bool s_soakKillEnemy = mc2::gate::get<"MC2_SOAK_KILL_ENEMY">();

	// Stores the variantName of the enemy killed this mission so the next
	// mission can assert it reached the load-bay inventory.
	static char s_soakKilledVariant[64] = {};
	static bool s_soakKillDone = false;      // armed once per mission
	static bool s_soakSalvageDone = false;   // salvage commit fired once per mission
	static bool s_soakInventoryChecked = false; // load-bay check fired once per mission

	// --- MC2_SOAK_SALVAGE_TRADEMOVER (ISSUE-42a repro fixture) ----------------
	// Requires MC2_SOAK_KILL_ENEMY + MC2_SOAK_AUTOWIN. Instead of the Phase-2
	// addMechToInventory shortcut, exercise the REAL mid-mission salvage-craft
	// flow (missiongui.cpp doMove salvage: recover -> setStatus(SHUTDOWN,force)
	// -> tradeMover -> orderPowerUp) on ONE disabled enemy mech before autowin,
	// so LogisticsData::setMissionCompleted's recovered-mech carry-over branch
	// is what the Phase-3 load-bay check verifies. Default OFF = byte-identical.
	static const bool s_soakSalvageTrademover = mc2::gate::get<"MC2_SOAK_SALVAGE_TRADEMOVER">();
	static bool s_soakTradeDone = false;     // armed once per mission

	// --- MC2_SOAK_PILOT_PROMOTE ----------------------------------------------
	// Grants a deployed pilot enough skill delta to cross the next rank
	// threshold, then runs the same promotePilot() call the UI uses.
	// Throttled to mission-index 2 and 5 within a campaign (1-based).
	// Default OFF = byte-identical when unset.
	static const bool s_soakPilotPromote = mc2::gate::get<"MC2_SOAK_PILOT_PROMOTE">();
	static bool s_soakPromoteFired = false;  // armed once per mission

	// --- MC2_FIXTURE_ORDER_LIVENESS (META-FIX design F) -----------------------
	// Behavioral order-liveness fixture: proves issued orders DO something
	// (issues #17 reluctant fire / #10 scripted stalls class). Requires
	// MC2_SOAK_AUTOWIN. Phases (fractions of the autowin timer, trademover
	// block shape): 0.2 = COMMANDER-origin unaimed ATTACK_TO_DESTROY per armed
	// home mech at the nearest live enemy (PLAYER origin never installs the
	// tac order — warrior.cpp orderAttackObject/setGeneralTacOrder trap);
	// 0.5 = COMMANDER-origin move order per home mech (300 m toward map
	// center — the move order replaces the attack order, giving the move leg
	// a clean 0.4*win displacement window; the REVIEWED order (move 0.2,
	// attack 0.3) self-sabotaged the move bound: a mech whose target spawns
	// in range stands and fires without ever covering 25 m — measured 1/11
	// stalled on stock mc2_24); 0.9 = one self-asserting summary line
	//   ORDER_LIVENESS_SUMMARY issued_move=N moved=N refused=N stalled=N verdict=PASS|FAIL
	// (moved = net displacement >= 25 m; refused = classified refusal via
	// LastMoveCalcErr; dead/disabled units exempt). [ORDER_FIXTURE] lines mark
	// each issued order. run_smoke.py hard-fails a missing summary or
	// verdict=FAIL ONLY when MC2_FIXTURE_ORDER_LIVENESS is set in the runner
	// env (VEG-SMOKE-FLOOR activation pattern); consumed by
	// scripts/check-order-liveness.py. Default OFF = byte-identical.
	static const bool s_fixtureOrderLiveness =
		mc2::gate::get<"MC2_FIXTURE_ORDER_LIVENESS">();
	struct OrderLivenessEntry {
		unsigned long   wid;
		Stuff::Vector3D startPos;
	};
	static const int kOrderLivenessCap = 32;   // bounded, s_firedWIDs precedent
	static OrderLivenessEntry s_olEntries[kOrderLivenessCap];
	static int  s_olIssuedMove   = 0;
	static bool s_olMoveIssued   = false;
	static bool s_olAttackIssued = false;
	static bool s_olSummaryDone  = false;

	// --- MC2_MOVER_COHERENCE_AUDIT (META-FIX design A) ------------------------
	// ~1 Hz read-only audit that the five per-mover registries (render gate
	// isOnGUI, force-bar roster, selection, tacmap blip, commandability) agree
	// as a pure function of mover state, for HOME-COMMANDER movers in SP.
	// Rules (contract: .claude/META-FIX-DESIGNS-2026-07-16.json CLASS A):
	//   I1  onGUI && exists && !destroyed && !disabled => roster icon present
	//   I1b ...unless the force bar is full (MAX_ICONS=16; addMech silently
	//       drops the 17th in release) — ADVISORY, not a violation.
	//   I2  !onGUI => no roster icon AND no PLAYER-origin tac order. A
	//       SELF/COMMANDER-origin order on an off-GUI mover is ADVISORY (I2b):
	//       auto-powerup (warrior.cpp handlePlayerOrder) and brain orders are
	//       legitimate non-player writers. The #50 signature (invisible mover
	//       drag-selected + player-ordered) violates this row.
	//   I3  isDisabled && !isDestroyed && exists => getAppearance()!=NULL
	//       (#42b-ADJACENT teardown-while-salvageable; the HISTORICAL #42b
	//       guard-shape bug is static rule S4's job, invisible to runtime).
	//   I4  every force-bar icon's unit is ObjectManager-registered (identity
	//       scan; read-only — getWatchID(true) would ASSIGN an id, so the
	//       registration round-trip is checked by pointer membership instead).
	//   I5  getSelected() => onGUI (selection-gate drift; lets static rule S3
	//       stay trimmed to three hard pins).
	// Violations emit one deduped stdout line (mirrored to the diag sink by
	// mc2AuditLine): "[MOVER_COHERENCE] VIOLATION rule=..." / "ADVISORY".
	// Dedup is per (mover,rule) for the process lifetime (mc2AuditOnce, cap 64
	// keys — one mission per process in smoke practice). A process-exit
	// summary line reports {checked,violations,advisories,audits}.
	//
	// OFF-MAP STAGING CARVE-OUT (INVIS-VEHICLES-FIX-1, 2026-07-16): a mover
	// AUTHORED at an off-map staging position with onGUI=1, brains alive,
	// waiting for a mission flag (mc2_10's convoy, census-verified: 12 movers
	// at x~-7500 exists=1 awake=1 onGUI=1 alpha=255, later driven on-map by
	// the ABL trigger) is a LEGITIMATE mover state. Position outside map
	// bounds is NOT a registry violation, so this audit deliberately keys on
	// NO positional term — static rule S6 (check-mover-registry-coherence.sh)
	// pins the audit region below position-free. The #50 LINKUP-PENDING row
	// is keyed on isOnGUI==0, which staging does NOT use (staging spawns
	// onGUI=1 like every non-destroyed part, mission.cpp part-spawn writes).
	// mc2_10's convoy is additionally commander 2 (allied), outside this
	// audit's home-commander scope — but the position-free contract holds
	// even for a hypothetical home-commander staged mover.
	//
	// MOVER-COHERENCE-AUDIT-BEGIN (S6 scan region: position-free by contract —
	// scripts/check-mover-registry-coherence.sh forbids positional terms in
	// this whole block; off-map staging is a legitimate mover state.)
	static const bool s_moverCoherenceAudit = mc2::gate::get<"MC2_MOVER_COHERENCE_AUDIT">();
	static float s_cohNextAuditTime = 0.0f;
	static long  s_cohChecked    = 0;   // mover-rule evaluations (cumulative)
	static long  s_cohViolations = 0;
	static long  s_cohAdvisories = 0;
	static long  s_cohAudits     = 0;   // audit ticks
	static bool  s_cohAtexit     = false;

	static void cohEmitSummary() {
		mc2AuditLinef("[MOVER_COHERENCE] summary checked=%ld violations=%ld "
			"advisories=%ld audits=%ld",
			s_cohChecked, s_cohViolations, s_cohAdvisories, s_cohAudits);
	}

	// rule ids for the dedup key (per mover partId).
	enum CohRule { COH_I1 = 1, COH_I1B, COH_I2_ICON, COH_I2_ORDER,
	               COH_I2B_ORDER, COH_I3, COH_I4, COH_I5 };

	static void cohEmit(bool violation, const char* rule, long partId,
	                    const char* name, CohRule ruleId, const char* detail) {
		unsigned long long key =
			(((unsigned long long)(unsigned long)partId) << 8) |
			(unsigned long long)ruleId;
		if (violation) s_cohViolations++; else s_cohAdvisories++;
		if (!mc2AuditOnce("MOVER_COHERENCE", key))
			return;
		mc2AuditLinef("[MOVER_COHERENCE] %s rule=%s part=%ld name=%s %s",
			violation ? "VIOLATION" : "ADVISORY", rule, partId,
			(name && name[0]) ? name : "?", detail ? detail : "");
	}

	static void auditMoverCoherence(float scenarioTimeNow,
	                                MissionInterfaceManagerPtr mi) {
		if (!ObjectManager || !mi || MPlayer || !Commander::home)
			return;   // SP home-commander scope only (MPlayer rosters unaudited)
		if (scenarioTimeNow < s_cohNextAuditTime)
			return;
		s_cohNextAuditTime = scenarioTimeNow + 1.0f;   // ~1 Hz
		s_cohAudits++;
		if (!s_cohAtexit) { s_cohAtexit = true; std::atexit(cohEmitSummary); }

		const long homeCmdr = Commander::home->getId();
		const long numMovers = ObjectManager->getNumMovers();

		for (long i = 0; i < numMovers; i++) {
			MoverPtr mover = ObjectManager->getMover(i);
			if (!mover || !mover->getExists())
				continue;
			if (mover->getCommanderId() != homeCmdr)
				continue;
			s_cohChecked++;

			const long  partId    = mover->getPartId();
			const char* name      = mover->getName();
			const bool  onGui     = mover->isOnGUI();
			const bool  destroyed = mover->isDestroyed();
			const bool  disabled  = mover->isDisabled();
			const bool  hasIcon   = mi->rosterHasMover(mover);

			// I1 / I1b — live onGUI mover must hold a force-bar icon.
			if (onGui && !destroyed && !disabled && !hasIcon) {
				if (mi->rosterIconCount() >= MAX_ICONS)
					cohEmit(false, "I1b", partId, name, COH_I1B,
						"detail=bar_full");
				else
					cohEmit(true, "I1", partId, name, COH_I1,
						"detail=icon_missing");
			}

			// I2 — off-GUI mover must be roster-absent and free of
			// PLAYER-origin orders (LINKUP-PENDING row, #50 class).
			if (!onGui) {
				if (hasIcon)
					cohEmit(true, "I2", partId, name, COH_I2_ICON,
						"detail=icon_present");
				MechWarriorPtr pilot = mover->getPilot();
				if (pilot) {
					TacticalOrderPtr order = pilot->getCurTacOrder();
					if (order && order->code != TACTICAL_ORDER_NONE) {
						char det[96];
						sprintf(det, "detail=tac_order code=%d origin=%d",
							(int)order->code, (int)order->origin);
						if (order->origin == ORDER_ORIGIN_PLAYER)
							cohEmit(true, "I2", partId, name,
								COH_I2_ORDER, det);
						else
							cohEmit(false, "I2b", partId, name,
								COH_I2B_ORDER, det);
					}
				}
			}

			// I3 — salvageable (disabled-not-destroyed) mover must keep its
			// appearance (#42b-adjacent teardown class).
			if (disabled && !destroyed && !mover->getAppearance())
				cohEmit(true, "I3", partId, name, COH_I3,
					"detail=appearance_null");

			// I5 — selected implies onGUI (selection-gate drift, #50 class).
			if (mover->getSelected() && !onGui)
				cohEmit(true, "I5", partId, name, COH_I5,
					"detail=selected_offgui");
		}

		// I4 — every force-bar icon's unit round-trips through ObjectManager
		// registration (identity membership; destroyed movers legitimately
		// keep icons through the death animation — registration only).
		const int iconCount = mi->rosterIconCount();
		for (int k = 0; k < iconCount; k++) {
			Mover* unit = mi->rosterIconMover(k);
			if (!unit) continue;
			bool registered = false;
			for (long i = 0; i < numMovers; i++) {
				if (ObjectManager->getMover(i) == unit) { registered = true; break; }
			}
			if (!registered)
				cohEmit(true, "I4", unit->getPartId(), unit->getName(),
					COH_I4, "detail=icon_unit_unregistered");
		}
	}
	// MOVER-COHERENCE-AUDIT-END
} // namespace

long Mission::update (void)
{
	ZoneScopedN("Mission::update");
	if (s_misSplit) {
		if (!s_misAtexit) { s_misAtexit = true; std::atexit(misEmit); }
		++s_misFrames;
	}
	MisScope _msTotal(MS_TOTAL);  // whole Mission::update; TOTAL - sum(subcalls) = unbracketed
	if (active)
	{
		turn++;

		memset(ObjectManager->moverLineOfSightTable, -1, ObjectManager->maxMovers*ObjectManager->maxMovers);

#ifdef LAB_ONLY
		MCTimeLOSCalc = 0;
		MCTimeAnimationCalc = 0;
#endif

		if (forcedFrameRate != -1.0f)
			frameLength /= forcedFrameRate;

		if ((missionLineChanged + 50) < turn)
		{
			#ifndef FINAL
			if (userInput->getKeyDown(KEY_X) && userInput->ctrl() && !userInput->alt() && !userInput->shift())
			{
				useCollisions ^= true;
				missionLineChanged = turn;
			}		
		
			if (userInput->getKeyDown(KEY_X) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				saveInMissionSave = true;
				missionLineChanged = turn;
			}		

			if (userInput->getKeyDown(KEY_Z) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				loadInMissionSave = true;
				missionLineChanged = turn;
			}		

			if (userInput->getKeyDown(KEY_Y) && userInput->ctrl() && !userInput->alt() && !userInput->shift())
			{
				DisplayCameraAngle ^= true;
				missionLineChanged = turn;
			}		

 			if (userInput->getKeyDown(KEY_N) && userInput->ctrl() && !userInput->alt() && !userInput->shift())
			{
				useSensors ^= true;
				missionLineChanged = turn;
			}

			if (userInput->getKeyDown(KEY_P) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				LogisticsData::instance->setResourcePoints(100000);
				missionLineChanged = turn;
			}		

			if (userInput->getKeyDown(KEY_R) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				if (forcedFrameRate == -1.0f)
					forcedFrameRate = 2.0f;
				else if (forcedFrameRate == 2.0f)
					forcedFrameRate = 5.0f;
				else if (forcedFrameRate == 5.0f)
					forcedFrameRate = 10.0f;
				else if (forcedFrameRate == 10.0f)
					forcedFrameRate = -1.0f;

				missionLineChanged = turn;
			}

			if (userInput->getKeyDown(KEY_C) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				neverEndingStory = true;
				missionLineChanged = turn;
			}

			// DEV HOTKEY (Ctrl+Alt+Shift+K): disable ALL enemy mechs on demand, the same
			// way MC2_SOAK_KILL_ENEMY does (OBJECT_STATUS_DISABLED, not destroyed) so they
			// stay salvageable. Lets you grab salvage without a full firefight. Disabled in
			// FINAL builds (this whole block is #ifndef FINAL).
			if (userInput->getKeyDown(KEY_K) && userInput->ctrl() && userInput->alt() && userInput->shift())
			{
				int killed = 0;
				for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
				{
					BattleMech* pM = ObjectManager->getMech(mi);
					if (!pM) continue;
					if (pM->isDisabled() || pM->isDestroyed()) continue;
					if (Team::home && pM->getTeamId() == Team::home->id) continue;  // skip friendlies
					pM->setStatus(OBJECT_STATUS_DISABLED, /*force=*/true);
					++killed;
				}
				printf("[DEVHOTKEY] kill-all-enemy killed=%d (Ctrl+Alt+Shift+K)\n", killed);
				fflush(stdout);
				missionLineChanged = turn;
			}

			{
				// Shadow softness (Poisson disk radius) — edge-triggered (single step
				// per press, not continuous while held) with 0.1 step for fine control.
				if (gos_GetKeyStatus(KEY_LBRACKET) == KEY_PRESSED) {
					float s = max(gos_GetTerrainShadowSoftness() - 0.1f, 0.1f);
					gos_SetTerrainShadowSoftness(s);
					CB_PRINTF("[SHADOW-KEY] [: softness=%.1f", s);
				}
				if (gos_GetKeyStatus(KEY_RBRACKET) == KEY_PRESSED) {
					float s = min(gos_GetTerrainShadowSoftness() + 0.1f, 10.0f);
					gos_SetTerrainShadowSoftness(s);
					CB_PRINTF("[SHADOW-KEY] ]: softness=%.1f", s);
				}
			}
			#endif
		}
	
		//---------------------------------------
		// The game believes we will drop well below par for a frame or two.
		// put the game into slo-mo so we don't lose any frames of animation.
		dynamicFrameTiming = true;
			
		if (saveInMissionSave)
		{
            char savegame_path[1024];
            S_snprintf(savegame_path, sizeof(savegame_path)/sizeof(savegame_path[0]), "%s" PATH_SEPARATOR "testgame.ims", savePath);
			save(savegame_path);
			saveInMissionSave = false;
		}

		if (!mc2SimTickFrozen(missionInterface) || MPlayer )
		{
			//First Frame we just set LastTimeGetTime.
			// After that, it increments based on System Time.
			// NOT the crazy GameOS frameRate.
			// S9D: under MC2_SMOKE_FIXED_TIMESTEP, substitute a fixed step so
			// frame N reaches an identical sim state every run. Single cached
			// bool check; OFF path is byte-identical to retail below.
			if (SmokeMode::fixedTimestepEnabled())
			{
				float milliseconds = SmokeMode::fixedTimestepMs();   // 1000/30
				const bool firstFrame = (LastTimeGetTime == 0xffffffff);
				// Seed LastTimeGetTime on the first frame so the first fixed
				// step is clean (no huge initial real-time delta); thereafter
				// advance it by exactly one fixed step.
				DWORD currentTimeGetTime =
					firstFrame ? timeGetTime()
					           : (DWORD)(LastTimeGetTime + (DWORD)milliseconds);
				if (!firstFrame)
					scenarioTime += (milliseconds / 1000.0f);
				LastTimeGetTime = currentTimeGetTime;
				// Deterministic clock probe (frames 60/120/180). Counts only
				// frames where the fixed step advanced scenarioTime.
				if (!firstFrame)
					SmokeMode::fixedTimestepOnSimFrame((double)scenarioTime);
			}
			else
			{
				DWORD currentTimeGetTime = timeGetTime();
				if (LastTimeGetTime != 0xffffffff)
				{
					float milliseconds = currentTimeGetTime - LastTimeGetTime;
					// MC2_SIM_SPEED: optional gated sim-speed multiplier (default 1.0,
					// clamped 0.25-4.0). Applied here (scenarioTime, wall-clock-driven)
					// so mission timers and frameLength-driven motion (mechcmd2.cpp)
					// advance in lockstep under the same multiplier. NOT applied on the
					// MC2_SMOKE_FIXED_TIMESTEP branch above — smoke determinism keeps 1x.
					scenarioTime += (milliseconds / 1000.0f) * simSpeedGetMultiplier();
				}
				LastTimeGetTime = currentTimeGetTime;
			}

			simClockTraceTick(scenarioTime);

			soundSystem->clearIsPaused();
		}
		else
		{
			//Keep track of system time.  Just don't add it to scenarioTime!!
			DWORD currentTimeGetTime = timeGetTime();
			LastTimeGetTime = currentTimeGetTime;

			soundSystem->setIsPaused();
		}

		
		//------------------------------------------------------------------------
		// There is a TINYYYYYYYYYY chance this will never go if timeGetTime()
		// happens to return 0 (one millisecond every approx. 49 days). I can live
		// with that...

//		if (MPlayer && (MPlayer->startTime >= 0.0))
//			runningTime = (float)(gos_GetElapsedTime()) - MPlayer->startTime;
//		else if (MPlayer && (scenarioTime > 10.0))
//			Fatal(MissionStartTime, " runningTime is not working...why? ");

			actualTime = scenarioTime;


		ZoneScopedN("GameLogic.Mission.Update");
		{ ZoneScopedN("Mission.clearArrays"); MisScope _ms(MS_CLEARARRAYS); mcTextureManager->clearArrays(); }

		if (missionInterface)
			{ ZoneScopedN("GameLogic.Mission.Interface"); MisScope _ms(MS_INTERFACE); missionInterface->update(); }

		{ ZoneScopedN("GameLogic.Mission.Camera"); MisScope _ms(MS_CAMERA); eye->update(); }

		// MVP-PUBLISH-EARLY-HOIST (gate MC2_MVP_PUBLISH_EARLY, default ON, =0
		// reverts). Publish THIS frame's world-to-clip matrix right after
		// eye->update() so the terrain compute snapshot taken later in this
		// update phase sees the current-frame camera, not the previous frame's.
		// The render-phase publish is kept and rewrites the identical matrix,
		// so gate OFF is byte-identical and A/B stays clean.
		//
		// Guard mirrors the render-phase publish predicate (eye->active &&
		// turn > 1). eye is always the live GameCamera here; SimpleCamera
		// pans and the editor run their own loops and self-publish, so this
		// never clobbers their publish.
		{
			static const bool s_mvpPublishEarly = mc2::gate::get<"MC2_MVP_PUBLISH_EARLY">();
			if (s_mvpPublishEarly && eye && eye->active && turn > 1)
			{
				gos_SetWorldToClipGL(eye->worldToClipGL());
			}
		}

		{ ZoneScopedN("GameLogic.Mission.VTol"); MisScope _ms(MS_VTOL); missionInterface->updateVTol(); }

		{ ZoneScopedN("GameLogic.Mission.Terrain"); MisScope _ms(MS_LAND_UPDATE); land->update(); }

		//ALWAYS update weather AFTER the camera.  May change the lights!
		if (useNonWeaponEffects)
			{ ZoneScopedN("GameLogic.Mission.Weather"); MisScope _ms(MS_WEATHER); weather->update(); }		//Should the rain fall during a pause?

		{ ZoneScopedN("GameLogic.Mission.Waypoints"); MisScope _ms(MS_WAYPOINTS); missionInterface->updateWaypoints(); }

#ifdef USE_PATH_COST_TABLE
		GlobalMoveMap[0]->resetPathCostTable();
#endif
		{ ZoneScopedN("GameLogic.PathManager"); MisScope _ms(MS_PATHMGR); PathManager->update(); }

		if (KillAmbientLight) {
		}
		
		//-----------------------------------------------------------
		// Lastly, process the terrain geometry which loads textures
		// Must do this to keep from Locking during the updateRenders phase
		// Also reset the object flags because we recalc those during geometry!
		{ ZoneScopedN("Mission::update clearObjBlocksActive"); MisScope _ms(MS_CLEAR_BLOCKS); land->clearObjBlocksActive(); }
		{ ZoneScopedN("Mission::update clearObjVerticesActive"); MisScope _ms(MS_CLEAR_VERTS); land->clearObjVerticesActive(); }
		{ ZoneScopedN("Mission::update terrainTextures->update"); MisScope _ms(MS_TERRAIN_TEX); land->terrainTextures->update(); }

		{ ZoneScopedN("GameLogic.Mission.TerrainGeometry"); MisScope _ms(MS_GEOMETRY); land->geometry(); }

		// 2026-05-13: begin GPU cull substrate frame BEFORE the pause-branch.
		// Was inside GameObjectManager::update (objmgr.cpp), which is
		// pause-gated.  Render-time submits (BldgAppearance/TreeAppearance::
		// render and GpuStaticPropRegistry::flush) keep appending substrate
		// records every frame regardless of pause, so this reset must also
		// run every frame to keep the ring slot and per-frame counter in
		// sync.  Calling without isEnabled gating is safe — substrate_frameBegin
		// internally checks isEnabled() and is a no-op when disabled.
		// Fixes the pause-smear bug where paused frames accumulated
		// substrate records across the un-reset ring slot, inflating
		// per-bucket instanceCount in compute cull and causing coalesce
		// multi-draw to render copies of other types' geometry at every
		// prop's origin.
		gpu_cull::substrate_frameBegin();

		// FREEZE-MOVERS-1: the `freeze_movers` driver verb rides the EXISTING
		// pause branch rather than adding a second freeze mechanism.
		// updateAppearancesOnly() (objmgr.cpp:4857) still runs recalcBounds +
		// setObjectParameters + updateGeometry so frozen actors keep drawing,
		// calls Appearance::update(false) so the gait does not advance, and
		// never calls setMoverParameters so torso/turret aim holds its last
		// value -- the drift that invalidated an earlier vehicle-vs-mech
		// luma A/B. mc2FreezeMoversActive() is 0 unless the verb or
		// MC2_FREEZE_MOVERS_AT armed it, so the default path is unchanged.
		if ( mc2SimTickFrozen(missionInterface) && !MPlayer )
			ObjectManager->updateAppearancesOnly( true, true, true );
		else
			{ MisScope _ms(MS_OBJMGR); ObjectManager->update(true, true, true); }

		// C1b GPU authority flip: compute_dispatch() has been MOVED to txmmgr.cpp
		// (between GpuStaticPropRegistry::flush() and GpuStaticPropBatcher::flush())
		// so that static prop substrate records appended during registry flush are
		// visible to the cull shader. compute_emitParitySummary() still runs here
		// (no static-prop records yet in the substrate; summary is for dynamic actors
		// from substrate_flushUpload earlier this frame — timing is acceptable).
		if (gpu_cull::compute_isEnabled()) {
			gpu_cull::compute_emitParitySummary();
		}

		{ ZoneScopedN("GameLogic.Mission.Craters"); craterManager->update(); }

		// TERRAIN-REAUTH-UNPIN-1 Half B: near-object displacement fade, mover
		// half. Gathers live mover positions in coarse-cell space and stamps
		// them into the visual-displacement damp map (min-combined with the
		// static building damp) so units always stand on TRUE gameplay height
		// with no visual pop. Rides MC2_TERRAIN_VISUAL_DISPLACE; whole block is
		// a single cheap bool check when the displace gate / damp map is off.
		if (gos_TerrainLodChunk_VisualDampWanted())
		{
			ZoneScopedN("GameLogic.Mission.VisualDampMovers");
			static std::vector<float> s_moverCells;
			s_moverCells.clear();
			const float halfMap = Terrain::worldUnitsMapSide * 0.5f;
			const float wupv    = Terrain::worldUnitsPerVertex;   // 128
			for (long mi = 0; mi < ObjectManager->getNumMovers(); ++mi)
			{
				MoverPtr mv = ObjectManager->getMover(mi);
				if (!mv || !mv->getExists())
					continue;
				Stuff::Vector3D mp = mv->getPosition();
				s_moverCells.push_back((mp.x + halfMap) / wupv);
				s_moverCells.push_back((halfMap - mp.y) / wupv);
			}
			static const float s_objfadeRadiusWu = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE_RADIUS">();   // the row clamps to [32, 1024]
			gos_TerrainLodChunk_UpdateVisualDampMovers(
				s_moverCells.data(), (int)(s_moverCells.size() / 2),
				s_objfadeRadiusWu / wupv, 48.0f / wupv);
		}

		//Do not UPDATE the textures during a pause.
		//This uncaches things which only objectManager->update can cache back in!!!!!
		if ( !mc2SimTickFrozen(missionInterface) || MPlayer )
			{ ZoneScopedN("GameLogic.Mission.TextureManager"); mcTextureManager->update(); }

		if (useSensors && ( !mc2SimTickFrozen(missionInterface) || MPlayer ) )
			{ ZoneScopedN("GameLogic.Mission.Sensors"); SensorManager->update(); }

		if (useCollisions && ( !mc2SimTickFrozen(missionInterface) || MPlayer ) )
			{ ZoneScopedN("GameLogic.Mission.Collisions"); ObjectManager->updateCollisions(); }

		if (missionBrain)
		{
			if ( !mc2SimTickFrozen(missionInterface) || MPlayer )
			{
				mc2_abl_trace::FireScope ablFire("mission", -1);
				long ablStmts;
				{ ZoneScopedN("GameLogic.AI.BrainExecute"); ablStmts = missionBrain->execute(); }
				ablFire.record(ablStmts, NumStateTransitions);
				long missionResult = missionBrain->getInteger();
				if (missionResult == 9999)
					return(terminationResult = 9999);
				if (!MPlayer)
					terminationResult = missionResult;
				// GAMEPLAY-DRIVER-2: re-assert a forced outcome. The line above
				// is the documented point where the mission brain trashes
				// terminationResult; a forced result set from the command
				// channel would otherwise live exactly one frame.
				if (forcedTerminationResult != mis_PLAYING) {
					terminationResult = forcedTerminationResult;
					terminationCounterStarted = true;
				}
			}
		}

		mc2_abl_trace::flushFrame(g_mc2FrameCounter, getBrainTickIndex());

		// Runtime-bridge v0: emit [MOVER v2] mover.state to stdout (env-gated,
		// throttled to MC2_BRIDGE_MOVER_PERIOD_SEC). Read-only; no-op unless
		// MC2_BRIDGE_MOVER_STATE=1.
		if (s_bridgeMoverState)
			bridgeEmitMoverState(scenarioTime);

		// SIM-IDENTITY-HASH-1: one world-state hash line per sim frame (opt-in,
		// MC2_SIM_IDENTITY_TRACE). Diffing two runs' logs names the FIRST
		// divergent sim frame directly -- the bridge above needs ~58k per-mover
		// lines and a bespoke script to answer the same question.
		//
		// simFrame is derived exactly as the capture sidecar derives its
		// `sim_frame` (gos_visual_capture.cpp write_sidecar) so a trace line and
		// a manifest taken in the same run carry the SAME frame number. That
		// derivation is only meaningful under the fixed clock; with the fixed
		// clock off it pins to 0 and the `t=` field is the usable diff axis --
		// which is correct, because determinism is not claimed at all then.
		mc2SimIdentityTraceFrame(
			(double)scenarioTime,
			(long)(SmokeMode::fixedClockSeconds() * 30.0 + 0.5));

		// META-FIX design A: mover-registry coherence audit (~1 Hz walk over
		// home-commander movers + force-bar icons; read-only). No-op unless
		// MC2_MOVER_COHERENCE_AUDIT=1 (static latch; gate-off = one bool).
		if (s_moverCoherenceAudit)
			auditMoverCoherence(scenarioTime, missionInterface);

		if (!MPlayer && !terminationCounterStarted)
		{
			bool playerForceAllDead = true;
			for (long i=0;i<ObjectManager->getNumMovers();i++)
			{
				MoverPtr mover = ObjectManager->getMover(i);
				if (mover->getCommanderId() == Commander::home->getId())
					if (!mover->isDisabled() && !mover->isDestroyed() && mover->isOnGUI()) {
						playerForceAllDead = false;
						break;
					}
			}

			if (playerForceAllDead && !neverEndingStory)
			{
				terminationResult = mis_PLAYER_LOST_BIG;
				terminationCounterStarted = true;
				missionTerminationTime = actualTime + 5.0/*seconds*/;
			}
		}

		if (!neverEndingStory && terminationCounterStarted) 
		{
			if (missionTerminationTime <= actualTime) {
				if (ControlGui::instance->resultsDone()) {
					scenarioResult = terminationResult;

					if ( !scenarioResult )
					{
						// mission brain frequently trashes the termination result
						if (!MPlayer) {
							int status = Team::home->objectives.Status();
							if (OS_SUCCESSFUL == status) 
								scenarioResult = mis_PLAYER_WIN_BIG;
							else
								scenarioResult = mis_PLAYER_LOST_BIG;
						}
					}

				}
				else if (!MPlayer) {
					bool playerForceAllDead = true;
					for (long i=0;i<ObjectManager->getNumMovers();i++)
					{
						MoverPtr mover = ObjectManager->getMover(i);
						if (mover->getCommanderId() == Commander::home->getId())
							if (!mover->isDisabled() && !mover->isDestroyed() && mover->isOnGUI()) {
								playerForceAllDead = false;
								break;
							}
					}

					if (playerForceAllDead)
					{
						terminationResult = mis_PLAYER_LOST_BIG;
						terminationCounterStarted = true;
						missionTerminationTime = actualTime + 5.0/*seconds*/;
					}
				}
			}
		} 
		else 
		{
			//As long as the ABL script returns -1, we won't do this.  NEEDED for Tutorials
			if (terminationResult != -1)
			{
				if (MPlayer) 
				{
					if (MPlayer->calcMissionStatus()) 
					{
						terminationCounterStarted = true;
						missionTerminationTime = actualTime + 5.0/*seconds*/;
						if (MPlayer->playerInfo[MPlayer->commanderID].winner)
							terminationResult = mis_PLAYER_WIN_BIG;
						else
							terminationResult = mis_PLAYER_LOST_BIG;

					}
				}
				else if (1 <= Team::home->objectives.Count()) 
				{
					int status = Team::home->objectives.Status();
					if (0 < Team::home->numPrimaryObjectives) 
					{
						if (OS_UNDETERMINED != status) 
						{
							if (!neverEndingStory)
							{
								if (OS_SUCCESSFUL == status) 
								{
									terminationResult = mis_PLAYER_WIN_BIG;
									terminationCounterStarted = true;
									missionTerminationTime = actualTime + 5.0/*seconds*/;
								} 
								else 
								{
									terminationResult = mis_PLAYER_LOST_BIG;
									terminationCounterStarted = true;
									missionTerminationTime = actualTime + 5.0/*seconds*/;
								}
							}
						}
					}
				}
			}
		}

		if (!MPlayer && !terminationCounterStarted)
		{
			if ((m_timeLimit != -1.0f) && (actualTime > m_timeLimit) && !neverEndingStory)
			{
				terminationResult = mis_PLAYER_LOST_BIG;
				terminationCounterStarted = true;
				missionTerminationTime = actualTime + 5.0/*seconds*/;
			}
		}

		if (userInput->getKeyDown(KEY_F) && !userInput->ctrl() && userInput->alt() && !userInput->shift())
		{
			showFrameRate ^= true;
			missionLineChanged = turn;
		}

#ifndef FINAL
		bool cheatWin = (userInput->getKeyDown(KEY_W) && userInput->shift() && userInput->ctrl() && userInput->alt());
		bool cheatLose = (userInput->getKeyDown(KEY_L) && userInput->shift() && userInput->ctrl() && userInput->alt()); 
		if (cheatWin)
			scenarioResult = mis_PLAYER_WIN_BIG;
		else if (cheatLose)
			scenarioResult = mis_PLAYER_LOST_BIG;

		//FORCE these to play to thwart any ABL bugs.
		// OPPOSITE for a reason!!!!!!!!
		// Duane decided to switch the order for our conveniance.  Thanks!!
		if ((terminationResult) && (terminationResult != -1) && (terminationResult < mis_PLAYER_DRAW))
		{
			soundSystem->playDigitalMusic(WIN_TUNE_0);
		}
		else if (terminationResult > mis_PLAYER_DRAW)
		{
			soundSystem->playDigitalMusic(LOSE_TUNE_0);
		}

		if (showFrameRate)
		{
			char text[1024];
			sprintf(text,"FrameRate: %f",1.0f/frameLength);
		
			DWORD width, height;
			Stuff::Vector4D moveHere;
			moveHere.x = 0.0f;
			moveHere.y = 0.0f;
		
			gos_TextSetAttributes (gosFontHandle, 0, gosFontScale, false, true, false, false);
			gos_TextStringLength(&width,&height,text);
		
			moveHere.z = width;
			moveHere.w = height;
		
			globalFloatHelp->setFloatHelp(text,moveHere,SD_GREEN,XP_BLACK,1.0f,true,false,false,false);
		}
#endif

		// CRASH-SOAK auto-win (always compiled; gated on MC2_SOAK_AUTOWIN env).
		// Independent of the #ifndef FINAL cheat path above so the harness works
		// in RelWithDebInfo regardless of FINAL. Fires once per mission after
		// MC2_SOAK_WIN_AFTER_SEC of scenario time. mis_PLAYER_WIN_BIG returned
		// from update() ends the mission instantly (no countdown).
		if (s_soakAutoWin && !scenarioResult)
		{
			static bool s_soakWinEmitted = false;
			const float winAfter = soakWinAfterSec();
			// Re-arm for each new mission: scenarioTime resets to 0 on start().
			if (scenarioTime < winAfter)
				s_soakWinEmitted = false;
			if (!s_soakWinEmitted && scenarioTime > winAfter)
			{
				scenarioResult = mis_PLAYER_WIN_BIG;
				s_soakWinEmitted = true;
				printf("[SOAK] autowin mission=%s t=%.1f\n",
					missionFileName, (float)scenarioTime);
				fflush(stdout);
			}
		}

		// --- MC2_SOAK_KILL_ENEMY ---------------------------------------------
		// Three phases per mission pair.
		// Phase 1 (current mission, t=60% win): disable one enemy mech.
		// Phase 2 (current mission, post-win):  commit it to inventory.
		// Phase 3 (next mission, first frame):  verify it is in load bay.
		// Re-arm clears kill/salvage flags but intentionally does NOT clear
		// s_soakKilledVariant until AFTER Phase 3 has fired, so the cross-
		// mission variant name survives the mission boundary.
		if (s_soakKillEnemy && s_soakAutoWin)
		{
			// PHASE 3 (next mission, first frames): load-bay check using the
			// variant name carried over from last mission.  Must run BEFORE
			// re-arm to avoid s_soakKilledVariant being cleared too early.
			if (!s_soakInventoryChecked && s_soakSalvageDone &&
				s_soakKilledVariant[0] &&
				scenarioTime >= 0.0f && scenarioTime < 1.0f)
			{
				LogisticsVariant* pChkVar =
					LogisticsData::instance
						? LogisticsData::instance->getVariant(s_soakKilledVariant)
						: nullptr;
				int present = 0;
				if (pChkVar && LogisticsData::instance)
					present = LogisticsData::instance->getVariantsInInventory(
						pChkVar, /*bIncludeForceGroup=*/true);
				printf("[SOAK] salvage-in-loadbay name=%s present=%d\n",
					s_soakKilledVariant, present > 0 ? 1 : 0);
				fflush(stdout);
				s_soakInventoryChecked = true;
			}

			// Re-arm each new mission (scenarioTime resets to 0 on start()).
			// Only clears variant name after Phase 3 has had its window.
			if (scenarioTime < soakWinAfterSec() * 0.5f)
			{
				s_soakKillDone = false;
				s_soakSalvageDone = false;
				s_soakTradeDone = false;
				if (s_soakInventoryChecked)
				{
					s_soakInventoryChecked = false;
					s_soakKilledVariant[0] = '\0';
				}
			}

			// PHASE 1: "cheat mode" — disable EVERY enemy mech at 60% of the win
			// timer so they are all DISABLED (not DESTROYED) and therefore all
			// salvageable. This maximally stresses the salvage system and mech-bay
			// capacity over a campaign. Remember the first variant for the Phase 3
			// load-bay round-trip check.
			const float killAt = soakWinAfterSec() * 0.6f;
			if (!s_soakKillDone && scenarioTime > killAt && !scenarioResult)
			{
				int killed = 0;
				for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
				{
					BattleMech* pM = ObjectManager->getMech(mi);
					if (!pM) continue;
					if (pM->isDisabled() || pM->isDestroyed()) continue;
					if (Team::home && pM->getTeamId() == Team::home->id) continue;
					// OBJECT_STATUS_DISABLED: salvage screen keeps it (isDisabled
					// true, isDestroyed false).
					pM->setStatus(OBJECT_STATUS_DISABLED, /*force=*/true);
					if (!s_soakKilledVariant[0])
					{
						strncpy(s_soakKilledVariant, pM->variantName,
							sizeof(s_soakKilledVariant) - 1);
						s_soakKilledVariant[sizeof(s_soakKilledVariant) - 1] = '\0';
					}
					++killed;
				}
				printf("[SOAK] kill-enemy killed=%d first=%s\n",
					killed, s_soakKilledVariant[0] ? s_soakKilledVariant : "(none)");
				fflush(stdout);
				s_soakKillDone = true;
			}

			// PHASE 1.5 (ISSUE-42a, MC2_SOAK_SALVAGE_TRADEMOVER): before autowin
			// fires, run the REAL mid-mission salvage-craft flow on ONE disabled
			// enemy mech — the exact sequence the salvage VTOL executes in
			// missiongui.cpp (recover / setStatus(SHUTDOWN,force) / tradeMover /
			// orderPowerUp). The mech becomes a home-commander mover, so mission
			// completion routes it through LogisticsData::setMissionCompleted's
			// recovered-mech carry-over branch (the path issue #42a suspects)
			// instead of the Phase-2 addMechToInventory shortcut (Phase 2 skips
			// it once it is home-team). Phase 3 then asserts the SAME variant
			// reached the load bay. Pair with MC2_LOG_LOGISTICS_FLOW +
			// MC2_DIAG_TAGS=LOGISTICS for per-unit cull/resolve/mech_lost JSONL.
			const float tradeAt = soakWinAfterSec() * 0.8f;
			if (s_soakSalvageTrademover && !s_soakTradeDone && s_soakKillDone &&
				scenarioTime > tradeAt && !scenarioResult)
			{
				BattleMech* pTrade = nullptr;
				for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
				{
					BattleMech* pM = ObjectManager->getMech(mi);
					if (!pM) continue;
					if (!pM->isDisabled() || pM->isDestroyed()) continue;
					if (Team::home && pM->getTeamId() == Team::home->id) continue;
					// Prefer the mech Phase 1 recorded so Phase 3's load-bay
					// assert tracks the traded mech; else first candidate.
					if (s_soakKilledVariant[0] &&
						strcmp(pM->variantName, s_soakKilledVariant) == 0)
					{
						pTrade = pM;
						break;
					}
					if (!pTrade)
						pTrade = pM;
				}
				char* newPilotName = (pTrade && LogisticsData::instance)
					? (char*)LogisticsData::instance->getBestPilot(pTrade->tonnage)
					: nullptr;
				if (pTrade && newPilotName && Team::home && Commander::home)
				{
					// Mirror missiongui.cpp salvage-craft recovery verbatim.
					while (((MoverPtr)pTrade)->recover() == false)
						;
					if (pTrade->isDisabled())
					{
						pTrade->setStatus(OBJECT_STATUS_SHUTDOWN, true);
						if (pTrade->getSensorSystem())
							pTrade->getSensorSystem()->broken = false;
						((MoverPtr)pTrade)->timeLeft = 1.0f;
						((MoverPtr)pTrade)->exploding = false;
					}
					tradeMover(pTrade, Team::home->getId(),
						Commander::home->getId(), newPilotName, "pbrain");
					pTrade->getPilot()->orderPowerUp(true, ORDER_ORIGIN_SELF);
					strncpy(s_soakKilledVariant, pTrade->variantName,
						sizeof(s_soakKilledVariant) - 1);
					s_soakKilledVariant[sizeof(s_soakKilledVariant) - 1] = '\0';
					printf("[SOAK] salvage-trademover name=%s variant=%s pilot=%s\n",
						pTrade->getName() ? pTrade->getName() : "(null)",
						s_soakKilledVariant, newPilotName);
					fflush(stdout);
				}
				else
				{
					printf("[SOAK] salvage-trademover no-candidate\n");
					fflush(stdout);
				}
				s_soakTradeDone = true;
			}

			// PHASE 2: After autowin, salvage ALL disabled enemy mechs into the
			// inventory (mirrors SalvageMechScreen -> addMechToInventory, done for
			// every salvageable mech). This is where mech-bay capacity gets stressed.
			if (!s_soakSalvageDone && scenarioResult)
			{
				int salvCount = 0;
				if (LogisticsData::instance)
				{
					for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
					{
						BattleMech* pM = ObjectManager->getMech(mi);
						if (!pM) continue;
						if (!pM->isDisabled() || pM->isDestroyed()) continue;
						if (Team::home && pM->getTeamId() == Team::home->id) continue;
						LogisticsVariant* pVar =
							LogisticsData::instance->getVariant(pM->variantName);
						if (!pVar) continue;
						LogisticsData::instance->addMechToInventory(
							pVar, (LogisticsPilot*)nullptr, 0, /*bSubtractPts=*/false);
						++salvCount;
					}
				}
				printf("[SOAK] salvage-check items=%d first=%s\n",
					salvCount, s_soakKilledVariant[0] ? s_soakKilledVariant : "(none)");
				fflush(stdout);
				s_soakSalvageDone = true;
			}
		}

		// --- MC2_FIXTURE_ORDER_LIVENESS (META-FIX design F) -------------------
		// See the namespace block above for the contract. Grammar of the
		// summary/order lines is reviewed contract — do not normalize.
		if (s_fixtureOrderLiveness && s_soakAutoWin && ObjectManager)
		{
			extern long LastMoveCalcErr;   // code/warrior.cpp:436 — same signal
			                               // MC2_BRAIN_FREEZE_TRACE reads
			const float olWin = soakWinAfterSec();

			if (scenarioTime < olWin * 0.1f)
			{
				s_olMoveIssued = s_olAttackIssued = s_olSummaryDone = false;
				s_olIssuedMove = 0;
			}

			// PHASE A (t = 20% win): COMMANDER-origin unaimed attack per armed
			// home mech at the nearest live enemy mover. Unaimed (-1) — the #9
			// aimed leg is MC2_AIMED_FIRE_FIXTURE's job. Runs FIRST so the
			// attack leg (hits/nofire evidence) accrues while the move phase
			// below later replaces the combat order for a clean displacement
			// window.
			if (!s_olAttackIssued && scenarioTime > olWin * 0.2f && !scenarioResult)
			{
				for (int mi = 0; mi < ObjectManager->numMechs; ++mi)
				{
					BattleMech* pM = ObjectManager->getMech(mi);
					if (!pM || pM->isDisabled() || pM->isDestroyed()) continue;
					if (!Team::home || pM->getTeamId() != Team::home->id) continue;
					MechWarriorPtr olPilot = pM->getPilot();
					if (!olPilot || pM->numWeapons <= 0 || !pM->canFireWeapons())
						continue;
					MoverPtr olEnemy = nullptr;
					float olBest = 1e30f;
					for (long ei = 0; ei < ObjectManager->getNumMovers(); ++ei)
					{
						MoverPtr pE = ObjectManager->getMover(ei);
						if (!pE || pE->isDisabled() || pE->isDestroyed()) continue;
						if (pE->getTeamId() == Team::home->id) continue;
						const float d = pM->distanceFrom(pE->getPosition());
						if (d < olBest) { olBest = d; olEnemy = pE; }
					}
					if (!olEnemy) continue;
					olPilot->orderAttackObject(false, ORDER_ORIGIN_COMMANDER,
						olEnemy, ATTACK_TO_DESTROY, ATTACKMETHOD_RANGED,
						FIRERANGE_OPTIMAL, -1, -1, TACORDER_PARAM_PURSUE);
					printf("[ORDER_FIXTURE] kind=attack wid=%lu tgt=%lu t=%.1f\n",
						(unsigned long)pM->getWatchID(),
						(unsigned long)olEnemy->getWatchID(),
						(float)scenarioTime);
				}
				fflush(stdout);
				s_olAttackIssued = true;
			}

			// PHASE M (t = 50% win): COMMANDER-origin move order per home mech,
			// 300 m toward map center; record start position per watch-ID.
			// Replaces the attack order — displacement is measured from HERE
			// to the 0.9*win summary (a clean 0.4*win window).
			if (!s_olMoveIssued && s_olAttackIssued &&
				scenarioTime > olWin * 0.5f && !scenarioResult)
			{
				extern float metersPerWorldUnit;
				const float stepWU = (metersPerWorldUnit > 0.0f)
					? (300.0f / metersPerWorldUnit) : 300.0f;
				for (int mi = 0; mi < ObjectManager->numMechs &&
					 s_olIssuedMove < kOrderLivenessCap; ++mi)
				{
					BattleMech* pM = ObjectManager->getMech(mi);
					if (!pM || pM->isDisabled() || pM->isDestroyed()) continue;
					if (!Team::home || pM->getTeamId() != Team::home->id) continue;
					MechWarriorPtr olPilot = pM->getPilot();
					if (!olPilot) continue;
					Stuff::Vector3D olPos = pM->getPosition();
					Stuff::Vector3D olGoal = olPos;
					const float olLen = sqrtf(olPos.x * olPos.x + olPos.y * olPos.y);
					if (olLen > 1.0f)
					{
						olGoal.x = olPos.x - (olPos.x / olLen) * stepWU;
						olGoal.y = olPos.y - (olPos.y / olLen) * stepWU;
					}
					else
						olGoal.x = olPos.x + stepWU;
					olPilot->orderMoveToPoint(false, true, ORDER_ORIGIN_COMMANDER,
						olGoal, -1, TACORDER_PARAM_RUN);
					s_olEntries[s_olIssuedMove].wid = pM->getWatchID();
					s_olEntries[s_olIssuedMove].startPos = olPos;
					++s_olIssuedMove;
					printf("[ORDER_FIXTURE] kind=move wid=%lu tgt=0 t=%.1f\n",
						(unsigned long)pM->getWatchID(), (float)scenarioTime);
				}
				fflush(stdout);
				s_olMoveIssued = true;
			}

			// PHASE S (t = 90% win): self-asserting summary. moved = net
			// displacement >= 25 m since the move order; dead/disabled units
			// are exempt (destroyed-before-compliance is not a dead-end);
			// non-movers with a classified path refusal count as refused
			// (classified counts as signal, not silence).
			if (!s_olSummaryDone && s_olMoveIssued && scenarioTime > olWin * 0.9f)
			{
				int olMoved = 0, olRefused = 0, olStalled = 0;
				for (int i = 0; i < s_olIssuedMove; ++i)
				{
					GameObjectPtr obj =
						ObjectManager->getByWatchID(s_olEntries[i].wid);
					if (!obj || obj->isDestroyed() || obj->isDisabled())
						continue;   // dead-unit exemption
					const float distM =
						obj->distanceFrom(s_olEntries[i].startPos);   // meters
					if (distM >= 25.0f)
						++olMoved;
					else if (LastMoveCalcErr != 0)
						++olRefused;
					else
						++olStalled;
				}
				const bool olPass = (s_olIssuedMove > 0) && (olStalled == 0);
				printf("ORDER_LIVENESS_SUMMARY issued_move=%d moved=%d "
					"refused=%d stalled=%d verdict=%s\n",
					s_olIssuedMove, olMoved, olRefused, olStalled,
					olPass ? "PASS" : "FAIL");
				fflush(stdout);
				s_olSummaryDone = true;
			}
		}

		// --- MC2_SOAK_PILOT_PROMOTE ------------------------------------------
		// Fires on campaign mission numbers 2 and 5 (via getCurrentMissionNum).
		// Grants enough gunnery to cross the next rank threshold, then runs
		// promotePilot() exactly as pilotreviewarea.cpp:215 does.
		// Throttle: getCurrentMissionNum() % 10 == 2 or == 5 — roughly 2
		// firings per 10-mission stretch without hammering every mission.
		if (s_soakPilotPromote && s_soakAutoWin)
		{
			// Re-arm at start of each mission, and count missions played once each
			// (edge-triggered) so the throttle does not depend on
			// getCurrentMissionNumber() semantics (not a clean 1..N per campaign).
			static int  s_soakMissionCount   = 0;
			static bool s_soakMissionCounted = false;
			if (scenarioTime < soakWinAfterSec() * 0.5f)
			{
				if (!s_soakMissionCounted) { ++s_soakMissionCount; s_soakMissionCounted = true; }
				s_soakPromoteFired = false;
			}
			else
			{
				s_soakMissionCounted = false;
			}

			// Throttle: ~twice per campaign — the 2nd and 5th missions played.
			const bool throttlePass =
				(s_soakMissionCount == 2 || s_soakMissionCount == 5);

			const float promoteAt = soakWinAfterSec() * 0.7f;
			if (!s_soakPromoteFired && throttlePass &&
				scenarioTime > promoteAt && !scenarioResult)
			{
				LogisticsPilot* lPilot = nullptr;
				const char* pilotName = "(none)";
				for (int wi = 1; wi <= MechWarrior::numWarriors && !lPilot; ++wi)
				{
					MechWarriorPtr mw = MechWarrior::warriorList[wi];
					if (!mw) continue;  // getPilot() below gates to real deployed pilots
					LogisticsPilot* lp =
						LogisticsData::instance
							? LogisticsData::instance->getPilot(mw->getName())
							: nullptr;
					if (!lp) continue;
					lPilot = lp;
					pilotName = lp->getName();
				}

				if (lPilot)
				{
					const long rankBefore = lPilot->getRank();
					// soakAddGunnery bumps both lifetime gunnery and newGunnery
					// delta so promotePilot() sees a mission gain large enough
					// to cross a rank threshold from any starting rank.
					// 11 pts guarantees crossing regardless of start (worst case
					// GREEN->REGULAR needs avg 50+epsilon; 11 pts covers all).
					lPilot->soakAddGunnery(11.0f);
					bool promoted = lPilot->promotePilot();
					const long rankAfter = lPilot->getRank();
					printf("[SOAK] pilot-promote pilot=%s rankBefore=%ld rankAfter=%ld ok=%d\n",
						pilotName, rankBefore, rankAfter, promoted ? 1 : 0);
					fflush(stdout);
				}
				else
				{
					printf("[SOAK] pilot-promote no-deployed-pilot\n");
					fflush(stdout);
				}
				s_soakPromoteFired = true;
			}
		}

#ifdef LAB_ONLY
		MCTimeMissionTotal 		= MCTimeTerrainUpdate +
									MCTimeCameraUpdate +
									MCTimeWeatherUpdate + 
									MCTimePathManagerUpdate +
									MCTimeRunBrainUpdate +
									MCTimePath1Update + 
									MCTimePath2Update + 
									MCTimePath3Update + 
									MCTimePath4Update + 
									MCTimePath5Update + 
									MCTimeCalcPath1Update +
									MCTimeCalcPath2Update +
									MCTimeCalcPath3Update +
									MCTimeCalcPath4Update +
									MCTimeCalcPath5Update +
									MCTimeCalcGoal1Update + 
									MCTimeCalcGoal2Update + 
									MCTimeCalcGoal3Update + 
									MCTimeCalcGoal4Update + 
									MCTimeCalcGoal5Update + 
									MCTimeCalcGoal6Update + 
									MCTimeTerrainGeometry +
									MCTimeInterfaceUpdate +
									MCTimeCraterUpdate +
									MCTimeTXMManagerUpdate +
									MCTimeSensorUpdate +
									MCTimeLOSUpdate +
									MCTimeCollisionUpdate +
									MCTimeMissionScript+ 
									MCTimeTerrainObjectsUpdate +
									MCTimeMechsUpdate +
									MCTimeVehiclesUpdate +
									MCTimeTurretsUpdate +
									MCTimeAllElseUpdate;
#endif
	}

	return scenarioResult;
}
