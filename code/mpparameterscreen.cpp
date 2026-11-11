#include <memory>
#include "mc2str.h"

#define MPPARAMETERSCREEN_CPP
/*************************************************************************************************\
MPParameterScreen.cpp			: Implementation of the MPParameterScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include <algorithm>
#include"mpparameterscreen.h"
#include"prefs.h"
#include"inifile.h"
#include"userinput.h"
#include "../resource.h"
#include"assert.h"
#include"multplyr.h"
#include"mechbayscreen.h"
#include"logisticsdata.h"
#include"comndr.h"
#include"missionbriefingscreen.h"
#include"chatwindow.h"
#include "mc2fmt.h"

extern bool quitGame;

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#include "mpparameterscreen_ids.h"

MPParameterScreen* MPParameterScreen::s_instance = nullptr;


GUID NO_VERSION_GUID = {
	0,0,0,0,0,0,0,0,0,0,0
};


const char* pPurchaseFiles[5] = 
{
	"purchase_Steiner",
	"purchase_Davion",
	"purchase_Liao",
	"purchase_Clan",
	"purchase_All"
};

MPParameterScreen::MPParameterScreen()
{
	status = RUNNING;
	helpTextArrayID = 12;
	bMapInfoDlg = 0;
	bShowNoMapDlg = 0;
	bBootDlg = 0;
	bErrorDlg = 0;
	bHostLeftDlg = 0;
	bDisconnectDlg = 0;

	chatToSend = IDS_MP_PARAMETER_NO_MAP_CHAT;

	s_instance = this;
}

MPParameterScreen::~MPParameterScreen()
{
	//ONLY destroy the 0 member.  All of the other were set equal which means deleting their pointers will crash!!
	// Well DONE.
	//
	playerParameters[0].factionDropList.destroy();
	playerParameters[0].teamNumberDropList.destroy();

	for (int i=1;i<MAX_MC_PLAYERS;i++)
	{
		playerParameters[i].factionDropList.specialDestroy();
		playerParameters[i].teamNumberDropList.specialDestroy();
	}
}

int MPParameterScreen::indexOfButtonWithID(int id)
{
	int i;
	for (i = 0; i < buttonCount; i++)
	{
		if (buttons[i].getID() == id)
		{
			return i;
		}
	}
	return -1;
}

void MPParameterScreen::init(FitIniFile* file)
{
	LogisticsScreen::init( *file, "Static", "Text", "Rect", "Button" );

	if ( buttonCount )
	{
		for ( int i = 0; i < buttonCount; i++ )
		{
			long id = buttons[i].getID();
			if ( (id < MP_INCREMENT_DROPWEIGHT || id > MP_DECREMENT_RP)
				 && id != MP_LOCKGAME && id != MP_BOOTPLAYER ) 
				buttons[i].setMessageOnRelease();
			if (buttons[i].getID() == 0)
			{
				buttons[i].setID(FIRST_BUTTON_ID + i);
			}

			if ( id != MB_MSG_NEXT && id != MB_MSG_PREV
				&& id != MP_PLAYER_PREFS && id != MB_MSG_MAINMENU )
			{
				buttons[i].setPressFX( LOG_VIDEOBUTTONS );
				buttons[i].setHighlightFX( LOG_DIGITALHIGHLIGHT );
				buttons[i].setDisabledFX( LOG_WRONGBUTTON );
			}
		}
	}

	int curY = 0;
	playerParameters[0].init( file, "PlayerParams" );

	for ( int i = 1; i < MAX_MC_PLAYERS; i++ )
	{
		playerParameters[i] = playerParameters[0];
		curY = i * (playerParameters[i].bottom() - playerParameters[i].top() + 5);
		playerParameters[i].move( 0, curY );
		
	}

	FullPathFileName path;
	path.init( artPath, "mcl_mp_loadmap", ".fit" );

	FitIniFile mpFile;

	if ( NO_ERR != mpFile.open( path ) )
	{
		char error[256];
		mc2::fmt::to_buf( error, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)) );
		Assert( 0, 0, error );
		return;		
	}

	mpLoadMap.init( &mpFile );
	mapInfoDlg.init();

	for (int i = 0; i < editCount; i++ )
	{
		edits[i].setNumeric( true );
		edits[i].allowIME( false );
	}

	edits[0].limitEntry( 4 );
	edits[1].limitEntry( 3 );
	edits[2].limitEntry( 2 );
	edits[3].limitEntry( 6 );



}

void MPParameterScreen::begin()
{

	fadeOutTime = 0.f;
	fadeTime = 0.f;
	MPlayer->setMode(MULTIPLAYER_MODE_PARAMETERS);
	playerCount = 0;
	bLoading = 0;
	bMapInfoDlg = 0;
	bShowNoMapDlg  = 0;
	bBootDlg = 0;
	bErrorDlg = 0;
	bDisconnectDlg = 0;

	MPlayer->initParametersScreen();

	if ( MPlayer->isHost() )
		setMission( MPlayer->missionSettings.map, (status != UP && status != NEXT) ? 1 : 0 );
	else
		setMissionClientOnly( MPlayer->missionSettings.map );

	delayTime = 0.f;
	status = RUNNING;

	bHostLeftDlg = 0;
	bootPlayerID = -1;

	for (auto & playerParameter : playerParameters)
	{
		playerParameter.insigniaName.clear();
	}

	bWaitingToStart = 0;

}


void MPParameterScreen::end()
{
	if ( status == PREVIOUS )
		MPlayer->closeSession();
	else if ( status == GOTOSPLASH )
	{
		MPlayer->closeSession();
		delete MPlayer;
		MPlayer = nullptr;
	}

	bLoading = 0;
	statics[15].setTexture( static_cast<unsigned long>(0) );

	delayTime = 0.f;
	bHostLeftDlg = 0; 
}

void MPParameterScreen::render(int xOffset, int yOffset )
{
	if ( VersionStatus::VERSION_STATUS_UNKNOWN == MPlayer->getVersionStatus() )
	{
		if ( bErrorDlg )
		{
			LogisticsOneButtonDialog::instance()->render();
		}
		return;
	}
	if ( status == NEXT && MPlayer->missionSettings.quickStart )
	{
		xOffset = 0;
		yOffset = 0;
	}
	LogisticsScreen::render(xOffset, yOffset);

	if ( bMapInfoDlg )
	{
		mapInfoDlg.render();
	}
	else if ( bLoading )
	{
		mpLoadMap.render( );
	}
	else if ( bShowNoMapDlg || bBootDlg || bDisconnectDlg )
	{
		LogisticsOKDialog::instance()->render();

	}
	else if ( bErrorDlg )
	{
		LogisticsOneButtonDialog::instance()->render();
	}
	else if ( !xOffset && !yOffset )
	{
		aPlayerParams* pFocus = nullptr;

		for ( int i = 0; i < playerCount; i++ )
		{
			if ( playerParameters[i].hasFocus() )
				pFocus = & playerParameters[i];
			
			playerParameters[i].render();
				 
		}

		if ( pFocus )
			pFocus->render();

		ChatWindow::instance()->render(xOffset, yOffset);
	}

	if ( bHostLeftDlg )
	{
		LogisticsOneButtonDialog::instance()->render();
	}


}

void MPParameterScreen::render()
{
	render(0, 0);
}

int	MPParameterScreen::handleMessage( unsigned long message, unsigned long who)
{
	if ( RUNNING == status )
	{

		switch ( who )
		{
		case 57/*MB_MSG_MAINMENU*/:
			{
				getButton( 57/*MB_MSG_MAINMENU*/ )->press( 0 );
				if (MPlayer->commanderID > -1) {
					MC2Player* pInfo = MPlayer->getPlayerInfo( MPlayer->commanderID );
					if ( pInfo->ready )
						pInfo->ready = 0;
					if (!MPlayer->isHost())
					{
						MPlayer->sendPlayerUpdate( nullptr, 5, MPlayer->commanderID );
					}
				}
				status = MAINMENU;
			}
			break;
		case 51/*MB_MSG_PREV*/:
			{
				getButton( 51/*MB_MSG_PREV*/ )->press( 0 );
				LogisticsOKDialog::instance()->setText( IDS_PROMPT_TO_DISCONNECT, IDS_DIALOG_NO, IDS_DIALOG_YES );
				LogisticsOKDialog::instance()->begin();
				bDisconnectDlg = true;

			}
			break;
		case 50/*MB_MSG_NEXT*/:
			{
				if (MPlayer->isHost())
					MPlayer->setInProgress(true);
				delayTime += frameLength;
				char text[256];
				cLoadString( IDS_MPLAYER_GAME_ABOUT_TO_START, text, 255 );
				MPlayer->sendChat( nullptr, -1, text );
			}
			break;
		case 101/*load map button*/:
			{
				//load map
				getButton( 101/*load map button*/ )->press( 0 );
				bLoading = true;
				mpLoadMap.begin();
				return 1;
			}
			break;
		case 102/*add ai player*/:
			{
				//add ai
				getButton( 102/*add ai player*/ )->press( 0 );
				status = DOWN;
				return 1;
			}
			break;

		case MAP_INFO:
			bMapInfoDlg = true;
			mapInfoDlg.setMap( MPlayer->missionSettings.map );
			mapInfoDlg.begin();
			break;

		case MP_PLAYER_PREFS:
			{

				MC2Player* pInfo = MPlayer->getPlayerInfo( MPlayer->commanderID );
				if ( pInfo->ready )
					pInfo->ready = 0;
				if (!MPlayer->isHost())
				{
					MPlayer->sendPlayerUpdate( nullptr, 5, MPlayer->commanderID );
				}
	
				status = UP;
			}

			break;

		case MP_LOCKGAME:
			if ( getButton( MP_LOCKGAME )->isPressed() )
			{
				MPlayer->setLocked( true );
			}
			else
				MPlayer->setLocked( false );

			break;
		case MP_BOOTPLAYER:
			{
				for( int i = 0; i < playerCount; i++ )
				{
					if ( playerParameters[i].isSelected() )
					{
						if ( MPlayer->commanderID == playerParameters[i].getCommanderID() )
						{
							soundSystem->playDigitalSample( LOG_WRONGBUTTON );
							return 0;
						}
						char str[256];
						cLoadString( IDS_MP_PARAMTERS_BOOT_PROMPT, str, 255 );
						char finalStr[256];
						for (auto & j : MPlayer->playerList)
						{
							if ( j.commanderID 
								== playerParameters[i].getCommanderID() )
							{
								sprintf( finalStr, str, j.name );
								LogisticsOKDialog::instance()->setText( IDS_DIALOG_OK, IDS_DIALOG_NO, IDS_DIALOG_YES );
								LogisticsOKDialog::instance()->setText( finalStr );
								LogisticsOKDialog::instance()->begin();
								bBootDlg = true;
								bootPlayerID = j.commanderID;
								return 0;
							}
						}
						break;						
					}
				}

				// if we got here, there was no selected player
				LogisticsOneButtonDialog::instance()->setText( IDS_MP_PARAMETERS_CANT_BOOT, IDS_DIALOG_OK, IDS_DIALOG_OK );
				LogisticsOneButtonDialog::instance()->begin();
				bErrorDlg = true;
			}

			break;

		case MP_INCREMENT_DROPWEIGHT:
			MPlayer->missionSettings.dropWeight +=10;
			MPParameterScreen::resetCheckBoxes();
			MPlayer->missionSettings.dropWeight = std::min(MPlayer->missionSettings.dropWeight, 1200L);
			break;
		case MP_DECREMENT_DROPWEIGHT:
			MPlayer->missionSettings.dropWeight -=10;
			MPParameterScreen::resetCheckBoxes();
			MPlayer->missionSettings.dropWeight = std::max(MPlayer->missionSettings.dropWeight, 30L);

			break;		
		case MP_INCREMENT_CBILLS:
			MPlayer->missionSettings.defaultCBills +=5000;
			MPlayer->missionSettings.defaultCBills = std::max(MPlayer->missionSettings.defaultCBills, 50000L);
			MPlayer->redistributeRP();

			MPParameterScreen::resetCheckBoxes();
			
			
			break;
		case MP_DECREMENT_CBILLS:
			MPlayer->missionSettings.defaultCBills -=5000;
			MPlayer->missionSettings.defaultCBills = std::max(MPlayer->missionSettings.defaultCBills, 50000L);
			MPlayer->redistributeRP();

			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_INCREMENT_TIME:
			if ( MPlayer->missionSettings.timeLimit < 0 )
			{
				MPlayer->missionSettings.timeLimit = 0;
			}
			MPlayer->missionSettings.timeLimit += 60;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_DECREMENT_TIME:
			if ( MPlayer->missionSettings.timeLimit <= 300 )
				MPlayer->missionSettings.timeLimit = -1;
			else
				MPlayer->missionSettings.timeLimit -= 60;

			MPParameterScreen::resetCheckBoxes();
			break;

		case MP_INCREMENT_RP:
			MPlayer->missionSettings.resourcePoints = std::max(MPlayer->missionSettings.resourcePoints, 0L);

			MPlayer->missionSettings.resourcePoints += 1000;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_DECREMENT_RP:
			MPlayer->missionSettings.resourcePoints -= 1000;
			MPlayer->missionSettings.resourcePoints = std::max(MPlayer->missionSettings.resourcePoints, 0L);

			MPParameterScreen::resetCheckBoxes();
			
			break;

		case MP_AIRSTRIKES:
			MPlayer->missionSettings.airStrike ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_SENSOR_PROBE:
			MPlayer->missionSettings.sensorProbe ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_MINELAYER:
			MPlayer->missionSettings.mineLayer ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_REPAIR:
			MPlayer->missionSettings.repairVehicle ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_SCOUTS:
			MPlayer->missionSettings.scoutCopter ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_RECOVERY:
			MPlayer->missionSettings.recoveryTeam ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_GUARDTOWER:
			MPlayer->missionSettings.guardTower ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_RESOURCE_BLDG:
			MPlayer->missionSettings.resourceBuilding ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_UNLIMITED_AMMO:
			MPlayer->missionSettings.unlimitedAmmo ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_VARIANTS:
			MPlayer->missionSettings.variants ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;
		case MP_ALLTECH:
			MPlayer->missionSettings.allTech ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;

		case MP_RP_FORMMECHS:
			MPlayer->missionSettings.resourceForMechs ^= 1;
			MPParameterScreen::resetCheckBoxes();
			
			break;

		case MP_QUICKSTART:
			MPlayer->missionSettings.quickStart ^= 1;
			MPParameterScreen::resetCheckBoxes();
			break;

		}
	}

	return 0;

}

