// BattleMech: BattleMech::update, render, renderShadows, hit-location and ejection helpers.
// Declared in the class headers; shared names in mech_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "mech_internal.h"
#include "hover_name.h"
#include "tacticaloverview.h"
#include "appearance_rng.h"
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "gos_profiler.h"

// MC2_GPU_CULL_LIFECYCLE=1 routes AI canBeSeen() combat gates to GPU-lagged visibility (same env var as objmgr.cpp).
// Read at static-init like the other readers, before any in-main env application.
static const bool s_gpuCullLifecycle = false;


constexpr int GestureGoalFallenBackward = 8;

long BattleMech::update (void)
{
    ZoneScopedN("GameLogic.Mech.Update");
    unitAnimation = appearance->pinnedSimulationAnimation();
    simulationPose = appearance->simPose;

	refreshPositionNormal();

	getPilot()->getIndex();

	playedCriticalHit = false;

	// What the appearance's draw side needs from this tick is decided below and applied by applyAppearancePush in presentation.
	bool statusPushed = false;
	long pushedStatus = 0;

	if (isDestroyed() || isDisabled())
		setTangible(false);

	if (withdrawing && (pilot->getStatus() == WARRIOR_STATUS_WITHDRAWN)) {
		setTangible(false);
		return(1);		//This guy is gone.
	}
	
	//Blow off the arms if we need to.
	// MUST be here so that they get blown off regardless of mech status
   	if (leftArmBlownThisFrame) {
   		leftArmBlownThisFrame = false;
   		// Frank says, blow the right one anyway --gd
   		control.settings.mech.blowRightArm = true;
   	}

   	if (rightArmBlownThisFrame) {
   		rightArmBlownThisFrame = false;
   		// Frank says, blow the left one anyway --gd
   		control.settings.mech.blowLeftArm = true;
   	}

   	if (control.settings.mech.blowLeftArm) 
   	{
   		appearance->blowLeftArm();
   		soundSystem->playDigitalSample(CRITICAL_HIT_SFX,getPosition(),true);
		control.settings.mech.blowLeftArm = false;
   	}

   	if (control.settings.mech.blowRightArm)
   	{
   		appearance->blowRightArm();
   		soundSystem->playDigitalSample(CRITICAL_HIT_SFX,getPosition(),true);
		control.settings.mech.blowRightArm = false;
   	}

	if (!isDestroyed() && !isDisabled())
	{
		#ifdef MC_PROFILE
		QueryPerformanceCounter(startCk);
		#endif

		calcLegStatus();
		
		updatePathLock(false);
		
		//Check if we are SHUTDOWN and NOT onGui.
		// If we are both, we probably are waiting to be linked up with.
		// See if anything on our side is close enough to link up with us.
		// If it is close enough, powerup and ourselves to the GUI!
		// ISSUE-50: gate on !isOnGUI() alone (was status==SHUTDOWN &&
		// !isOnGUI()). If a hidden link-up mech ever got powered before the
		// team walked into LOS (pre-fix selection hole in moverInRect let
		// select-all grab it; any order auto-powers a SHUTDOWN mech), status
		// left SHUTDOWN and this branch could never fire again — the mech
		// was latched invisible for the rest of the mission. Keying on
		// !isOnGUI() lets an already-powered link-up mech still self-heal
		// (corePower(true) is idempotent). Scope unchanged in practice:
		// enemies and normal deploys spawn onGUI (mission.cpp), only
		// link-up reinforcements are ever off-GUI here.
		if (!isOnGUI())
		{
			// ISSUE-50 diagnostics: MC2_LINKUP_TRACE=1 logs each off-GUI
			// home mover's link-up state ~once/sec so the latch is
			// observable in the field. Default OFF = no output.
			static const bool s_linkupTrace = false;
			const bool linkupLos =
				Team::home->teamLineOfSight(position, getAppearRadius());
			if (s_linkupTrace && ((turn & 31) == 0))
			{
				printf("[LINKUP] part=%d name=%s status=%d onGUI=0 los=%d pos=(%.0f,%.0f)\n",
					static_cast<int>(getPartId()), getName() ? getName() : "(null)",
					static_cast<int>(status), linkupLos ? 1 : 0,
					position.x, position.y);
				fflush(stdout);
			}
			if (linkupLos)
			{
				if (s_linkupTrace)
				{
					printf("[LINKUP] linkup-complete part=%d name=%s\n",
						static_cast<int>(getPartId()), getName() ? getName() : "(null)");
					fflush(stdout);
				}
				mission->missionInterface->addMover(this);
				setSelected(1);
				pilot->corePower(true);
				setOnGUI(true);
				// Mod-tolerance: MC2X stub-substituted chassis can reach
				// update() with sensorSystem still NULL (no COMPONENT_FORM_SENSOR
				// in inventory, and the init-time fallback at BattleMech::init
				// could not allocate). Writing ->broken on a NULL sensor is a
				// WRITE at offset 0x28 (the 'broken' member) — the recurring
				// MC2X mid-campaign crash. Skip the flag write when absent.
				if (sensorSystem)
					sensorSystem->broken = false;
			}
			else
			{
				if (sensorSystem)
					sensorSystem->broken = true;
			}
		}
		
		if ((status == OBJECT_STATUS_SHUTDOWN) || (status == OBJECT_STATUS_SHUTTING_DOWN))
			appearance->setGestureGoal(MECH_STATE_PARKED);

		if (getAwake() && !isDisabled() && getTeam() && !godMode) 
		{
			markDistanceMoved = 0.0;
		}

		if (timeToClearSelection != 0.0 && scenarioTime > timeToClearSelection)
		{
			timeToClearSelection = 0.0;
			setSelected(false);
			setTargeted(false);
		}

		if (g_moveReconEnabled) { g_moveRecon_movers_total++; g_moveRecon_frame_movers++; }
		{ MoveReconScope _recon_ctrl(&g_moveRecon_ctrl_ns, &g_moveRecon_frame_ctrl_ns);
		{ ZoneScopedN("GameLogic.Mech.Control"); control.update(this); }
		}

		if (!getAwake()) {
			appearance->setGestureGoal(MECH_STATE_PARKED);
			shutDownThisFrame = false;
		}
		
		#ifdef MC_PROFILE
		QueryPerformanceCounter(endCk);
		srCtrlUpd += (endCk.LowPart - startCk.LowPart);
		#endif

		#ifdef MC_PROFILE
		QueryPerformanceCounter(startCk);
		#endif


		bool emergencyStop = false;
		if (!isDisabled()) {
			MoverPtr ramTarget = nullptr;
			if ((getPilot()->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_OBJECT) && (getPilot()->getCurTacOrder()->attackParams.method == ATTACKMETHOD_RAMMING))
				ramTarget = static_cast<MoverPtr>(getPilot()->getCurTacOrder()->getRamTarget());
			if (ramTarget)
				ramTarget->updatePathLock(false);

			emergencyStop = crashAvoidanceSystem();

			if (ramTarget)
				ramTarget->updatePathLock(true);
		}

		#ifdef MC_PROFILE
		QueryPerformanceCounter(endCk);

		srDyneUpd += (endCk.LowPart - startCk.LowPart);
		#endif

		// At this point, the other updates have completed, its time to
		// apply the velocity and rotations to the mech.
		// NOTE:
		// All positions in the game are in World Units.  All velocities
		// are in Meters/sec.  This is to provide easy data entry for designers
		// We can't store positions as meters because terrain tiles are fixed
		// pixel(world unit) sizes which would require a boatload of math
		// with its crazy floating point inconsistencies.  This will mess up
		// the terrain.  So, whenever you want a position derived from
		// a velocity, multiply velocity by worldUnitsPerMeter.

		#ifdef MC_PROFILE
		QueryPerformanceCounter(startCk);
		#endif

		if (teleportPosition.x > -999990.0) {
			setPosition(teleportPosition);
			teleportPosition.x = -999990.0;
			teleportPosition.y = -999990.0;
			teleportPosition.z = -999990.0;
		}

		glm::vec3 newPosition;
		glm::vec3 vel = getRotationVector();

		if (!appearance->isJumpAirborne())
		{
			float velMag = appearance->getVelocityMagnitude() * engineSpeedScale();
			if (emergencyStop)
				velMag = 0.0;
	
			velMag *= 0.01f * control.settings.mech.throttle;
			
			float velFactor = 1.0;
				float velAngle = land->getTerrainAngle(position);
				velAngle *= DEGREES_TO_RADS;
				velFactor = cos(velAngle);
	
			float velMult = velMag * velFactor * frameLength;
			velMult = std::min(velMult, DistanceToWaypoint);
			vel *= velMult * worldUnitsPerMeter;
			setVelocity(vel);
	
			newPosition = position + vel;
			newPosition.z = TerrainRuntime::groundElevation(newPosition);
			if ((getMoveType() == MOVETYPE_AIR) &&
				(newPosition.z < MapData::waterDepth))
				newPosition.z = MapData::waterDepth;
		}
		else
		{
			vel = appearance->getVelocity();
			vel *= frameLength;
			setVelocity(vel);
			
			newPosition = position + vel;
			newPosition.z = TerrainRuntime::groundElevation(newPosition);
		}

		if (newMoveChunk) {
			if (!statusChunk.jumpOrder) {
				glm::vec3 correctPos;
				land->cellToWorld(moveChunk.stepPos[0][0], moveChunk.stepPos[0][1], correctPos);
				correctPos.z = 0.0;
				glm::vec3 curPos = position;
				curPos.z = 0.0;
				curPos = curPos - correctPos;
				float posDelta = glm::length(curPos);
				if (posDelta > MPlayer->warpFactor)
					newPosition = correctPos;
				if (!GameMap->inBounds(moveChunk.stepPos[0][0], moveChunk.stepPos[0][1]))
					Fatal(0, " mech.update: newMoveChunk stepPos not on map! ");
			}
			newMoveChunk = false;
		}

		setPosition(newPosition);
		updateTorso(NewRotation);

		//Don't let them move if they are shutdown!!
		if (status != OBJECT_STATUS_SHUTDOWN)
			updateDynamics();

		markDistanceMoved += glm::length(vel);

		if (!isDisabled())
			updatePathLock(true);

		mineCheck();

		float zPos = TerrainRuntime::groundElevation(position);
		position.z = zPos;
		if ((getMoveType() == MOVETYPE_AIR) &&
			(zPos < MapData::waterDepth))
			position.z = MapData::waterDepth;


		#ifdef MC_PROFILE
		QueryPerformanceCounter(endCk);

		srObjtUpd += (endCk.LowPart - startCk.LowPart);
		#endif
		
		#ifdef MC_PROFILE
		QueryPerformanceCounter(startCk);
		#endif

			
		if (withdrawing) unitAnimation->withdrawSeconds += frameLength;
        else unitAnimation->withdrawSeconds = 0;
		if (withdrawing && unitAnimation->withdrawSeconds >= 3.0f && (pilot->getStatus() != WARRIOR_STATUS_WITHDRAWN))
			ObjectManager->getObjectType(typeHandle)->handleDestruction(this, nullptr);

		if (appearance) {
			MovePathPtr path = pilot->getMovePath();
			bool brake = true;
			if (path && path->numSteps && (path->curStep < path->numSteps))
				brake = false;
			appearance->setBrake(brake);
			if ((pilot->getCurrentTarget()) || (pilot->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_OBJECT) || (pilot->getCurTacOrder()->code == TACTICAL_ORDER_ATTACK_POINT))
				appearance->setCombatMode(true);
			else
				appearance->setCombatMode(false);

			if (isJumping())
				setTangible(false);
			else if (!isDestroyed() && !isDisabled())
				setTangible(true);
		}

		#ifdef MC_PROFILE
		QueryPerformanceCounter(endCk);

		srApprUpd += (endCk.LowPart - startCk.LowPart);
		#endif

		#ifdef MC_PROFILE
		QueryPerformanceCounter(endCk);

		srObjtUpd += (endCk.LowPart - startCk.LowPart);
		#endif
		
		if (getDebugFlag(OBJECT_DFLAG_DISABLE))
			disable(DEBUGGER_DEATH);
	}
	else if (isDisabled() && !isDestroyed())
	{
		updatePathLock(false);

		control.settings.mech.gestureGoal = -1;
		updateDynamics();

		float velMag = appearance->getVelocityMagnitude() * engineSpeedScale();

		glm::vec3 vel = getRotationVector();
		vel *= velMag * worldUnitsPerMeter;
		vel *= frameLength;
		setVelocity(vel);

		glm::vec3 newPosition;
		newPosition = position + vel;
		newPosition.z = TerrainRuntime::groundElevation(newPosition);

		setPosition(newPosition);

	
		if (appearance)
		{
			appearance->setCombatMode(false);
		}
	
		if (!fallen)
		{
			fallen = appearance->haveFallen();
		}

		if (!startDisabled)
		{
			timeLeft -= frameLength;
			if ((timeLeft < 1.0f) && !exploding)
			{
				// Disable just plays an eject effect now.
				// NO crater or it will look dumb.
				// DO NOT play disable effect if pilot is dead!!
				if (getPilot()->alive())
				{
					glm::vec3 cockpitPos = appearance->getNodeNamePosition("cockpit");
					ObjectManager->createExplosion(MECH_DISABLE_EXPLOSION_ID,nullptr,cockpitPos,0.0f,0.0f);
				}

				appearance->startSmoking(-1);
				exploding = true;
			}
		}
		
		if (getMoveType() == MOVETYPE_AIR)
		{
			setStatus(OBJECT_STATUS_DESTROYED);
			timeLeft = 0.0f;		
		}
	}
	else
	{
		control.settings.mech.gestureGoal = -1;
		updateDynamics();

		float velMag = appearance->getVelocityMagnitude() * engineSpeedScale();

		glm::vec3 vel = getRotationVector();
		vel *= velMag * worldUnitsPerMeter;
		vel *= frameLength;
		setVelocity(vel);

		glm::vec3 newPosition;
		newPosition = position + vel;
		newPosition.z = TerrainRuntime::groundElevation(newPosition);

		setPosition(newPosition);

		if (!playedHeloDestruct)
		{
			soundSystem->playDigitalSample(HELICOPTER_DEATH,getPosition());
			playedHeloDestruct = true;
		}

	
		if (appearance)
		{
			appearance->setGestureGoal(GestureGoalFallenBackward);

			
			appearance->setCombatMode(false);
		}

		if (!fallen)
		{
			fallen = appearance->haveFallen();
			timeLeft = 1.0f;
		}

		if (fallen)
		{
			timeLeft -= frameLength;
			if ((timeLeft < 0.4) && !exploding)
			{
				glm::vec3 cockpitPos = appearance->getNodeNamePosition("cockpit");
				if (getMoveType() != MOVETYPE_AIR)
					ObjectManager->createExplosion(MECH_EXPLOSION_ID,nullptr,cockpitPos,(static_cast<BattleMechTypePtr>(getObjectType()))->explDmg,(static_cast<BattleMechTypePtr>(getObjectType()))->explRad);
				else
					ObjectManager->createExplosion(VEHICLE_EXPLOSION_ID,nullptr,cockpitPos,0.0f,0.0f);

				exploding = true;
			}
			else if (((timeLeft < -2.5f) && !mechRemoved && (getMoveType() != MOVETYPE_AIR)) ||
					 ((timeLeft < -1.5f) && !mechRemoved && (getMoveType() == MOVETYPE_AIR)))
			{
				mechRemoved = true;
			}
			else if (mechRemoved)
			{
				statusPushed = true;
				pushedStatus = getStatus();
			}
		}
	}


	// Feed the sim half of the appearance: the animation state machine and SimPose read these sim copies, never the draw state.
	// Everything the draw side needs (names, pose, status, alpha, sensor level, bars, visual state) is pushed by presentation
	// from this mech's state: see applyAppearancePush. The tick no longer writes it.
	if (!mechRemoved && !isDestroyed())
	{
		statusPushed = true;
		pushedStatus = getStatus();
	}
	{
		Appearance::SimInput simInput;
		simInput.position = position;
		simInput.rotation = rotation;
		simInput.torsoRotation = torsoRotation;
		simInput.hasStatus = statusPushed;
		simInput.status = pushedStatus;
		simInput.debugMove = (control.getType() == CONTROL_PLAYER);
		appearance->setSimInput(simInput);
	}

	// Water wake is a request on the sim animation state (the effect is created in presentation).
	int watercellR, watercellC;
	land->worldToCell(position,watercellR, watercellC);
	if (GameMap->getDeepWater(watercellR, watercellC) ||
		GameMap->getShallowWater(watercellR, watercellC))
	{
		appearance->startWaterWake();
	}
	else
	{
		appearance->stopWaterWake();
	}

	AppearancePush push;
	push.hasStatus = statusPushed;
	push.status = pushedStatus;

	//Must save sensor data HERE now so fades happen OK.
	if (getTeamId() != Team::home->getId())
	{
		conStat = getContactStatus(Team::home->getId(), true);

		if ((conStat == CONTACT_VISUAL) || isDestroyed() || isDisabled() || ShowMovers || (MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]))
		{
			if (alphaValue != 0xff)
			{
				fadeTime += frameLength;
				if (fadeTime > ContactFadeTime)
				{
					fadeTime = ContactFadeTime;
					alphaValue = 0xff;
				}
				else
				{
					float fadeValue = fadeTime / ContactFadeTime * 255.0f;
					alphaValue = fadeValue;
				}
			}

			push.hasAlpha = true;
			push.sensorLevel = 0;
		}
		else	//Pretty much anything else is fading out.
		{
			if (alphaValue != 0x0)
			{
				fadeTime -= frameLength;
				if (fadeTime < 0.0f)
				{
					fadeTime = 0.0f;
					alphaValue = 0x0;
				}
				else
				{
					float fadeValue = fadeTime / ContactFadeTime * 255.0f;
					alphaValue = fadeValue;
				}
			}

			push.hasAlpha = true;

			if (conStat == CONTACT_SENSOR_QUALITY_1)
				push.sensorLevel = 1;
			else if (conStat == CONTACT_SENSOR_QUALITY_2)
				push.sensorLevel = 2;
			else if (conStat == CONTACT_SENSOR_QUALITY_3)
				push.sensorLevel = 3;
			else if (conStat == CONTACT_SENSOR_QUALITY_4)
				push.sensorLevel = 4;
		}
	}
	else
	{
		if (isOnGui)
		{
			alphaValue = 0xff;
			push.sensorLevel = 5;
			push.hasAlpha = true;
		}
	}

	// Sim half only: gesture state machine, frame advance, SimPose. update() returns before any draw work for a battle mech.
	{ ZoneScopedN("GameLogic.Mech.SimPose"); appearance->update(); }

	appearancePush = push;
	++pushSerial;

	//Must make room for holdFireIcons on HUD if this mech might need one!	
	if (isOnGui && !mechRemoved && (attackRange == FIRERANGE_CURRENT) && !isDisabled() )
	{
		mc2::renderSubmissions().addTriangle(holdFireIconHandle,MC2_ISHUDLMNT);
		mc2::renderSubmissions().addTriangle(holdFireIconHandle,MC2_ISHUDLMNT);
	}
	
	return(1);		//Mech is still around,  false means whack this mech from list.
}

