// GroundVehicle: the per-frame update, the GPU-lagged visibility gate, and Save/CopyTo/Load.
// Declared in the class headers; shared names in gvehicl_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "gvehicl_internal.h"
#include "Gates/gate.h"


// C3: env-gated lifecycle routing killswitch (same env var as objmgr.cpp).
// MC2_GPU_CULL_LIFECYCLE=1 routes AI canBeSeen() combat gates to GPU-lagged visibility.
static const bool s_gpuCullLifecycle = mc2::gate::get<"MC2_GPU_CULL_LIFECYCLE">();

// Sim-time stand-ins for "has left the screen" (camera-independent): how long a withdrawing vehicle / an ejected vehicle
// pilot lives before it is removed.
static constexpr float GV_WITHDRAW_SEQUENCE_SECONDS = 3.0f;
static constexpr float GV_VEHICLE_PILOT_RUN_SECONDS = 4.0f;

long GroundVehicle::update (void)
{
    unitAnimation = appearance->simulationAnimation();
    simulationPose = appearance->simPose;

	// What the draw side of the appearance needs from this tick is collected here and applied by applyAppearancePush in
	// presentation; the tick feeds only the sim half of the appearance (setSimInput, animation requests, update()).
	AppearancePush push;
	GVPush gp;
	auto commitPush = [&]() { appearancePush = push; gvPush = gp; ++pushSerial; };
	// The sim half of the appearance gets this tick's pose inputs; presentation gets the same parameters through gp / push.
	auto feedSimHalf = [&](float pitch, float roll) {
		Appearance::SimInput simInput;
		simInput.position = position;
		simInput.rotation = rotation;
		simInput.torsoRotation = turretRotation;
		simInput.pitch = pitch;
		simInput.roll = roll;
		simInput.hasStatus = true;
		simInput.status = getStatus();
		appearance->setSimInput(simInput);
		gp.hasParams = true;
		gp.pitch = pitch;
		gp.roll = roll;
		push.hasStatus = true;
		push.status = getStatus();
	};

	if (withdrawing && (pilot->getStatus() == WARRIOR_STATUS_WITHDRAWN)) 
	{
		setTangible(false);
		return(1);
	}

	if (refitBuddyWID && !ObjectManager->getByWatchID(refitBuddyWID))
		refitBuddyWID = 0;
	if (recoverBuddyWID && !ObjectManager->getByWatchID(recoverBuddyWID))
		recoverBuddyWID = 0;

	positionNormal = land->getTerrainNormal(position);

	updatePathLock(false);

	if (timeToClearSelection != 0.0 && scenarioTime > timeToClearSelection)
	{
		timeToClearSelection = 0.0;
		setSelected(false);
		setTargeted(false);
	}

	if (isDestroyed() && (timeLeft < 0.0))
	{
		setTangible(false);
		push.sensorLevel = 0;

		if (!deadByCrushed)
		{
			float speed = glm::length(dVel);
			if (speed)
			{
				glm::vec3 velDiff = dAcc;
				velDiff *= frameLength;
				dVel = dVel + velDiff;
				speed = glm::length(dVel); 
				if (speed < Stuff::SMALL)
				{
					dVel.x = dVel.y = dVel.z = 0.0;
				}
				
				glm::vec3 posDiff = dVel;
				posDiff *= frameLength;
				position = position + posDiff;
				float elev = TerrainRuntime::groundElevation(position);
				if (position.z < elev)
				{
					position.z = elev;
					dRacc = glm::vec3(0.0f);
					dRVel = glm::vec3(0.0f);
					dTime -= frameLength;
					if (dTime < 0.0)
						dVel.x = dVel.y = dVel.z = 0.0;
				}
				
				glm::vec3 rvDiff = dRacc;
				rvDiff *= frameLength;
				dRVel = dRVel + rvDiff;
				
				glm::vec3 rotDiff = dRVel;
				rotDiff *= frameLength;
				dRot = dRot + rotDiff;
			}
		}
		else
		{
			float elev = TerrainRuntime::groundElevation(position);
			if (position.z < elev)
			{
				position.z = elev;
			}
		}
	
		updateAnimations();		//Keep calling so they stop moving!
		
		if (appearance)
		{
			rotation = dRot.y;

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

			feedSimHalf(dRot.x, dRot.z);
			if (appearance)
				appearance->update();
		}
			
		commitPush();
 		return(true);
	}
	else if (isDestroyed() && timeLeft >= 0.0)
	{
		setTangible(false);
		push.sensorLevel = 0;
		timeLeft -= frameLength;
		if (timeLeft < 0.0)
		{
			updateAnimations();		//Keep calling so they stop moving!
			
			if (appearance)
			{
				rotation = dRot.y;
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

			feedSimHalf(dRot.x, dRot.z);
			}
 
			if (!appearance->playDestruction())
			{
				if ((static_cast<GroundVehicleTypePtr>(getObjectType()))->explDmg > 0.0f)
					ObjectManager->createExplosion(SPLASH_VEHICLE_EXPLOSION_ID,nullptr,position, (static_cast<GroundVehicleTypePtr>(getObjectType()))->explDmg,(static_cast<GroundVehicleTypePtr>(getObjectType()))->explRad);
				else if (pathLocks)
				{
					ObjectManager->createExplosion(VEHICLE_EXPLOSION_ID,nullptr,position, 0.0f,0.0f);
				}
				else
				{
					if (RandomNumber(100) < 25)
					{
						ObjectManager->createExplosion(INFANTRY_BLOOD_EFFECT,nullptr,position, 0.0f,0.0f);
					}
					else
					{
						ObjectManager->createExplosion(INFANTRY_BOOM_EFFECT,nullptr,position, 0.0f,0.0f);
					}
				}
			}

			exploding = true;

			/*
			if ((dAcc.z == 0.0) && !deadByCrushed)
			{
				//Calc initial Velocity, rotation and set Acceleration to down in World.
				long xlatBase = 12.0f + 50.0 / tonnage / 2.0f;
				long upBase = 25.0f + 100.0 / tonnage;
				long rotBase = 25.0f + 100.0 / tonnage;
				dVel.x = RandomNumber(xlatBase * 2.0) - xlatBase;
				dVel.y = RandomNumber(xlatBase * 2.0) - xlatBase;
				dVel.z = RandomNumber(upBase) + upBase;
				
				dRVel.x = RandomNumber(rotBase * 2.0) - rotBase;
				dRVel.y = RandomNumber(rotBase * 2.0) - rotBase;
				dRVel.z = RandomNumber(rotBase * 2.0) - rotBase;
				
				dRot.Zero();
				
				dAcc.x = dVel.x * 0.1f;
				dAcc.y = dVel.y * 0.1f;
				dAcc.z = dVel.z * 0.5f;
				dAcc.Negate(dAcc);
				
				dRacc.x = dRVel.x * 0.1f;
				dRacc.y = dRVel.y * 0.1f;
				dRacc.z = dRVel.z * 0.1f;
				dRacc.Negate(dRacc);
				
				dTime = 4.0f;
			}
			*/
			
			if (!MPlayer && getAwake())
				createVehiclePilot();

			commitPush();
			return(1);
		}
	}
	else if (getAwake() && !isDisabled())
	{

		markDistanceMoved = 0.0;
	}
	else if (isDisabled() && timeLeft)
	{
		if (!startDisabled) {
		}
	}
	
	gp.noPilot = true;
	{
		ZoneScopedN("GameLogic.Units.Vehicles AI");
		control.update(this);
	}



	bool emergencyStop = false;
	{
		ZoneScopedN("GameLogic.Units.Vehicles Mover");
		if (!isDisabled())
			emergencyStop = crashAvoidanceSystem();


	//--------------------------------------------------------------------
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


	float velMag = 0.0;
	if (!emergencyStop)
		velMag = velocityMag;


	// The old tests used the local inView, which is still false here, so a withdrawing vehicle was destroyed on its first
	// withdrawing tick and a vehicle pilot on its first tick. The legacy intent is "gone once it has left the screen",
	// as for mechs; the camera-independent form is a fixed sim-time sequence (a move off the map still ends it earlier,
	// in Mover::setPosition).
	if (withdrawing || isVehiclePilot)
		exitTimer += frameLength;
	else
		exitTimer = 0.0f;

	if (withdrawing && (exitTimer >= GV_WITHDRAW_SEQUENCE_SECONDS) && (pilot->getStatus() != WARRIOR_STATUS_WITHDRAWN))
		getObjectType()->handleDestruction(this, nullptr);

	if (isVehiclePilot && (exitTimer >= GV_VEHICLE_PILOT_RUN_SECONDS))
		MC2_DESTROY(this, "vehicle_pilot_offscreen");

	if (isVehiclePilot && (pilot->getCurTacOrder()->code == TACTICAL_ORDER_NONE) )
	{
		glm::vec3 location;
		location = glm::vec3(0.0f);

		pilot->orderMoveToPoint (false, true, ORDER_ORIGIN_PLAYER, location, -1, TACORDER_PARAM_RUN);
	}


	if (teleportPosition.x > -999990.0) {
		setPosition(teleportPosition);
		teleportPosition.x = -999990.0;
		teleportPosition.y = -999990.0;
		teleportPosition.z = -999990.0;
	}

	//-----------------------------------------------------
	// Terrain Angle should effect movement speed.
	// This is accomplished by finding the angle between
	// the normal to the tile and the vehicle's J vector.
	// This angle will be less than 90 if the vehicle is going uphill
	// and greater than 90 if the vehicle is going downhill.
	glm::vec3 vel = getRotationVector();
	if (velMag > Stuff::SMALL)
	{
		glm::vec3 worldK;

		worldK = land->getTerrainNormal(position);

		Rotate(worldK,rotation);

		glm::vec3 pitchK;
		pitchK = worldK;
		pitchK.x = 0.0f;
		pitchK = glm::normalize(pitchK);

		glm::vec3 up;
		up.x = up.y = 0.0f;
		up.z = 1.0f;

		float pitchAngle = glm::dot(up, pitchK);
		pitchAngle = acos(pitchAngle) * RADS_TO_DEGREES;
		if (pitchK.y < 0.0f)
			pitchAngle = -pitchAngle;

		bool useHillFactor = (moveLevel == 0) || (!GameMap->getShallowWater(cellPositionRow, cellPositionCol) && !GameMap->getDeepWater(cellPositionRow, cellPositionCol));
		if (useHillFactor) {
			float pitch = sin(pitchAngle * DEGREES_TO_RADS);
			if ( (pitch != 0.0f))
			{
				float hillFactor = pitch * velMag * gvHillSpeedFactor;
				velMag -= hillFactor;
				if (velMag < hillFactor)
					velMag = 2.0;
			}
		}
	}

	float velMult = velMag * frameLength;
	if (velMult > DistanceToWaypoint)
		velMult = DistanceToWaypoint;
	vel *= velMult * worldUnitsPerMeter;

	{
		glm::vec3 newPosition;
		newPosition = position + vel;
		newPosition.z = TerrainRuntime::groundElevation(newPosition);
		if ((moveLevel == 1) && (newPosition.z < Terrain::waterElevation))
			newPosition.z = Terrain::waterElevation;
		int newCellRow = 0;
		int newCellCol = 0;
		int tileRow = 0;
		int tileCol = 0;
		land->worldToTileCell(newPosition, tileRow, tileCol, newCellRow, newCellCol);
	}

	setVelocity(vel);

	markDistanceMoved += glm::length(vel);

	glm::vec3 newPosition;
	newPosition = position + vel;
	newPosition.z = TerrainRuntime::groundElevation(newPosition);
	if ((moveLevel == 1) && (newPosition.z < Terrain::waterElevation))
		newPosition.z = Terrain::waterElevation;

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
				Fatal(0, " gvehicl.update: newMoveChunk stepPos not on map! ");
		}
		newMoveChunk = false;
	}
	
	setPosition(newPosition);
	updateTurret(NewRotation);

	updateDynamics();

	if (!isDisabled())
		updatePathLock(true);

	sweepTime += frameLength;
	mineCheck();
	if (!MPlayer || MPlayer->isServer()) 
	{
		if (mineLayer != 0)
		{
			if (pilot->getCurTacOrder()->moveParams.mode == MOVE_MODE_MINELAYING)
			{
				if ((cellPositionRow != cellRowToMine) || (cellPositionCol != cellColToMine))
				{
					timeInCurrent += frameLength;

					if (timeInCurrent > MineWaitTime)
					{
						cellRowToMine = cellPositionRow;
						cellColToMine = cellPositionCol;
						timeInCurrent = 0.0f;

						GameMap->setMine(cellRowToMine, cellColToMine, 1);
						
						if (MPlayer) {
							MPlayer->addMineChunk(cellRowToMine,
												  cellColToMine,
												  1,
												  1,
												  0);
						}
						WeaponShotInfo shot;
						shot.init(0, -2, 1.0, GROUNDVEHICLE_LOCATION_TURRET, 0);
						handleWeaponHit(&shot, MPlayer != nullptr);
					}
				}
			}
		}
		
		if (aerospaceSpotter)
		{
			GameObjectPtr curTarget = pilot->getCurrentTarget();
			float dist = 0.0f;
			if (curTarget) 
			{
				glm::vec3 distance;
				distance = curTarget->getPosition() - getPosition();
				dist = glm::length(distance);
				dist *= metersPerWorldUnit;
			}

			if (curTarget && (dist <= WeaponRanges[WEAPON_RANGE_MEDIUM][1]) && lineOfSight(curTarget))
			{
				timeInCurrent += frameLength;
				if (timeInCurrent > StrikeWaitTime)
				{
					timeInCurrent = 0.0f;
					
					CallArtillery (getCommander()->getId(), ARTILLERY_LARGE, curTarget->getPosition(), StrikeTimeToImpact, false);
				}
			}
		}
	}

	float zPos = TerrainRuntime::groundElevation(position);
	position.z = zPos;
	if ((moveLevel == 1) && (zPos < MapData::waterDepth))
		position.z = MapData::waterDepth;
	}  // end GameLogic.Units.Vehicles Mover

	if (!isDestroyed() || (timeLeft > 0.0))
	{
		ZoneScopedN("GameLogic.Units.Vehicles AppearanceUpdate");
		if (appearance)
		{
			updateAnimations();

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

			feedSimHalf(0.0f, 0.0f);
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
			
			if (pilot->getCurTacOrder()->moveParams.mode == MOVE_MODE_MINELAYING)
			{
				appearance->startActivity(MINELAYER_EFFECT_ID,true);
			}
			else if (refitBuddyWID && (appearance->getCurrentGestureId() == 4))
			{
				appearance->startActivity(REPAIR_VEHICLE_EFFECT_ID,true);
				soundSystem->playDigitalSample(REPAIRBAY_FX,getPosition(),false);
			}
			else
			{
				appearance->stopActivity();
			}
			
 			//if (inView)		//Must do this regardless of inView.
			{
				//-----------------------------------------------------	
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
						push.hasAlpha = true;
					}
				}

				// setVisibility and the FORCE-ALL-VISIBLE-1 override run in applyAppearancePush.
				gp.visible = true;

				appearance->update();
				
			}
		}

		setTangible(true);
	}

	{
		ZoneScopedN("GameLogic.Units.Vehicles Collisions");
		float xCoord = position.x - Terrain::mapTopLeft3d.x;
		float yCoord = Terrain::mapTopLeft3d.y + position.y ;

		float divisor = (Terrain::verticesBlockSide * Terrain::worldUnitsPerVertex);
		xCoord /= divisor;
		yCoord /= divisor;

		long blockNumber = float2long(xCoord) + (float2long(yCoord) * Terrain::blocksMapSide);
		addMoverToList(blockNumber);
	}


	if (getDebugFlag(OBJECT_DFLAG_DISABLE))
		disable(DEBUGGER_DEATH);

	commitPush();
	return(1);		//Vehicle is still around,  false means whack this vehicle from list.
}

