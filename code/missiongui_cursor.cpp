// MissionInterfaceManager: camera movement, drag selection, jump/recover checks and the target / range / no-target / move / jump / run cursors.
// Declared in the class headers; shared names in missiongui_shared_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include <utility>
#include "missiongui_shared_internal.h"
constexpr float ATTILA_FACTOR = 1.0f;
constexpr float ATTILA_ROTATION_FACTOR = 4.1f;
constexpr float ATTILA_THRESHOLD = 0.01f;

bool MissionInterfaceManager::moveCameraAround( bool lineOfSight, bool passable, bool ctrl, bool bGui, 
											   long moverCount, long nonMoverCount )
{
	bool bRetVal = 0;
	bool middleClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isMiddleClick());

	if ( useLeftRightMouseProfile && ((userInput->isLeftClick() && userInput->getKeyDown(KEY_T)) || userInput->isLeftDoubleClick()) && target )
	{
		if (eye)
			(static_cast<GameCamera *>(eye))->setTarget(target);
	}

	// Stage 2.E: when MC2_VISUAL_DIFF_CAPTURE=1, skip edge-scroll entirely.
	// The OS cursor sits wherever the user left it (often a screen corner),
	// which would drive the camera differently each run and torpedo
	// same-config visual-diff determinism.
	if (!cameraClicked && !isPaused() && !VisualDiff::isCaptureEnabled() && !SmokeMode::state().enabled) // sebi: only scoll if not paused
	{
		if ( mouseX <= (screenScrollLeft))
			scrollLeft();

		if ((mouseX >= (Environment.screenWidth - screenScrollRight)) )
			scrollRight();

		if ( (mouseY <= (screenScrollUp) && mouseY >= -screenScrollUp ) )
			scrollUp();

		if ( (mouseY >= (Environment.screenHeight - screenScrollDown) ) )
			scrollDown();
	}

	if (attilaXAxis < -ATTILA_THRESHOLD)
	{
		float oldScrollInc = scrollInc;
		scrollInc *= fabs(attilaXAxis) * ATTILA_FACTOR;
		scrollLeft();
		scrollInc = oldScrollInc;
	}
	
	if (attilaXAxis > ATTILA_THRESHOLD)
	{
		float oldScrollInc = scrollInc;
		scrollInc *= fabs(attilaXAxis) * ATTILA_FACTOR;
		scrollRight();
		scrollInc = oldScrollInc;
	}
	
	if (attilaYAxis < -ATTILA_THRESHOLD)
	{
		float oldScrollInc = scrollInc;
		scrollInc *= fabs(attilaYAxis) * ATTILA_FACTOR;
		scrollUp();
		scrollInc = oldScrollInc;
	}
	
	if (attilaYAxis > ATTILA_THRESHOLD)
	{
		float oldScrollInc = scrollInc;
		scrollInc *= fabs(attilaYAxis) * ATTILA_FACTOR;
		scrollDown();
		scrollInc = oldScrollInc;
	}
 	
	if (attilaRZAxis < -ATTILA_THRESHOLD)
	{
		eye->rotateLeft(rotationInc * fabs(attilaRZAxis) * frameLength * ATTILA_ROTATION_FACTOR);
	}
	
	if (attilaRZAxis > ATTILA_THRESHOLD)
	{
		eye->rotateRight(rotationInc * fabs(attilaRZAxis) * frameLength * ATTILA_ROTATION_FACTOR);
	}
	
	if (cameraClicked)
	{
		bRetVal = 1;
		int mouseXDelta = userInput->getMouseXDelta();
		float actualRot = rotationInc * 0.1f * abs(mouseXDelta);
		if (mouseXDelta > 0)
		{
			eye->rotateRight(actualRot);
			bRetVal = 1;	
		}
		else if (mouseXDelta < 0)
		{
			eye->rotateLeft(actualRot);
			bRetVal = 1;	
		}
		
		int mouseYDelta = userInput->getMouseYDelta();
		float actualTilt = rotationInc * 0.1f * abs(mouseYDelta);
		if (mouseYDelta > 0)
		{
			eye->tiltDown(actualTilt);
			bRetVal = 1;
	
		}
		else if (mouseYDelta < 0)
		{
			eye->tiltUp(actualTilt);
			bRetVal = 1;
	
		}
		// Manual right-drag rotate/tilt while overview is active = the user took
		// over the camera; suppress the auto-return on exit.
		if (mouseXDelta || mouseYDelta)
			g_tacticalOverview.notifyUserPan();
		userInput->setMouseCursor( mState_ROTATE_CAMERA );

		long mouseWheelDelta = userInput->getMouseWheelDelta();
		if (mouseWheelDelta)
		{
			g_tacticalOverview.onWheel(-mouseWheelDelta,
			                           /*atCeiling=*/(g_tacticalOverview.setpoint() > 0.0f) || (eye->getCameraAltitude() >= 5900.0f),
			                           frameLength, /*worldOwnsWheel=*/true);
			//Mouse wheel just picks zooms now.
			//float actualZoom = zoomInc * abs(mouseWheelDelta) * 0.0001f * eye->getScaleFactor();
			// [MOUSE-ANCHORED-ZOOM-1 / FIX-4] zoom toward cursor when gated on.
			if (mouseWheelDelta > 0)
			{
				anchoredZoom(userInput->getMouseX(), userInput->getMouseY(), &MissionInterfaceManager::zoomChoiceOut);
			}
			else
			{
				anchoredZoom(userInput->getMouseX(), userInput->getMouseY(), &MissionInterfaceManager::zoomChoiceIn);
			}

			if ( target )
				userInput->setMouseCursor( makeTargetCursor( lineOfSight, moverCount, nonMoverCount ) );
			else
				userInput->setMouseCursor( makeNoTargetCursor( passable, lineOfSight, ctrl, bGui, moverCount, nonMoverCount ) );
			bRetVal = 1;
		}

		return bRetVal;
	}
	
	long mouseWheelDelta = userInput->getMouseWheelDelta();
	if (mouseWheelDelta)
	{
		g_tacticalOverview.onWheel(-mouseWheelDelta,
		                           /*atCeiling=*/(g_tacticalOverview.setpoint() > 0.0f) || (eye->getCameraAltitude() >= 5900.0f),
		                           frameLength, /*worldOwnsWheel=*/true);
		//Mouse wheel just picks zooms now.
		//float actualZoom = zoomInc * abs(mouseWheelDelta) * 0.0001f * eye->getScaleFactor();
		// [MOUSE-ANCHORED-ZOOM-1 / FIX-4] zoom toward cursor when gated on.
		if (mouseWheelDelta > 0)
		{
			anchoredZoom(userInput->getMouseX(), userInput->getMouseY(), &MissionInterfaceManager::zoomChoiceOut);
		}
		else
		{
			anchoredZoom(userInput->getMouseX(), userInput->getMouseY(), &MissionInterfaceManager::zoomChoiceIn);
		}

		if ( target )
			userInput->setMouseCursor( makeTargetCursor( lineOfSight, moverCount, nonMoverCount ) );
		else
			userInput->setMouseCursor( makeNoTargetCursor( passable, lineOfSight, ctrl, bGui, moverCount, nonMoverCount ) );
		bRetVal = 1;
	}

	if (middleClicked)
	{
		cameraNormal();
	}				
	
	doDrag(bGui);

	return bRetVal;

}

