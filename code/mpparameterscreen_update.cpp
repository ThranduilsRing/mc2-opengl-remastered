/* MPParameterScreen::update(), split from mpparameterscreen.cpp (no behaviour change). */
#include <memory>
#include "mc2str.h"
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

void MPParameterScreen::update()
{

	if ( MPlayer->commanderID < 0 )  // don't do anything until we've been initalized
		return;

	if ( VersionStatus::VERSION_STATUS_UNKNOWN == MPlayer->getVersionStatus() )
	{
		return;
	}
	else if ( VersionStatus::VERSION_STATUS_BAD == MPlayer->getVersionStatus() )
	{
		if ( bErrorDlg )
		{
			LogisticsOneButtonDialog::instance()->update();
			if ( LogisticsOneButtonDialog::instance()->isDone() )
			{
				LogisticsOneButtonDialog::instance()->end();
				bHostLeftDlg = 0;
				bErrorDlg = 0;

				// might need to kill the game here
				quitGame = true;
			}

		}
		else
		{
			LogisticsOneButtonDialog::instance()->setText( IDS_MP_CONNECTION_ERROR_WRONG_VERSION,
				IDS_DIALOG_OK, IDS_DIALOG_OK );
			LogisticsOneButtonDialog::instance()->begin();
			bErrorDlg = true;
		}
  
		return;

	}	
	
	char text[256];
	
	// game not front and center at the top of the screen
	textObjects[0].setText( MPlayer->sessionName );
    // sebi: !MPlayer->sessionName - always false
	//if ( !MPlayer->sessionName || !strlen( MPlayer->sessionName ) )
	if ( !strlen( MPlayer->sessionName ) )
	{
		if ( MPlayer->isHost() )
			textObjects[0].setText( IDS_STRING26150 );
		else
			textObjects[0].setText( IDS_WAITHOST2 );
	}

	// hack for some reason we don't get here from the zone
	userInput->mouseOn();
	userInput->setMouseCursor( mState_NORMAL );

	//Yet Another hack.  FadeOutscreen will overwriting us if we were booted and rejoined.
	// If our status is RUNNING, reset the FadeOutTime.
	if (status != GOTOSPLASH)
		fadeOutTime = 0.0f;

	if ( MPlayer->missionSettings.quickStart )
	{
		getButton( MB_MSG_NEXT )->setText( IDS_STRING27171 );
	}
	else
		getButton( MB_MSG_NEXT )->setText( IDS_NEXT );

	char str[256];
	cLoadString( IDS_MP_PARAM_HOST_IP, str, 255 );
	char IPAddressStr[256];
	sprintf( IPAddressStr, str, MPlayer->sessionIPAddress );
	textObjects[11].setText( IPAddressStr );

	if ( !bWaitingToStart && MPlayer->missionSettings.inProgress )
	{
		bWaitingToStart = true;
		soundSystem->playBettySample( BETTY_DEPLOY_MSG );
	}
	else if ( !MPlayer->missionSettings.inProgress )
	{
		bWaitingToStart = false;
	}

	if ( delayTime )
		delayTime += frameLength;

	if ( delayTime > 5.f )
	{
		if ( getButton( MB_MSG_NEXT )->isEnabled() )
		{

			getButton( 50/*MB_MSG_NEXT*/ )->press( 0 );
			int faction = MPlayer->getPlayerInfo( MPlayer->commanderID )->faction;
			faction = std::max(faction, 0);
			if ( MPlayer->missionSettings.allTech )
				LogisticsData::instance->setPurchaseFile( pPurchaseFiles[4] );
			else
				LogisticsData::instance->setPurchaseFile( pPurchaseFiles[faction] );
			LogisticsData::instance->setCurrentMission( MPlayer->missionSettings.map );
			MPlayer->sendMissionSetup(nullptr, 6, nullptr);

			status = NEXT;
			
		}
		else {
			if (MPlayer->isHost())
				MPlayer->setInProgress(false);
			delayTime = 0.f;
			bWaitingToStart = 0;
		}
	}

	if ( MPlayer )
	{
		FullPathFileName path;
		path.init( missionPath, MPlayer->missionSettings.map, ".pak" );
		if ( !fileExists( path ) ) // disable the ready button if the map isn't around...
		{
			getButton( MAP_INFO )->disable( true );
		}
		else
		{
			getButton( MAP_INFO )->disable( false );
			if ( !statics[15].getColor() )
			{
				long textureHandle = MissionBriefingScreen::getMissionTGA( MPlayer->missionSettings.map );
				if ( textureHandle )
				{
					statics[15].setTexture( textureHandle );
					statics[15].setUVs( 0, statics[15].getFileHeight() - 1.f, statics[15].getFileHeight() - 1.f, 0 );
					statics[15].setColor( 0xffffffff );
				}
			}
		}
	}


	// load up my own icon and send out to everyone else....
/*	MC2Player* pPlayer = MPlayer->getPlayerInfo( MPlayer->commanderID );

	if ( pPlayer && !MPlayer->insigniaList[MPlayer->commanderID])
	{

		FullPathFileName path;
		path.init( "data\\multiplayer\\insignia\\", pPlayer->insigniaFile, ".tga" );

		File file;
		if ( NO_ERR == file.open( path ) )
		{
			long size = file.getLength();

			unsigned char* pData = new unsigned char[size];

			file.read( pData, size );
			MPlayer->sendPlayerInsignia( (char*)pPlayer->insigniaFile, pData, size );
			MPlayer->insigniaList[MPlayer->commanderID] = 1;
		}
	}*/


	if ( MPlayer->launchedFromLobby )
		getButton( MB_MSG_PREV )->disable( true );
	else
		getButton( MB_MSG_PREV )->disable( false );

	if ( bMapInfoDlg )
	{
		mapInfoDlg.update();
		if ( mapInfoDlg.isDone() )
		{
			bMapInfoDlg = 0;
		}
	}
	else if ( bHostLeftDlg || bErrorDlg )
	{
		LogisticsOneButtonDialog::instance()->update();
		if ( LogisticsOneButtonDialog::instance()->isDone() )
		{
			LogisticsOneButtonDialog::instance()->end();
			bHostLeftDlg = 0;
			bErrorDlg = 0;
		}
	}
	else if ( bShowNoMapDlg || bBootDlg || bDisconnectDlg)
	{
		LogisticsOKDialog::instance()->update();
		if ( LogisticsOKDialog::instance()->isDone() )
		{
			if ( LogisticsOKDialog::instance()->getStatus() == YES )
			{
				if ( bShowNoMapDlg )
					MPlayer->launchBrowser( MPlayer->missionSettings.url );	
				else if ( bBootDlg )
				{
					MC2Player* player= MPlayer->getPlayerInfo( bootPlayerID );
					MPlayer->bootPlayer( player->player );
				}
				else if ( bDisconnectDlg )
				{
					MPlayer->closeSession();
					status = PREVIOUS;
				}
			}

			LogisticsOKDialog::instance()->end();
			if ( bShowNoMapDlg )
			{
				mapName = MPlayer->missionSettings.map;
				

				char chatStr[256];
				char final[1024];

				cLoadString( chatToSend, chatStr, 255 );
				sprintf( final, chatStr, prefs.playerName[0] );
				MPlayer->sendChat( nullptr, -1, final );
			}
				bShowNoMapDlg = false;
				bBootDlg = false;
				bDisconnectDlg = false;
		}
		return;
	}


	else if ( bLoading )
	{
		mpLoadMap.update();
		if ( mpLoadMap.isDone() )
		{
			if ( mpLoadMap.getStatus() == YES )
			{
				 // need to pull in this map information....
				const char* pName = mpLoadMap.getMapFileName();
				LogisticsData::instance->setCurrentMission( pName );

				// now I need to update the other people....
				setMission(pName);

				resetCheckBoxes();
				
			}
			bLoading = false;
		}
		return;
	}
	else
	{	
		bool bEditHasFocus = 0;

		// disable necessary stuff
		if ( !MPlayer->isHost() )
		{
			if ( MPlayer->missionSettings.mapGuid == NO_VERSION_GUID )
			{
				checkVersionClientOnly( MPlayer->missionSettings.map );
			}
			if ( MPlayer->playerInfo[MPlayer->commanderID].leftSession ) // I've been booted!
			{
				if ( status != GOTOSPLASH )
				{
					status = GOTOSPLASH;
					beginFadeOut( .5 );
				}
				return;
			}
			for ( int i = MP_INCREMENT_DROPWEIGHT; i < MP_DECREMENT_RP+1; i++ )
			{
				aButton* pButton = getButton( i );
				if ( pButton )
				{
					pButton->showGUIWindow( 0 );
				}
			}

			for (int i = MP_DECREMENT_CBILLS+1; i < MP_RP_FORMMECHS+1; i++ )
			{
				aButton* pButton = getButton( i );
				if ( pButton )
				{
					pButton->disable( true );
				}
			}

			for (int i = 0; i < 3; i++ )
				edits[i].setReadOnly( true );

			getButton( 101/*load map button*/ )->showGUIWindow( false );
			getButton( MP_LOCKGAME )->showGUIWindow( false );
			getButton( MP_BOOTPLAYER )->showGUIWindow( false );

			if ( MPlayer->startLoading || MPlayer->startLogistics )
			{
				int faction = MPlayer->getPlayerInfo( MPlayer->commanderID )->faction;
				faction = std::max(faction, 0);
				if ( MPlayer->missionSettings.allTech )
					LogisticsData::instance->setPurchaseFile( pPurchaseFiles[4] );
				else
					LogisticsData::instance->setPurchaseFile( pPurchaseFiles[faction] );
				LogisticsData::instance->setCurrentMission( MPlayer->missionSettings.map );

				status = NEXT;
				return;
			}

			if ( !mc2::str::iequals(mapName, MPlayer->missionSettings.map)
				&& strlen( MPlayer->missionSettings.map )) // check for new map
			{
				//
				setMissionClientOnly( MPlayer->missionSettings.map );
			}
		
		
		}
		else
		{

			// make sure King of the hill missions have a time limit
			if ( MPlayer->missionSettings.timeLimit  < 60 &&
				(MPlayer->missionSettings.missionType == MISSION_TYPE_KING_OF_THE_HILL
				|| MPlayer->missionSettings.missionType == MISSION_TYPE_LAST_MAN_ON_THE_HILL 
				|| MPlayer->missionSettings.missionType == MISSION_TYPE_TERRITORIES) )
			{
				MPlayer->missionSettings.timeLimit = 300;
			}
			if ( MPlayer->missionSettings.dropWeight < 30 )
			{
				MPlayer->missionSettings.dropWeight = 30;
			}
			if ( MPlayer->missionSettings.defaultCBills < 50000 )
			{
				MPlayer->missionSettings.defaultCBills = 50000;
				MPlayer->redistributeRP();

			}
			for ( int i = MP_INCREMENT_DROPWEIGHT; i < MP_RP_FORMMECHS+1; i++ )
			{
				aButton* pButton = getButton( i );
				if ( pButton )
				{
					pButton->disable( false );
					pButton->showGUIWindow( true );
				}
			}

			for (int i = 0; i < 3; i++ )
				edits[i].setReadOnly( false );


			// see if they clicked the launch button and there are too many players
			if ( playerCount > MPlayer->missionSettings.maxPlayers )
			{
				if ( userInput->isLeftClick() &&
					getButton( MB_MSG_NEXT )->pointInside( userInput->getMouseX(), userInput->getMouseY() ) )
				{
					char errorStr[256];
					LogisticsOneButtonDialog::instance()->setText( IDS_PLAYER_LEFT,
						IDS_DIALOG_OK, IDS_DIALOG_OK );

					cLoadString( IDS_MP_PARAM_ERROR_TOO_MANY_PLAYERS, errorStr, 255 );
					LogisticsOneButtonDialog::instance()->setText( errorStr );
					LogisticsOneButtonDialog::instance()->begin();
					bErrorDlg = true;

				}
			}

			MC2Player* data = MPlayer->getPlayerInfo( MPlayer->commanderID );
			{
				if ( data )
					data->ready = true; // default to ready for host since they have to press launch anyway
				else
					return;
			}


			getButton( 101/*load map button*/ )->showGUIWindow( true );

			//now check for specifics...
			bool bDisable = MPlayer->missionSettings.timeLimit <= 0;
			getButton( MP_DECREMENT_TIME )->disable( bDisable );
			bDisable = MPlayer->missionSettings.timeLimit >= 3600.f;
			getButton( MP_INCREMENT_TIME )->disable( bDisable );

			bDisable = MPlayer->missionSettings.resourcePoints <= 0;
			getButton( MP_DECREMENT_RP )->disable( bDisable );
			bDisable = MPlayer->missionSettings.resourcePoints >= 99900.f;
			getButton( MP_INCREMENT_RP )->disable( bDisable );

			
			bDisable = MPlayer->missionSettings.defaultCBills <= 50000;
			getButton( MP_DECREMENT_CBILLS )->disable( bDisable );
			bDisable = MPlayer->missionSettings.defaultCBills >= 999000.f;
			getButton( MP_INCREMENT_CBILLS )->disable( bDisable );

			bDisable = MPlayer->missionSettings.dropWeight <= 30;
			getButton( MP_DECREMENT_DROPWEIGHT )->disable( bDisable );
			bDisable = MPlayer->missionSettings.dropWeight >= 1200.f;
			getButton( MP_INCREMENT_DROPWEIGHT )->disable( bDisable );

			getButton( MP_QUICKSTART )->disable( 0 );

			getButton( MP_LOCKGAME )->showGUIWindow( true );
			getButton( MP_BOOTPLAYER )->showGUIWindow( true );
			getButton( MP_BOOTPLAYER )->disable( false );



			for(int i = 0; i < playerCount; i++ )
			{
				if ( playerParameters[i].hasFocus() )
				{
					if ( playerParameters[i].getCommanderID() == MPlayer->commanderID )	
						getButton( MP_BOOTPLAYER )->disable( true );
				}
			}
		
			
		}
		
		int oldEditFocus = -1;
		for ( int i = 0; i < editCount; i++ )
		{
			if ( edits[i].hasFocus() )
				oldEditFocus = i;
		}
		if ( !ChatWindow::instance()->pointInside(userInput->getMouseX(), userInput->getMouseY()) )
		{
			LogisticsScreen::update();
		}

		if ( oldEditFocus != -1 )
		{
			bEditHasFocus = true;
			if ( !edits[oldEditFocus].hasFocus() )
			{
				std::string text = edits[oldEditFocus].entry();
				long val = atoi( text.c_str() );
				switch (oldEditFocus)
				{
					case 0:
						MPlayer->missionSettings.dropWeight = val;
						MPParameterScreen::resetCheckBoxes();
						break;
					case 1:
						MPlayer->missionSettings.defaultCBills = val * 1000;
						if (MPlayer->isHost())
							MPlayer->redistributeRP();
						MPParameterScreen::resetCheckBoxes();
						
						break;
					case 2:
						MPlayer->missionSettings.timeLimit = val * 60;
						MPParameterScreen::resetCheckBoxes();
						break;
					case 3:
						MPlayer->missionSettings.resourcePoints = val;
						MPParameterScreen::resetCheckBoxes();
						break;
				}
			}
		}

		
	/// now make all enabled so they draw correctly...
		for (int i = MP_AIRSTRIKES; i < MP_RP_FORMMECHS+1; i++ )
		{
			aButton* pButton = getButton( i );
			if ( pButton )
			{
				pButton->disable( false );
			}
		}

		getButton( MP_QUICKSTART )->disable( 0 );

		if ( MPlayer )
		{
			int oldPlayerCount = playerCount;
			const MC2Player* players = MPlayer->getPlayers(playerCount);
			
			if ( oldPlayerCount != playerCount )
			{
				// new player... need to redistribute rp
				int maxCommander = -1;
				int teamID = -1;
				for( int i = 0; i < playerCount; i++ )
				{
					if ( players[i].commanderID > maxCommander )
					{
						maxCommander = players[i].commanderID;
						teamID = players[i].team;
					}
				}
			}
			bool bReady = true;
			
			const MC2Player* sortedPlayers[MAX_MC_PLAYERS];
			for ( int i = 0; i < playerCount; i++ )
			{
				sortedPlayers[i] = &players[i];
			}

			qsort( sortedPlayers, playerCount, sizeof( MC2Player* ), sortPlayers );

			for(int i = 0; i < playerCount; i++ )
			{
				playerParameters[i].setData( sortedPlayers[i] );
			}
			int team = -1;
			if ( playerCount )
				team = sortedPlayers[0]->team;
			bool bUniqueTeam = 0;
			for(int i = 0; i < playerCount; i++ )
			{
//				if ( !ChatWindow::instance()->pointInside(userInput->getMouseX(), userInput->getMouseY() ) )
					playerParameters[i].update();
				bReady &= sortedPlayers[i]->ready;
				if ( !MPlayer->missionSettings.quickStart )
					bReady &= ((sortedPlayers[i]->cBills) ? 1 : 0);

				if ( playerParameters[i].hasFocus() )
					bEditHasFocus = true;

				if ( sortedPlayers[i]->team != team )
					bUniqueTeam = 1;

			}

			bReady &= !bEditHasFocus;
			bReady &= (MPlayer->isHost());
			bReady &= strlen( MPlayer->missionSettings.map ) ? 1 : 0;
			bReady &= (playerCount > 1);
			bReady &= (MPlayer->missionSettings.maxPlayers >= playerCount);
			bReady &= bUniqueTeam;
			bReady &= (MPlayer->missionSettings.dropWeight >= 30);

			bool bPress = MPlayer->missionSettings.airStrike;
			getButton( MP_AIRSTRIKES )->press( bPress );

			bPress = MPlayer->missionSettings.sensorProbe;
			getButton( MP_SENSOR_PROBE )->press( bPress );

			bPress = MPlayer->missionSettings.mineLayer;
			getButton( MP_MINELAYER )->press( bPress );

			bPress = MPlayer->missionSettings.repairVehicle;
			getButton( MP_REPAIR )->press( bPress );

			bPress = MPlayer->missionSettings.scoutCopter;
			getButton( MP_SCOUTS )->press( bPress );

			bPress = MPlayer->missionSettings.recoveryTeam;
			getButton( MP_RECOVERY )->press( bPress );

			bPress = MPlayer->missionSettings.guardTower;
			getButton( MP_GUARDTOWER )->press( bPress );

			bPress = MPlayer->missionSettings.resourceBuilding;
			getButton( MP_RESOURCE_BLDG )->press( bPress );

			bPress = MPlayer->missionSettings.unlimitedAmmo;
			getButton( MP_UNLIMITED_AMMO )->press( bPress );

			bPress = !MPlayer->missionSettings.variants;
			getButton( MP_VARIANTS )->press( bPress );

			bPress = MPlayer->missionSettings.allTech;
			getButton( MP_ALLTECH )->press( bPress );

			bPress = MPlayer->missionSettings.resourceForMechs;
			getButton( MP_RP_FORMMECHS )->press( bPress );

			bPress = MPlayer->missionSettings.quickStart;
			getButton( MP_QUICKSTART )->press( bPress );

			bPress = MPlayer->missionSettings.locked;
			getButton( MP_LOCKGAME )->press( bPress );

			if ( !strlen(MPlayer->missionSettings.map) )
			{
				getButton( MAP_INFO )->disable( true );
			}
//			else
//				getButton( MAP_INFO )->disable( false );

	
			textObjects[1].setText( MPlayer->missionSettings.name );

			
//			cLoadString( IDS_MP_LM_MAP_LIST_TYPE, tmp, 255 );
//			cLoadString( IDS_MP_LM_TYPE0 + MPlayer->missionSettings.missionType, tmp2, 127 );
//			sprintf( text, tmp, tmp2 );
//			textObjects[7].setText( text );
			mc2::fmt::to_buf( text, "{:d}k", mc2::fmt::as_i32(MPlayer->missionSettings.defaultCBills/1000) );
			if ( !edits[1].hasFocus() )
				edits[1].setEntry( text  );
			mc2::fmt::to_buf( text, "{:d}", mc2::fmt::as_i32(MPlayer->missionSettings.resourcePoints) );
			if ( !edits[3].hasFocus() )
				edits[3].setEntry( text );
			mc2::fmt::to_buf( text, "{:.0f}", fabs(MPlayer->missionSettings.timeLimit/60.f) );
			if ( !edits[2].hasFocus() )
			{
				if ( MPlayer->missionSettings.timeLimit/60.f <= 0 )
					cLoadString( IDS_MP_PARAM_UNLIMITED, text, 255 );
				edits[2].setEntry( text );
			}

			mc2::fmt::to_buf( text, "{:d}", mc2::fmt::as_i32(MPlayer->missionSettings.dropWeight) );
			if ( !edits[0].hasFocus() )
				edits[0].setEntry( text );

			if ( bReady )
			{
				getButton( MB_MSG_NEXT )->disable( false );
			}
			else
			{
				getButton( MB_MSG_NEXT )->disable( true );
				if ( delayTime )
				{
					char text[256];
					cLoadString( IDS_MP_LAUNCH_ABORTED, text, 255 );
					MPlayer->sendChat( nullptr, -1, text );
					delayTime = 0.f;
					bWaitingToStart = 0;
					MPlayer->setInProgress( 0 );
				}
			}

	

		}

		if ( !bEditHasFocus )
			ChatWindow::instance()->update();

	}

	helpTextID = 0;
	helpTextHeaderID = 0;



	/*
	for ( int i = 0; i < buttonCount; i++ )
	{
		buttons[i].update();
		if ( buttons[i].pointInside( userInput->getMouseX(), userInput->getMouseY() )
			&& userInput->isLeftClick() )
		{
			handleMessage( buttons[i].getID(), buttons[i].getID() );
		}

	}
	*/
}
