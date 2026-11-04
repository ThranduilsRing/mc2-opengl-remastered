//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//								WARRIOR.CPP

#include "warrior_internal.h"
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "sim_trace.h"

long MechWarrior::getVehicleStatus (void) {

	long myVehicleStatus = -1;
	GameObjectPtr myVehicle = getVehicle();
	if (myVehicle)
		myVehicleStatus = myVehicle->getStatus();
	return(myVehicleStatus);
}

// PATHFINDING-JUMP-FAIL-1: backoff gate + constants. Default-OFF killswitch: when
// MC2_PATH_JUMP_FAIL_BACKOFF is unset, s_jumpFailBackoffOn is false and every new
// branch below is inert -> byte-identical legacy behavior (no stamp, no short-circuit).
// Cached once at first use (repo getenv idiom, mover.cpp style).
static const bool s_jumpFailBackoffOn = mc2::gate::get<"MC2_PATH_JUMP_FAIL_BACKOFF">();
// Frames to suppress re-requesting a goal cell that just failed a jump solve.
// ~45 path-manager update() frames ~= 0.5-1s of doomed re-solves eliminated.
static const long kJumpFailBackoffFrames = 45;

// Defined in movemgr.cpp (PATHFINDING-FACTS-2 counter, advanced once per update()).
extern long MovePathManager_getFrameCounter (void);

void MechWarrior::noteJumpPathFailed (void) {

	if (!s_jumpFailBackoffOn)
		return;
	// Key on the pilot's CURRENT move goal cell -- the exact same basis
	// requestMovePath compares against (getMoveGoal -> worldToCell). Using one
	// source of truth guarantees the stamp and the short-circuit test agree,
	// regardless of any goal revision the solver applied internally.
	glm::vec3 goalLoc;
	if (getMoveGoal(&goalLoc) == MOVEGOAL_NONE)
		return;
	int goalCellR, goalCellC;
	land->worldToCell(goalLoc, goalCellR, goalCellC);
	lastFailedJumpGoalCellR = goalCellR;
	lastFailedJumpGoalCellC = goalCellC;
	lastJumpFailFrame = MovePathManager_getFrameCounter();
}

bool MechWarrior::getJumpBackoff (int32_t &goalR, int32_t &goalC, int32_t &age) {

	if (lastJumpFailFrame < 0)
		return false;
	const long elapsed = MovePathManager_getFrameCounter() - lastJumpFailFrame;
	if ((elapsed < 0) || (elapsed >= kJumpFailBackoffFrames))
		return false;
	goalR = lastFailedJumpGoalCellR;
	goalC = lastFailedJumpGoalCellC;
	age = static_cast<int32_t>(elapsed);
	return true;
}

void MechWarrior::setJumpBackoff (int32_t goalR, int32_t goalC, int32_t age) {

	lastFailedJumpGoalCellR = goalR;
	lastFailedJumpGoalCellC = goalC;
	lastJumpFailFrame = MovePathManager_getFrameCounter() - age;
}

void MechWarrior::requestMovePath (long selectionIndex, unsigned long moveParams, long source) {

	// PATHFINDING-JUMP-FAIL-1: short-circuit doomed jump re-requests. If the pilot's
	// current move goal is the SAME cell as the last jump solve that found no path,
	// and we are still inside the backoff window, skip the enqueue entirely -- this
	// is what collapses the queue width (the doomed solve was re-requested every AI
	// tick with no failure memory). Only AI/non-home jumpers ever fail jump solves
	// (jump path is enemy-only, mover.cpp), so player orders are never affected.
	if (s_jumpFailBackoffOn && (lastJumpFailFrame >= 0)) {
		glm::vec3 goalLoc;
		if (getMoveGoal(&goalLoc) != MOVEGOAL_NONE) {
			int goalCellR, goalCellC;
			land->worldToCell(goalLoc, goalCellR, goalCellC);
			if ((goalCellR == lastFailedJumpGoalCellR) &&
			    (goalCellC == lastFailedJumpGoalCellC)) {
				long now = MovePathManager_getFrameCounter();
				if ((now - lastJumpFailFrame) < kJumpFailBackoffFrames)
					return;  // doomed: don't re-enqueue yet
			}
		}
	}

	MC2_SIM_TRACE_LINE("REQ pilot={:d} src={:d} params={:d} sel={:d}", static_cast<int32_t>((getVehicle() ? getVehicle()->getPartId() : -1)), static_cast<int32_t>(source), static_cast<uint32_t>(moveParams), static_cast<int32_t>(selectionIndex));
	PathManager->request(this, selectionIndex, moveParams, source);
}