// Presentation phase: apply what the last update() collected to the draw side of the appearance, before the presentation
// loop's recalcBounds / pose blend / update(false). A frame with no new tick applies nothing.
void GroundVehicle::applyAppearancePush (void)
{
	if (pushSerial == pushPresented)
		return;
	pushPresented = pushSerial;

	auto* objectAppearance = static_cast<ObjectAppearance*>(appearance);
	if ( Team::home == getTeam() )
		objectAppearance->pilotNameID = getPilot()->descID;
	else if ( MPlayer )
	{
		strcpy( objectAppearance->pilotName, (commanderId > -1) ? MPlayer->playerInfo[commanderId].name : " " );
	}
	else
		objectAppearance->pilotNameID = getPilot()->descID;
	if (gvPush.noPilot)
		objectAppearance->pilotNameID = IDS_NOPILOT;

	if (gvPush.hasParams)
	{
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

		appearance->setObjectParameters(position, rotation, TacticalOverview::maskWorldBars(drawFlags), teamId,relationship);
		appearance->setMoverParameters(turretRotation,gvPush.pitch,gvPush.roll,!pathLocks);
	}
	if (appearancePush.hasStatus)
		appearance->setObjStatus(appearancePush.status);

	if (appearancePush.hasAlpha)
		appearance->setAlphaValue(alphaValue);
	if (appearancePush.sensorLevel >= 0)
		appearance->setSensorLevel(appearancePush.sensorLevel);

	if (gvPush.visible)
	{
		appearance->setVisibility(true,true);

		// FORCE-ALL-VISIBLE-1: single chokepoint, after every contact branch and before the appearance is drawn. No-op
		// unless MC2_FORCE_ALL_VISIBLE.
		BYTE drawAlpha = alphaValue;   // presentation never writes a sim-owned field (mover.h AppearancePush contract)
		mc2_vis::forceVisibleOverride(appearance, drawAlpha);
	}
}