// Presentation phase: apply what the last update() decided to the draw side of the appearance, in the order the tick used to,
// before the presentation loop's recalcBounds / pose blend / update(false). A frame with no new tick applies nothing.
void BattleMech::applyAppearancePush (void)
{
	if (pushSerial == pushPresented)
		return;
	pushPresented = pushSerial;

	auto* objectAppearance = static_cast<ObjectAppearance*>(appearance);
	if ( getTeam() == Team::home )
	{
		objectAppearance->pilotNameID = getPilot()->descID;
		objectAppearance->pilotName[0] = 0;
		appearance->setMechName( variantName );
	}
	else if ( MPlayer )
	{
		strcpy( objectAppearance->pilotName, (commanderId > -1) ? MPlayer->playerInfo[commanderId].name : " " );
	}
	else
		objectAppearance->pilotNameID = -1;
	if (isDisabled() && !isDestroyed())
		objectAppearance->pilotNameID = IDS_NOPILOT;

	long homeRelations = 2;
	if (getTeam() == Team::home) {
		if (getCommander() == Commander::home)
			homeRelations = 0;
		else
			homeRelations = 1;
	}
	appearance->setObjectParameters(position, rotation, TacticalOverview::maskWorldBars(drawFlags), getTeamId(), homeRelations);

	if (getMoveType() == MOVETYPE_AIR)
		appearance->setMoverParameters(torsoRotation,0.0f,0.0f,true);
	else
		appearance->setMoverParameters(torsoRotation);

	if (appearancePush.hasStatus)
		appearance->setObjStatus(appearancePush.status);

	if (control.getType() == CONTROL_PLAYER)
		appearance->setDebugMoveMode();

	appearance->setVisibility(true,true);

	// GAMEADAPTERS-VISUAL-STATE-BRIDGE-1 (Slice 1): mirror player-visible mech
	// visual facts into the renderer-owned MechVisualState. Sanitized here on
	// the game side; the renderer only ever sees the clamped by-value copy.
	// No GL, no renderer access -- same feed pattern as setObjStatus() above.
	// heat01 stays 0: the MechWarrior heat sim is compiled out (USEHEAT never
	// defined), so there is no live heat to read. See RenderCore/MechVisualState.h.
	{
		RenderCore::MechVisualState vs;
		vs.heat01   = 0.0f;
		// getStatusRating() returns ~1.0 pristine .. 0.0 wrecked. It divides by
		// per-location maxArmor with no zero guard, so the result can be NaN/Inf
		// on edge mechs -- sanitizeMechVisual01() maps any non-finite to 0.
		vs.damage01 = RenderCore::sanitizeMechVisual01(1.0f - getStatusRating());
		uint32_t flags = 0u;
		if (getSelected())  flags |= RenderCore::kMechVisualFlag_Selected;
		const long st = getStatus();
		if (st == OBJECT_STATUS_SHUTDOWN)  flags |= RenderCore::kMechVisualFlag_Shutdown;
		if (st == OBJECT_STATUS_DISABLED)  flags |= RenderCore::kMechVisualFlag_Disabled;
		if (st == OBJECT_STATUS_DESTROYED) flags |= RenderCore::kMechVisualFlag_Destroyed;
		flags = RenderCore::packMechRelation(flags, static_cast<uint32_t>(homeRelations));
		vs.flags = flags;
		// static_cast safe: BattleMech::appearance is always Mech3DAppearance*
		// (created in BattleMech::init). setVisualState is
		// mech-specific so it is not on the Appearance base.
		static_cast<Mech3DAppearance*>(appearance)->setVisualState(vs);
	}

	if (appearancePush.hasAlpha)
		appearance->setAlphaValue(alphaValue);
	if (appearancePush.sensorLevel >= 0)
		appearance->setSensorLevel(appearancePush.sensorLevel);

	// FORCE-ALL-VISIBLE-1: single chokepoint, after every contact branch and before the appearance is drawn. No-op unless
	// MC2_FORCE_ALL_VISIBLE.
	{
		BYTE drawAlpha = alphaValue;   // presentation never writes a sim-owned field (mover.h AppearancePush contract)
		mc2_vis::forceVisibleOverride(appearance, drawAlpha);
	}

	float barStatus = getTotalEffectiveness();

	DWORD color = 0xff7f7f7f;
	if ((teamId > -1) && (teamId < 8)) {
		if (getTeam()->isFriendly(Team::home))
			color = SB_GREEN;
		else if (getTeam()->isEnemy(Team::home))
			color = SB_RED;
		else
			color = SB_BLUE;
	}

	appearance->setBarColor(color);
	appearance->setBarStatus(barStatus);
}

