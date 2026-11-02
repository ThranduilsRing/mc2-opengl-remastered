// MissionInterfaceManager: the VTOL, old-style and area-of-effect control styles, waypoint paths and the click-driven update overload.
// Declared in the class headers; shared names in missiongui_shared_internal.h.
#include "missiongui_shared_internal.h"
#include "unit_animation_tick.h"
#include "vtol_simulation.h"
#include "save_archive.h"
#include "appearance_rng.h"
#include "command_replay.h"


namespace { std::uint64_t s_vtolHash = 14695981039346656037ull; }
std::uint64_t vtolSimulationHash() { return s_vtolHash; }
void setVtolSimulationGesture(BldgAppearance& appearance, unsigned long gesture)
{
    appearance.setGesture(gesture);
    auto& state = *appearance.simulationAnimation();
    if (appearance.appearType->isRandom(gesture))
        state.currentFrame = mc2_anim_rng::randomNumber(appearance.appearType->getNumFrames(gesture) - 1);
}

void MissionInterfaceManager::updateVTol()
{
    if (simPresentationPass) {
        for (long vtolNum = 0; vtolNum < MAX_TEAMS; ++vtolNum) {
            if (!paintingVtol[vtolNum] || !vTol[vtolNum]) continue;
            const auto& state = *vTol[vtolNum]->simulationAnimation();
            const bool startBeam = state.ejectionSerial != vTol[vtolNum]->appliedEffects_.ejectionSerial;
            vTol[vtolNum]->presentAnimation(state);
            vTol[vtolNum]->recalcBounds();
            vTol[vtolNum]->update(false);
            if (startBeam) {
					if (recoveryBeam[vtolNum])
					{
						glm::mat4 	shapeOrigin;
						glm::mat4	localToWorld;
						glm::mat4	localResult;

						glm::vec3 dustPos = vTol[vtolNum]->position;
						glm::vec3 wakePos;
						mc2ToRender(wakePos, dustPos);

						mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
						mc2::math::buildTranslation(shapeOrigin, wakePos);

						gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);

						recoveryBeam[vtolNum]->SetLoopOff();
						recoveryBeam[vtolNum]->SetExecuteOn();
						recoveryBeam[vtolNum]->Start(&info);
					}
            }
			if (dustCloud[vtolNum] && dustCloud[vtolNum]->IsExecuted())
			{
				glm::mat4 	shapeOrigin;
				glm::mat4	localToWorld;
				glm::mat4	localResult;

				glm::vec3 dustPos = vTol[vtolNum]->position;
				glm::vec3 wakePos;
				mc2ToRender(wakePos, dustPos);

				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, wakePos);

				mc2::math::Obb boundingBox;
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);

				dustCloud[vtolNum]->Execute(&info);
			}

			if (recoveryBeam[vtolNum] && recoveryBeam[vtolNum]->IsExecuted())
			{
				glm::mat4 	shapeOrigin;
				glm::mat4	localToWorld;
				glm::mat4	localResult;

				glm::vec3 dustPos = vTol[vtolNum]->position;
				glm::vec3 wakePos;
				mc2ToRender(wakePos, dustPos);

				mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
				mc2::math::buildTranslation(shapeOrigin, wakePos);

				mc2::math::Obb boundingBox;
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);

				recoveryBeam[vtolNum]->Execute(&info);
			}

        }
        return;
    }
	long commanderID = Commander::home->getId();
	paintingMyVtol = paintingVtol[commanderID];
	
	for (long vtolNum = 0;vtolNum < MAX_TEAMS; vtolNum++)
	{
		if ( paintingVtol[vtolNum] && (!bPaused || MPlayer))
		{
			auto& clock = *vTol[vtolNum]->simulationAnimation();
			if (clock.lastAnimAdvanceFrame == static_cast<uint32_t>(turn)) continue;
			clock.lastAnimAdvanceFrame = static_cast<uint32_t>(turn);
			if (vehicleID[vtolNum] != 147)		//We are the standard vtol.
			{
				if (!vehicleDropped[vtolNum] && (vTol[vtolNum]->unitAnim().currentFrame >= 145))
				{
					vehicleDropped[vtolNum] = true;
					soundSystem->playDigitalSample(VTOL_DROP,vPos[vtolNum]);
					
					if (MPlayer) 
					{
						if (commanderID == vtolNum)
						{
							//--------------------------------------------------------
							// This will get the other machines to enable the mover...
							MPlayer->sendReinforcement(vehicleID[vtolNum], MPlayer->reinforcements[commanderID][0], "noname", commanderID, vPos[vtolNum], 2);
						}
					}
					else
						addVehicle( vPos[vtolNum] );
				}
				else if (vTol[vtolNum]->unitAnim().currentFrame >= 300)
				{
					paintingVtol[vtolNum] = 0;
					vTol[vtolNum].reset();
					
					if (dustCloud[vtolNum])
					{
						dustCloud[vtolNum]->Kill();
						dustCloud[vtolNum].reset();
					}
					
					if (recoveryBeam[vtolNum])
					{
						recoveryBeam[vtolNum]->Kill();
						recoveryBeam[vtolNum].reset();
					}

					//We should have set this when the helo was brought in.
					// Let it takeoff now that the vtol is out of the way.
					if (MPlayer) {
						if ((vtolNum < 0) || (vtolNum >= MAX_MC_PLAYERS))
							STOP(("updateVTOL: bad vtolNum"));
						if (MPlayer->reinforcements[vtolNum][0] < 0)
							STOP(("updateVTOL: bad reinforcement"));
						MoverPtr mover = MPlayer->moverRoster[MPlayer->reinforcements[vtolNum][0]];
						if (mover) {
							TacticalOrder tacOrder;
							tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_POWERUP, true);
							tacOrder.pack(nullptr, nullptr);					
							if (!MPlayer->isServer())
								MPlayer->sendPlayerOrder(&tacOrder, false, 1, &mover);
							else
								mover->handleTacticalOrder(tacOrder);
						}
						MPlayer->reinforcements[vtolNum][0] = -1;
						}
					else if (reinforcement)
					{
						reinforcement->getPilot()->orderPowerUp(true, ORDER_ORIGIN_SELF);
						reinforcement = nullptr;
					}

					if ( !MPlayer || vtolNum == MPlayer->commanderID )
						controlGui.unPressAllVehicleButtons();
				}
			}
			else	//Update the KARNOV animation
			{
				if ((vTol[vtolNum]->getCurrentGestureId() == 1) && !vTol[vtolNum]->getInTransition())
				{
					setVtolSimulationGesture(*vTol[vtolNum], 0);

					//Needs to STAY false, probably in MPlayer too because
					// If the mech we are about to recover gets destroyed, we need to abort!!
					// Probably in Multiplayer now its possible to recover even if destroyed!
					if (!MPlayer)
						mechRecovered[vtolNum] = false;		//Need to know when mech is done so Karnov can fly away.

					vTolTime[vtolNum] = scenarioTime;
					
					++vTol[vtolNum]->simulationAnimation()->ejectionSerial;

					//Actually the Karnov Deploy Sound Effect!
					soundSystem->playDigitalSample(RADAR_HUM,vTol[vtolNum]->position,false);
				}
				else if ((vTol[vtolNum]->getCurrentGestureId() == 0) && !mechRecovered[vtolNum] && mechToRecover[vtolNum] && !mechToRecover[vtolNum]->isDestroyed())
				{
					if (scenarioTime > vTolTime[vtolNum] + Mover::recoverTime)
					{
						bool doIt = true;
						if (MPlayer)
						{
							if (commanderID == vtolNum)
							{
								//--------------------------------------------------------
								// This will get the other machines to enable the mover...
								MPlayer->sendReinforcement(vehicleID[vtolNum], MPlayer->reinforcements[vtolNum][1], MPlayer->reinforcementPilot[vtolNum], vtolNum, vPos[vtolNum], 5);
							}
							else
								doIt = false;
						}
						
						if (doIt && !mechToRecover[vtolNum]->isDestroyed())
						{
							//Seems like we don't want to do this on every machine, do we?
							// -fs

							//STAY in recover until its done.  I know it shouldn't be possible but trust me, its happening.
							while ((mechToRecover[vtolNum])->recover() == false)
								;

							if (mechToRecover[vtolNum]->isDisabled())
							{
								mechToRecover[vtolNum]->setStatus(OBJECT_STATUS_SHUTDOWN,true);
								mechToRecover[vtolNum]->getSensorSystem()->broken = false;
								(mechToRecover[vtolNum])->timeLeft = 1.0f;
								(mechToRecover[vtolNum])->exploding = false;
							}
							char* newPilotName = nullptr;
							if (MPlayer)
								newPilotName = MPlayer->reinforcementPilot[vtolNum];
							else
								newPilotName = const_cast<char*>(LogisticsData::instance->getBestPilot( mechToRecover[vtolNum]->tonnage ));

							mission->tradeMover(mechToRecover[vtolNum], Commander::commanders[vtolNum]->getTeam() ? Commander::commanders[vtolNum]->getTeam()->getId() : -1, vtolNum, newPilotName, "pbrain");
							mechRecovered[vtolNum] = true;
							mechToRecover[vtolNum]->getPilot()->orderPowerUp(true, ORDER_ORIGIN_SELF);
							if (MPlayer)
								MPlayer->reinforcements[vtolNum][1] = -1;
						}
					}
				}
				else if ((vTol[vtolNum]->getCurrentGestureId() == 0) && (mechRecovered[vtolNum] || (mechToRecover[vtolNum] && mechToRecover[vtolNum]->isDestroyed())) && (!vTol[vtolNum]->getInTransition()))
				{
					setVtolSimulationGesture(*vTol[vtolNum], 2);	//Fly Away!!
				}
				else if ((vTol[vtolNum]->getCurrentGestureId() == 2) && (!vTol[vtolNum]->getInTransition()))
				{
					paintingVtol[vtolNum] = 0;
					vTol[vtolNum].reset();
					
					if (dustCloud[vtolNum])
					{
						dustCloud[vtolNum]->Kill();
						dustCloud[vtolNum].reset();
					}
					
					if (recoveryBeam[vtolNum])
					{
						recoveryBeam[vtolNum]->Kill();
						recoveryBeam[vtolNum].reset();
					}

					//Check if mech was recovered.  If not, restore the resource points!!!
					// This can happen if we call in a recovery vehicle and the mech is destroyed
					// BEFORE the recovery vehicle recovers it!!
					if (!mechRecovered[vtolNum]) {
						if (MPlayer) {
							if (MPlayer->isServer()) {
								glm::vec3 pos;
								MPlayer->sendReinforcement(10000, 0, "noname", vtolNum, pos, 6);
							}
							}
						else
							LogisticsData::instance->setResourcePoints(LogisticsData::instance->getResourcePoints() + 10000);
					}

					if ( !MPlayer || vtolNum == MPlayer->commanderID )
						controlGui.unPressAllVehicleButtons();
				}
			}
	

            if (vTol[vtolNum]) {
                auto& state = *vTol[vtolNum]->simulationAnimation();
                mc2::sim::advanceLinearAnimation(state, state.bdFrameRate,
                    vTol[vtolNum]->appearType->getNumFrames(state.bdAnimationState), frameLength);
            }
        }
    }
    if (!mc2::replay::active()) return;
    mc2::save::Archive state;
    for (long i = 0; i < MAX_TEAMS; ++i) {
        bool active = paintingVtol[i] && vTol[i] != nullptr;
        state(active);
        if (!active) continue;
        auto recovered = mechToRecover[i] ? mechToRecover[i]->getWatchID() : 0;
        state(i, vehicleID[i], vehicleDropped[i], mechRecovered[i], vTolTime[i],
              vPos[i].x, vPos[i].y, vPos[i].z, recovered, *vTol[i]->simulationAnimation());
    }
    s_vtolHash = 14695981039346656037ull;
    for (auto byte : state.bytes()) s_vtolHash = (s_vtolHash ^ byte) * 1099511628211ull;
}