long MechWarrior::combatDecisionTree (void) {

	long result = -1;

	combatUpdate = scenarioTime + CombatUpdateFrequency;

	glm::vec3 tPoint;

	MoverPtr myVehicle = getVehicle();
	Assert(myVehicle != nullptr, 0, " Pilot has no vehicle! ");

	// Let's see if we have ANY ammo...
	bool outOfAmmo = true;
	for (long i = 0; i < myVehicle->getNumAmmoTypes(); i++)
		if (myVehicle->getAmmoTypeTotal(i) > 0) {
			outOfAmmo = false;
			break;
		}

	GameObjectPtr target = getLastTarget(); //getAttackTarget(ORDER_CURRENT);

	// Our target is our last target, if we had one (and we weren't explicitly
	// told to hold fire since then)...
	glm::vec3 *targetPoint = nullptr;
	long attackType = ATTACK_TO_DESTROY;
	long aimLocation = -1;
	if (curTacOrder.isCombatOrder()) {
		attackType = curTacOrder.attackParams.type;
		aimLocation = curTacOrder.attackParams.aimLocation;
		if (curTacOrder.code == TACTICAL_ORDER_ATTACK_POINT)
		{
			tPoint = getAttackTargetPoint();
			targetPoint = &tPoint;

			//MUST set the suppressionFire flag if we are host and this is from client
			// AND we are an artilleryPiece WITHOUT suppressionFire set AND
			// OUR attackParams are method == ATTACKMETHOD_RANGED, range = FIRERANGE_CURRENT
			// AND pursue = false.  Complex but keeps from having to figure out how to pass
			// the friggering flag around.
			if (MPlayer && MPlayer->isServer())
			{
				if (myVehicle->isGuardTower() && !(myVehicle->suppressionFire) &&
					(curTacOrder.attackParams.method == ATTACKMETHOD_RANGED) &&
					(curTacOrder.attackParams.range == FIRERANGE_CURRENT) &&
					(curTacOrder.attackParams.pursue == false))
				{
					myVehicle->suppressionFire = true;
				}
			}
		}
	}
	else
	{
		if (lastTargetConserveAmmo)
			attackType = ATTACK_CONSERVING_AMMO;
	}

	if (target) {
		if (!curTacOrder.isCombatOrder()) {
			glm::vec3 pos = target->getPosition();
			if (myVehicle->distanceFrom(pos) > attackRadius) {
				setLastTarget(nullptr);
				target = nullptr;
			}
		}
	}

	if (!target && (curTacOrder.code != TACTICAL_ORDER_ATTACK_POINT)) {
		#if WARRIOR_DEBUGGING_ENABLED
			if (debugFlags & WARRIOR_DEBUG_FLAG_COMBAT) {
				char s[128];
				mc2::fmt::to_buf(s, "{} ({:.2f}) has no attack target.\n", mc2::fmt::safe(callsign), getSituationFireRange());
				debugPrint(s, true);
			}
		#endif
		return(result);
	}

	if (target) {
		if (target->isDestroyed()) {
			#if WARRIOR_DEBUGGING_ENABLED
				if (debugFlags & WARRIOR_DEBUG_FLAG_COMBAT) {
					char s[128];
					mc2::fmt::to_buf(s, "{} ({:.2f}) has a destroyed target.\n", mc2::fmt::safe(callsign), getSituationFireRange());
					debugPrint(s, true);
				}
			#endif
			return(result);
		}

		if (target->isDisabled() && !lastTargetObliterate) {
			#if WARRIOR_DEBUGGING_ENABLED
				if (debugFlags & WARRIOR_DEBUG_FLAG_COMBAT) {
					char s[128];
					mc2::fmt::to_buf(s, "{} ({:.2f}) has a disabled target.\n", mc2::fmt::safe(callsign), getSituationFireRange());
					debugPrint(s, true);
				}
			#endif
			return(result);
		}
	}

	#if WARRIOR_DEBUGGING_ENABLED
		if (debugFlags & WARRIOR_DEBUG_FLAG_COMBAT) {
			char s[512];
			if (!myVehicle->canFireWeapons()) {
				mc2::fmt::to_buf(s, "{}'s ({:.2f}) vehicle cannot fire now.\n", mc2::fmt::safe(callsign), getSituationFireRange());
				debugPrint(s, true);
				}
			else if (weaponsStatusResult < 1) {
				switch (weaponsStatusResult) {
					case WEAPONS_STATUS_CANNOT_FIRE:
						mc2::fmt::to_buf(s, "{}'s ({:.2f}) vehicle cannot fire now.", mc2::fmt::safe(callsign), getSituationFireRange());
						break;
					case WEAPONS_STATUS_NO_TARGET:
						mc2::fmt::to_buf(s, "{} ({:.2f}) has no target.\n", mc2::fmt::safe(callsign), getSituationFireRange());
						break;
					case WEAPONS_STATUS_OUT_OF_RANGE:
						mc2::fmt::to_buf(s, "{} ({:.2f}) out of range.\n", mc2::fmt::safe(callsign), getSituationFireRange());
						break;
					case 0: {
						long numWeaponsNotReady = 0;
						long numWeaponsOutOfRange = 0;
						long numWeaponsNotLocked = 0;
						long numWeaponsOutOfAmmo = 0;
						long numWeaponsNoChance = 0;
						long numWeaponsTooHot = 0;
						for (long i = 0; i < myVehicle->numWeapons; i++) {
							if (weaponsStatus[i] == WEAPON_STATUS_NOT_READY)
								numWeaponsNotReady++;
							if (weaponsStatus[i] == WEAPON_STATUS_OUT_OF_AMMO)
								numWeaponsOutOfAmmo++;
							if (weaponsStatus[i] == WEAPON_STATUS_OUT_OF_RANGE)
								numWeaponsOutOfRange++;
							if (weaponsStatus[i] == WEAPON_STATUS_NOT_LOCKED)
								numWeaponsNotLocked++;
							if (weaponsStatus[i] == WEAPON_STATUS_NO_CHANCE)
								numWeaponsNoChance++;
							if (weaponsStatus[i] == WEAPON_STATUS_TOO_HOT)
								numWeaponsTooHot++;
						}
						mc2::fmt::to_buf(s, "{} ({:.2f}) has no shot: {:d} !ready, {:d} !ammo, {:d} !inrange, {:d} !locked, {:d} !chance, {:d} hot\n", mc2::fmt::safe(callsign), getSituationFireRange(), mc2::fmt::as_i32(numWeaponsNotReady), mc2::fmt::as_i32(numWeaponsOutOfAmmo), mc2::fmt::as_i32(numWeaponsOutOfRange), mc2::fmt::as_i32(numWeaponsNotLocked), mc2::fmt::as_i32(numWeaponsNoChance), mc2::fmt::as_i32(numWeaponsTooHot));
						}
						break;
					default:
						mc2::fmt::to_buf(s, "{} ({:.2f})  cannot fire for unknown reason.\n", mc2::fmt::safe(callsign), getSituationFireRange());
				}
				debugPrint(s, true);
			}
		}
	#endif

	// So we don't muck up the brains at this late date, simply report we're
	// out of ammo...
	if (attackType != ATTACK_CONSERVING_AMMO && outOfAmmo )
	{
		radioMessage(RADIO_AMMO_OUT, TRUE);
	}

	BOOL cantConserve = FALSE;	// report if we're asked to use ammo conservation and that's impossible
	if (attackType == ATTACK_CONSERVING_AMMO)
		cantConserve = TRUE;

	bool firedWeapon = false;
	if (myVehicle->canFireWeapons() && (weaponsStatusResult > 0)) {
		// First, a taunt...
		// Only Taunt Moving things!!
		if (target && target->isMover() && RollDice(1))
			radioMessage(RADIO_TAUNT, true);

		// Let loose...
		float targetTime = getLastTargetTime();

		// Track how much damage I've done to the target when weapons finally hit.
		// If I've done enough to blow the target, STOP firing!!
		float damageDoneToTarget = 0.0f;

		for (long curWeapon = 0; curWeapon < myVehicle->numWeapons; curWeapon++)
		{
			long weaponIndex = myVehicle->numOther + curWeapon;
			if (weaponsStatus[curWeapon] > 0)
			{
				if (!target || (target && (damageDoneToTarget < target->getDestructLevel())))
				{
					if ( attackType != ATTACK_CONSERVING_AMMO || myVehicle->getWeaponIsEnergy(weaponIndex))
					{
						float dmgDone = 0.0f;
						if ( NO_ERR == myVehicle->fireWeapon(target, targetTime, myVehicle->numOther + curWeapon, attackType, aimLocation, targetPoint, dmgDone) )
							cantConserve = FALSE;
						firedWeapon = true;
						damageDoneToTarget += dmgDone;
					}
				}
			}
			result = NO_ERR;
		}
	}

	// ISSUE-17 (MC2_FIRE_DECISION_TRACE, default OFF = byte-identical): refusal-
	// side instrumentation. The fire decision has six SILENT per-weapon gates in
	// calcWeaponsStatus (recycle / ammo / range / LOS+fire-arc conflated into
	// NOT_LOCKED / odds<1) and none of them is observable — the reason-tally
	// debug block above is dead (WARRIOR_DEBUG_FLAG_COMBAT never set). When a
	// combat order is active and NO weapon fired this combat update, emit one
	// COMBAT JSONL event with the per-gate tallies, plus one direct lineOfSight
	// probe to split LOS from fire-arc within NOT_LOCKED. Naturally throttled to
	// the combat-update cadence (this function re-arms combatUpdate above).
	{
		static const bool s_fireDecisionTrace =
			mc2::gate::get<"MC2_FIRE_DECISION_TRACE">();
		if (s_fireDecisionTrace && !firedWeapon && curTacOrder.isCombatOrder() &&
			mc2_diag::tagEnabled("COMBAT")) {
			long tally[6] = {0, 0, 0, 0, 0, 0};	// [0]=rd [1]=am [2]=rn [3]=lk [4]=ch [5]=hot
			for (long tw = 0; tw < myVehicle->numWeapons; tw++) {
				switch (weaponsStatus[tw]) {
					case WEAPON_STATUS_NOT_READY:	tally[0]++; break;
					case WEAPON_STATUS_OUT_OF_AMMO:	tally[1]++; break;
					case WEAPON_STATUS_OUT_OF_RANGE:tally[2]++; break;
					case WEAPON_STATUS_NOT_LOCKED:	tally[3]++; break;
					case WEAPON_STATUS_NO_CHANCE:	tally[4]++; break;
					case WEAPON_STATUS_TOO_HOT:		tally[5]++; break;
				}
			}
			long losFlag = -1;
			float dist = -1.0f;
			if (target) {
				losFlag = myVehicle->lineOfSight(target) ? 1 : 0;
				dist = myVehicle->distanceFrom(target->getPosition());
			}
			else if (targetPoint) {
				losFlag = myVehicle->lineOfSight(*targetPoint) ? 1 : 0;
				dist = myVehicle->distanceFrom(*targetPoint);
			}
			char _nf[256];
			mc2::fmt::to_buf(_nf, "{{\"ev\":\"nofire\",\"wid\":{:d},\"tgt_wid\":{:d},\"ord\":{:d},\"result\":{:d},\"rd\":{:d},\"am\":{:d},\"rn\":{:d},\"lk\":{:d},\"ch\":{:d},\"hot\":{:d},\"los\":{:d},\"dist\":{:.0f}}}", mc2::fmt::as_i32(static_cast<long>(vehicleWID)), mc2::fmt::as_i32(target ? static_cast<long>(target->getWatchID()) : 0L), mc2::fmt::as_i32(static_cast<int>(curTacOrder.code)), mc2::fmt::as_i32(static_cast<long>(weaponsStatusResult)), mc2::fmt::as_i32(tally[0]), mc2::fmt::as_i32(tally[1]), mc2::fmt::as_i32(tally[2]), mc2::fmt::as_i32(tally[3]), mc2::fmt::as_i32(tally[4]), mc2::fmt::as_i32(tally[5]), mc2::fmt::as_i32(losFlag), static_cast<double>(dist));
			mc2_diag::writeEvent("COMBAT", 1, 0, _nf);
		}
	}

	if (curTacOrder.attackParams.tactic == TACTIC_STOP_AND_FIRE) {
		if (firedWeapon)
			clearMoveOrders();
	}
	if (cantConserve)	// probably a lie
	{
		for (long curWeapon = myVehicle->numOther; curWeapon < myVehicle->numOther + myVehicle->numWeapons; curWeapon++)
		{
			if (myVehicle->getWeaponShots(curWeapon) == UNLIMITED_SHOTS && myVehicle->isWeaponWorking(curWeapon))
			{
				cantConserve = FALSE;	// it is a lie! we have a working energy weapon, it's just recycling or something...
				break;
			}
		}
	}

	if (cantConserve)
	{
		radioMessage(RADIO_ILLEGAL_ORDER);
		setLastTarget(nullptr);
		clearCurTacOrder();	// so we don't keep trying (and announcing that it doesn't work!!)
	}

	return(result);
}

