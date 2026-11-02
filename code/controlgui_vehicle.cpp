#include "render_view.h"
// ControlGui: the vehicle tab: update, render, click handling and the vehicle command state.
// Declared in the class headers; shared names in controlgui_internal.h.
#include "controlgui_internal.h"
#include "mc2fmt.h"
#include <utility>


void ControlGui::updateVehicleTab(int mouseX, int mouseY, bool bLOS )
{
	// slice C site 6: "is the vehicle tab active" comes from the registry
	// under the gate; the legacy PRESSED-bit poll otherwise. Hoisted — button
	// state cannot change inside this loop.
	const bool vehicleTabActive = hudTabsGateOn() && hudTabRegistry.activeTab()
		? ( hudTabRegistry.activeTab()->buttonCommand == VEHICLE_TAB )
		: ( ( getButton( VEHICLE_TAB )->state & ControlButton::PRESSED ) != 0 );

	for ( int i = LAST_VEHICLE - 1; i > -1 ; i-- )
	{
		if ( vehicleTabActive )
		{
			if ( vehicleButtons[i].state & ControlButton::PRESSED )
			{
				setRolloverHelpText( IDS_AIRSTRIKE_HELP_NO_LOS + 2 * (i) );
				if ( i == 0 && bLOS )
				{
					setRolloverHelpText( IDS_AIRSTRIKE_HELP );
				}
			}

			if ( vehicleButtons[i].location[0].x <= mouseX && mouseX <= vehicleButtons[i].location[2].x
					&& mouseY >= vehicleButtons[i].location[0].y && mouseY <= vehicleButtons[i].location[1].y  )
			{
				helpTextHeaderID = vehicleData[i].helpTextHeader;
				helpTextID = vehicleData[i].helpTextID;
				
				if ( vehicleButtons[i].ID == STOP_VEHICLE )
				{
					mouseInVehicleStopButton = true;
				}

				if ( userInput->leftMouseReleased() && !userInput->isLeftDrag()
					&& !userInput->wasLeftDrag() 
					&& !(vehicleButtons[i].state & ControlButton::PRESSED) )
				{
					{
						handleVehicleClick( vehicleButtons[i].ID );
					}
				}
		
				long lastX = mouseX - userInput->getMouseXDelta();
				long lastY = mouseY - userInput->getMouseYDelta();
				if (  vehicleButtons[i].location[0].x >= lastX || lastX >= vehicleButtons[i].location[2].x
					|| lastY <= vehicleButtons[i].location[0].y || lastY >= vehicleButtons[i].location[1].y  )
					soundSystem->playDigitalSample( LOG_HIGHLIGHTBUTTONS );
			}
		}

		if ( vehicleButtons[i].ID != STOP_VEHICLE )
		{
			if ( vehicleCosts[i] > LogisticsData::instance->getResourcePoints()
				&& (!addingVehicle && !( vehicleButtons[i].state & ControlButton::PRESSED ) ) )
			{
				vehicleButtons[i].disable( true );
			}
			else if ( ( addingArtillery || addingVehicle || addingSalvage ) && !( vehicleButtons[i].state & ControlButton::PRESSED ) )
				vehicleButtons[i].disable( true );
			else
				vehicleButtons[i].disable( false );
		}
		else
			vehicleButtons[i].disable( false );
	}

	long maxUnits = MAX_ICONS;
	if ( MPlayer )
	{
		long playerCount;
		MPlayer->getPlayers( playerCount );
		if (playerCount)
			maxUnits = (MAX_MULTIPLAYER_MECHS_IN_LOGISTICS/playerCount) + 4;
		else
			maxUnits = 0;

		if ( maxUnits > MAX_ICONS )
			maxUnits = MAX_ICONS;

	}
	
	if ( forceGroupBar.getIconCount() >= maxUnits )
	{
		getButton( REPAIR_VEHICLE )->disable( true );
		getButton( PEGASUS_SCOUT )->disable( true );
		getButton( RECOVERY_TEAM )->disable( true );
		getButton( MINELAYER )->disable( true );
		getButton( GUARD_TOWER )->disable( true );
	}

	if ( LogisticsData::instance && !LogisticsData::instance->gotPilotsLeft() )
		getButton( RECOVERY_TEAM )->disable( true );


	if ( LogisticsData::instance ) // turn off things that aren't allowed in mplayer
	{
		bool disableAll = (MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]);
		if (disableAll) {
			getButton( 	LARGE_AIRSTRIKE )->disable( true );
			getButton( 	GUARD_TOWER )->disable( true );
			getButton( 	SENSOR_PROBE )->disable( true );
			getButton( 	REPAIR_VEHICLE )->disable( true );
			getButton( 	PEGASUS_SCOUT )->disable( true );
			getButton( 	MINELAYER )->disable( true );
			getButton( 	RECOVERY_TEAM )->disable( true );  
			}
		else {
			if ( !LogisticsData::instance->canHaveAirStrike()   )
				getButton( 	LARGE_AIRSTRIKE )->disable( true );
			if ( !LogisticsData::instance->canHaveArtilleryPiece() )
				getButton( 	GUARD_TOWER )->disable( true );
			if ( !LogisticsData::instance->canHaveSensorStrike() )
				getButton( 	SENSOR_PROBE )->disable( true );
			if ( !LogisticsData::instance->canHaveRepairTruck() )
				getButton( 	REPAIR_VEHICLE )->disable( true );
			if ( !LogisticsData::instance->canHaveScoutCopter() )
				getButton( 	PEGASUS_SCOUT )->disable( true );
			if ( !LogisticsData::instance->canHaveMineLayer() )
				getButton( 	MINELAYER )->disable( true );
			if ( !LogisticsData::instance->canHaveSalavageCraft() )
				getButton( 	RECOVERY_TEAM )->disable( true );  
		}
	}
}
void ControlGui::renderVehicleTab()
{
	long cost = -1;
	char buffer[256];

	LogisticsData::instance->rpJustAdded = 0;

	for ( int i = 0; i < LAST_VEHICLE; i++ )
	{
		vehicleButtons[i].render();

		// MC2_HUD_READABILITY_V1 (default OFF): the stock disabled-label gray
		// (0xff7f7f7f) is near-invisible against the dark vehicle-tab panel at
		// 4K. Gate-on brightens ONLY the disabled color; enabled/pressed stay
		// stock. Gate-off is byte-identical (same literal as before).
		static int s_hudReadability = -1;
		if ( s_hudReadability < 0 )
		{
			s_hudReadability = mc2::gate::get<"MC2_HUD_READABILITY_V1">();
		}
		const long disabledColor = s_hudReadability ? 0xffb8c2cc : 0xff7f7f7f;
		long color = vehicleButtons[i].isEnabled() ?  0xff005392 : disabledColor;

		if ( vehicleButtons[i].state & ControlButton::PRESSED )
		{
			ControlButton::makeUVs( vehicleButtons[i].location, ControlButton::ENABLED, *vehicleButtons[i].data );	
			cost = vehicleCosts[i];
			GUI_RECT rect = { static_cast<long>(vehicleButtons[i].location[0].x), static_cast<long>(vehicleButtons[i].location[0].y),
				static_cast<long>(vehicleButtons[i].location[2].x), static_cast<long>(vehicleButtons[i].location[2].y) };
			vehicleButtons[i].render();
			drawEmptyRect( rect, 0xffffffff, 0xffffffff );
			color = 0xffffffff;
		}

		if ( vehicleCosts[i] )
		{
			// need to draw cost under the button
			long left = float2long(vehicleButtons[i].location[0].x);
			long top = float2long(vehicleButtons[i].location[2].y + TEXT_SKIP);
			long right = float2long(vehicleButtons[i].location[2].x);
			long bottom = float2long(vehicleButtons[i].location[2].y + 30 + TEXT_SKIP);

			mc2::fmt::to_buf(buffer, "{:d}", mc2::fmt::as_i32(vehicleCosts[i]));
			vehicleFont.render( buffer, left, top, right - left, bottom - top, color, 0, 2 );
		}
	}

	long left = RPLEFT;
	long top = RPTOP;

	// draw in RP
	char originalStr[256];

	cLoadString( IDS_RP, originalStr, 256 );
	sprintf( buffer, originalStr, static_cast<long>(mc2::render_view::hud().resourcePoints) );

	//We are there.  Start flashing.
	/// TUTORIAL!!!
	if (rpNumFlashes)
	{
		rpFlashTime += mc2::render_view::clock().dt;
		if ( rpFlashTime > .5f )
		{
			rpFlashTime = 0.0f;
			rpNumFlashes--;
		}
		else if ( rpFlashTime > .25f )
		{
			vehicleFont.render( buffer, left, top, 0, 0, 0xff5c96c2, 0, 0 );
		}
	}
	else
	{
		vehicleFont.render( buffer, left, top, 0, 0, 0xff5c96c2, 0, 0 );
	}



}

