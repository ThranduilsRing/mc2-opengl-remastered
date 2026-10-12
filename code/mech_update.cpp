// BattleMech: BattleMech::update, render, renderShadows, hit-location and ejection helpers.
// Declared in the class headers; shared names in mech_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "mech_internal.h"
#include "appearance_rng.h"
#include "Gates/gate.h"

// MC2_GPU_CULL_LIFECYCLE=1 routes AI canBeSeen() combat gates to GPU-lagged visibility (same env var as objmgr.cpp).
// Read at static-init like the other readers, before any in-main env application.
static const bool s_gpuCullLifecycle = mc2::gate::get<"MC2_GPU_CULL_LIFECYCLE">();


constexpr int GestureGoalFallenBackward = 8;

long BattleMech::update (void)
{
	positionNormal = land->getTerrainNormal(position);

	getPilot()->getIndex();

	playedCriticalHit = false;

	if ( getTeam() == Team::home )
	{
		((ObjectAppearance*)appearance)->pilotNameID = getPilot()->descID;
		((ObjectAppearance*)appearance)->pilotName[0] = 0;
		appearance->setMechName( variantName );
	}
	else if ( MPlayer )
	{
		strcpy( ((ObjectAppearance*)appearance)->pilotName, (commanderId > -1) ? MPlayer->playerInfo[commanderId].name : " " );
	}
	else
		((ObjectAppearance*)appearance)->pilotNameID = -1;

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
			static const bool s_linkupTrace = mc2::gate::get<"MC2_LINKUP_TRACE">();
			const bool linkupLos =
				Team::home->teamLineOfSight(position, getAppearRadius());
			if (s_linkupTrace && ((turn & 31) == 0))
			{
				printf("[LINKUP] part=%d name=%s status=%d onGUI=0 los=%d pos=(%.0f,%.0f)\n",
					(int)getPartId(), getName() ? getName() : "(null)",
					(int)status, linkupLos ? 1 : 0,
					position.x, position.y);
				fflush(stdout);
			}
			if (linkupLos)
			{
				if (s_linkupTrace)
				{
					printf("[LINKUP] linkup-complete part=%d name=%s\n",
						(int)getPartId(), getName() ? getName() : "(null)");
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
			BattleMechTypePtr mechType = (BattleMechTypePtr)ObjectManager->getObjectType(typeHandle);
			getTeam()->markSeen(position,mechType->LOSFactor);
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
		control.update(this);
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
				ramTarget = (MoverPtr)getPilot()->getCurTacOrder()->getRamTarget();
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

		Stuff::Vector3D newPosition;
		Stuff::Vector3D vel = getRotationVector();

		if (!appearance->isJumpAirborne())
		{
			float velMag = appearance->getVelocityMagnitude();
			if (emergencyStop)
				velMag = 0.0;
	
			velMag *= 0.01f * control.settings.mech.throttle;
			
			float velFactor = 1.0;
				float velAngle = land->getTerrainAngle(position);
				velAngle *= DEGREES_TO_RADS;
				velFactor = cos(velAngle);
	
			float velMult = velMag * velFactor * frameLength;
			if (velMult > DistanceToWaypoint)
				velMult = DistanceToWaypoint;
			vel *= velMult * worldUnitsPerMeter;
			setVelocity(vel);
	
			newPosition.Add(position, vel);
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
			
			newPosition.Add(position, vel);
			newPosition.z = TerrainRuntime::groundElevation(newPosition);
		}

		if (newMoveChunk) {
			if (!statusChunk.jumpOrder) {
				Stuff::Vector3D correctPos;
				land->cellToWorld(moveChunk.stepPos[0][0], moveChunk.stepPos[0][1], correctPos);
				correctPos.z = 0.0;
				Stuff::Vector3D curPos = position;
				curPos.z = 0.0;
				curPos.Subtract(curPos, correctPos);
				float posDelta = curPos.GetLength();
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

		markDistanceMoved += vel.GetLength();

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

		bool inView = appearance->recalcBounds();
		if (inView)
			windowsVisible = turn;
			
		if (withdrawing && !inView && (pilot->getStatus() != WARRIOR_STATUS_WITHDRAWN))
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
	
		((ObjectAppearance*)appearance)->pilotNameID = IDS_NOPILOT;
		control.settings.mech.gestureGoal = -1;
		updateDynamics();

		float velMag = appearance->getVelocityMagnitude();

		Stuff::Vector3D vel = getRotationVector();
		vel *= velMag * worldUnitsPerMeter;
		vel *= frameLength;
		setVelocity(vel);

		Stuff::Vector3D newPosition;
		newPosition.Add(position, vel);
		newPosition.z = TerrainRuntime::groundElevation(newPosition);

		setPosition(newPosition);

		bool inView = appearance->recalcBounds();
		if (inView)
			windowsVisible = turn;
	
		if (appearance)
		{
			appearance->setInView(inView);
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
					Stuff::Vector3D cockpitPos = appearance->getNodeNamePosition("cockpit");
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

		float velMag = appearance->getVelocityMagnitude();

		Stuff::Vector3D vel = getRotationVector();
		vel *= velMag * worldUnitsPerMeter;
		vel *= frameLength;
		setVelocity(vel);

		Stuff::Vector3D newPosition;
		newPosition.Add(position, vel);
		newPosition.z = TerrainRuntime::groundElevation(newPosition);

		setPosition(newPosition);

		if (!playedHeloDestruct)
		{
			soundSystem->playDigitalSample(HELICOPTER_DEATH,getPosition());
			playedHeloDestruct = true;
		}

		bool inView = appearance->recalcBounds();
		if (inView)
			windowsVisible = turn;
	
		if (appearance)
		{
			appearance->setGestureGoal(GestureGoalFallenBackward);

			appearance->setInView(inView);
			
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
				Stuff::Vector3D cockpitPos = appearance->getNodeNamePosition("cockpit");
				if (getMoveType() != MOVETYPE_AIR)
					ObjectManager->createExplosion(MECH_EXPLOSION_ID,nullptr,cockpitPos,((BattleMechTypePtr)getObjectType())->explDmg,((BattleMechTypePtr)getObjectType())->explRad);
				else
					ObjectManager->createExplosion(VEHICLE_EXPLOSION_ID,nullptr,cockpitPos,0.0f,0.0f);

				craterManager->addCrater(CRATER_4,position,mc2_appearance_rng::randomNumber(180));
				exploding = true;
			}
			else if (((timeLeft < -2.5f) && !mechRemoved && (getMoveType() != MOVETYPE_AIR)) ||
					 ((timeLeft < -1.5f) && !mechRemoved && (getMoveType() == MOVETYPE_AIR)))
			{
				mechRemoved = true;
			}
			else if (mechRemoved)
				appearance->setObjStatus(getStatus());
		}
	}


	// Setup the Render stuff here.  Must do in Update to avoid texture lock in render!
	long relationship = 0;
	if (getTeamId() == Team::home->getId())
	{
		//Two Possibilities.  Either we have the same commander or we don't.
		// If we do, we are on the same "team", if not we are ALLIES!!!
		if (getCommanderId() != Commander::home->getId())
		{
			relationship = 1;
		}
	}
	else
	{
		relationship = 2;
	}

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
	
	if (!mechRemoved && !isDestroyed())
		appearance->setObjStatus(getStatus());

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
		flags = RenderCore::packMechRelation(flags, (uint32_t)homeRelations);
		vs.flags = flags;
		// static_cast safe: BattleMech::appearance is always Mech3DAppearance*
		// (created in BattleMech::init). setVisualState is
		// mech-specific so it is not on the Appearance base.
		static_cast<Mech3DAppearance*>(appearance)->setVisualState(vs);
	}

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
			
			appearance->setAlphaValue(alphaValue);
			appearance->setSensorLevel(0);
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
			
			appearance->setAlphaValue(alphaValue);
			
			if (conStat == CONTACT_SENSOR_QUALITY_1)
			{
				appearance->setSensorLevel(1);
			}
			else if (conStat == CONTACT_SENSOR_QUALITY_2)
			{
				appearance->setSensorLevel(2);
			}
			else if (conStat == CONTACT_SENSOR_QUALITY_3)
			{
				appearance->setSensorLevel(3);
			}
			else if (conStat == CONTACT_SENSOR_QUALITY_4)
			{
				appearance->setSensorLevel(4);
			}
		}
	}
	else
	{
		if (isOnGui)
		{
			alphaValue = 0xff;
			appearance->setSensorLevel(5);
			appearance->setAlphaValue(alphaValue);
		}
	}

	// FORCE-ALL-VISIBLE-1: single chokepoint, after every contact branch above
	// and before the appearance is pushed. No-op unless MC2_FORCE_ALL_VISIBLE.
	mc2_vis::forceVisibleOverride(appearance, alphaValue);

	appearance->update();

	appearance->updateFootprints();
	
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

	//Must make room for holdFireIcons on HUD if this mech might need one!	
	if (isOnGui && !mechRemoved && (attackRange == FIRERANGE_CURRENT) && !isDisabled() )
	{
		mcTextureManager->addTriangle(holdFireIconHandle,MC2_ISHUDLMNT);
		mcTextureManager->addTriangle(holdFireIconHandle,MC2_ISHUDLMNT);
	}
	
	return(1);		//Mech is still around,  false means whack this mech from list.
}