bool MissionInterfaceManager::canJump()
{
	bool canJump = true;
	long i=0;

	Team* pTeam = Team::home;
	
	while ((i<pTeam->getRosterSize()))
	{
		Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
		{
			if ( !pMover->canJump() )
			{
				canJump = false;
				break;
			}
		}
		i++;
	}

	return canJump;

}

bool MissionInterfaceManager::canJumpToWPos()
{
	bool passable = 0;
	if ( Terrain::IsGameSelectTerrainPosition( wPos ) )
	{
		int cellR, cellC;
		land->worldToCell(wPos, cellR, cellC);
		passable = GameMap->getPassable(cellR,cellC);
	}
	
	if (passable)
	{
		bool canJump = true;
		long i=0;

		Team* pTeam = Team::home;
		
		while ((i<pTeam->getRosterSize()))
		{
			Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
			if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
			{
				if ( pMover->canJump() )
				{
					TacticalOrder lastQueuedTacOrder;
					long err = pMover->getPilot()->peekQueuedTacOrder(-1, &lastQueuedTacOrder);
					glm::vec3 lastWayPoint;
					if (err)
						lastWayPoint = pMover->getPosition();
					else
						lastWayPoint = lastQueuedTacOrder.getWayPoint(0);
					glm::vec3 distance;
					distance = lastWayPoint - wPos;

					//JUMP distance is always JUST horizontal.  NO Vertical!!
					distance.z = 0.0f;
					float dist = glm::length(distance);
					
					if (dist > pMover->getJumpRange())
						canJump = false;
				}
				else
					canJump = false;
			}
			i++;
		}

		return canJump;
	}

	return 0;
	
}