// G7b K3: the old render() split at the render extract. Contact status, team, effectiveness, tonnage, isOnGui and the
// fire range are read here; renderView keeps the main-only parts (GPU readback lag, appearance, icons).
void BattleMech::extractRenderView (mc2::render_view::RenderView& view)
{
	using mc2::render_view::Mode;
	GameObject::extractRenderView(view);
	contactView = mc2::contactVisibility(getContactStatus(Team::home->getId(), true), getTeamId() == Team::home->getId()
		|| isDestroyed() || isDisabled() || ShowMovers
		|| (MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]));
	view.contact = contactView;
	if (!Terrain::IsGameSelectTerrainPosition(position))
		return;
	if (getTeamId() != Team::home->getId())
	{
		long cStat = contactView.quality;

		if (contactView.model)
		{
			DWORD color = 0xff7f7f7f;
			if ((teamId > -1) && (teamId < 8)) {
				if (getTeam()->isFriendly(Team::home))
					color = SB_GREEN;
				else if (getTeam()->isEnemy(Team::home))
					color = SB_RED;
				else
					color = SB_BLUE;
			}
			view.mode = Mode::Full;
			view.barColor = color;
			view.barStatus = getTotalEffectiveness();
			view.nameId = descID;
		}
		else if ((cStat == CONTACT_SENSOR_QUALITY_1) || (cStat == CONTACT_SENSOR_QUALITY_2))
		{
			view.mode = Mode::Sensor;
		}
		else if (cStat == CONTACT_SENSOR_QUALITY_3)
		{
			view.mode = Mode::SensorText;
			view.nameId = mc2::hoverNameId(cStat, tonnage, descID);
		}
		else if (cStat == CONTACT_SENSOR_QUALITY_4)
		{
			view.mode = Mode::SensorText;
			view.nameId = mc2::hoverNameId(cStat, tonnage, descID);
		}
	}
	else if (isOnGui)
	{
		DWORD color = 0x0000ff00;
		if (getTeamId() == Team::home->getId())
		{
			//Two Possibilities.  Either we have the same commander or we don't.
			// If we do, we are on the same "team", if not we are ALLIES!!!
			if (getCommanderId() != Commander::home->getId())
			{
				color = 0x000000ff;
			}
		}
		else
		{
			color = 0x00ff0000;
		}
		view.mode = Mode::Own;
		view.barColor = color;
		view.barStatus = getTotalEffectiveness();
		view.nameId = descID;
		if (attackRange == FIRERANGE_CURRENT && !isDisabled())
			view.flags |= mc2::render_view::FireIcon;
	}
}