void BattleMech::render (void) 
{
	if (Terrain::IsGameSelectTerrainPosition(position))
	{
		if (getTeamId() != Team::home->getId())
		{
			long cStat = conStat;
	
			if ((cStat == CONTACT_VISUAL) || isDestroyed() || isDisabled() || ShowMovers || (MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]))
			{
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
				appearance->setObjectNameId(descID);
				appearance->setVisibility(true,true);
				appearance->render();
			}
			else if (cStat == CONTACT_SENSOR_QUALITY_1)
			{
				appearance->setBarColor(SB_RED);
				appearance->setVisibility(true,true);
				appearance->render();
	
			}
			else if (cStat == CONTACT_SENSOR_QUALITY_2)
			{
				appearance->setBarColor(SB_RED);
				appearance->setVisibility(true,true);
				appearance->render();
	
			}
			else if (cStat == CONTACT_SENSOR_QUALITY_3)
			{
				appearance->setBarColor(SB_RED);
				appearance->setVisibility(true,true);
				appearance->render();
	
				// C3: route canBeSeen() to GPU-lagged visibility; 1-frame HUD lag: accepted.
				const bool mechInViewQ3 = s_gpuCullLifecycle
					? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(getHandle()))
					: appearance->canBeSeen();
				if ( mechInViewQ3 )
				{
					if (tonnage < 40)
						drawSensorTextHelp (appearance->getScreenPos().x, appearance->getScreenPos().y+20.0f, IDS_SENSOR_LIGHT_MECH,SD_RED,false);
					else if (tonnage < 65)
						drawSensorTextHelp (appearance->getScreenPos().x, appearance->getScreenPos().y+20.0f, IDS_SENSOR_MEDIUM_MECH,SD_RED,false);
					else if (tonnage < 80)
						drawSensorTextHelp (appearance->getScreenPos().x, appearance->getScreenPos().y+20.0f, IDS_SENSOR_HEAVY_MECH,SD_RED,false);
					else
						drawSensorTextHelp (appearance->getScreenPos().x, appearance->getScreenPos().y+20.0f, IDS_SENSOR_ASSAULT_MECH,SD_RED,false);
				}
			}
			else if (cStat == CONTACT_SENSOR_QUALITY_4)
			{
				appearance->setBarColor(SB_RED);
				appearance->setVisibility(true,true);
				appearance->render();

				// C3: route canBeSeen() to GPU-lagged visibility; 1-frame HUD lag: accepted.
				const bool mechInViewQ4 = s_gpuCullLifecycle
					? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(getHandle()))
					: appearance->canBeSeen();
				if ( mechInViewQ4 )
					drawSensorTextHelp (appearance->getScreenPos().x, appearance->getScreenPos().y+20.0f,descID,SD_RED,false);
			}
		}
		else
		{
			if (isOnGui)
			{
				float barStatus = getTotalEffectiveness();

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

				appearance->setBarColor(color);
				appearance->setBarStatus(barStatus);
				appearance->setObjectNameId(descID);
				appearance->setVisibility(true,true);
				appearance->setSensorLevel(0);
				appearance->render();
				// C3: route canBeSeen() to GPU-lagged visibility; 1-frame fire-icon lag: accepted.
				const bool mechInViewFire = s_gpuCullLifecycle
					? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(getHandle()))
					: appearance->canBeSeen();
				if ( attackRange == FIRERANGE_CURRENT && !isDisabled() && mechInViewFire )
					appearance->drawIcon( holdFireIconHandle, 5, 5, color | 0xff000000 );
			}
		}
	}

	if (drawTerrainGrid)
	{
		MovePathPtr path = pilot->getMovePath();
		gosASSERT(path != NULL);
		if (path->numSteps)
		{
			Stuff::Vector4D lineStart, lineEnd;
			for (long i=0;i<path->numSteps;i++)
			{
				if (i != (path->numSteps-1))
				{
					Stuff::Vector3D startPos = path->stepList[i].destination;
					Stuff::Vector3D endPos = path->stepList[i+1].destination;
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
	
	damageThisFrame = 0.0f;		//always reset here for the next frame.
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
			
		if (getTeamId() != Team::home->getId())
		{
			long cStat = conStat;
	
			if ((cStat == CONTACT_VISUAL) && (alphaValue == 0xff))
			{
				appearance->renderShadows();
			}
		}
		else		//We are Home Team and we always see ourselves.  Unless we haven't linked up yet!
		{
			if (isOnGui && (alphaValue == 0xff))
			{
				appearance->renderShadows();
			}
		}
	}
}	

float BattleMech::relFacingTo (Stuff::Vector3D goal, long bodyLocation) {

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

float BattleMech::calcAttackChance (GameObjectPtr target, long aimLocation, float targetTime, long weaponIndex, float modifiers, long* range, Stuff::Vector3D* targetPoint) {

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

bool BattleMech::isJumping (Stuff::Vector3D* goal) {

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
	return((float)numJets * Terrain::worldUnitsPerVertex);
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
   			sprintf(s, "[%.2f] mech.Ejected HE: [%05d]%s", scenarioTime, getPartId(), getName());
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