void MissionInterfaceManager::doDrag(bool bGui)
{
	// Formation line draw mode owns the left-drag gesture: no select box and
	// no drag-select while armed or drawing.
	if ( TacticalOverview::formationLineEnabled()
		&& g_tacticalOverview.flState() != TacticalOverview::FL_IDLE )
	{
		dragStart = glm::vec3(0.0f);
		dragEnd = glm::vec3(0.0f);
		isDragging = FALSE;
		return;
	}

	if ( ((gos_GetKeyStatus( KEY_LMOUSE ) == KEY_PRESSED)) && !bGui && !cameraClicked
		&& !dragStart.x)
	{
	
 			dragStart.x = wPos.x;
			dragStart.y = wPos.y;
			dragStart.z = wPos.z;
			isDragging = 0;			
	}
	
	else if ((gos_GetKeyStatus( KEY_LMOUSE ) == KEY_HELD)
		&& !isDragging && ( !(dragStart.x == 0.f && dragStart.y == 0.f )
		&& (dragStart.x != wPos.x && dragStart.y != wPos.y ) ) )
	{
		isDragging = TRUE;
		dragEnd.x = userInput->getRawMouseX();
		dragEnd.y = userInput->getRawMouseY();
	}

	else if (isDragging)
	{
		dragEnd.x = userInput->getRawMouseX();
		dragEnd.y = userInput->getRawMouseY();
	
		if ( (gos_GetKeyStatus( KEY_LMOUSE ) == KEY_RELEASED
			|| gos_GetKeyStatus( KEY_LMOUSE ) == KEY_FREE) && isDragging)
		{

		

			GameObject*	objsInRect[MAX_ICONS];
			int count = 0;
			memset( objsInRect, 0, sizeof( GameObject* ) * 16 );
			for (long i=0;i<ObjectManager->getNumMovers();i++)
			{
				glm::vec3 screenStart;
				Stuff::Vector4D screenPos;
				// [PROJECTZ:SelectionPicking id=picking_drag_select_origin]
				// [PROJECTZ:SelectionPicking id=picking_drag_select_origin]
				PROJECTZ_SITE("picking_drag_select_origin", "SelectionPicking");
				eye->projectForSelectionPicking( dragStart, screenPos );
				screenStart.x = screenPos.x;
				screenStart.y = screenPos.y;
			   GameObjectPtr	mover = ObjectManager->getMover(i);
			   if (ObjectManager->moverInRect(i,screenStart,dragEnd) && 
				   mover->getCommanderId() == Commander::home->getId() && 
				   count < MAX_ICONS
				   && !mover->isDisabled())
				{
				   objsInRect[count] = mover;
				   count++;
				}
			}

			if ( count )
			{
				controlGui.setInfoWndMover( nullptr );

				if ( !userInput->shift() )
				{
					for (int i=0;i<ObjectManager->getNumMovers();i++)
					{
						ObjectManager->getMover(i)->setSelected( 0 );
					}
				}

				for (int i = 0; i < count; i ++ )
				{
					objsInRect[i]->setSelected( true );
				}

				if ( count == 1 )
					(controlGui).setInfoWndMover( static_cast<Mover*>(objsInRect[0]) );
			}
			
			dragStart = glm::vec3(0.0f);
			dragEnd = glm::vec3(0.0f);		
			isDragging = FALSE;
		}
	}
	else if ( gos_GetKeyStatus(KEY_LMOUSE) == KEY_RELEASED )
	{
		dragStart = glm::vec3(0.0f);
	}
}

