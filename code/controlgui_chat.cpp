// ControlGui: pause, tab switch, movie / pilot video, rollover help text and the chat window.
// Declared in the class headers; shared names in controlgui_internal.h.
#include "controlgui_internal.h"
#include "../GameOS/gameos/utils/mc2log.h"

#include <string_view>
#include <utility>
#include "mc2str.h"


void ControlGui::beginPause()
{
	getButton( SAVE_COMMAND )->press( 1 );
	pauseWnd->begin(renderObjectives);
}
void ControlGui::endPause()
{
	getButton( SAVE_COMMAND )->press( 0 );
	pauseWnd->end();
}

void ControlGui::switchTabs(int direction)
{
	// slice C site 7 (hotkey cycle): registration order IS the cycle order
	// (tacmap -> vehicle -> info == the legacy hand-written table), so cycle()
	// replaces the 3-way if/else. Legacy table otherwise.
	if ( hudTabsGateOn() && hudTabRegistry.count() > 0 )
	{
		setActiveHudTab( hudTabRegistry.cycle( direction ), "cycle" );
		return;
	}

	int ID = TACMAP_TAB;
		for ( int i = TACMAP_TAB; i < TACMAP_TAB + 3; i ++ )
	{
		if ( getButton( i )->state & ControlButton::PRESSED )
		{
			ID = i;
			getButton( i )->press( false );
		}
	}
	if ( ID == INFO_TAB )
	{
		if ( direction > 0 )
			ID = TACMAP_TAB;
		else
			ID = VEHICLE_TAB;
	}

	else if ( ID == VEHICLE_TAB )
	{
		if ( direction > 0 )
			ID = INFO_TAB;
		else
			ID = TACMAP_TAB;
	}
	else
	{
		if ( direction > 0 )
			ID = VEHICLE_TAB;
		else
			ID = INFO_TAB;
	}

	getButton( ID )->press( true );

}

void ControlGui::playMovie( const char* fileName )
{
	if (moviePlaying)
	{
		objectiveObservationSink("movie", "gui", "busy", fileName);
		return;
	}

	// Briefing videos must not block a boot-to-purchase capture.
	if ( mc2::gate::isNonEmpty<"MC2_BOOT_TO_BAY">() )
	{
		MC2_EVENT(Core, "VIDEO", 1, "event=skip name={} reason=boot_to_bay", mc2::str::from_c( fileName ));
		objectiveObservationSink("movie", "gui", "boot_to_bay", fileName);
		return;
	}

	// The layout predates the current resolution offsets.
	RECT vRect;
	vRect.left   = videoRect.left   + hiResOffsetX;
	vRect.right  = videoRect.right  + hiResOffsetX;
	vRect.top    = videoRect.top    + hiResOffsetY;
	vRect.bottom = videoRect.bottom + hiResOffsetY;

	char realName[1024];
	_splitpath(fileName,nullptr,nullptr,realName,nullptr);

	FullPathFileName movieName;
	movieName.init(moviePath,realName,".bik");
	bMovie = std::make_unique_for_overwrite<MC2Movie>();
	bMovie->init(movieName,vRect,true);
	// Requests remain distinct from decoder and GUI acceptance.
	MC2_EVENT(Core, "VIDEO", 1, "event=play_requested name={}", std::string_view( realName ));

	// A rejected movie must not flash the video border.
	bMovie->update();
	if (bMovie->isPlaying())
	{
		moviePlaying = true;
		objectiveObservationSink("movie", "gui", "accepted", movieName);
	}
	else
	{
		moviePlaying = false;
		bMovie.reset();
		bMovie = nullptr;
		objectiveObservationSink("movie", "gui", "rejected", movieName);
	}
}

bool ControlGui::resultsDone()
{
	if( ( renderStatusInfo && objectiveTime > Team::home->objectives.Count() + 4 ) )
	{
		// need to empty out force group bar now...
		forceGroupBar.removeAll();
		return true;
	}

	return false;
}

void ControlGui::setRolloverHelpText( unsigned long textID )
{
	if ( helpTextID && textID != 0 )
		return;

	if ( useLeftRightMouseProfile )
		textID ++;

	helpTextHeaderID = 0;
	helpTextID = textID;
}