void MPParameterScreen::initializeMap ( mc2::czstring fileName )
{
	s_instance->setMission(fileName, true);
}

void MPParameterScreen::setMission( mc2::czstring fileName, bool resetData )
{
	FullPathFileName path;
	path.init( missionPath, fileName, ".fit" );
	FitIniFile missionFile;
	
	if ( NO_ERR != missionFile.open( const_cast<char*>(static_cast<const char*>(path)) ) )
	{
		char errorStr[256];
		mc2::fmt::to_buf( errorStr, "couldn't open file {}", mc2::fmt::safe(fileName) );
		Assert( 0, 0, errorStr );
		return;
	}

	if ( resetData )
	{
		long result = 0;

		char missionName[256];
		result = missionFile.seekBlock( "MissionSettings" );
		Assert( result == NO_ERR, 0, "Coudln't find the mission settings block in the mission file" );

		bool bRes;
		result = missionFile.readIdBoolean( "MissionNameUseResourceString", bRes );
		Assert( result == NO_ERR, 0, "couldn't find the MissionNameUseResourceString" );
		if ( bRes )
		{
			unsigned long lRes;
			result = missionFile.readIdULong( "MissionNameResourceStringID", lRes );
			Assert( result == NO_ERR, 0, "couldn't find the MissionNameResourceStringID" );
			cLoadString( lRes, missionName, 255 );
		}
		else
		{
			result = missionFile.readIdString( "MissionName", missionName, 255 );
			Assert( result == NO_ERR, 0, "couldn't find the missionName" );
		}
		
		gosASSERT( strlen( missionName ) < MAXLEN_MAP_NAME );

		strcpy( MPlayer->missionSettings.map, fileName );
		strcpy( MPlayer->missionSettings.name, missionName );

			
		result = missionFile.readIdLong( "ResourcePoints", MPlayer->missionSettings.resourcePoints );
		result = missionFile.readIdLong("AdditionalCBills", MPlayer->missionSettings.defaultCBills );
		if (MPlayer->isHost())
			MPlayer->redistributeRP();
		float fTmp;
		result = missionFile.readIdFloat( "DropWeightLimit", fTmp );
		MPlayer->missionSettings.dropWeight = fTmp;
		result = missionFile.readIdFloat( "TimeLimit", MPlayer->missionSettings.timeLimit );
		if ( result != NO_ERR )
			MPlayer->missionSettings.timeLimit = -1.f;
		result = missionFile.readIdBoolean( "UnlimitedAmmoEnabledDefault", MPlayer->missionSettings.unlimitedAmmo );
		result = missionFile.readIdBoolean( "NoVariantsEnabledDefault", MPlayer->missionSettings.variants );
		MPlayer->missionSettings.variants ^= 1;
		result = missionFile.readIdBoolean( "AllTechEnabledDefault", MPlayer->missionSettings.allTech );
		result = missionFile.readIdBoolean( "AirStrikesEnabledDefault", MPlayer->missionSettings.airStrike );
		result = missionFile.readIdBoolean( "ArtilleryPieceEnabledDefault", MPlayer->missionSettings.guardTower );
		result = missionFile.readIdBoolean( "RepairVehicleEnabledDefault", MPlayer->missionSettings.repairVehicle );
		result = missionFile.readIdBoolean( "SalvageCraftEnabledDefault", MPlayer->missionSettings.recoveryTeam );
		result = missionFile.readIdBoolean( "SensorProbesEnabledDefault", MPlayer->missionSettings.sensorProbe );
		result = missionFile.readIdBoolean( "ScoutCoptersEnabledDefault", MPlayer->missionSettings.scoutCopter );
		result = missionFile.readIdBoolean( "MineLayersEnabledDefault", MPlayer->missionSettings.mineLayer );
		result = missionFile.readIdBoolean( "ResourceBuildingsEnabledDefault", MPlayer->missionSettings.resourceBuilding );
		result = missionFile.readIdBoolean( "ScoutCoptersEnabledDefault", MPlayer->missionSettings.scoutCopter );
		result = missionFile.readIdBoolean( "RPsForMechsEnabledDefault", MPlayer->missionSettings.resourceForMechs );
		result = missionFile.readIdString( "DownloadURL", MPlayer->missionSettings.url, 255 );
		unsigned long lTmp;
		if ( NO_ERR == missionFile.readIdULong( "MaximumNumberOfTeams", lTmp ) )
			MPlayer->missionSettings.maxTeams = lTmp;
		else
			MPlayer->missionSettings.maxTeams = 8;

		if ( NO_ERR ==  missionFile.readIdULong( "MaximumNumberOfPlayers", lTmp ) )
			MPlayer->missionSettings.maxPlayers = lTmp;
		else
			MPlayer->missionSettings.maxPlayers = 8;
		
		unsigned long tmp;
		result = missionFile.readIdULong( "MissionType", tmp );
			MPlayer->missionSettings.missionType = tmp;

		// divvy up the cBills!
		

 	mapName = MPlayer->missionSettings.map;
	}

	long textureHandle = MissionBriefingScreen::getMissionTGA( fileName );
	statics[15].setTexture( textureHandle );
	statics[15].setUVs(0, statics[15].getFileHeight() - 1.f, statics[15].getFileHeight() - 1.f, 0 );
	statics[15].setColor( 0xffffffff );

	getButton( MAP_INFO )->disable( false );

	GUID tmpGuid = getGUIDFromFile( fileName );
	MPlayer->missionSettings.mapGuid = tmpGuid;




}