void ControlGui::handleVehicleClick( int ID )
{
	if ( getButton( ID )->state & ControlButton::HIDDEN )
		return;

	if ( getButton( ID )->state & ControlButton::DISABLED ) // ignore disabled button
	{	
		// need to play sound here
		soundSystem->playDigitalSample(INVALID_GUI);
		return;
	}

	if ((ID == STOP_VEHICLE) && paintingMyVtol)
	{
		//You cannot stop it once the VTOL is in flight, no matter what the button looks like.
		soundSystem->playDigitalSample(INVALID_GUI);
		return;
	}
	
	int oldID = -1;
	for ( int i = 0; i < LAST_VEHICLE; i++ )
	{
		if ( vehicleButtons[i].state & ControlButton::PRESSED )
			oldID = vehicleButtons[i].ID;
		if ( vehicleButtons[i].ID != ID )
			vehicleButtons[i].disable( 1 );
		else 
		{
			vehicleButtons[i].press( 1 );
			if ( ID != STOP_VEHICLE )
			{
				int cost = vehicleCosts[i];
				LogisticsData::instance->decrementResourcePoints( cost );
				if (MPlayer) {
					glm::vec3 pos;
					MPlayer->sendReinforcement(-cost, 0, "noname", MPlayer->commanderID, pos, 6);
				}

			}
		}


	}

	soundSystem->playDigitalSample( LOG_SELECT );


	switch ( ID )
	{
		case LARGE_AIRSTRIKE:
		case SENSOR_PROBE:
			addingArtillery = true;
			getButton( INFO_COMMAND )->press( 0 );
			break;

		case REPAIR_VEHICLE:
		case PEGASUS_SCOUT:
		case MINELAYER:
		case GUARD_TOWER:
			addingVehicle = true;
			getButton( INFO_COMMAND )->press( 0 );
			break;
		case RECOVERY_TEAM:
			addingSalvage = true;
			getButton( INFO_COMMAND )->press( 0 );
			break;
		case STOP_VEHICLE:
			addingArtillery = addingVehicle = addingSalvage = false;
			for (int i = 0; i < LAST_VEHICLE - 1; i++ )
			{
				if ( vehicleCosts[i] <= LogisticsData::instance->getResourcePoints() )
				{
					vehicleButtons[i].disable( 0 );
					vehicleButtons[i].press( 0 );
				}
			}

			
			if ( oldID != -1 )
			{
				int cost = vehicleCosts[oldID - LARGE_AIRSTRIKE];
				LogisticsData::instance->addResourcePoints( cost );

				if ( MPlayer )
				{
					MPlayer->playerInfo[MPlayer->commanderID].resourcePoints += cost;
					MPlayer->playerInfo[MPlayer->commanderID].resourcePointsGained += cost;

					if (!MPlayer->isHost())
					{
						MPlayer->sendPlayerUpdate( nullptr, 5, MPlayer->commanderID );
					}

				}
			}
			
			idToUnPress = ID;
			
			break;
		default:
		//	gosASSERT( 0 );
			break;
	}
}