void	ControlGui::toggleHoldPosition()
{
	if ( getButton( CUR_RANGE )->state == ControlButton::ENABLED )
	{
		setRange( FIRERANGE_CURRENT );
	}
	else 
	{
		setRange( FIRERANGE_OPTIMAL );
	}
	
	soundSystem->playDigitalSample( LOG_SELECT );

	if (MPlayer && !MPlayer->isServer())
		MPlayer->sendHoldPosition();
	else {
		for (long i = 0; i < Team::home->getRosterSize(); i++) {
			Mover* pMover = Team::home->getMover( i );
			if ( pMover->isSelected() && commanderIsHome(pMover->getCommander()) )
			{
				if (pMover->getPilot()->getCurTacOrder()->code != TACTICAL_ORDER_NONE) {
					pMover->getPilot()->getCurTacOrder()->attackParams.range = static_cast<FireRangeType>(pMover->attackRange);
					if (pMover->attackRange == FIRERANGE_CURRENT)
						pMover->getPilot()->getCurTacOrder()->attackParams.pursue = false;
					else if (getFireFromCurrentPos())
						pMover->getPilot()->getCurTacOrder()->attackParams.pursue = false;
					else
						pMover->getPilot()->getCurTacOrder()->attackParams.pursue = true;
					pMover->getPilot()->getCurTacOrder()->pack(nullptr, nullptr);
					pMover->handleTacticalOrder(*pMover->getPilot()->getCurTacOrder());
				}
			}
		}
	}
}

int ControlGui::updateChat()
{
	if ( bChatting && MPlayer)
	{
		personalEdit.update();
		if ( !personalEdit.hasFocus() )
		{
//			toggleChat(0);
		}

		return 1;
	}

	return 0;
}

void ControlGui::toggleChat(bool isTeamOnly)
{
	bChatting ^= 1;
	if ( !bChatting && MPlayer ) // over -- broadcast messages
	{
		std::string tmp = personalEdit.entry();

		if ( !tmp.empty() )
		{
		
			char team = chatIsTeamOnly ? MPlayer->getPlayerInfo( MPlayer->commanderID )->team : -1;
			MPlayer->sendChat(nullptr, team, tmp.data());    // writable C string, used inside the call
		}
		personalEdit.setFocus( 0 );
	}
	else if ( MPlayer )
	{
		personalEdit.limitEntry( 127 );
		personalEdit.setFocus( true );
		personalEdit.setEntry( "" );
	}

	chatIsTeamOnly = isTeamOnly;
}

void ControlGui::eatChatKey()
{
	if ( bChatting )
		personalEdit.setFocus( true );
	else
		personalEdit.setFocus( false );
}

void ControlGui::setChatText( const char* playerName, const char* message, unsigned long color, unsigned long chatColor )
{
    int i = 0;
	for (; i < MAX_CHAT_COUNT - 1; i++ )
	{
		memcpy( &chatInfos[i], &chatInfos[i+1], sizeof( ChatInfo ) );
	}

	i = MAX_CHAT_COUNT - 1;

	gosASSERT( strlen( message ) < 128 );
	gosASSERT( strlen( playerName ) < 32 );

	chatInfos[i].backgroundColor = color;
	strcpy( chatInfos[i].message, message );
	strcpy( chatInfos[i].playerName, playerName );
	chatInfos[i].time = scenarioTime;
	chatInfos[i].chatTextColor = chatColor;
	objectiveObservationSink("chat", "stored", "accepted", chatInfos[i].message);

	long totalHeight = chatEdit.font.height( message, chatEdit.width() );
	long lineHeight = chatEdit.font.height( "A", chatEdit.width() );


	chatInfos[i].messageLength = totalHeight/lineHeight;

	if ( chatInfos[i].messageLength > 1 )
		chatInfos[i].messageLength --;

	if ( MPlayer && ChatWindow::instance() )
	{
		ChatWindow::instance()->update();
	}

}