void BattleMech::renderView (const mc2::render_view::RenderView& view)
{
	appearance->contactView = view.contact;
	appearance->setSensorLevel(view.contact.sensorLevel());
	// Native opaque bodies cannot implement the old alpha fade. Admit model
	// and HUD atomically on visual acquisition, including the first frame.
	appearance->setAlphaValue(view.contact.model ? 0xff : 0);
	using mc2::render_view::Mode;
	switch (view.mode)
	{
		case Mode::Full:
			appearance->setBarColor(view.barColor);
			appearance->setBarStatus(view.barStatus);
			appearance->setObjectNameId(view.nameId);
			appearance->setVisibility(true,true);
			appearance->render();
			break;

		case Mode::Sensor:
			appearance->setBarColor(SB_RED);
			appearance->setVisibility(true,true);
			appearance->render();
			break;

		case Mode::SensorText:
		{
			appearance->setBarColor(SB_RED);
			appearance->setVisibility(true,true);
			appearance->render();

			// C3: route canBeSeen() to GPU-lagged visibility; 1-frame HUD lag: accepted.
			const bool mechInView = s_gpuCullLifecycle
				? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(getHandle()))
				: appearance->canBeSeen();
			if ( mechInView )
				drawSensorTextHelp (appearance->getScreenPos().x, appearance->getScreenPos().y+20.0f, view.nameId,SD_RED,false);
			break;
		}

		case Mode::Own:
		{
			appearance->setBarColor(view.barColor);
			appearance->setBarStatus(view.barStatus);
			appearance->setObjectNameId(view.nameId);
			appearance->setVisibility(true,true);
			appearance->setSensorLevel(0);
			appearance->render();
			// C3: route canBeSeen() to GPU-lagged visibility; 1-frame fire-icon lag: accepted.
			const bool mechInViewFire = s_gpuCullLifecycle
				? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(getHandle()))
				: appearance->canBeSeen();
			if ( view.has(mc2::render_view::FireIcon) && mechInViewFire )
				appearance->drawIcon( holdFireIconHandle, 5, 5, view.barColor | 0xff000000 );
			break;
		}

		default:
			break;
	}

	// Debug-only path overlay: still reads the pilot's live move path (MC2 debug key; not on in normal play).

	if (drawTerrainGrid)
	{
		MovePathPtr path = pilot->getMovePath();
		gosASSERT(path != nullptr);
		if (path->numSteps)
		{
			Stuff::Vector4D lineStart, lineEnd;
			for (long i=0;i<path->numSteps;i++)
			{
				if (i != (path->numSteps-1))
				{
					glm::vec3 startPos = path->stepList[i].destination;
					glm::vec3 endPos = path->stepList[i+1].destination;
					startPos.z = TerrainRuntime::groundElevation(startPos);
					endPos.z = TerrainRuntime::groundElevation(endPos);
					
					// [PROJECTZ:ScreenXYOracle id=mech_path_step_start]
					eye->projectForScreenXY(startPos,lineStart);
					// [PROJECTZ:ScreenXYOracle id=mech_path_step_end]
					eye->projectForScreenXY(endPos,lineEnd);

					lineStart.z = lineEnd.z = HUD_DEPTH;

					LineElement newElement(lineStart,lineEnd,SD_BLUE,nullptr,-1);
					newElement.draw();
				}
			}
		}
	}
}