void GroundVehicle::renderShadows (void)
{
	if (Terrain::IsGameSelectTerrainPosition(position))
	{
		if ((getStatus() != OBJECT_STATUS_DESTROYED) && (getStatus() != OBJECT_STATUS_DISABLED))
		{
			if (getTeamId() != Team::home->getId())
			{
				long cStat = conStat;
		
				if (cStat == CONTACT_VISUAL)
				{
					appearance->renderShadows();
				}
			}
			else		//We are Home Team and we always see ourselves.
			{
				appearance->renderShadows();
			}
		}
	}
}	


void GroundVehicle::render (void) 
{
	if (Terrain::IsGameSelectTerrainPosition(position))
	{
		if (teamId != Team::home->getId())
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
				appearance->render();
			}
			else if (cStat == CONTACT_SENSOR_QUALITY_1)
			{
				appearance->setBarColor(SB_RED);
				appearance->render();
				
			}
			else if (cStat == CONTACT_SENSOR_QUALITY_2)
			{
				appearance->setBarColor(SB_RED);
				appearance->render();
				
			}
			else if (cStat == CONTACT_SENSOR_QUALITY_3)
			{
				appearance->setBarColor(SB_RED);
				appearance->render();
	
				long resourceID = 0;
				if (tonnage <= 40)
					resourceID = IDS_SENSOR_LIGHT_VEHICLE;
				else if (tonnage <= 65)
					resourceID = IDS_SENSOR_MEDIUM_VEHICLE;
				else
					resourceID = IDS_SENSOR_HEAVY_VEHICLE;
					
				// C3: route canBeSeen() to GPU-lagged visibility; 1-frame HUD lag: accepted.
				const bool gvInViewQ3 = s_gpuCullLifecycle
					? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(getHandle()))
					: appearance->canBeSeen();
				if ( gvInViewQ3 )
					drawSensorTextHelp (appearance->getScreenPos().x, appearance->getScreenPos().y+20.0f, resourceID,SD_RED,false);
			}
			else if (cStat == CONTACT_SENSOR_QUALITY_4)
			{
				appearance->setBarColor(SB_RED);
				appearance->render();

				// C3: route canBeSeen() to GPU-lagged visibility; 1-frame HUD lag: accepted.
				const bool gvInViewQ4 = s_gpuCullLifecycle
					? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(getHandle()))
					: appearance->canBeSeen();
				if ( gvInViewQ4 )
					drawSensorTextHelp (appearance->getScreenPos().x, appearance->getScreenPos().y+20.0f,descID,SD_RED,false);
			}
			else if (alphaValue != 0x0)	//What if we are out of LOS and NOT on sensors!!!  Let 'em fade out.
			{
				appearance->setBarColor(SB_RED);
				appearance->render();
			}
		}
		else 
		{
			float barStatus = getTotalEffectiveness();
			
			DWORD color = 0xff7f7f7f;
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
			appearance->render();
			if ( attackRange == FIRERANGE_CURRENT && !isDisabled() )
			{
				// Vertex reservation lives with the draw (presentation), not in the tick.
				mc2::renderSubmissions().addTriangle(holdFireIconHandle,MC2_DRAWALPHA);
				mc2::renderSubmissions().addTriangle(holdFireIconHandle,MC2_DRAWALPHA);
				appearance->drawIcon( holdFireIconHandle, 5, 5, color );
			}
		}
	}

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
					
					// [PROJECTZ:ScreenXYOracle id=gvehicl_path_step_start]
					eye->projectForScreenXY(startPos,lineStart);
					// [PROJECTZ:ScreenXYOracle id=gvehicl_path_step_end]
					eye->projectForScreenXY(endPos,lineEnd);

					lineStart.z = lineEnd.z = HUD_DEPTH;

					LineElement newElement(lineStart,lineEnd,SD_GREEN,nullptr,-1);
					newElement.draw();
				}
			}
		}
	}
	
	
}