bool MissionInterfaceManager::canAddVehicle( const glm::vec3& pos )
{
	long commanderID = Commander::home->getId();

 	if ( paintingVtol[commanderID] )
		return 0;
	int tileI, tileJ;
	land->worldToTile( pos, tileJ, tileI );

	if ( tileJ > -1 && tileJ < land->realVerticesMapSide 
		 && tileI > -1 && tileI < land->realVerticesMapSide )
	{
		if ( Team::home->teamLineOfSight(pos, 0.0f) )
		{
			if ( GameMap->getPassable( pos )
				&& (!TerrainRuntime::sampleWaterClass(pos) ))
				return true;
			else 
				return false;
		}	
	}
	

	return false;
}

bool MissionInterfaceManager::canRecover( const glm::vec3& pos )
{
	long commanderID = Commander::home->getId();

 	if ( paintingVtol[commanderID] )
		return 0;
		
	int tileI, tileJ;
	land->worldToTile( pos, tileJ, tileI );

	if ( tileJ > -1 && tileJ < land->realVerticesMapSide 
		 && tileI > -1 && tileI < land->realVerticesMapSide )
	{
		if ( Team::home->teamLineOfSight(pos, 0.0f) )
		{
			if ( target && (target->getObjectClass() == BATTLEMECH) && target->isDisabled() && !target->isDestroyed() )
				return true;
			else 
				return false;
		}	
	}
	

	return false;
}