int __cdecl sortPlayers( const void* p1, const void* p2 )
{
	MC2Player* player1 = *static_cast<MC2Player**>(const_cast<void*>(p1));
	MC2Player* player2 = *static_cast<MC2Player**>(const_cast<void*>(p2));

	if ( player1->team < player2->team )
		return -1;
	else if ( player1->team > player2->team )
		return 1;
	else if ( player1->teamSeniority > player2->teamSeniority )
		return -1;

	return 1;
	
}

GUID			MPParameterScreen::getGUIDFromFile( mc2::czstring pNewMapName)
{
	GUID retVal;
	memset( &retVal, 0xff, sizeof( GUID ) );
	FullPathFileName path;
	path.init( missionPath, pNewMapName, ".pak" );
	PacketFile pakFile;
	if ( NO_ERR != pakFile.open( const_cast<char*>(static_cast<const char*>(path)) ) )
	{
		return retVal;
	}

	int packetCount = pakFile.getNumPackets();
	pakFile.seekPacket( packetCount - 1 );
	if ( sizeof( GUID ) == pakFile.getPacketSize( ) ) 
	{
		pakFile.readPacket( packetCount - 1, reinterpret_cast<unsigned char*>(&retVal) );

		return retVal;
	}
	else
	{
		memset( &retVal, 0, sizeof( GUID ) );
		return retVal;
	}

}