void ControlGui::unPressAllVehicleButtons()
{
	for ( int i = LARGE_AIRSTRIKE; i < MAX_VEHICLE; i++ )
	{
		getButton( i )->press( 0 );
	}

	addingArtillery = 0;
	addingVehicle = 0;
	addingSalvage = 0;
}

void ControlGui::disableAllVehicleButtons()
{
	for ( int i = LARGE_AIRSTRIKE; i < MAX_VEHICLE - 1; i++ )
	{
		getButton( i )->disable( 1 );
	}

//	addingArtillery = 0;
//	addingVehicle = 0;

}

const char* ControlGui::getVehicleName( long& ID)
{
	for ( int i = GUARD_TOWER; i <= RECOVERY_TEAM; i++ )
	{
		if ( getButton( i )->state & ControlButton::PRESSED )
		{
			if ( i == GUARD_TOWER)
			{
				ID = vehicleIDs[4];
				return vehiclePilots[4];
			}
			ID = vehicleIDs[i - REPAIR_VEHICLE];
			return vehiclePilots[i - REPAIR_VEHICLE];
		}
	}

//	char s[1024];
//	sprintf(s, "getvehiclename: ID = %d", ID);
//	PAUSE((s));
	return nullptr;
}

const char* ControlGui::getVehicleNameFromID (long ID) {

	for (long i = 0; i < 5; i++)
		if (vehicleIDs[i] == ID)
			return(vehiclePilots[i]);
	return(nullptr);
}