long MissionInterfaceManager::makeTargetCursor( bool lineOfSight, long moverCount, long nonMoverCount)
{
	long commanderID = Commander::home->getId();

 	long currentCursor = makeRangeCursor( lineOfSight );
	if ( !moverCount && !nonMoverCount )
		currentCursor = mState_NORMAL;

	if ( ( userInput->getKeyDown( WAYPOINT_KEY ) ) )
	{
		currentCursor = mState_DONT;
		return currentCursor;
	}

	
	if ( controlGui.isSelectingInfoObject()  )
	{
		if ( target->isMover() )
		{
			currentCursor = mState_INFO;
		}
		else 
			currentCursor = mState_DONT;
		
		return currentCursor;
	}
	
	else if ( controlGui.isAddingVehicle() && !paintingVtol[commanderID] )
	{
		currentCursor = mState_DONT;
		return currentCursor;
	}

	else if ( controlGui.isAddingSalvage() && !paintingVtol[commanderID] )
	{
		if ( canSalvage(target) )
		{
			currentCursor = mState_SALVAGE;
			target->setTargeted(true);
		}
		else
			currentCursor = mState_XSALVAGE;

		return currentCursor;
	}
	else if ( controlGui.isAddingAirstrike() && !paintingVtol[commanderID] )
	{
		if ( controlGui.getButton( ControlGui::SENSOR_PROBE )->state == ControlButton::PRESSED )
		{
			currentCursor = lineOfSight ? mState_SENSORSTRIKE : mState_UNCERTAIN_AIRSTRIKE ;
		}
		else
		{
			currentCursor = lineOfSight ? mState_AIRSTRIKE : mState_UNCERTAIN_AIRSTRIKE ;
		}
		return currentCursor;
	}
	else if ( controlGui.getSalvage()  )
	{
		if ( canSalvage( target ) )
		{
			currentCursor = mState_SALVAGE;
			target->setTargeted(true);
		}
		else
			currentCursor = mState_XSALVAGE;
		
		return currentCursor;
	}
	else if ( controlGui.getRepair() )
	{
		if ( canRepair( target ) )
			currentCursor = mState_REPAIR;
		else
			currentCursor = mState_XREPAIR;

		if (target)
			target->setTargeted(true);

		return currentCursor;
	}
	else if ( controlGui.getMines() )
	{
		currentCursor = mState_DONT;
		return currentCursor;
	}
	else if ( controlGui.getGuardTower() )
	{
		currentCursor = lineOfSight ? mState_GUARDTOWER_LOS : mState_GUARDTOWER;
		return currentCursor;
	}
	else if ( target->isCaptureable(Team::home->getId() ) && moverCount )
	{
		for ( int i = 0; i < Team::home->getRosterSize(); i++ )
		{
			Mover* pMvr = Team::home->getMover( i );
			if ( pMvr->isSelected() && pMvr->getCommander()->getId() == Commander::home->getId() )
			{
				if ((pMvr->getObjectClass() == BATTLEMECH) &&
					(pMvr->getMoveType() == MOVETYPE_GROUND))
				{
					if ( target->getCaptureBlocker( pMvr) )
					{
						currentCursor = mState_XCAPTURE;
						break;
					}
					else
					{
						currentCursor = mState_CAPTURE;
					}
				}
				else
				{
					currentCursor = mState_XCAPTURE;
				}
			}
		}
	}	

			
	switch (target->getObjectClass())
	{
		case BATTLEMECH:
		case GROUNDVEHICLE:
		case ELEMENTAL:
		{
			if (target->getTeam() && !target->getTeam()->isFriendly(Team::home) && !target->getTeam()->isNeutral( Team::home ) )
			{
				target->setTargeted(true); 
				if ( target->isDisabled() )
				{
					currentCursor = mState_NORMAL;
				}
			}
			else
			{
				target->setTargeted(true);		//For friendlies, let us know we need to draw their names!

				currentCursor = mState_NORMAL;
				controlGui.setRolloverHelpText( IDS_UNIT_SELECT_HELP );
			}
		}
		break;
			
		case BUILDING:
		case TREEBUILDING:			
		case TURRET:
		case GATE:
		case TREE:
		case TERRAINOBJECT:
		{
			if ( target->getObjectType()->getSubType() == BUILDING_SUBTYPE_LANDBRIDGE )
			{
				if ( gos_GetKeyStatus( (gosEnum_KeyIndex)commands[std::to_underlying(FORCE_FIRE_KEY)].key ) == KEY_HELD ||
					 controlGui.getFireFromCurrentPos() )
				{
					target->setTargeted(true);
				}
				else
				{
					target = nullptr;
					// find out if this position is passable.  Landbridges kinda are, or they'd be useless.
					int cellR, cellC;
					bool passable = 1;
					bool lineOfSight = 0;
					if ( Terrain::IsGameSelectTerrainPosition( wPos ) )
					{
						land->worldToCell(wPos, cellR, cellC);
						passable = GameMap->getPassable(cellR, cellC);
					}
	
					return makeNoTargetCursor( passable, lineOfSight, 0, 0, moverCount, nonMoverCount );
				}
			}
			target->setTargeted(true);
			TeamPtr targetTeam = target->getTeam();

			if ( targetTeam && Team::home->isFriendly( targetTeam ) && 
				target->getFlag(OBJECT_FLAG_CANREFIT) && target->getFlag(OBJECT_FLAG_MECHBAY) )  
			{
				if (canRepairBay(target))
				{
					currentCursor = mState_REPAIR;
				}
				else
				{
					currentCursor = mState_XREPAIR;
				}
			}
			else if ( targetTeam && !controlGui.getGuard() &&
				Team::home->isFriendly( targetTeam ) && 				
				gos_GetKeyStatus( (gosEnum_KeyIndex)commands[std::to_underlying(FORCE_FIRE_KEY)].key ) != KEY_HELD // not forcing fire
				&& controlGui.getCurrentRange() == FIRERANGE_OPTIMAL ) // range attacks now are force fire
			{
				return makeNoTargetCursor( false, lineOfSight, 0, 0, moverCount, nonMoverCount );
			}
		}
		break;

		case BRIDGE:
			target = nullptr;
			return makeNoTargetCursor( false, lineOfSight, 0, 0, moverCount, nonMoverCount );
			break;

			
		case DEBRIS:
		{
		}
		break;			
	}
	
	if ( controlGui.getGuard() )
	{
		currentCursor = mState_GUARD;
		if (target)
			target->setTargeted(true);
	}
	else if ( gos_GetKeyStatus( (gosEnum_KeyIndex)commands[std::to_underlying(FORCE_FIRE_KEY)].key ) == KEY_HELD
		|| ( controlGui.getCurrentRange() != FIRERANGE_OPTIMAL && controlGui.getCurrentRange() != FIRERANGE_CURRENT ) )
	{
		currentCursor = makeRangeCursor( lineOfSight );
	}



	return currentCursor;
}