#include "save_struct_zero.h"
void GroundVehicle::Save (PacketFilePtr file, long packetNum)
{
	MC2_SAVE_STRUCT(GroundVehicleData, data);
	CopyTo(&data);

	//PacketNum incremented in ObjectManager!!
	file->writePacket(packetNum,reinterpret_cast<MemoryPtr>(&data),sizeof(GroundVehicleData),STORAGE_TYPE_ZLIB);
}

void GroundVehicle::CopyTo (GroundVehicleData *data)
{																	   
	data->accel 				   	 = accel;               
	data->velocityMag 	             = velocityMag;         
														 
	data->suspension                 = suspension;          
	data->movementEnabled            = movementEnabled;     
														 
	data->turretEnabled              = turretEnabled;       
	data->turretRotation 			 = turretRotation;      		
	data->turretBlownThisFrame       = turretBlownThisFrame;
														 
	data->captureable                = captureable;         
	data->deadByCrushed 			 = deadByCrushed;       		
														 
	data->canRefit 					 = canRefit;            	
	data->canRecover                 = canRecover;          
	data->refitting 				 = refitting;           		
	data->recovering                 = recovering;          
	data->mineSweeper                = mineSweeper;         
	data->sweepTime 				 = sweepTime;           		
	data->mineLayer                  = mineLayer;           
	data->aerospaceSpotter           = aerospaceSpotter;    
	data->cellColToMine              = cellColToMine;       
	data->cellRowToMine              = cellRowToMine;       
														 
	data->notMineYet                 = notMineYet;          
														 
	data->battleRating 				 = battleRating;        	
														 
	data->descID 					 = descID;              		
														 
	data->timeInCurrent 			 = timeInCurrent;       		
														 
	data->dVel                       = dVel;                
	data->dRot                       = dRot;                
	data->dAcc                       = dAcc;                
	data->dRVel                      = dRVel;               
	data->dRacc                      = dRacc;               
	data->dTime                      = dTime;               

	data->isVehiclePilot			 = isVehiclePilot;

	if (sensorSystem)
		data->sensorOK					 = !sensorSystem->broken;
	else
		data->sensorOK					 = false;

	Mover::CopyTo(data);
}