void MechWarrior::updateActions (void) {

	if ((teamId == -1) || !brainsEnabled[teamId]) {
		clearCurTacOrder(false);
		clearMoveOrders();
		clearAttackOrders();
		tacOrder[ORDERSTATE_GENERAL].init();
		tacOrder[ORDERSTATE_PLAYER].init();
		tacOrder[ORDERSTATE_ALARM].init();
		setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
		return;
	}

	MoverPtr myVehicle = getVehicle();

	if (myVehicle->isCaptured()) {
		clearCurTacOrder();
		setLastTarget(nullptr);
		return;
	}

	// MC2_FX_FORCE_SPAWN fixture (default-OFF). Idle tier1/visual captures spawn
	// NO weapon FX, so the PPC TUBE path never renders headlessly. This forces
	// ONE mech to fire its ENERGY weapons (PPCs -> tube ribbons) at a point in
	// front of it. Mechs ALWAYS tick here (unlike static buildings, which are
	// update-skipped), so the FX drives + renders + cleans via the normal
	// weaponbolt path. Fire-at-POINT: fireWeapon(target=NULL, &targetPoint) --
	// the target-specific logic is if(target)-guarded (mech.cpp). Fires ONCE
	// (global latch). Output is stdout, so it needs MC2_LOG=1 to be visible.
	static const bool s_fxForceSpawn =
		mc2::gate::isSet<"MC2_FX_FORCE_SPAWN">() && !mc2::gate::is<"MC2_FX_FORCE_SPAWN", "0">();
	// VK-FIELD-VFX-2: continuous re-fire mode for a deterministic on-camera capture
	// window. MC2_FX_FORCE_SPAWN=loop (or =2) re-fires one mech's whole weapon bank on a
	// fixed cadence so there is ALWAYS a live bolt on screen during a desktop grab
	// (the =1 fire-once fixture leaves only a ~0.5s transient that is easy to miss).
	static const bool s_fxLoop =
		mc2::gate::is<"MC2_FX_FORCE_SPAWN", "loop">() || mc2::gate::is<"MC2_FX_FORCE_SPAWN", "2">() ||
		mc2::gate::is<"MC2_FX_FORCE_SPAWN", "3">()    || mc2::gate::is<"MC2_FX_FORCE_SPAWN", "4">() ||
		mc2::gate::is<"MC2_FX_FORCE_SPAWN", "5">()    || mc2::gate::is<"MC2_FX_FORCE_SPAWN", "6">() ||
		mc2::gate::is<"MC2_FX_FORCE_SPAWN", "7">()    || mc2::gate::is<"MC2_FX_FORCE_SPAWN", "8">() ||
		mc2::gate::is<"MC2_FX_FORCE_SPAWN", "9">();
    // Stock Point emission is zero; populate it only for the existing oracle.
    if (mc2::gate::is<"MC2_FX_FORCE_SPAWN", "9">())
        gosFX::PointCloud::PrepareOracle();

	if (s_fxLoop) {
		static float s_nextFire = 0.0f;
		if (myVehicle && scenarioTime > 5.0f && scenarioTime >= s_nextFire && myVehicle->canFireWeapons()) {
			glm::vec3 pos = myVehicle->getPosition();
			glm::vec3 targetPoint = pos;
			targetPoint.x += 150.0f;
			int firedCount = 0;
			for (long cw = 0; cw < myVehicle->numWeapons; cw++) {
				long wi = myVehicle->numOther + cw;
				float dmgDone = 0.0f;
				if (NO_ERR == myVehicle->fireWeapon(nullptr, scenarioTime, wi,
						ATTACK_TO_DESTROY, -1, &targetPoint, dmgDone))
					firedCount++;
			}
			if (firedCount > 0) {
				s_nextFire = scenarioTime + 0.05f;   // ~20 volleys/sec -> a live bolt every frame
				mc2::fmt::print_out("[FX_FORCE_SPAWN loop] event=mech_fire wid={:d} weapons={:d} pos={:.0f},{:.0f},{:.0f} scenarioTime={:.2f}\n",
					mc2::fmt::as_i32(myVehicle->getWatchID()), mc2::fmt::as_i32(firedCount), pos.x, pos.y, pos.z, scenarioTime);
				fflush(stdout);
				// VK-FIELD-VFX-5: fire-event capture arm. With
				// MC2_VK_PRESENT_DUMP_ON_FIRE set, the FIRST loop volley arms the
				// present-PPM pin ~0.5s after itself, so the capture lands
				// mid-volley instead of pin-frame roulette against the mission's
				// intro/scenario clock. Consumed by verifyReadbackContent
				// (engine_present_vk.cpp) + the gameosmain verify window.
				extern uint32_t g_mc2RenderFrame;   // g_mc2FxFireCaptureFrame is a RENDER-clock frame
				extern uint32_t g_mc2FxFireCaptureFrame;
				// VK-FIELD-VFX-8: MC2_VISUAL_CAPTURE_ON_FIRE arms the same pin for
				// the GL engine capture (gos_visual_capture.cpp), so GL/VK pairs
				// capture the same fire-relative instant.
				static const bool s_dumpOnFire =
					(mc2::gate::get<"MC2_VK_PRESENT_DUMP_ON_FIRE">() ||
					 mc2::gate::get<"MC2_VISUAL_CAPTURE_ON_FIRE">());
				if (s_dumpOnFire && g_mc2FxFireCaptureFrame == 0) {
					g_mc2FxFireCaptureFrame = g_mc2RenderFrame + 30u;
					mc2::fmt::print_out("[FX_FORCE_SPAWN loop] event=capture_armed frame={:d} pin={:d}\n", mc2::fmt::as_u32(g_mc2RenderFrame), mc2::fmt::as_u32(g_mc2FxFireCaptureFrame));
					fflush(stdout);
				}
			}
		}
	} else if (s_fxForceSpawn) {
		// Fire ALL weapons (not just energy): tube ribbons come from PPC AND
		// missiles (SRM/LRM/TBolt) -- the energy-only filter both missed the
		// missile tubes and caught non-tube lasers. Spread across up to 8
		// DISTINCT mechs (per-WID latch) so at least one with a tube weapon
		// fires, then stop. dmgDone/target=NULL -> FX only, no real damage.
		static long s_firedWIDs[8];
		static int  s_firedMechs = 0;
		if (s_firedMechs < 8 && myVehicle && scenarioTime > 5.0f && myVehicle->canFireWeapons()) {
			long wid = myVehicle->getWatchID();
			bool already = false;
			for (int i = 0; i < s_firedMechs; i++)
				if (s_firedWIDs[i] == wid) { already = true; break; }
			if (!already) {
				glm::vec3 pos = myVehicle->getPosition();
				glm::vec3 targetPoint = pos;
				targetPoint.x += 150.0f;   // a point in front; any on-map point spawns a bolt
				int firedCount = 0;
				for (long cw = 0; cw < myVehicle->numWeapons; cw++) {
					long wi = myVehicle->numOther + cw;
					float dmgDone = 0.0f;
					if (NO_ERR == myVehicle->fireWeapon(nullptr, scenarioTime, wi,
							ATTACK_TO_DESTROY, -1, &targetPoint, dmgDone))
						firedCount++;
				}
				if (firedCount > 0) {
					s_firedWIDs[s_firedMechs++] = wid;
					mc2::fmt::print_out("[FX_FORCE_SPAWN v1] event=mech_fire mech={:d} wid={:d} weapons={:d} pos={:.0f},{:.0f},{:.0f} scenarioTime={:.2f}\n", mc2::fmt::as_i32(s_firedMechs), mc2::fmt::as_i32(wid), mc2::fmt::as_i32(firedCount), pos.x, pos.y, pos.z, scenarioTime);
					fflush(stdout);
				}
			}
		}
	}

	// MC2_AIMED_FIRE_FIXTURE (ISSUE-9 probe, default-OFF = byte-identical):
	// headless called-shot driver. Every home-team mech without an active combat
	// order is issued an aimed-HEAD attack (ATTACK_TO_DESTROY / RANGED /
	// OPTIMAL, aim = MECH_BODY_LOCATION_HEAD, pursue) at the nearest live enemy
	// mech. Origin MUST be ORDER_ORIGIN_COMMANDER: orderAttackObject only
	// installs the tac order (setGeneralTacOrder -> curTacOrder) for COMMANDER
	// origin — with PLAYER origin only lastTarget is set and fire is UNAIMED
	// (aimLocation flows solely from curTacOrder.attackParams in
	// combatDecisionTree). Re-issues when the order clears (target died), so a
	// run with MC2_DIAG_TAGS=COMBAT yields a hit-location distribution ("loc"
	// field) to A/B MC2_CALLED_SHOT_FIX. Global 2 s throttle bounds order spam.
	static const bool s_aimedFireFixture =
		mc2::gate::get<"MC2_AIMED_FIRE_FIXTURE">();
	if (s_aimedFireFixture && myVehicle && myVehicle->isMech() &&
		Team::home && (myVehicle->getTeam() == Team::home) &&
		(scenarioTime > 5.0f) && !curTacOrder.isCombatOrder() &&
		myVehicle->canFireWeapons()) {
		static float s_nextAimedOrderTime = 0.0f;
		if (scenarioTime >= s_nextAimedOrderTime) {
			s_nextAimedOrderTime = scenarioTime + 2.0f;
			MoverPtr enemy = nullptr;
			float bestDist = 1e30f;
			// Mover iteration (not getMech): warrior.cpp only sees the BattleMech
			// forward declaration (mech.h is USE_MECHS-gated here).
			for (long mi = 0; mi < ObjectManager->getNumMovers(); mi++) {
				MoverPtr pM = ObjectManager->getMover(mi);
				if (!pM || !pM->isMech() || pM->isDisabled() || pM->isDestroyed())
					continue;
				if (pM->getTeamId() == Team::home->getId())
					continue;
				float d = myVehicle->distanceFrom(pM->getPosition());
				if (d < bestDist) {
					bestDist = d;
					enemy = pM;
				}
			}
			if (enemy) {
				long res = orderAttackObject(false, ORDER_ORIGIN_COMMANDER, enemy,
					ATTACK_TO_DESTROY, ATTACKMETHOD_RANGED, FIRERANGE_OPTIMAL,
					MECH_BODY_LOCATION_HEAD, -1, TACORDER_PARAM_PURSUE);
				mc2::fmt::print_out("[AIMED_FIRE v1] event=order wid={:d} tgt_wid={:d} dist={:.0f} res={:d}\n", mc2::fmt::as_i32(static_cast<long>(vehicleWID)), mc2::fmt::as_i32(static_cast<long>(enemy->getWatchID())), static_cast<double>(bestDist), mc2::fmt::as_i32(res));
				fflush(stdout);
			}
		}
	}

	if (combatUpdate <= scenarioTime)
		combatDecisionTree();

	movementDecisionTree();
}

//extern L_INTEGER startCk;
//extern L_INTEGER endCk;
//extern long		 srWAblUpd;