long MissionInterfaceManager::makeRangeCursor( bool lineOfSight)
{
	long currentCursor = mState_NORMAL;
	long currentRange = controlGui.getCurrentRange();

	if ( bEnergyWeapons )
		return lineOfSight ? mState_ENERGY_WEAPONS_LOS : mState_ENERGY_WEAPONS;
	
	switch( currentRange )
	{
	case FIRERANGE_SHORT:
		currentCursor = lineOfSight ? mState_SHRTRNG_LOS :  mState_SHRTRNG_ATTACK;
		break;
	case FIRERANGE_MEDIUM:
		currentCursor = lineOfSight ? mState_MEDRNG_LOS :   mState_MEDRNG_ATTACK;
		break;
	case FIRERANGE_LONG:
		currentCursor =  lineOfSight ? mState_LONGRNG_LOS :  mState_LONGRNG_ATTACK;
		break;
	case FIRERANGE_OPTIMAL:
		currentCursor =  lineOfSight ? mState_ATTACK_LOS :  mState_GENERIC_ATTACK;
		break;

	case FIRERANGE_CURRENT:
		currentCursor = lineOfSight ? mState_CURPOS_ATTACK_LOS : mState_CURPOS_ATTACK;
		break;
	}

	if ( controlGui.getFireFromCurrentPos() )
		currentCursor = lineOfSight ? mState_CURPOS_ATTACK_LOS : mState_CURPOS_ATTACK;

	return currentCursor;
}