bool ControlGui::getMines()
{
	if ( getButton( LAYMINES )->state & ControlButton::PRESSED )
		return 1;

	return 0;
}
		
bool ControlGui::getSalvage()
{
	if ( getButton( SALVAGE )->state & ControlButton::PRESSED )
		return 1;

	return 0;
}
bool ControlGui::getRepair()
{
	if ( getButton( REPAIR )->state & ControlButton::PRESSED )
		return 1;

	return 0;
}

bool ControlGui::getGuardTower()
{
	if ( getButton( GUARDTOWER )->state & ControlButton::PRESSED )
		return 1;

	return 0;
}


void ControlGui::renderHelpText()
{
	if ( helpTextID )
	{
		char buffer[1024];
		// EQUIP-ADD-UI-NAME-1 (S3): ADD-filled component help ids have no
		// string-table entry; resolve from the def. Miss -> stock path.
		const char* addHelpText = equipmentAddUiTextForStringId( helpTextID );
		if ( addHelpText )
		{
			strncpy( buffer, addHelpText, 1023 );
			buffer[1023] = 0;
		}
		else
			cLoadString( helpTextID, buffer, 1024 );

		DWORD width, height;
		width = helpFont.width( buffer );
		height = helpFont.height( buffer, width );

		gos_TextStringLength( &width, &height, buffer );
		gos_TextSetRegion( HELPAREA_LEFT, HELPAREA_BOTTOM - 3 * height, Environment.screenWidth, Environment.screenHeight );
		drawShadowText( 0xffffffff, 0xff000000, helpFont.getTempHandle(),
			HELPAREA_LEFT, HELPAREA_BOTTOM - height, true, buffer, false, 
			helpFont.getSize() );

		if ( helpTextHeaderID )
		{
			cLoadString( helpTextHeaderID, buffer, 1024 );
			long yPos = HELPAREA_BOTTOM - height;
			drawShadowText( 0xffffffff, 0xff000000, helpFont.getTempHandle(),
				HELPAREA_LEFT, yPos - height, 1, buffer, true, helpFont.getSize() );
		}
	}
}