void ControlGui::renderChatText()
{
	long curLine = 0;

	if ( bChatting && MPlayer)
	{
		playerNameEdit.setColor( 0xff007f00 );
		if ( MPlayer )
		{
			MC2Player* pInfo = MPlayer->getPlayerInfo( MPlayer->commanderID );
			long color = MPlayer->colors[pInfo->baseColor[BASECOLOR_TEAM]];
			long textColor = 0xff000000;

			if ( ((color & 0xff) + ( (color & 0xff00)>>8 ) + ( (color & 0xff0000)>>16 ))/3 < 85 )
				textColor = 0xffffffff;

			playerNameEdit.setColor( color );
			playerNameEdit.setTextColor( textColor );
			long width = playerNameEdit.getFontObject()->width( pInfo->name ) + playerNameEdit.getFontObject()->width( " " );
			playerNameEdit.resize( width + 8, playerNameEdit.height() );
			playerNameEdit.setEntry( pInfo->name );
			personalEdit.move( playerNameEdit.right() - personalEdit.left(), 0 );

		}
		else
			playerNameEdit.setEntry( "Heidi" );
		playerNameEdit.render();
		personalEdit.render();
		curLine++;
	}

	int lineCount = 0;
	int curTime = scenarioTime;
    int i;
	for (i = MAX_CHAT_COUNT - 1; i > -1; i-- )
	{
		if ( chatInfos[i].messageLength && curTime - chatInfos[i].time < CHAT_DISPLAY_TIME )
		{
			lineCount += chatInfos[i].messageLength;
		}

		if ( lineCount > MAX_CHAT_COUNT || curTime - chatInfos[i].time > CHAT_DISPLAY_TIME )
			break;
	}

	i++;

	float height = chatEdit.height();

	for ( ; i < MAX_CHAT_COUNT; i++ )
	{
		if ( i < 0 )
			continue;

		if ( lineCount > MAX_CHAT_COUNT )
		{
			// for now ignore
		//	int numToMove = lineCount - MAX_CHAT_COUNT;
		//	chatEdit.move( 0, -numToMove * (height + 1 ));
		//	chatEdit.render();
		//	chatEdit.move( 0, numToMove * (height+1 ));
		//	curLine += MAX_CHAT_COUNT - numToMove;

			lineCount -= chatInfos[i].messageLength;
		}
		else
		{
			if ( chatInfos[i].messageLength )
			{
				long color = chatInfos[i].backgroundColor;
				long textColor = 0xff000000;

				if ( ((color & 0xff) + ( (color & 0xff00)>>8 ) + ( (color & 0xff0000)>>16 ))/3 < 85 )
					textColor = 0xffffffff;

				chatEdit.resize( chatEdit.width(), (chatEdit.height()+2) * chatInfos[i].messageLength );
				playerNameEdit.setColor( color );
				playerNameEdit.setTextColor( textColor );
				playerNameEdit.move( 0, curLine * (height+1) );
				long width = playerNameEdit.getFontObject()->width( chatInfos[i].playerName ) + playerNameEdit.getFontObject()->width( " " );
				playerNameEdit.resize( width + 8, playerNameEdit.height() );
				playerNameEdit.setEntry( chatInfos[i].playerName );
				chatEdit.setText( chatInfos[i].message );
				chatEdit.setColor( chatInfos[i].chatTextColor );
				chatEdit.move( playerNameEdit.right() - chatEdit.left() + 4, curLine * (height+1) );
				int oldHeight = chatEdit.font.height();
				chatEdit.resize( chatEdit.width(), chatInfos->messageLength * oldHeight );
				playerNameEdit.render();
				chatEdit.render();
				chatEdit.resize( chatEdit.width(), oldHeight );
				chatEdit.move( 0, -curLine * (height+1) );
				playerNameEdit.move( 0, -curLine * (height+1) );
			}

	
			curLine += chatInfos[i].messageLength;
		}
	}

	chatEdit.resize( chatEdit.width(), height );
}

bool ControlGui::isMoviePlaying()
{
	if ( moviePlaying )
		return true;
	else if ( forceGroupBar.isPlayingVideo() )
		return true;

	return false;
}

bool ControlGui::playPilotVideo( MechWarrior* pPilot, char movieCode )
{
	// stupid name format = "Worm 1 filter.avi"
	//CHANGED to "worm1"  for CD 8.3 reasons.
	// -fs
	char fileName[512];
	strcpy( fileName, moviePath );

	char realPilotName[8];
	strncpy(realPilotName, pPilot->getName(), 7);
	realPilotName[7] = '\0'; // sebi: fix

	strcat( fileName, realPilotName ); // swap in pilot name when videos are done
	char tmp[3];
	tmp[0] = movieCode;
	tmp[1] = '\0';
	tmp[2] = '\0';
	strcat( fileName, tmp );

	return forceGroupBar.setPilotVideo( fileName, pPilot );


}

void ControlGui::endPilotVideo()
{
	forceGroupBar.setPilotVideo( nullptr, nullptr );
}

void ControlGui::cancelInfo()
{
	gosEnum_KeyIndex key;
	bool bC, bA, bS;
	MissionInterfaceManager::instance()->getHotKey( std::to_underlying(INFO_COMMAND_INDEX), key, bS, bC, bA );

	if ( gos_GetKeyStatus( key )!= KEY_HELD )
		getButton( INFO_COMMAND )->press( 0 );

}

void ControlGui::showServerMissing()
{

	if ( !MPlayer )
		return;

	if ( MPlayer->isServerMissing() )
	{
		// send a chat message
		if ( !bServerWarningShown )
		{
			char text[256];
			cLoadString(IDS_SERVER_MIGRATING, text, 255 );
			setChatText( "", text, 0, 0xffffffff );
			bServerWarningShown = true;
	
			// pause the game
			MissionInterfaceManager::instance()->togglePauseWithoutMenu();

		}
		

	}
	else 
	{
		if ( bServerWarningShown )
			MissionInterfaceManager::instance()->togglePauseWithoutMenu();
		bServerWarningShown = 0;
	}
	

}