long MissionInterfaceManager::makeNoTargetCursor( bool passable, bool lineOfSight, bool bCtrl, bool bGui,
												 long moverCount, long nonMoverCount )
{
	//-------------------------------------------------------------------------
	// We are not over enything.  Switch to move cursor based on input.
	// If we are on passable terrain, move.
	// If passable and runCommand, run.
	// If passable and jumpCommand and canJump, jump.
	// If not passable, dont.
	// If passable and jumpCommand and cantJump, dont.

	long commanderID = Commander::home->getId();
 	int cursorType = makeMoveCursor( lineOfSight );
	if ( !moverCount )
		cursorType = mState_NORMAL;

	if ( gos_GetKeyStatus( (gosEnum_KeyIndex)commands[std::to_underlying(FORCE_FIRE_KEY)].key ) == KEY_HELD // not forcing fire
				|| controlGui.getFireFromCurrentPos() )
	{
		return makeRangeCursor( lineOfSight );
	}
	

	if ( !passable && !selectionIsHelicopters() )
		cursorType = mState_DONT;
		
	if ( controlGui.isSelectingInfoObject() )
	{
		cursorType = mState_DONT;
		return cursorType;
	}
	else if ( controlGui.isAddingAirstrike() && !paintingVtol[commanderID] )
	{
		if ( bGui && (!controlGui.isOverTacMap() || !controlGui.isButtonPressed( ControlGui::SENSOR_PROBE)) )
			cursorType = mState_NORMAL;
		else
		{
			if ( controlGui.getButton( ControlGui::SENSOR_PROBE )->state == ControlButton::PRESSED )
			{
				cursorType = lineOfSight ? mState_SENSORSTRIKE : mState_UNCERTAIN_AIRSTRIKE ;
			}
			else
				cursorType = lineOfSight ? mState_AIRSTRIKE : mState_UNCERTAIN_AIRSTRIKE;
		}
		return cursorType;
	}
	else if ( controlGui.isAddingVehicle() )
	{
		if ( canAddVehicle( wPos ) && !bGui)
			cursorType = mState_VEHICLE;
		else if ( !paintingVtol[commanderID] && ! bGui)
			cursorType = mState_CANTVEHICLE;
		else if ( bGui )
			cursorType = mState_NORMAL;
	}
	else if ( controlGui.isAddingSalvage())
	{
		if ( canRecover( wPos ) && !bGui )
			cursorType = mState_SALVAGE;
		else if ( !paintingVtol[commanderID] & !bGui) 
			cursorType = mState_XSALVAGE;
		else if ( bGui )
			cursorType = mState_NORMAL;

	}
	else if ( userInput->getKeyDown( WAYPOINT_KEY ) || controlGui.getMines() )
	{
		if ( controlGui.getMines() )
		{
			if ( !passable )
				cursorType = mState_XMINES;
			else
				cursorType = mState_LAYMINES;
		}

		else
		{
			cursorType  = lineOfSight ? mState_WALKWAYPT_LOS : mState_WALKWAYPT;
			if ( controlGui.getRun() )
				cursorType  = lineOfSight ? mState_RUNWAYPT_LOS : mState_RUNWAYPT;
			else if ( controlGui.getJump() )
				cursorType  = lineOfSight ? mState_JUMPWAYPT_LOS: mState_JUMPWAYPT;
		}

		return cursorType;
		

	}

	else if ( controlGui.getJump() )
	{
		if ( canJumpToWPos() )
		{	
			if ( bCtrl )
				cursorType = lineOfSight ? mState_JUMPWAYPT_LOS : mState_JUMPWAYPT;
			else
				cursorType = makeJumpCursor( lineOfSight );

			controlGui.setRolloverHelpText( IDS_UNIT_HELP );
		}
		else
			cursorType = mState_DONT;
			
		if ( !passable && !selectionIsHelicopters() )
			cursorType = mState_DONT;


	}
	else if ( controlGui.getRun() && moverCount )
	{
		if ( bCtrl )
			cursorType = lineOfSight ? mState_RUNWAYPT_LOS : mState_RUNWAYPT;
		else
			cursorType = makeRunCursor( lineOfSight );
			
		if ( !passable && !selectionIsHelicopters() )
			cursorType = mState_DONT;
		else
			controlGui.setRolloverHelpText( IDS_UNIT_HELP );
	}
	else if ( controlGui.getMines() )
	{
		cursorType = mState_LAYMINES;
		if ( !passable )
			cursorType = mState_XMINES;
	}
	else if ( controlGui.getSalvage() )
	{
		cursorType = makeMoveCursor( lineOfSight );
		if ( !passable )
			cursorType = mState_DONT;
	}
	else if ( controlGui.getRepair() )
	{
		cursorType = makeMoveCursor( lineOfSight );
		if ( !passable )
			cursorType = mState_DONT;
	}
	else if ( controlGui.getGuard() )
	{
		cursorType = lineOfSight ? mState_GUARD_LOS : mState_GUARD;
		if ( !passable )
			cursorType = mState_DONT;
	}
	else if ( controlGui.getGuardTower() )
	{
		cursorType = lineOfSight ? mState_GUARDTOWER_LOS : mState_GUARDTOWER;
	}
	else if ( moverCount )
	{
		controlGui.setRolloverHelpText( IDS_UNIT_HELP );
	}
	
	userInput->setMouseFrame(0);		

	return cursorType;
}

float slopeTest[8] = {0.09849140f, 0.30334668f, 0.53451114f, 0.82067879f, 1.21850353f, 1.87086841f, 3.29655821f, 10.15317039f};
long MissionInterfaceManager::makeMoveCursor( bool bLineOfSite )
{

	long rotation = calcRotation();

	// leigh put these in the file funny
	// OK, rotations go clockwise starting from 180...
	gosASSERT( rotation >= 0 && rotation < 32 );
	return mState_WALK1 + 32 * bLineOfSite + rotation;
}