// Stock-style hover name box next to the cursor for the object the click would hit.
void ControlGui::renderHoverTip()
{
	if ( !g_hoverTipNameID )
		return;
	// The string and its measure change only with the hovered object (or the screen size), not every frame.
	static char buffer[256];
	static DWORD width = 0, height = 0;
	static long cachedID = 0, cachedScreenW = 0;
	if ( cachedID != g_hoverTipNameID || cachedScreenW != Environment.screenWidth )
	{
		cLoadString( g_hoverTipNameID, buffer, sizeof(buffer) );
		width = helpFont.width( buffer );
		// A wrap area exactly as wide as the text makes aFont::height count a second line; give it slack.
		height = helpFont.height( buffer, width + 8 );
		cachedID = g_hoverTipNameID;
		cachedScreenW = Environment.screenWidth;
	}
	if ( !buffer[0] )
		return;
	const long pad = 3;
	long left = g_hoverTipX + 16;
	long top = g_hoverTipY + 20;
	const long w = static_cast<long>(width), h = static_cast<long>(height);
	if ( left + w + 2 * pad > Environment.screenWidth )
		left = Environment.screenWidth - w - 2 * pad;
	if ( left < 0 )
		left = 0;
	if ( top + h + 2 * pad > Environment.screenHeight )
		top = g_hoverTipY - h - 2 * pad - 4;
	GUI_RECT box = { left, top, left + w + 2 * pad, top + h + 2 * pad };
	drawRect( box, 0xc0000000 );
	drawEmptyRect( box, 0xffa0a0a0, 0xffa0a0a0 );
	gos_TextSetRegion( box.left, box.top, box.right, box.bottom );
	drawShadowText( 0xffffffff, 0xff000000, helpFont.getTempHandle(),
		left + pad, top + pad, true, buffer, false, helpFont.getSize() );
}

void ControlGui::renderInfoTab()
{
	if ( getButton( INFO_TAB )->state & ControlButton::PRESSED )
	{
		infoWnd->render();


	}
}

bool ControlGui::isSelectingInfoObject()
{
	if ( getButton( INFO_COMMAND )->state & ControlButton::PRESSED )
		return true;

	return false;
}

void ControlGui::setInfoWndMover( Mover* pMover )
{
	infoWnd->setUnit( pMover );
	if ( isSelectingInfoObject() )
	{
		handleClick( INFO_TAB ); // fake click
		// unpress the info button
		gosEnum_KeyIndex key;
		bool bC, bA, bS;
		MissionInterfaceManager::instance()->getHotKey( std::to_underlying(INFO_COMMAND_INDEX), key, bS, bC, bA );
		if ( gos_GetKeyStatus( key )!= KEY_HELD )
			getButton( INFO_COMMAND )->press( 0 );
	}
}


void ControlGui::setVehicleCommand( bool bSet )
{
	if ( (getButton( REPAIR )->isEnabled()) )
		getButton( REPAIR )->press( bSet );
	else if ( (getButton( SALVAGE )->isEnabled()) )
		getButton( SALVAGE )->press( bSet );
	else if ( (getButton( LAYMINES )->isEnabled()) )
		getButton( LAYMINES )->press( bSet );
	else 
	{
		Team* pTeam = Team::home;
		if ( pTeam )
		{
			for ( int i = 0; i < pTeam->getRosterSize(); ++i )
			{
				Mover* pMover = static_cast<Mover*>(pTeam->getMover( i ));
				if ( !commanderIsHome(pMover->getCommander()) )
					continue;
				bool bGuardTower = ( pMover->maxMoveSpeed ? 0 : 1 );
				if ( bGuardTower )
				{
					pMover->suppressionFire = bSet;

					if (!bSet && MPlayer && !MPlayer->isServer())
					{
						//We send a TAC_ORDER_WAIT which will clear the suppression fire message.
						// Otherwise, this is handled when we doGuardTower if pressed.
						TacticalOrder tacOrder;
						tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_WAIT);

						MPlayer->sendPlayerOrder(&tacOrder, false, 1, &pMover);
					}
				}
			}
		}
	}
}
bool ControlGui::getVehicleCommand( )
{
	if (	(getButton( REPAIR )->state & ControlButton::PRESSED)
		||	(getButton( SALVAGE )->state & ControlButton::PRESSED)
		||	(getButton( LAYMINES )->state & ControlButton::PRESSED)
		||  (getButton( GUARDTOWER )->state & ControlButton::PRESSED))
			return true;

	return false;
}