void MissionInterfaceManager::drawWayPointPaths()
{
	Team* pTeam = Team::home;
   	for (long i=0;i<pTeam->getRosterSize();i++)
   	{
		Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
   		if ( pMover->isSelected() && commanderIsHome(pMover->getCommander()) )
   		{
   			pMover->updateDrawWaypointPath();
   		}
   	}
}
	
void MissionInterfaceManager::updateOldStyle( bool shiftDn, bool altDn, bool ctrlDn, 
											 bool bGui, bool lineOfSight, bool passable, 
											 long moverCount, long nonMoverCount )
{
	ZoneScopedN("MIF.OldStyle");

	printDebugInfo();

	long commanderID = Commander::home->getId();
	
   	if ( userInput->getKeyDown( WAYPOINT_KEY ) || controlGui.getMines() )
   	{
		drawWayPointPaths(); 	
		//Can Never make a patrol path.  Causes movement wubbies!
//		if ( makePatrolPath() )
//			return;
   	}

	int mState = userInput->getMouseCursor();

	bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick() && !lastUpdateDoubleClick);
	bool rightClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isRightClick());

	// M1.6 Q6 4-site observable: set to true immediately after each of
	// the setSelected(true) writer sites this style body owns (Shift+
	// additive site and plain-LMB-select site). Read at tail by
	// tryStaticPropPick to short-circuit when the legacy path already
	// claimed this click. Local scope; no leakage.
	bool moverSelectedThisFrame = false;

	if ( update( leftClicked, rightClicked, mouseX, mouseY, target,  lineOfSight ) )
		return;

    // Selection resolves own units first through the same bounds service.
    if (selectClicked && !bGui && !ctrlDn && !bForcedShot && !bAimedShot
        && !controlGui.getGuard() && !controlGui.getRepair() && !controlGui.getSalvage()
        && !controlGui.getGuardTower() && !controlGui.isAddingSalvage()
        && !controlGui.isAddingVehicle() && !controlGui.isAddingAirstrike()) {
        if (auto* selected = pickGameplaySelection(mouseX, mouseY))
            target = selected;
    }


	if ( bGui ) // don't target or anything if we are int the control panel
	{
		if ( !canJump() && controlGui.getJump() )
			controlGui.toggleJump();

		userInput->setMouseCursor( mState );
		if ( controlGui.mouseInVehicleStopButton )
			return;
		if ( !userInput->isLeftDrag() && !controlGui.isAddingAirstrike() && !controlGui.isAddingVehicle() )
			return;
	}
	
	
	Team* pTeam = Team::home;
	
	if (target) // if there is a target, make the appropritate cursor
	{
		userInput->setMouseCursor( makeTargetCursor( lineOfSight, moverCount, nonMoverCount ) );
	}
	else // make a move cursor
	{
		userInput->setMouseCursor( makeNoTargetCursor( passable, lineOfSight, ctrlDn, bGui, moverCount, nonMoverCount ) );
	}

	// Suppress the move-on-release when this click landed on an overview squad
	// card (it already selected the squad; don't also move under the card).
	if ( userInput->leftMouseReleased() && g_tacticalOverview.consumeReleaseSuppression() )
	{
	}
	else if ( userInput->leftMouseReleased() && !userInput->wasLeftDrag() && !bGui && !lastUpdateDoubleClick) // move on the mouse ups
	{
		// PRECISION (companion to the per-frame O(1) approx at update():~778):
		// the per-frame wPos uses screenToGroundPlaneApprox (fast preview/status
		// bar, but a z=0 ground-plane point that drifts ~elevation in XY on hilly
		// terrain). For the DISCRETE move/place/recover action below we need the
		// real terrain-surface hit, so recompute wPos once here via inverseProject
		// (its actual job — terrain-surface/placement-Z). Runs only on click, so
		// its O(quads) cost is fine; move-orders land on the correct elevated cell.
		{
			Stuff::Vector2DOf<long> clickXY;
			clickXY.x = mouseX;
			clickXY.y = mouseY;
			const std::chrono::steady_clock::time_point _cpuT0 =
				s_gpParityOn ? std::chrono::steady_clock::now()
				             : std::chrono::steady_clock::time_point{};
			eye->inverseProject(clickXY, wPos);
			// GPU-GROUND-PICK-PARITY-1: precise per-click move-order pick (R2 case).
			// Depth-unproject reads the true rendered terrain surface -> the recon
			// expects GPU >= CPU accuracy on slopes; sample distant clicks for the
			// reverse-Z precision check. Parity-only; wPos is untouched.
			if (s_gpParityOn) {
				const unsigned long long _cpuNs = (unsigned long long)
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - _cpuT0).count();
				gpParitySite(mouseX, mouseY, _cpuNs, wPos.x, wPos.y, wPos.z, "Lmove");
			}
		}

		if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && canAddVehicle( wPos )) ||
			(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && canRecover( wPos )))
		{
			// Target has already been confirmed  as a mover(BattleMech) by canRecover
			// OR it doesn't matter because the canAddVehicle part of the beginVtol function doesn't reference it!
			// Again, Its OK because we only use it for salvage craft.
			// Need to pass this in so that Multiplayer can pass it in.
			beginVtol(-1,commanderID,nullptr,static_cast<MoverPtr>(target));	//In Single player, this should always be zero?
			return;
		}
		else if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && !canAddVehicle( wPos )) || 
				(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && !canRecover( wPos )))
		{
			soundSystem->playDigitalSample( INVALID_GUI );
			return;
		}
		else if ( controlGui.isAddingAirstrike() && !paintingVtol[commanderID] ) // if we're painting the vtol, carry on
		{
			addAirstrike();
			return;
		}
		
		else if (!target)
		{
			if ( controlGui.isSelectingInfoObject() )
			{
				soundSystem->playDigitalSample( INVALID_GUI );
				controlGui.cancelInfo();
			}
			else if (Terrain::IsGameSelectTerrainPosition(wPos))
			{
				
				if (controlGui.getGuardTower())
					doGuardTower();
				else if ( passable || selectionIsHelicopters())
				{
					if ( controlGui.getGuard() )
					{
						doGuard(target);
					}
					else if (!bForcedShot)
						doMove( wPos );
				}
				else
				{
					userInput->setMouseCursor(mState_DONT);
				}
			}
			else
			{
				userInput->setMouseCursor(mState_DONT);
			}
		}
		else 
			//We clicked on a target.  If mouse cursor is normal, we want to select a friendly.
				//if the mouse cursor is some form of attack, attack the target.
		{
			if ( target->isMover() && ( target->getTeamId() == Team::home->getId() || 
					CONTACT_VISUAL == (static_cast<Mover*>(target))->getContactStatus(Team::home->getId(), true)
					|| target->isDisabled() ) )
				{
					controlGui.setInfoWndMover( static_cast<Mover*>(target) );
				}
		
			if ( controlGui.getSalvage() )
				doSalvage();
			else if ( controlGui.getGuard() )
			{
				doGuard(target);
			}
			else if ( target->getTeamId() == Team::home->getId() && !target->isDisabled() )
			{
				
				if (shiftDn && target->getCommanderId() == Commander::home->getId() )
				{
					bool alreadyThere = false;
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
						if (pMover == target && pMover->isSelected() && commanderIsHome(pMover->getCommander()))
							alreadyThere = true;
					}
					
					if (!alreadyThere && !target->isDisabled()) {
						target->setSelected( true );
						// M1.6 Q6 site 1: legacy Shift+additive-select on
						// friendly mover. Sets fallback gate; tail
						// tryStaticPropPick short-circuits so Section 11
						// invariant holds (no M1.6 log line in this case).
						moverSelectedThisFrame = true;
					} else {
						// M1.6 Q6 RESOLVED: setSelected(false) toggle-off
						// site MUST NOT set the gate per spec rationale.
						target->setSelected( false );
					}
				}
				else if ( controlGui.getRepair() && canRepair( target ) )
				{
					doRepair(target);
				}
			 	else if ( controlGui.getSalvage() && canSalvage( target ))
					doSalvage();
				else if ( controlGui.getGuardTower() )
					doGuardTower();
				else if ( target->getTeam() && Team::home->isFriendly(target->getTeam()) && target->getFlag(OBJECT_FLAG_CANREFIT) && target->getFlag(OBJECT_FLAG_MECHBAY) && canRepairBay(target) )
					doRepairBay(target);
				else if ( controlGui.isSelectingInfoObject() && target->isMover() )
					controlGui.setInfoWndMover( static_cast<Mover*>(target) );
				else if ( target->isMover() && !target->isDisabled() && target->getCommanderId() == Commander::home->getId() )
				{
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
						if (commanderIsHome(pMover->getCommander()))
						{
							// M1.6 Q6 RESOLVED: clear-others setSelected(false)
							// loop MUST NOT set the gate.
							pMover->setSelected( false );
						}
					}

					target->setSelected( true );
					// M1.6 Q6 site 2: legacy plain-LMB-select on friendly
					// mover. Sets fallback gate.
					moverSelectedThisFrame = true;
				}
				else
					soundSystem->playDigitalSample( INVALID_GUI );


			}
			else
			{
				if ( controlGui.getGuard() ) // trying to guard invalid thing, cancel guard
					controlGui.toggleGuard();
				else if ( userInput->getMouseCursor() == mState_INFO && 
						target->isMover()  )
							controlGui.setInfoWndMover( static_cast<Mover*>(target) );
				else if ( !target->isDisabled() && !bForcedShot && !bAimedShot)
					doAttack();
			
			}
		}
	}

	// Task 7: Ctrl+Shift+LMB inspector pick. Runs before the legacy
	// gameplay picks so the inspector can claim the click first.
	// Priority: ImGui window capture > inspector > existing gameplay picks.
	//
	// Gate uses IsWindowHovered(AnyWindow) rather than WantCaptureMouse.
	// WantCaptureMouse is tainted by mouse_any_down (true whenever any button
	// is held, even over the viewport) and would suppress the pick during the
	// entire duration of every click.  IsWindowHovered is purely spatial --
	// true only when the cursor is physically over an ImGui window -- so it
	// correctly blocks the pick only when the user is interacting with ImGui.
	bool pickedByInspector = false;