void MPParameterScreen::setMissionClientOnly( mc2::czstring pNewMapName )
{
	FullPathFileName path;
	path.init( missionPath, pNewMapName, ".fit" );
	FitIniFile missionFile;
	
	if ( NO_ERR != missionFile.open( const_cast<char*>(static_cast<const char*>(path)) ) )
	{
		char tmp[256];
		char final[1024];
		cLoadString( IDS_MP_PARAM_NO_MAP, tmp, 255 );
		sprintf( final, tmp, MPlayer->missionSettings.map, MPlayer->missionSettings.url );
		LogisticsOKDialog::instance()->setFont(IDS_MP_PARAM_NO_MAP_FONT);
		LogisticsOKDialog::instance()->setText( final );
		LogisticsOKDialog::instance()->begin();
		bShowNoMapDlg = true;
		getButton( MAP_INFO )->disable( true );

		chatToSend = IDS_MP_PARAMETER_NO_MAP_CHAT;
		statics[15].setColor( 0 );
		return;
	}

	checkVersionClientOnly( pNewMapName );


	getButton( MAP_INFO )->disable( false );

	long textureHandle = MissionBriefingScreen::getMissionTGA( pNewMapName );
	if ( textureHandle )
	{
		statics[15].setTexture( textureHandle );
		statics[15].setUVs( 0, statics[15].getFileHeight() - 1.f, statics[15].getFileHeight() - 1.f, 0 );
		statics[15].setColor( 0xffffffff );
	}


 	mapName = MPlayer->missionSettings.map;
}