void BattleMech::renderShadows (void)
{
	if (Terrain::IsGameSelectTerrainPosition(position))
	{
		if ((appearance->getCurrentGestureId() == 14) ||
			(appearance->getCurrentGestureId() == 15) ||  
			(appearance->getCurrentGestureId() >= 21))
		{
			return;			//No shadows on a fall!
		}
			
		if (contactView.shadow)
			appearance->renderShadows();
	}
}	

float BattleMech::relFacingTo (glm::vec3 goal, long bodyLocation) {

	float relFacing = Mover::relFacingTo(goal);

	switch (bodyLocation) {
		case MECH_BODY_LOCATION_HEAD:
		case MECH_BODY_LOCATION_CTORSO:
		case MECH_BODY_LOCATION_LTORSO:
		case MECH_BODY_LOCATION_RTORSO:
			relFacing += torsoRotation;
			break;
		case MECH_ARMOR_LOCATION_RCTORSO:
		case MECH_ARMOR_LOCATION_RLTORSO:
		case MECH_ARMOR_LOCATION_RRTORSO:
			relFacing += (torsoRotation /* + 180.0*/);
			break;
		case MECH_BODY_LOCATION_LARM:
			relFacing += (torsoRotation + leftArmRotation);
			break;
		case MECH_BODY_LOCATION_RARM:
			relFacing += (torsoRotation + rightArmRotation);
			break;
		case MECH_BODY_LOCATION_RLEG:
		case MECH_BODY_LOCATION_LLEG:
			break;
		default:
			// Facing for a mech is ALWAYS based on Torso Rotation by default!!!
			//relFacing += torsoRotation;
			break;
	}
	
	return(relFacing);

}