#ifdef MC2_IMGUI
	if (ctrlDn && shiftDn && leftClicked && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
		GameplayPickRequest inspReq;
		inspReq.mouseX               = mouseX;
		inspReq.mouseY               = mouseY;
		inspReq.shiftDn              = true;    // shift IS held (ctrl+shift+LMB); required by tryGameplayPick gate
		inspReq.leftClicked          = true;
		inspReq.bGui                 = bGui;
		inspReq.bLeftDouble          = false;
		inspReq.moverSelectedThisFrame = false; // inspector always attempts the lookup
		GameplayPickResult inspResult = tryGameplayPick(inspReq);
		EditorInspector::setPickResult(mouseX, mouseY, inspResult.lookup);
		if (inspResult.lookup.isValid) {
			if (inspResult.lookup.kind == RenderWorld::RenderObjectKind::StaticProp) {
				EditorInspector::StaticPropInspectorData sd;
				sd.populated   = true;
				sd.recipeIndex = static_cast<int32_t>(inspResult.lookup.gameObjectId);
				GameAdapters::StaticProp::getRecipeShapeName(
				    sd.recipeIndex, sd.shapeName);
				sd.materialGpuActive = GameAdapters::StaticProp::isMaterialGpuActive();
				{
					RenderCore::MaterialHandle mh;
					mh.bits = inspResult.lookup.materialHandleBits;
					sd.materialIdx = mh.index();
					sd.materialGen = mh.generation();
				}
				sd.materialGpuPopulated = GameAdapters::StaticProp::getMaterialGpuData(
				    inspResult.lookup.materialHandleBits, &sd.materialGpu);
				EditorInspector::setStaticPropData(sd);
			} else if (inspResult.lookup.kind == RenderWorld::RenderObjectKind::Mech) {
				BattleMech* bm = GameAdapters::Mech::findMechByHandle(inspResult.lookup.handle);
				if (bm) {
					EditorInspector::MechInspectorData md;
					md.populated   = true;
					strncpy_s(md.variantName, sizeof(md.variantName), bm->variantName, _TRUNCATE);
					strncpy_s(md.longName,    sizeof(md.longName),    bm->longName,    _TRUNCATE);
					md.chassisClass = static_cast<int>(bm->chassisClass);
					md.teamId       = bm->getTeamId();
					md.disabled     = bm->isDisabled();
					md.destroyed    = bm->isDestroyed();
					md.crippled     = bm->isCrippled();
					md.conStat      = bm->conStat;
					for (int i = 0; i < MAX_MOVER_ARMOR_LOCATIONS; ++i) {
						md.totalCurArmor += bm->armor[i].curArmor;
						md.totalMaxArmor += static_cast<float>(bm->armor[i].maxArmor);
					}
					for (int i = 0; i < MAX_MOVER_BODY_LOCATIONS; ++i) {
						md.totalCurStr += bm->body[i].curInternalStructure;
						md.totalMaxStr += static_cast<float>(bm->body[i].maxInternalStructure);
					}
					MechWarriorPtr pilot = bm->getPilot();
					if (pilot)
						strncpy_s(md.pilotName, sizeof(md.pilotName), pilot->getName(), _TRUNCATE);
					EditorInspector::setMechData(md);
				}
#ifdef MC2_IS_EDITOR
				else if (Unit* u = EditorObjectMgr::instance()->findUnitByMechHandle(inspResult.lookup.handle)) {
					EditorInspector::MechInspectorData md;
					md.populated = true;
					strncpy_s(md.variantName, sizeof(md.variantName), u->getDisplayName(), _TRUNCATE);
					md.teamId    = u->getAlignment();
					Pilot* ep    = u->getPilot();
					if (ep)
						strncpy_s(md.pilotName, sizeof(md.pilotName), ep->getName(), _TRUNCATE);
					EditorInspector::setMechData(md);
				}
#endif
			}
		}
		// IMG-INSPECT-2: terrain CPU fallback when object-ID misses.
		// Terrain::IsGameSelectTerrainPosition(wPos) is the canonical
		// "world point is on selectable map terrain" guard (also used
		// at line ~789 for LOS/cell lookup). If OID buffer is disabled
		// (MC2_OBJECT_ID_BUFFER=0), inspResult.lookup.isValid is always
		// false here so terrain wins on any in-map click - expected.
		bool inspectorConsumed = inspResult.lookup.isValid;
		if (!inspResult.lookup.isValid && land &&
				Terrain::IsGameSelectTerrainPosition(wPos)) {
			EditorInspector::TerrainInspectorData td;
			td.populated = true;
			td.worldX    = static_cast<float>(wPos.x);
			td.worldY    = static_cast<float>(wPos.y);
			td.worldZ    = static_cast<float>(wPos.z);
			land->worldToTile(wPos, td.tileRow, td.tileCol);
			land->worldToCell(wPos, td.cellRow, td.cellCol);
			EditorInspector::setTerrainData(td);
			inspectorConsumed = true;
		}
		if (inspectorConsumed)
			pickedByInspector = true;
	}
