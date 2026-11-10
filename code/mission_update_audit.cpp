// Mission: the runtime-bridge mover.state emitter and the mover-registry coherence audit that Mission::update calls
// once per frame. Declared in mission_internal.h.
#include <algorithm>
#include "mission_internal.h"
#include "mc2fmt.h"
#include "runtime_observation_scope.h"
#include "../GameOS/gameos/runtime_observation.h"
#include "../GameOS/gameos/runtime_bridge_protocol.h"
#include <chrono>

namespace mc2::observation {
PhaseScope::PhaseScope(unsigned index, std::string_view name) : index_(index), name_(name) {
    if (enabled())
        startNs_ = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
PhaseScope::~PhaseScope() {
    if (!startNs_) return;
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    recordZone(index_, name_, now - startNs_);
}
bool bridgePaused() { return bridge::control().paused(); }
int bridgeTakeSteps(int most) { return bridge::control().takeSteps(most); }
bool bridgeTakeSpeed(float& speed) { return bridge::control().takeSpeed(speed); }
void bridgeNoteTicks(int count) { bridge::control().noteTicks(count); }
void observeFrame(bool active) {
    if (!enabled()) return;
    beginFrame();
    auto& sample = current;
    sample.censusValid = active && ObjectManager;
    if (sample.censusValid) {
        sample.movers = ObjectManager->getNumMovers();
        for (long i = 0; i < sample.movers; ++i) {
            const auto mover = ObjectManager->getMover(i);
            const auto pilot = mover ? mover->getPilot() : nullptr;
            if (pilot) { ++sample.pilots; if (pilot->getBrain()) ++sample.brains; }
            if (mover && mc2::bridge::configured()) {
                if (sample.transformCount < sample.transforms.size()) {
                    const auto position = mover->getPosition();
                    sample.transforms[sample.transformCount++] = {mover->getPartId(), static_cast<long>(mover->getHandle()),
                        mover->getTeamId(), position.x, position.y, position.z, mover->getRotation()};
                } else ++sample.transformDropped;
            }
        }
    }
}
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
	const bool  s_bridgeMoverState = mc2::gate::get<"MC2_BRIDGE_MOVER_STATE">();
namespace {
	static float s_bridgeNextEmitTime = 0.0f;   // scenarioTime of next burst
	static float bridgeMoverPeriodSec() {
		return 2.0f;   // the row clamps to >= 0.25
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
	static void bridgeSanitize(char* dst, size_t cap, mc2::czstring src) {
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
}  // namespace

	void bridgeEmitMoverState(float scenarioTimeNow) {
		if (!s_bridgeMoverState || !ObjectManager)
			return;
		if (scenarioTimeNow < s_bridgeNextEmitTime)
			return;
		s_bridgeNextEmitTime = scenarioTimeNow + bridgeMoverPeriodSec();

		const long kCap = 64;
		long total = ObjectManager->getNumMovers();
		long emitted = 0;
		mc2::fmt::print_out("[MOVER v2 begin] t={:.1f} count={:d}\n", scenarioTimeNow, mc2::fmt::as_i32(total));
		for (long i = 0; i < total && emitted < kCap; ++i) {
			MoverPtr mover = ObjectManager->getMover(i);
			if (!mover)
				continue;

			long              partId = mover->getPartId();
			char              name[64];
			bridgeSanitize(name, sizeof(name), mover->getName());
			long              team   = mover->getTeamId();
			glm::vec3   pos    = mover->getPosition();

			// HP as a 0..1 damage-derived fraction (getDamageLevel is the only
			// HP signal reachable without poking per-location body arrays).
			float dmg = mover->getDamageLevel();
			float hpFrac = 1.0f - dmg;
			hpFrac = std::clamp(hpFrac, 0.0f, 1.0f);

			char              pilotName[64];
			bridgeSanitize(pilotName, sizeof(pilotName), nullptr);   // default "?"
			long              orderVal  = -1;
			const char*       orderName = "none";
			long              targetId  = -1;

			// v2 brain/path/target detail. Defaults are emitted even when the
			// pilot or path is absent so the [MOVER v2] grammar stays fixed-arity
			// and the editor parser never has to handle optional fields.
			long              brainState = -1;   // ABL brain FSM state index (opaque)
			long              moveState  = -1;   // moveOrders.moveState
			glm::vec3   pgoal;             pgoal = glm::vec3(0.0f);   // world goal of active path
			long              pCurStep   = -1;   // step index being headed for
			long              pNumSteps  = 0;    // total steps in active path
			long              pCost      = -1;   // A* total path cost
			glm::vec3   tgtPos;            tgtPos = glm::vec3(0.0f);  // current target world pos

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

			mc2::fmt::print_out("[MOVER v2] id={:d} name={} team={:d} pos={:.1f},{:.1f},{:.1f} hp={:.2f} pilot={} order={:d}/{} target={:d} brain={:d} mstate={:d} pgoal={:.1f},{:.1f},{:.1f} pstep={:d}/{:d} pcost={:d} tgtpos={:.1f},{:.1f},{:.1f}\n",
				mc2::fmt::as_i32(partId), mc2::fmt::safe(name), mc2::fmt::as_i32(team), pos.x, pos.y, pos.z,
				hpFrac, mc2::fmt::safe(pilotName), mc2::fmt::as_i32(orderVal), mc2::fmt::safe(orderName), mc2::fmt::as_i32(targetId),
				mc2::fmt::as_i32(brainState), mc2::fmt::as_i32(moveState), pgoal.x, pgoal.y, pgoal.z,
				mc2::fmt::as_i32(pCurStep), mc2::fmt::as_i32(pNumSteps), mc2::fmt::as_i32(pCost),
				tgtPos.x, tgtPos.y, tgtPos.z);
			++emitted;
		}
		fflush(stdout);
	}

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
	const bool s_moverCoherenceAudit = mc2::gate::get<"MC2_MOVER_COHERENCE_AUDIT">();
namespace {
	static float s_cohNextAuditTime = 0.0f;
	static long  s_cohChecked    = 0;   // mover-rule evaluations (cumulative)
	static long  s_cohViolations = 0;
	static long  s_cohAdvisories = 0;
	static long  s_cohAudits     = 0;   // audit ticks
	static bool  s_cohAtexit     = false;

	static void cohEmitSummary() {
		mc2AuditLinef("[MOVER_COHERENCE] summary checked={:d} violations={:d} "
			"advisories={:d} audits={:d}",
			mc2::fmt::as_i32(s_cohChecked), mc2::fmt::as_i32(s_cohViolations), mc2::fmt::as_i32(s_cohAdvisories), mc2::fmt::as_i32(s_cohAudits));
	}

	// rule ids for the dedup key (per mover partId).
	enum class CohRule { COH_I1 = 1, COH_I1B, COH_I2_ICON, COH_I2_ORDER,
	               COH_I2B_ORDER, COH_I3, COH_I4, COH_I5 };
	using enum CohRule;

	static void cohEmit(bool violation, mc2::czstring rule, long partId,
	                    mc2::czstring name, CohRule ruleId, mc2::czstring detail) {
		unsigned long long key =
			((static_cast<unsigned long long>(static_cast<unsigned long>(partId))) << 8) |
			static_cast<unsigned long long>(ruleId);
		if (violation) s_cohViolations++; else s_cohAdvisories++;
		if (!mc2AuditOnce("MOVER_COHERENCE", key))
			return;
		mc2AuditLinef("[MOVER_COHERENCE] {} rule={} part={:d} name={} {}",
			mc2::fmt::safe(violation ? "VIOLATION" : "ADVISORY"), mc2::fmt::safe(rule), mc2::fmt::as_i32(partId),
			mc2::fmt::safe((name && name[0]) ? name : "?"), mc2::fmt::safe(detail ? detail : ""));
	}

}  // namespace

	void auditMoverCoherence(float scenarioTimeNow,
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
						mc2::fmt::to_buf(det, "detail=tac_order code={:d} origin={:d}", mc2::fmt::as_i32(static_cast<int>(order->code)), mc2::fmt::as_i32(static_cast<int>(order->origin)));
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