long BattleMech::getBodyState (void) {

	long gID = appearance->getCurrentGestureId();
	return(MechStateByGesture[gID]);
}

bool BattleMech::isWeaponReady (long weaponIndex) {

	if (inventory[weaponIndex].disabled || !(inventory[weaponIndex].health))
		return(false);
	else if (inventory[weaponIndex].readyTime > scenarioTime)
		return(false);
	return(true);
}

float BattleMech::calcAttackChance (GameObjectPtr target, long aimLocation, float targetTime, long weaponIndex, float modifiers, long* range, glm::vec3* targetPoint) {

	if ((weaponIndex < numOther) || (weaponIndex >= numOther + numWeapons))
		return(-1000.0);


	float attackChance = Mover::calcAttackChance(target, aimLocation, targetTime, weaponIndex, modifiers, range, targetPoint);

	return(attackChance);
}

long BattleMech::calcHitLocation (GameObjectPtr attacker, long weaponIndex, long attackSource, long attackType) {

	// Need to use the Line-of-fire params to calc section and arc of hit.
	// For now, just use the arc of entry to the target, and select section
	// without using obstructions...
	long armorLocation = -1;

	long section = MechHitSectionTable[attackSource];

	float entryAngle = 0.0;
	if (attacker)
		entryAngle = relFacingTo(attacker->getPosition());
	else
		entryAngle = RandomNumber(360) - 180;
	long arc;

	if ((entryAngle >= -45.0) && (entryAngle <= 45.0))
		arc = MECH_HIT_ARC_FRONT;
	else if ((entryAngle  > -135.0) && (entryAngle < -45.0))
		arc = MECH_HIT_ARC_LEFT;
	else if ((entryAngle > 45.0) && (entryAngle < 135))
		arc = MECH_HIT_ARC_RIGHT;
	else
		arc = MECH_HIT_ARC_REAR;

	if (attackSource == ATTACKSOURCE_MINE) // if this came from a mine
	{
		if (getAppearance()->getOldGestureGoal() == MECH_STATE_FALLEN_FORWARD)
		{
			arc = MECH_HIT_ARC_FRONT;
			section = MECH_HIT_SECTION_MIDDLE;
		}
		if (getAppearance()->getOldGestureGoal() == MECH_STATE_FALLEN_BACKWARD)
		{
			arc = MECH_HIT_ARC_REAR;
			section = MECH_HIT_SECTION_MIDDLE;
		}
	}

	long locationRoll = RandomNumber(100);
	long startEntry = section * (NUM_MECH_ARMOR_LOCATIONS * NUM_MECH_HIT_ARCS) + (arc * NUM_MECH_ARMOR_LOCATIONS);
	for (armorLocation = 0; armorLocation < NUM_MECH_ARMOR_LOCATIONS; armorLocation++) {
		if (locationRoll < MechHitLocationTable[startEntry + armorLocation])
			break;
		locationRoll -= MechHitLocationTable[startEntry + armorLocation];
	}
	return(armorLocation);
}