#endif

	// M1.6: env-gated static-prop pick attempt. Runs AFTER the legacy
	// mover-selection path above. Short-circuits internally when
	// moverSelectedThisFrame == true (Section 11 + Q6/Q8 invariant) or
	// when MC2_STATIC_PROP_PICK / MC2_OBJECT_ID_BUFFER are off.
	if (!pickedByInspector) {
		const bool bLeftDouble = userInput->isLeftDoubleClick();
		tryStaticPropPick(moverSelectedThisFrame,
		                  shiftDn,
		                  leftClicked,
		                  bGui,
		                  bLeftDouble,
		                  mouseX,
		                  mouseY);
		// M2.6: mech-pick consumer fires second. Both callers invoke the
		// shared tryGameplayPick spine; each kind-guards on r.lookup.kind.
		// At most one hit branch fires per click (handle ranges disjoint:
		// static-prop indices < kMechHandleBase, mech indices >= 65536).
		tryMechPick(moverSelectedThisFrame,
		            shiftDn,
		            leftClicked,
		            bGui,
		            bLeftDouble,
		            mouseX,
		            mouseY);
	}
}
void MissionInterfaceManager::updateAOEStyle(bool shiftDn, bool altDn, bool ctrlDn,
											  bool bGui, bool lineOfSight, bool passable, 
											  long moverCount, long nonMoverCount )
{
	ZoneScopedN("MIF.AOEStyle");
	printDebugInfo();

	long commanderID = Commander::home->getId();

   	if ( userInput->getKeyDown( WAYPOINT_KEY ) || controlGui.getMines() )
   	{
		drawWayPointPaths(); 	
		//Can Never make a patrol path.  Causes movement wubbies!
//		if ( makePatrolPath() )
//			return;
   	}

	int mState = userInput->getMouseCursor();

	bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick());
	bool rightClicked = (!userInput->isLeftDrag() && !userInput->wasRightDrag() && userInput->rightMouseReleased());

	// M1.6 Q6 4-site observable: set to true immediately after each of
	// the setSelected(true) writer sites this style body owns (Shift+
	// additive site and plain-LMB-select site). Read at tail by
	// tryStaticPropPick to short-circuit when the legacy path already
	// claimed this click. Local scope; no leakage.
	bool moverSelectedThisFrame = false;

	if ( update( leftClicked, rightClicked, mouseX, mouseY, target,  lineOfSight ) )
		return;

    // Selection resolves own units first through the same bounds service.
    if (selectClicked && !bGui && !ctrlDn && !bForcedShot && !bAimedShot
        && !controlGui.getGuard() && !controlGui.getRepair() && !controlGui.getSalvage()
        && !controlGui.getGuardTower() && !controlGui.isAddingSalvage()
        && !controlGui.isAddingVehicle() && !controlGui.isAddingAirstrike()) {
        if (auto* selected = pickGameplaySelection(mouseX, mouseY))
            target = selected;
    }


	if ( bGui ) // don't target or anything if we are int the control panel
	{
		if ( !canJump() && controlGui.getJump() )
			controlGui.toggleJump();

		userInput->setMouseCursor( mState );
		if ( controlGui.mouseInVehicleStopButton )
			return;
		if ( !userInput->isLeftDrag() && !controlGui.isAddingAirstrike() && !controlGui.isAddingVehicle() && !controlGui.isAddingSalvage() )
			return;
	}
	
	Team* pTeam = Team::home;
	
	if (target) // if there is a target, make the appropritate cursor
	{
		userInput->setMouseCursor( makeTargetCursor( lineOfSight, moverCount, nonMoverCount ) );
	}
	else // make a move cursor
	{
		userInput->setMouseCursor( makeNoTargetCursor( passable, lineOfSight, ctrlDn, bGui, moverCount, nonMoverCount ) );
	}

	if ( userInput->rightMouseReleased() && !userInput->wasRightDrag() && !bGui) // move on the mouse ups
	{
		// Discrete-click precision fix, mirror of updateOldStyle (438e0117): the
		// per-frame wPos is screenToGroundPlaneApprox (z=0 ground-plane point that
		// drifts ~elevation in XY on hilly terrain / oblique camera pitch -> move
		// orders landed on the "other side of the map"). For this discrete right-
		// click move/place/recover action recompute wPos once via inverseProject
		// (the real terrain-surface hit). Click-only, so its O(quads) cost is fine.
		{
			Stuff::Vector2DOf<long> clickXY;
			clickXY.x = mouseX;
			clickXY.y = mouseY;
			const std::chrono::steady_clock::time_point _cpuT0 =
				s_gpParityOn ? std::chrono::steady_clock::now()
				             : std::chrono::steady_clock::time_point{};
			eye->inverseProject(clickXY, wPos);
			// GPU-GROUND-PICK-PARITY-1: precise per-click move-order pick (R2 case),
			// right-click variant. Parity-only; wPos is untouched.
			if (s_gpParityOn) {
				const unsigned long long _cpuNs = (unsigned long long)
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - _cpuT0).count();
				gpParitySite(mouseX, mouseY, _cpuNs, wPos.x, wPos.y, wPos.z, "Rmove");
			}
		}

		if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && canAddVehicle( wPos )) ||
			(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && canRecover( wPos )))
		{
			// Target has already been confirmed  as a mover(BattleMech) by canRecover
			// OR it doesn't matter because the canAddVehicle part of the beginVtol function doesn't reference it!
			// Again, Its OK because we only use it for salvage craft.
			// Need to pass this in so that Multiplayer can pass it in.
			beginVtol(-1,commanderID,nullptr,static_cast<MoverPtr>(target));	//In Single player, this should always be zero?
			return;
		}
		else if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && !canAddVehicle( wPos )) || 
				(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && !canRecover( wPos )))
		{
			soundSystem->playDigitalSample( INVALID_GUI );
			return;
		}
		else if ( controlGui.isAddingAirstrike() && !paintingVtol[commanderID] ) // if we're painting the vtol, carry on
		{
			addAirstrike();
			return;
		}
		
		else if (!target )
		{
			if ( controlGui.isSelectingInfoObject() )
			{
				soundSystem->playDigitalSample( INVALID_GUI );
				controlGui.cancelInfo();
			}
			else if (Terrain::IsGameSelectTerrainPosition(wPos))
			{
				if ( passable || selectionIsHelicopters() )
				{
					if (controlGui.getGuardTower())
						doGuardTower();
					else if ( controlGui.getGuard() )
					{
						doGuard(nullptr);
					}
					else				
						doMove( wPos );
				}
				else
				{
					userInput->setMouseCursor(mState_DONT);
				}
			}
			else
			{
				userInput->setMouseCursor(mState_DONT);
			}
		} 
		else if ( controlGui.getSalvage() )
			doSalvage();
		else if ( controlGui.getGuard() )
			doGuard(target);
		else if ( controlGui.getRepair() && canRepair( target ) )
			doRepair(target);
		else if ( controlGui.getGuardTower() )
			doGuardTower();
		else if ( target->getTeam() && Team::home->isFriendly(target->getTeam()) && target->getFlag(OBJECT_FLAG_CANREFIT) && target->getFlag(OBJECT_FLAG_MECHBAY) && canRepairBay(target) )
			doRepairBay(target);
		else if ( controlGui.getSalvage() && canSalvage( target ))
			doSalvage();
		else if ( controlGui.isSelectingInfoObject() && target->isMover() )
			controlGui.setInfoWndMover( static_cast<Mover*>(target) );
		else if ( target->getTeam() != Team::home && !target->isDisabled() )
			doAttack();
		else if (!bForcedShot && !bAimedShot)
			soundSystem->playDigitalSample( INVALID_GUI );
	
	}
	else if ( selectClicked && !bGui )
	{
			//We clicked on a target.  If mouse cursor is normal, we want to select a friendly.
				//if the mouse cursor is some form of attack, attack the target.
		if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && canAddVehicle( wPos )) || 
			(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && canRecover( wPos )))
		{
			// Target has already been confirmed  as a mover(BattleMech) by canRecover.
			// OR it doesn't matter because the canAddVehicle part of the beginVtol function doesn't reference it!
			// Again, Its OK because we only use it for salvage craft.
			// Need to pass this in so that Multiplayer can pass it in.
			beginVtol(-1,commanderID,nullptr,static_cast<MoverPtr>(target));	//In Single player, this should always be zero?
			return;
		}
		else if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && !canAddVehicle( wPos )) || 
				(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && !canRecover( wPos )))
		{
			soundSystem->playDigitalSample( INVALID_GUI );
			return;
		}
		else if ( controlGui.isAddingAirstrike() && !paintingVtol[commanderID] ) // if we're painting the vtol, carry on
		{
			addAirstrike();
			return;
		}

		else if ( target )
		{
			if ( target->isMover() && ( target->getTeamId() == Team::home->getId() || 
					CONTACT_VISUAL == (static_cast<Mover*>(target))->getContactStatus(Team::home->getId(), true)
					|| target->isDisabled() ) )
				{
					controlGui.setInfoWndMover( static_cast<Mover*>(target) );
				}
		
			if (target->getCommanderId() == Commander::home->getId() && !target->isDisabled()  )
			{
				
				if (shiftDn)
				{
					bool alreadyThere = false;
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
						if (pMover == target && pMover->isSelected() && commanderIsHome(pMover->getCommander()) )
							alreadyThere = true;
					}
					
					if (!alreadyThere && !target->isDisabled()) {
						target->setSelected( true );
						// M1.6 Q6 site 3: legacy Shift+additive-select on
						// friendly mover (AOE style). Sets fallback gate;
						// tail tryStaticPropPick short-circuits so
						// Section 11 invariant holds.
						moverSelectedThisFrame = true;
					} else {
						// M1.6 Q6 RESOLVED: setSelected(false) toggle-off
						// site MUST NOT set the gate per spec rationale.
						target->setSelected( false );
					}
				}
				else if ( target->isMover() )
				{
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
						if (commanderIsHome(pMover->getCommander()))
						{
							// M1.6 Q6 RESOLVED: clear-others setSelected(false)
							// loop MUST NOT set the gate.
							pMover->setSelected( false );
						}
					}

					target->setSelected( true );
					// M1.6 Q6 site 4: legacy plain-LMB-select on friendly
					// mover (AOE style). Sets fallback gate.
					moverSelectedThisFrame = true;
				}
			}
		}
		else
		{	
			controlGui.cancelInfo();
			soundSystem->playDigitalSample( INVALID_GUI );
		}
	}

	// Task 7: Ctrl+Shift+LMB inspector pick. Runs before the legacy
	// gameplay picks so the inspector can claim the click first.
	// Priority: ImGui window capture > inspector > existing gameplay picks.
	//
	// Gate uses IsWindowHovered(AnyWindow) rather than WantCaptureMouse.
	// WantCaptureMouse is tainted by mouse_any_down (true whenever any button
	// is held, even over the viewport) and would suppress the pick during the
	// entire duration of every click.  IsWindowHovered is purely spatial --
	// true only when the cursor is physically over an ImGui window -- so it
	// correctly blocks the pick only when the user is interacting with ImGui.
	bool pickedByInspector = false;