void MPParameterScreen::checkVersionClientOnly( mc2::czstring pNewMapName )
{
	GUID version = getGUIDFromFile( pNewMapName );
	if ( MPlayer->missionSettings.mapGuid != NO_VERSION_GUID &&
		version != MPlayer->missionSettings.mapGuid && version != NO_VERSION_GUID) // if 0 it simply wasn't in the file at all, should take this out as soon as maps are resaved
	{
		char tmp[256];
		char final[1024];
		cLoadString( IDS_MP_PARAM_MAP_WRONG_VERSION, tmp, 255 );
		sprintf( final, tmp, MPlayer->missionSettings.url );
		LogisticsOKDialog::instance()->setFont(IDS_MP_PARAM_NO_MAP_FONT);
		LogisticsOKDialog::instance()->setText( final );
		LogisticsOKDialog::instance()->begin();
		bShowNoMapDlg = true;
		getButton( MAP_INFO )->disable( true );

		chatToSend = IDS_MAP_WRONG_VERSION_CHAT;
		statics[15].setColor( 0 );	

		// need to disable the ready button
		for( int i = 0; i < MAX_MC_PLAYERS; i++ )
		{
			if ( playerParameters[i].getCommanderID() == MPlayer->commanderID )
			{
				playerParameters[i].disableReadyButton();
				break;
			}
		}

	}
}



void MPParameterScreen::resetCheckBoxes()
{

	if ( MPlayer->isHost() )
	{
		long playerCount = 0;
		const MC2Player* players = MPlayer->getPlayers(playerCount);

		for ( int i = 0; i < playerCount; i++ )
		{
			MC2Player* pWrite = MPlayer->getPlayerInfo( players[i].commanderID );
			if ( pWrite->commanderID != MPlayer->commanderID )
				pWrite->ready = 0;
				
		}
	}

}

void MPParameterScreen::setHostLeftDlg( mc2::czstring playerName )
{
	LogisticsOneButtonDialog::instance()->setPlayerLeftText( playerName );
	LogisticsOneButtonDialog::instance()->begin();
	bHostLeftDlg = true;

}