long BattleMech::transferHitLocation (long hitLocation) {

	Assert((hitLocation >= 0) && (hitLocation < NUM_MECH_BODY_LOCATIONS), hitLocation, " BattleMech.transferHitLocation: bad hitLocation ");
	return(MechTransferHitTable[hitLocation]);
}

bool BattleMech::isJumping (glm::vec3* goal) {

	if (goal)
		*goal = jumpGoal;
	return(inJump);
}

float BattleMech::getJumpRange (long* numOffsets, long* jumpCost) {

	static long jetsToOffsets[6] = {8, 32, 56, 80, 104, 128};
	long numJets = numJumpJets;
	if (getPilot()->isLongJump())
		numJets++;
	if (numOffsets) {
		if (numJets > 5)
			*numOffsets = jetsToOffsets[5];
		else
			*numOffsets = jetsToOffsets[numJets];
	}
	if (jumpCost) {
		if (numJets > 0)
			*jumpCost = DefaultMechJumpCost;
		else
			*jumpCost = 0;
	}
	return(static_cast<float>(numJets) * Terrain::worldUnitsPerVertex);
}

bool BattleMech::handleFall (bool forward) {

	return(false);
}

bool BattleMech::handleEjection (void) {

	// Let's let the unit know we're gone if we're a point...
// Don't do this!!! if we loose the point, we have no point! Simple, no?
//	if (getPoint() == this) {
//		MoverPtr newPointVehicle = getGroup()->selectPoint(true);
		// If there is no new point, all units must be blown away.
		// How do we want to handle this?
//	}

	if (pilot && ((pilot->getStatus() == WARRIOR_STATUS_NORMAL) || (pilot->getStatus() == WARRIOR_STATUS_WITHDRAWING))) {
		getPilot()->eject();

		ejectOrderGiven = true;
	
		destroyBodyLocation(MECH_BODY_LOCATION_HEAD);


   		if (CombatLog) 
   		{
   			char s[1024];
			(*std::format_to_n(s, sizeof(s) - 1, "[{:.2f}] mech.Ejected HE: [{:05d}]{}", scenarioTime, mc2::fmt::as_i32(getPartId()), mc2::fmt::safe(getName())).out = '\0');
   			CombatLog->write(s);
   			CombatLog->write(" ");
   		}
		disable(EJECTION_DEATH);

#ifdef USE_IFACE
		theInterface->RemoveMech(getPartId());
#endif

#ifdef USE_MOODMUSIC
		if (alignment == homeTeam->getAlignment())
			friendlyDestroyed = true;
		else
			enemyDestroyed = true;
#endif
	}

	return(true);
}