#ifdef MC2_IMGUI
	if (ctrlDn && shiftDn && leftClicked && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
		GameplayPickRequest inspReq;
		inspReq.mouseX               = mouseX;
		inspReq.mouseY               = mouseY;
		inspReq.shiftDn              = true;    // shift IS held (ctrl+shift+LMB); required by tryGameplayPick gate
		inspReq.leftClicked          = true;
		inspReq.bGui                 = bGui;
		inspReq.bLeftDouble          = false;
		inspReq.moverSelectedThisFrame = false; // inspector always attempts the lookup
		GameplayPickResult inspResult = tryGameplayPick(inspReq);
		EditorInspector::setPickResult(mouseX, mouseY, inspResult.lookup);
		if (inspResult.lookup.isValid) {
			if (inspResult.lookup.kind == RenderWorld::RenderObjectKind::StaticProp) {
				EditorInspector::StaticPropInspectorData sd;
				sd.populated   = true;
				sd.recipeIndex = static_cast<int32_t>(inspResult.lookup.gameObjectId);
				GameAdapters::StaticProp::getRecipeShapeName(
				    sd.recipeIndex, sd.shapeName);
				sd.materialGpuActive = GameAdapters::StaticProp::isMaterialGpuActive();
				{
					RenderCore::MaterialHandle mh;
					mh.bits = inspResult.lookup.materialHandleBits;
					sd.materialIdx = mh.index();
					sd.materialGen = mh.generation();
				}
				sd.materialGpuPopulated = GameAdapters::StaticProp::getMaterialGpuData(
				    inspResult.lookup.materialHandleBits, &sd.materialGpu);
				EditorInspector::setStaticPropData(sd);
			} else if (inspResult.lookup.kind == RenderWorld::RenderObjectKind::Mech) {
				BattleMech* bm = GameAdapters::Mech::findMechByHandle(inspResult.lookup.handle);
				if (bm) {
					EditorInspector::MechInspectorData md;
					md.populated   = true;
					strncpy_s(md.variantName, sizeof(md.variantName), bm->variantName, _TRUNCATE);
					strncpy_s(md.longName,    sizeof(md.longName),    bm->longName,    _TRUNCATE);
					md.chassisClass = static_cast<int>(bm->chassisClass);
					md.teamId       = bm->getTeamId();
					md.disabled     = bm->isDisabled();
					md.destroyed    = bm->isDestroyed();
					md.crippled     = bm->isCrippled();
					md.conStat      = bm->conStat;
					for (int i = 0; i < MAX_MOVER_ARMOR_LOCATIONS; ++i) {
						md.totalCurArmor += bm->armor[i].curArmor;
						md.totalMaxArmor += static_cast<float>(bm->armor[i].maxArmor);
					}
					for (int i = 0; i < MAX_MOVER_BODY_LOCATIONS; ++i) {
						md.totalCurStr += bm->body[i].curInternalStructure;
						md.totalMaxStr += static_cast<float>(bm->body[i].maxInternalStructure);
					}
					MechWarriorPtr pilot = bm->getPilot();
					if (pilot)
						strncpy_s(md.pilotName, sizeof(md.pilotName), pilot->getName(), _TRUNCATE);
					EditorInspector::setMechData(md);
				}
#ifdef MC2_IS_EDITOR
				else if (Unit* u = EditorObjectMgr::instance()->findUnitByMechHandle(inspResult.lookup.handle)) {
					EditorInspector::MechInspectorData md;
					md.populated = true;
					strncpy_s(md.variantName, sizeof(md.variantName), u->getDisplayName(), _TRUNCATE);
					md.teamId    = u->getAlignment();
					Pilot* ep    = u->getPilot();
					if (ep)
						strncpy_s(md.pilotName, sizeof(md.pilotName), ep->getName(), _TRUNCATE);
					EditorInspector::setMechData(md);
				}
#endif
			}
		}
		// IMG-INSPECT-2: terrain CPU fallback when object-ID misses.
		// Terrain::IsGameSelectTerrainPosition(wPos) is the canonical
		// "world point is on selectable map terrain" guard (also used
		// at line ~789 for LOS/cell lookup). If OID buffer is disabled
		// (MC2_OBJECT_ID_BUFFER=0), inspResult.lookup.isValid is always
		// false here so terrain wins on any in-map click - expected.
		bool inspectorConsumed = inspResult.lookup.isValid;
		if (!inspResult.lookup.isValid && land &&
				Terrain::IsGameSelectTerrainPosition(wPos)) {
			EditorInspector::TerrainInspectorData td;
			td.populated = true;
			td.worldX    = static_cast<float>(wPos.x);
			td.worldY    = static_cast<float>(wPos.y);
			td.worldZ    = static_cast<float>(wPos.z);
			land->worldToTile(wPos, td.tileRow, td.tileCol);
			land->worldToCell(wPos, td.cellRow, td.cellCol);
			EditorInspector::setTerrainData(td);
			inspectorConsumed = true;
		}
		if (inspectorConsumed)
			pickedByInspector = true;
	}