void GroundVehicle::Load (GroundVehicleData *data)
{
	Mover::Load(data);

	accel 				   	 = data->accel;               
	velocityMag 	         = data->velocityMag;         
													 
	suspension               = data->suspension;          
	movementEnabled          = data->movementEnabled;     
													 
	turretEnabled            = data->turretEnabled;       
	turretRotation 			 = data->turretRotation;      		
	turretBlownThisFrame     = data->turretBlownThisFrame;
													 
	captureable              = data->captureable;         
	deadByCrushed 			 = data->deadByCrushed;       		
													 
	canRefit 				 = data->canRefit;            	
	canRecover               = data->canRecover;          
	refitting 				 = data->refitting;           		
	recovering               = data->recovering;          
	mineSweeper              = data->mineSweeper;         
	sweepTime 				 = data->sweepTime;           		
	mineLayer                = data->mineLayer;           
	aerospaceSpotter         = data->aerospaceSpotter;    
	cellColToMine            = data->cellColToMine;       
	cellRowToMine            = data->cellRowToMine;       
													 
	notMineYet               = data->notMineYet;          
													 
	battleRating 			 = data->battleRating;        	
													 
	descID 					 = data->descID;              		
													 
	timeInCurrent 			 = data->timeInCurrent;       		
													 
	dVel                     = data->dVel;                
	dRot                     = data->dRot;                
	dAcc                     = data->dAcc;                
	dRVel                    = data->dRVel;               
	dRacc                    = data->dRacc;               
	dTime                    = data->dTime;               

	isVehiclePilot			 = data->isVehiclePilot;

	if (data->sensorOK)
	{
		sensorSystem = SensorManager->newSensor();
		sensorSystem->setOwner(this);
		sensorSystem->setRange(MasterComponent::masterList[inventory[sensor].masterID].getSensorRange());
		SensorManager->addTeamSensor(teamId, sensorSystem);
	}
}

bool  GroundVehicle::burnRefitPoints (float pointsToBurn)
{
	if (canRefit) 
	{
		armor[GROUNDVEHICLE_LOCATION_TURRET].curArmor -= pointsToBurn;
		if ( armor[GROUNDVEHICLE_LOCATION_TURRET].curArmor <= 0.0f )
		{
			armor[GROUNDVEHICLE_LOCATION_TURRET].curArmor = 0.f;
			if (getCommanderId() == Commander::home->getId())
				soundSystem->playBettySample( BETTY_REPAIR_GONE );
		}
		return(true);
	}
	else
		return(false);
}
