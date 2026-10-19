/*************************************************************************************************\
controlGui.cpp			: Implementation of the controlGui component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "controlgui_internal.h"
#include "../mclib/texture_assets.h"
#include "../GameOS/gameos/utils/mc2log.h"
#include "mc2fmt.h"


// ---- MISSION-INTERFACE-PERF-1 (MC2_IFACE_COST_SPLIT / MC2_MIF_SPLIT) ----
// ControlGui::update sub-phase per-frame ns accumulators. DEFINED here;
// folded + emitted once per frame by mfFrameEnd() in missiongui.cpp (extern
// array pattern per TOBJSPLIT precedent, terrobj.cpp). CgPhase order MUST
// match the MF_CG_* tail of MifPhase in missiongui.cpp. chrono is fine:
// once-per-frame per-phase brackets, not per-element probes. Zero chrono
// calls when both envs unset. Default OFF.
unsigned long long g_ifaceCgFrameNs[8] = {0};

ControlGui* ControlGui::instance = nullptr;

std::unique_ptr<ButtonData[]>	ControlGui::buttonData = nullptr;
std::unique_ptr<ButtonData[]> ControlGui::vehicleData = nullptr;

long ControlGui::hiResOffsetX = 0;
long ControlGui::hiResOffsetY = 0;

long ControlGui::OBJECTIVESTOP= 0;
long ControlGui::OBJECTIVESLEFT= 0;
long ControlGui::OBJECTIVESSKIP= 0;
long ControlGui::OBJECTIVESTOTALRIGHT= 0;
long ControlGui::OBJEECTIVESHEADERSKIP= 0;
long ControlGui::OBJECTIVESHEADERTOP= 0;
long ControlGui::OBJECTIVEBOXX= 0;
long ControlGui::OBJECTIVEBOXSKIP= 0;
long ControlGui::OBJECTIVECHECKSKIP= 0;
long ControlGui::OBJECTIVEHEADERLEFT = 0;
long ControlGui::HELPAREA_LEFT = 0;
long ControlGui::HELPAREA_BOTTOM = 0;
long ControlGui::RPLEFT = 0;
long ControlGui::RPTOP = 0;

int __cdecl sortStats( const void* pPlayer1, const void* pPlayer2 );

long ControlGui::vehicleCosts[LAST_VEHICLE] = 
{
	7000, 
	8000,
	2000,
	6000,
	2000,
	4000,
	10000,
	0
};

const char* ControlGui::vehicleNames[5] = {
	"repairTruck",
	"scout",
	"minelayer",
	"pv20600",
	"guardTower"// needs to be recovery

};

long ControlGui::vehicleIDs[5] = 
{
	182,
	393,
	120,
	147,
	415
};

const char* ControlGui::vehiclePilots[5] =
{
	"PMWRepair",
	"PMWScout",
	"PMWMinelayer",
	"PMWRecover",
	"PMWGuardTower"
};


unsigned long ControlGui::WALK =	0;
unsigned long ControlGui::RUN =		0x01;
unsigned long ControlGui::GUARD =	0x02;
unsigned long ControlGui::JUMP =	0x04;

MoveInfo ControlGui::objectiveMoveInfo[OBJECTVE_MOVE_COUNT] = 
{
	0.0f,-640,
	0.0f, 0
};

 MoveInfo ControlGui::missionResultsMoveInfo[RESULTS_MOVE_COUNT] = 
 {

	0.0f, -65,
	0.16f, 0,
	0.24f, -10
};


ControlGui::ControlGui()
{
	gosASSERT( !instance );
	instance = this;
	idToUnPress = -1;
	renderObjectives = 0;
	addingVehicle = 0;
	addingArtillery = 0;
	addingSalvage = 0;
	wasLayingMines = 0;

	bMovie = nullptr;
	moviePlaying = 0;

	helpTextHeaderID = 0;
	helpTextID = 0;

	rectInfos = nullptr;
	rectCount = 0;	
	staticInfos = nullptr;
	staticCount = 0;
	buttons = nullptr;
	buttonData = nullptr;
	vehicleButtons = nullptr;
	vehicleData = nullptr;

	videoInfos = nullptr;
	videoInfoCount = 0;

	timerInfos = nullptr;
	timerInfoCount = 0;

	curOrder = 0;
	fireFromCurrentPos = 0;

	pauseWnd = std::make_unique_for_overwrite<PauseWindow>();

	renderStatusInfo = 0;
	missionStatusInfos = nullptr;

	objectiveTime = 0.0f;

	infoWnd = nullptr;
	bChatting = 0;

	if ( !infoWnd )
	{
		infoWnd = std::make_unique_for_overwrite<InfoWindow>();
	}

	objectiveInfos = nullptr;

	swapResolutions( Environment.screenWidth, Environment.screenHeight );
	resultsTime = 0;

	twoMinWarningPlayed = false;
	thirtySecondWarningPlayed = false;

	tabFlashTime = 0;

	memset( chatInfos, 0, MAX_CHAT_COUNT * sizeof ( ChatInfo ) );

	chatEdit.text.clear();
	chatEdit.text.reserve( 512 );
	playerNameEdit.setBufferSize( 16 ); // pre - alloc 

	buttonToPress 	= -1;
	rpNumFlashes 	= 0;
	rpFlashTime		= 0.0f; 
	chatIsTeamOnly = 0;
	bServerWarningShown = 0;

}

ControlGui::~ControlGui()
{
	if ( staticInfos )
		staticInfos.reset();
	staticInfos = nullptr;

	if ( videoInfos )
		videoInfos.reset();

	if ( objectiveInfos )
		objectiveInfos.reset();

	if ( timerInfos )
		timerInfos.reset();

	timerInfos = nullptr;

	if ( rectInfos )
		rectInfos.reset();
	rectInfos = nullptr;

	if ( buttonData )
		buttonData.reset();
	buttonData = nullptr;

	if ( buttons )
		buttons.reset();
	buttons = nullptr;

	if ( infoWnd )
		infoWnd.reset();
	infoWnd = nullptr;

	if ( pauseWnd )
		pauseWnd.reset();
	pauseWnd = nullptr;

	if ( vehicleButtons )
		vehicleButtons.reset();
	vehicleButtons = nullptr;

	if ( vehicleData )
		vehicleData.reset();
	vehicleData = nullptr;

	if ( missionStatusInfos )
		missionStatusInfos.reset();

	missionStatusInfos = nullptr;
	instance = nullptr;

	if (bMovie)
	{
		bMovie.reset();
		bMovie = nullptr;
	}
}

bool ControlGui::pushButton (long buttonId)
{
	buttonToPress = buttonId;

	//I KNOW its hard-coded, OK?
	if ((buttonId >= 12) && (buttonId <= 14))
	{
		//Unpress TacMap if we didn't want to press that.
		if ((buttonId != 12) && getButton(12)->state & ControlButton::PRESSED)
			getButton(12)->press(false);
	
		//Unpress Info Button if we didn't want to press that.
		if ((buttonId != 13) && getButton(13)->state & ControlButton::PRESSED)
			getButton(13)->press(false);
	
		//Unpress VehicleTab if we didn't want to press that.
		if ((buttonId != 14) && getButton(14)->state & ControlButton::PRESSED)
			getButton(14)->press(false);
	}

	return true;
}

bool ControlGui::flashRPTotal (long numFlashes)
{
	rpNumFlashes = numFlashes;
	rpFlashTime = 0.0f;

	return true;
}

bool ControlGui::animateTacMap (int buttonId,float timeToScroll,long numFlashes)
{
	return tacMap.animate(abs(buttonId), numFlashes);
}

bool ControlGui::isOverTacMap()
{
	bool bMouseInsideTacArea = 0;

	// HUD-inverse space (see ControlGui::update above): rect authored at 100%,
	// drawn shrunk by s_hud_scale.
	if ( rectInfos[0].rect.left <= userInput->getMouseHudX()
		&& rectInfos[0].rect.right >= userInput->getMouseHudX()
		&& rectInfos[0].rect.top <= userInput->getMouseHudY()
		&& rectInfos[0].rect.bottom >= userInput->getMouseHudY() )
	{
		bMouseInsideTacArea = true;
	}

	// slice C site 4: registry owns the "does the active tab count as the tac
	// map" verdict under the gate.
	if ( hudTabsGateOn() && hudTabRegistry.activeTab() )
		return hudTabRegistry.activeTab()->countsAsTacMap;

	if ( getButton( TACMAP_TAB )->state & ControlButton::PRESSED )
		return true;

	return false;
}

bool ControlGui::inRegion( int mouseX, int mouseY, bool bPaused )
{

	for ( int i = 0; i < LAST_COMMAND; i++ )
	{
		if ( (buttons[i].location[0].x)  <= mouseX && 
			 buttons[i].location[3].x >= mouseX && 
			 buttons[i].location[0].y <= mouseY &&
			 buttons[i].location[1].y >= mouseY )
			 return true;
	}
	
	for (int i = 0; i < staticCount; i++ )
	{
		if ( staticInfos[i].isInside( mouseX, mouseY ) )
			return true;
	}

	for (int i = 0; i < rectCount; i++ )
	{
		if ( (rectInfos[i].rect.left)  <= mouseX && 
			 rectInfos[i].rect.right >= mouseX && 
			 rectInfos[i].rect.top <= mouseY &&
			 rectInfos[i].rect.bottom >= mouseY )
			 return true;
	}

	if ( bPaused && pauseWnd->inRect( mouseX, mouseY ) )
		return true;

	return false;
}

void ControlGui::addMover (MoverPtr mover)
{
	if (mover->getCommanderId() == Commander::home->getId()) 
	{
		if ( turn > 3 )
			mover->removeFromUnitGroup( 1 );

		if ( mover->getObjectType()->getObjectTypeClass() == BATTLEMECH_TYPE )
			forceGroupBar.addMech( mover );
		else //if ( mover->getSpeed() > 0 ) // don't add guard towers
			forceGroupBar.addVehicle( mover );
	}
}

void ControlGui::removeMover (MoverPtr mover) {

	if (mover->getCommanderId() == Commander::home->getId())
		forceGroupBar.removeMover(mover);
}

void ControlGui::initMechs()
{
	forceGroupBar.removeAll();

	Team* pTeam = Team::home;

	if ( !pTeam )
		return;

	for ( int i = 0; i < pTeam->getRosterSize(); ++i )
	{
		Mover* pMover = pTeam->getMover( i );
		if (pMover && pMover->getExists() && pMover->isOnGUI())
		{
			if (pMover->getCommander()->getId() == Commander::home->getId())
			{
				pMover->setSelected( 1 );
		
				addMover(pMover);
			}
		}
	}

	if ( MPlayer )
	{
		mpStats[0].init();

		for ( int i = 0; i < 8; i++ )
		{
			mpStats[i+1] = mpStats[i];
		}
	}
}

bool ControlGui::getRun( )
{
	return curOrder == RUN;
}
void ControlGui::toggleJump( )
{
	curOrder ^= JUMP;
	curOrder &= JUMP;
	getButton( JUMP_COMMAND )->press( curOrder & JUMP ? true : false);
	getButton( RUN_COMMAND )->press( curOrder & RUN ? true : false);
	getButton( GUARD_COMMAND )->press( curOrder & GUARD ? true : false);
}
bool ControlGui::getJump( )
{
	return curOrder == JUMP;
}
void ControlGui::toggleDefaultSpeed( )
{
	curOrder ^= RUN;
	curOrder &= ( RUN | GUARD );
	getButton( RUN_COMMAND )->press( curOrder & RUN ? true : false );
	getButton( GUARD_COMMAND )->press( curOrder & GUARD ? true : false );
	getButton( JUMP_COMMAND )->press( curOrder & JUMP ? true : false);

}

void ControlGui::setDefaultSpeed()
{
	curOrder = 0;
	getButton( JUMP_COMMAND )->press( 0 );
	getButton( GUARD_COMMAND )->press( 0 );
	getButton( RUN_COMMAND )->press( 0 );
	getButton( REPAIR )->press( 0 );
	getButton( SALVAGE )->press( 0 );
	getButton( INFO_COMMAND )->press( 0 );
}
bool ControlGui::isDefaultSpeed()
{
	return getWalk();
}
bool ControlGui::getWalk( )
{
	return curOrder == WALK;
}
void ControlGui::toggleGuard( )
{
	curOrder ^= GUARD;
	curOrder &= ( GUARD | RUN ); // can run and guard
	getButton( GUARD_COMMAND )->press( curOrder & GUARD ? true : false );
	getButton( JUMP_COMMAND )->press( curOrder & JUMP ? true : false);
	getButton( RUN_COMMAND )->press( curOrder & RUN ? true : false);
}
bool ControlGui::getGuard( )
{
	return curOrder == GUARD;
}

void ControlGui::setRange( int Range )
{
	// apply to all selected mechs
	Team* pTeam = Team::home;
	for ( int i = 0; i < pTeam->getRosterSize(); ++i )
	{
		Mover* pMover = pTeam->getMover( i );
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
		{
			pMover->attackRange = Range;
			// only want to apply to future order
//			pMover->getPilot()->changeAttackRange( Range );
		}
	}
}

int		ControlGui::getCurrentRange()
{
	int range = -1;
	
	Team* pTeam = Team::home;
	for ( int i = 0; i < pTeam->getRosterSize(); ++i )
	{
		Mover* pMover = pTeam->getMover( i );
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
		{
			if ( range != -1 && range != pMover->attackRange )
			{
				return FIRERANGE_OPTIMAL;
			}
			else
				range = pMover->attackRange;
		}
	}

	return range;
	
}


void ControlButton::render()
{
	if ( state != HIDDEN )
	{
		unsigned long gosID = mc2::textures::sourceId( data->textureHandle );
		gos_SetRenderState( gos_State_Texture, gosID ); 
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_AlphaTest, true);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );

		gos_DrawQuads( location, 4 );

		if ( data->textID )
		{
			char buffer[256];
			cLoadString( data->textID, buffer, 256 );
			DWORD width;
			int   visTop, visBot;
			gos_TextSetAttributes( data->textFont.getTempHandle(), data->textColors[state], data->textFont.getSize(), true, true, false, false, 2 );
			gos_TextVisualBounds( &width, &visTop, &visBot, buffer );

			data->textFont.render( buffer, location[0].x,
				(location[0].y + location[2].y)/2 - (visTop + visBot + 1)/2,
				location[2].x - location[0].x,
				location[2].y - location[0].y,
				data->textColors[state], 0, 2 );
		}
	}
}

void ControlButton::press(bool bPress)
{
	if ( !isEnabled() )
		return;

	state = bPress ? PRESSED : ENABLED;
	ControlButton::makeUVs( location, state, *data );	
	
}

void ControlButton::makeAmbiguous( bool bAmbiguous )
{
	state = bAmbiguous ? AMBIGUOUS : ENABLED;
	ControlButton::makeUVs( location, state, *data );	

}

void ControlButton::disable( bool bDisable )
{
	state = (bDisable ? DISABLED : (state == PRESSED ?  PRESSED : ENABLED));
	ControlButton::makeUVs( location, state, *data );
}

void ControlButton::hide( bool bHide )
{
	if ( !bHide && state != HIDDEN )
		return;
	state = bHide ? HIDDEN : ENABLED;
	if ( state !=  HIDDEN )
		ControlButton::makeUVs( location, state, *data );
}

bool ControlButton::isEnabled()
{
	return state == ENABLED || state == PRESSED || state == AMBIGUOUS;
}

void ControlButton::move( float deltaX, float deltaY )
{
	for ( int i = 0; i < 4;i++ )
	{
		location[i].x += deltaX;
		location[i].y += deltaY;
	}
}



ControlButton*		ControlGui::getButton( int ID )
{
	for( int i = 0; i < LAST_COMMAND; i++ )
	{
		if ( buttons[i].ID == ID )
			return &buttons[i];
	}

	for (int i = 0; i < LAST_VEHICLE; i++ )
	{
		if ( vehicleButtons[i].ID == ID )
			return &vehicleButtons[i];
	}


	return nullptr;
}

void ControlButton::makeUVs( gos_VERTEX* vertices, int State, ButtonData& data )
{
		float left = data.stateCoords[State][0];
		float top = data.stateCoords[State][1];

		if ( left == -1 || top == -1 )
		{
			MC2_LOG(Core, Debug, "makeUVs given an Invalid state");
		}
		
		float width = data.textureWidth;
		float height = data.textureHeight;

		float right = left + width;
		float bottom = top + height;

		if ( data.fileWidth && data.fileHeight ) // will crash if 0
		{

			if ( data.textureRotated )
			{
				vertices[0].u = right/(float)data.fileWidth + (.1f / (float)data.fileWidth);;
				vertices[1].u = left/(float)data.fileWidth + (.1f / (float)data.fileWidth);;
				vertices[2].u = left/(float)data.fileWidth + (.1f / (float)data.fileWidth);
				vertices[3].u = right/(float)data.fileWidth + (.1f / (float)data.fileWidth);

				vertices[0].v = top/(float)data.fileHeight + (.1f / (float)data.fileWidth);;
				vertices[1].v = top/(float)data.fileHeight + (.1f / (float)data.fileWidth);;
				vertices[2].v = bottom/(float)data.fileHeight + (.1f / (float)data.fileHeight);;
				vertices[3].v = bottom/(float)data.fileHeight + (.1f / (float)data.fileHeight);;
			}
			else

			{
				{
					vertices[0].u = vertices[1].u =  left/(float)data.fileWidth + (.1f / (float)data.fileWidth);;
					vertices[2].u = vertices[3].u = right/(float)data.fileWidth + (.1f / (float)data.fileWidth);

					vertices[0].v = vertices[3].v = top/(float)data.fileHeight + (.1f / (float)data.fileWidth);;
					vertices[1].v = vertices[2].v = bottom/(float)data.fileHeight + (.1f / (float)data.fileHeight);
				}
			}
		}
}

void ControlButton::toggle()
{
	if ( state & DISABLED )
		return;
	else if ( state & HIDDEN )
		return;
	if ( state & PRESSED )
		press( 0 );
	else
		press( 1 );
}

void ControlButton::setColor( unsigned long newColor )
{
	for ( int i = 0; i < 4; i++ )
		location[i].argb = newColor;
}