#endif

	// M1.6: env-gated static-prop pick attempt. Runs AFTER the legacy
	// mover-selection path above. Short-circuits internally when
	// moverSelectedThisFrame == true (Section 11 + Q6/Q8 invariant) or
	// when MC2_STATIC_PROP_PICK / MC2_OBJECT_ID_BUFFER are off.
	if (!pickedByInspector) {
		const bool bLeftDouble = userInput->isLeftDoubleClick();
		tryStaticPropPick(moverSelectedThisFrame,
		                  shiftDn,
		                  leftClicked,
		                  bGui,
		                  bLeftDouble,
		                  mouseX,
		                  mouseY);
		// M2.6: mech-pick consumer (mirrors updateOldStyle tail).
		tryMechPick(moverSelectedThisFrame,
		            shiftDn,
		            leftClicked,
		            bGui,
		            bLeftDouble,
		            mouseX,
		            mouseY);
	}
}

void MissionInterfaceManager::updateWaypoints (void)
{
	// Update the waypoint markers so that they are visible and must happen AFTER camera update!!
	// Or they wiggle something fierce.
   	if ( userInput->getKeyDown( WAYPOINT_KEY ) || controlGui.getMines() )
   	{
   		Team* pTeam = Team::home;
   		for (long i=0;i<pTeam->getRosterSize();i++)
   		{
			Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
   			if ( pMover->isSelected() && commanderIsHome(pMover->getCommander()) )
   			{
   				pMover->updateDrawWaypointPath();
   			}
   		}
   		
   	}
}