long MissionInterfaceManager::makeJumpCursor( bool bLineOfSite )
{
	long rotation = calcRotation();

	// leigh put these in the file funny
	// OK, rotations go clockwise starting from 180...
	gosASSERT( rotation >= 0 && rotation < 32 );
	return mState_JUMP1 + 32 * bLineOfSite + rotation;

}


long MissionInterfaceManager::makeRunCursor( bool bLineOfSite )
{
	long rotation = calcRotation();

	// leigh put these in the file funny
	// OK, rotations go clockwise starting from 180...
	gosASSERT( rotation >= 0 && rotation < 32 );
	return mState_RUN1 + 32 * bLineOfSite + rotation;

}

long MissionInterfaceManager::calcRotation()
{
	glm::vec3 pos;
	pos.x = 0.f;
	pos.y = 0.f;
	pos.z = 0.f;
	float count = 0;
	for (long i = 0; i < Team::home->getRosterSize(); i++)
	{
		Mover* pMover = Team::home->getMover( i );
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
		{
			pos.x += pMover->getPosition().x;
			pos.y += pMover->getPosition().y;
			pos.z += pMover->getPosition().z;
			count += 1.f;
		}
	}

	if ( count && (turn > 3))
	{
		pos.x /= count;
		pos.y /= count;
		pos.z /= count;
	}
	else
		return mState_NORMAL;

	//Now works by finding the vector in world space between the mouse cursor
	// and the movers.  It then normalizes that vector and multiplies by 200
	// to make large.  It then adds that vector to the camera position and we
	// do the projections based on the camera center (which is always the
	// center of the screen) and the new vector.  Complex but it works!!
	// -fs
	glm::vec3 actualPos;
	actualPos = pos - wPos;
	actualPos.z = 0.0f;
	
	float actualLength = glm::length(actualPos);
	if (actualLength > Stuff::SMALL)
		actualPos = (mc2::math::divide(actualPos, actualLength));
	actualPos *= 200.0f;
	
	glm::vec3 camPos = eye->getPosition();
	
	Stuff::Vector4D screenPosMover;
	Stuff::Vector4D screenPosGoal;
	actualPos = camPos + actualPos;
	// [PROJECTZ:ScreenXYOracle id=gui_mover_rotation_pos]
	// [PROJECTZ:ScreenXYOracle id=gui_mover_rotation_pos]
	eye->projectForScreenXY( actualPos, screenPosMover );

	// need to find second position
	// [PROJECTZ:ScreenXYOracle id=gui_cam_rotation_goal]
	// need to find second position
	// [PROJECTZ:ScreenXYOracle id=gui_cam_rotation_goal]
	eye->projectForScreenXY( camPos, screenPosGoal );
	

	//Do it the MC1 way!
	float slope = 0.0f;
	if (fabs(screenPosGoal.x - screenPosMover.x) > Stuff::SMALL)
		slope = fabs(screenPosGoal.y - screenPosMover.y) / fabs(screenPosGoal.x - screenPosMover.x);

	long counter = 0;
	long rotation = 0;
	//while (slope > slopeTest[counter] && counter < 8)
    // sebi: ORIG BUG FIX: thanks AddressSanitizer
	while (counter < 8 && slope > slopeTest[counter])
		counter++;
	if (screenPosGoal.y < screenPosMover.y)
	{
		if (screenPosGoal.x < screenPosMover.x)	// quadrant 0
			rotation = counter;
		else							// quadrant 1
			rotation = 8 + (8 - counter);
	}
	else
	{
		if (screenPosGoal.x > screenPosMover.x)	// quadrant 2
		{
			rotation = 16 + counter;
		}
		else							// quadrant 3
		{
			rotation = 24 + (8 - counter);
			if (rotation == 32)
				rotation = 0;
		}
	}

	return rotation;
}