int MissionInterfaceManager::update( bool leftClickedClick, bool rightClickedClick, int MouseX, int MouseY, GameObject* pTarget, bool bLOS )
{
	ZoneScopedN("MIF.Commands");
	bool shiftDn = userInput->shift();
	bool ctrlDn = userInput->ctrl();
	bool altDn = userInput->alt();
	bool wayPtDn = userInput->getKeyDown( WAYPOINT_KEY );

	bool bRetVal = 0;

	int i = 0;
	int last = MAX_COMMAND;

	if ( controlGui.updateChat() )
	{
		i = 100;
		last = 102;
		bRetVal = 1;
	}


	if ( gos_GetKeyStatus( WAYPOINT_KEY ) == KEY_RELEASED )
	{
		Team* pTeam  = Team::home;
		for (long i = 0; i < pTeam->getRosterSize(); i++)
		{
			Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
			if ( pMover->isSelected() && !pMover->getPilot()->getExecutingTacOrderQueue() && commanderIsHome(pMover->getCommander()) )
			{
				if (MPlayer && !MPlayer->isServer()) {
					TacticalOrder tacOrder;
					tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_WAYPOINTS_DONE);
					tacOrder.pack(nullptr, nullptr);					
					MPlayer->sendPlayerOrder(&tacOrder, false, 1, &pMover);
					}
				else {
					pMover->getPilot()->setExecutingQueue(true);
					pMover->getPilot()->executeTacOrderQueue();
				}
			}
		}
		bRetVal = 1;
	}

	mouseX = MouseX;
	mouseY = mouseY;
	setTarget( pTarget );


	if ( !bRetVal )
	{
		for ( int j = KEY_0; j < KEY_9 + 1; j++ )
		{
			if ( userInput->getKeyDown( (gosEnum_KeyIndex)j )
				&& gos_GetKeyStatus( (gosEnum_KeyIndex)j ) != KEY_HELD )
			{
				if ( ctrlDn )
					makeForceGroup( j - KEY_0 );
				else if ( shiftDn )
					selectForceGroup( j - KEY_0, 0 );
				else
					selectForceGroup( j - KEY_0, 1 );
			}
		}
	}

	for ( ; i < last; i++ )
	{
		// Reserved command slots have no action. Polling their zero key emits
		// synchronous PAUSE diagnostics three times per frame.
		if (!commands[i].function && !commands[i].releaseFunction)
			continue;

		int key = commands[i].key;
		if ( userInput->getKeyDown( gosEnum_KeyIndex(key & 0x0000000ff) ) )
		{
			// check for shifts and stuff
			// must check way pt first, because it can be shift or ctrl
			if ( (key & WAYPT) )
			{	if ( !wayPtDn )
   					continue;
			}
			else if ( ((key & SHIFT) ? true : false) != shiftDn )
				continue;
			else if ( ((key & CTRL) ? true : false) != ctrlDn )
				continue;
			else if ( ((key & ALT) ? true : false) != altDn )
				continue;
			
			if ( commands[i].key != -1 )
				userInput->setMouseCursor(bLOS ? commands[i].cursorLOS : commands[i].cursor);

			if ( !commands[i].singleClick )
			{
				if ( (this->*commands[i].function)() )
					bRetVal = 1;
			}
			else if ( gos_GetKeyStatus( (gosEnum_KeyIndex)(key & 0x000fffff) ) != KEY_HELD )
			{
				if ( this->commands[i].function && (this->*commands[i].function)() )
					bRetVal = 1;
				terrainLineChanged = turn;
			}	
		}
		else if ( gos_GetKeyStatus( gosEnum_KeyIndex(key & 0x000fffff) ) == KEY_RELEASED 
			&& commands[i].releaseFunction )
		{
			if ( (this->*commands[i].releaseFunction)() )
				bRetVal = 1;
		}
	}

	return bRetVal;
}
