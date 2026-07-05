#define MISSIONSELECTIONSCREEN_CPP
/*************************************************************************************************\
MissionSelectionScreen.cpp			: Implementation of the MissionSelectionScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"missionselectionscreen.h"
#include"mechbayscreen.h"
#include"logisticsdata.h"
#include "../resource.h"
#include"missionbriefingscreen.h"
#include"sounds.h"
#include"mc2movie.h"
#include"gamesound.h"

#define VIDEO_RECT 7
#define MAP_RECT 3
#define MSG_FIRST_MISSION 100
#define LB_RECT 4
#define RP_TEXT 4

MissionSelectionScreen::MissionSelectionScreen(  )
{
	status = RUNNING;
	LogisticsScreen::helpTextArrayID = 3;
	bMovie = 0;

	missionCount = 0;
	// pressedButton was left uninitialized -> update() reads garbage at line 177
	// (the `!= -1` guard passes for any non-(-1) value) and indexes
	// missionNames[garbage] -> wild READ crash. Nondeterministic: triggers only
	// when the uninitialized value happens to satisfy the bounds checks, so a
	// rebuild that shifts memory layout can surface it. Init to the "none" sentinel.
	pressedButton = -1;

	memset( missionNames, 0, sizeof( const char* ) * MAX_MISSIONS_IN_GROUP );
	bStop = 0;

	missionDescriptionListBox.setPressFX( -1 );
	missionDescriptionListBox.setHighlightFX( -1 );
	missionDescriptionListBox.setDisabledFX( -1 );
}

MissionSelectionScreen::~MissionSelectionScreen()
{
	if (bMovie)
	{
		delete bMovie;
		bMovie = NULL;
	}

	missionDescriptionListBox.destroy();
}

//-------------------------------------------------------------------------------------------------


void MissionSelectionScreen::init( FitIniFile* file )
{
	LogisticsScreen::init( *file, "CMStatic", "CMTextEntry", "CMRect", "CMButton" );
	// Route the hover help text (set by LogisticsScreen::update) into the defs help area.
	defsHelpTextKey = "game.mission_selection.text.help_text_area";
	for ( int i= 0; i < buttonCount; i++ )
		buttons[i].setMessageOnRelease();

	missionCount = 0;

	missionDescriptionListBox.init( rects[LB_RECT].left(), rects[LB_RECT].top(),
		rects[LB_RECT].width(), rects[LB_RECT].height() );

	missionDescriptionListBox.setOrange(true);

	getButton( MN_MSG_PLAY )->setPressFX( LOG_VIDEOBUTTONS );
	getButton( MN_MSG_STOP )->setPressFX( LOG_VIDEOBUTTONS );
	getButton( MN_MSG_PAUSE )->setPressFX( LOG_VIDEOBUTTONS );
	getButton( MN_MSG_PLAY )->setHighlightFX( LOG_DIGITALHIGHLIGHT );
	getButton( MN_MSG_STOP )->setHighlightFX( LOG_DIGITALHIGHLIGHT );
	getButton( MN_MSG_PAUSE )->setHighlightFX( LOG_DIGITALHIGHLIGHT );


}

void MissionSelectionScreen::render(int xOffset, int yOffset )
{
	// Mission overview renders via the defs GuiText (routed in updateListBox);
	// the legacy aListBox is no longer drawn.

	// VIDCOM: composite the decoded video frame THROUGH the defs image (ImGui
	// layer) so it overlays the notransmission fallback, instead of the old gos
	// quad which drew underneath the defs overlay. Passing 0 when nothing is
	// playing clears the override so the element shows the notransmission art.
	if ( !xOffset && !yOffset )
	{
		const char* vidcom = "game.mission_selection.image.no_transmission_thing";
		if ( bMovie && bMovie->isPlaying() && bMovie->getTextureHandle() )
			setDefsElementGosTexture( vidcom, bMovie->getTextureHandle() );
		else
			setDefsElementGosTexture( vidcom, 0 );
	}

	LogisticsScreen::render( xOffset, yOffset );
	if ( !xOffset && !yOffset )
	{
		/*
		gos_VERTEX v[4];

		for( int i = 0; i < 4; i++ )
		{
			v[i].argb = 0xffffffff;
			v[i].frgb = 0;
			v[i].rhw = .5;
			v[i].u = 0.f;
			v[i].v = 0.f;
			v[i].x = rects[VIDEO_RECT].left()+1 + xOffset;
			v[i].y = rects[VIDEO_RECT].top()+1 + yOffset;
			v[i].z = 0.f;
		}

		v[2].x = v[3].x = v[0].x + rects[VIDEO_RECT].width()-2;
		v[2].y = v[1].y = v[0].y + rects[VIDEO_RECT].height()-2;

		v[2].u = v[3].u = 1.0f;
		v[2].v = v[1].v = 1.0f;

		gos_SetRenderState( gos_State_Texture,  videoTexture );
		gos_DrawQuads( v, 4 );
		gos_SetRenderState( gos_State_Texture,  0 );
		*/

		// bMovie->render() (gos quad) removed — the frame now composites via the
		// defs VIDCOM image above. bMovie->update() still decodes each frame.
	}


	operationScreen.render( xOffset, yOffset );


}

void MissionSelectionScreen::update()
{
	if (!playedLogisticsTune)
	{
		soundSystem->playDigitalMusic(LogisticsData::instance->getCurrentMissionTune());
		playedLogisticsTune = true;
	}

	if ( bStop )
	{
		if ( bMovie )
			bMovie->stop();
		bStop = false;
	}

	if ( bMovie )
		bMovie->update();

	LogisticsScreen::update();
	operationScreen.update();

	long oldButton = -1;
	int highlightButton = -1;

	long mouseX = userInput->getMouseX();
	long mouseY = userInput->getMouseY();

	unsigned long highlightColor = 0;

	for ( int i = 0; i < operationScreen.buttonCount; i++ )
	{
		if ( operationScreen.buttons[i].isShowing() )
		{
			if ( operationScreen.buttons[i].pointInside( mouseX, mouseY ) )
			{
				highlightButton = i;
				highlightColor = operationScreen.buttons[i].getColor( );
			}

			if ( operationScreen.buttons[i].isPressed() && i != pressedButton )
			{
				handleMessage( 0, operationScreen.buttons[i].getID() );
				oldButton = pressedButton;
				pressedButton = i;
				break;
			}
		}
	}

	if ( oldButton != -1 )
		operationScreen.buttons[oldButton].press( 0 );

	missionDescriptionListBox.update();

	// Use >= 0 (not != -1): any negative index, not just the -1 sentinel, must
	// be rejected before missionNames[pressedButton] or it reads before the array.
	if ( pressedButton >= 0 && pressedButton < missionCount
		&& pressedButton < MAX_MISSIONS_IN_GROUP && missionNames[pressedButton] )
	{
		operationScreen.textObjects[0].setText( LogisticsData::instance->getMissionFriendlyName(
			missionNames[pressedButton] ) );
//		if ( highlightColor )
//			operationScreen.textObjects[0].setColor( highlightColor );
	}
	else
	{
		operationScreen.textObjects[0].setText( "" );
	}

	if ( LogisticsData::instance->skipLogistics() ) // the launch button
	{
		buttons[5].showGUIWindow( 1 );
	}
	else
		buttons[5].showGUIWindow( 0 );

	

}

void MissionSelectionScreen::begin()
{
	status = RUNNING;
	playedLogisticsTune = false;

	if ( fadeInTime )
		operationScreen.beginFadeIn( fadeInTime );

	// initialize the operation
	const char* str = LogisticsData::instance->getCurrentOperationFileName();

	FullPathFileName fileName;
	fileName.init( artPath, str, ".fit" );

	FitIniFile file;
	if ( NO_ERR != file.open( fileName ) )
	{
		char errorStr[256];
		sprintf( errorStr, "couldn't open file %s", (const char*)fileName );
		Assert( 0, 0, errorStr );
	}
	
	operationScreen.moveTo( 0, 0 );
	operationScreen.init( file, "Static", "Text", "Rect", "Button" );
	
	operationScreen.moveTo( rects[MAP_RECT].left(), 
							rects[MAP_RECT].top() );


	
	str = LogisticsData::instance->getCurrentVideoFileName();
	if ( str && strlen( str ) )
	{
		FullPathFileName videoName;
		videoName.init( moviePath, str, ".bik" );

		if (fileExists(videoName) || fileExistsOnCD(videoName))
		{
			// rects[VIDEO_RECT] is already in screen-space (the .fit
			// layout file is authored against the game's 800x600
			// logical coordinate system, which the renderer maps to
			// the actual window). An earlier fix here added MAP_RECT
			// on top, which double-counted after video_open was fixed
			// to preserve the origin — pushed the briefing behind the
			// objectives pane. Use VIDEO_RECT as-is.
			RECT movieRect;
			movieRect.left = rects[VIDEO_RECT].left()+1;
			movieRect.top = rects[VIDEO_RECT].top()+1;
			movieRect.right = movieRect.left + rects[VIDEO_RECT].width()-2;
			movieRect.bottom = movieRect.top + rects[VIDEO_RECT].height()-2;
		
			//If there is one already here, cause we loaded a savegame or something,
			// Toss it to prevent leaking from the system Heap!
			if (bMovie)
			{
				bMovie->stop();
				delete bMovie;
				bMovie = NULL;
			}

			bMovie = new MC2Movie;
			bMovie->init(videoName,movieRect,true);

			if (Environment.Renderer == 3)
			{
				//DO NOT show the movies by default in software.
				// It will still work, if they hit play.
				bMovie->stop();
			}

			if ( LogisticsData::instance->getVideoShown() )
				bMovie->stop();

			LogisticsData::instance->setVideoShown( );
		}
	}

	missionCount = MAX_MISSIONS_IN_GROUP;
	int result = LogisticsData::instance->getCurrentMissions( missionNames, missionCount );

	EString selMissionName = LogisticsData::instance->getCurrentMission();
	gosASSERT( result == NO_ERR );

	bool bPressed = 0;
    int i = 0;
	for (; i < missionCount; i++ )
	{
		if ( i >= operationScreen.buttonCount )
		{
			Assert( 0, 0, "not enough buttons on the operation screen" );
			break;
		}
		else
		{
			operationScreen.buttons[i].setID( MSG_FIRST_MISSION + i );
			operationScreen.buttons[i].setPressFX( LOG_SELECT );
			if ( !LogisticsData::instance->getMissionAvailable( missionNames[i] ) )
				operationScreen.buttons[i].disable( true );


			else
			{
				operationScreen.buttons[i].disable( false );
				if ( selMissionName.Compare( missionNames[i], 0 ) == 0 )
				{
					operationScreen.buttons[i].press( true );
					handleMessage( 0, MSG_FIRST_MISSION + i );
					bPressed = 1;
					pressedButton = i;

				}
				if ( !bPressed && !selMissionName.Length()  )
				{
					bPressed = 1;
					pressedButton = i;
					operationScreen.buttons[i].press( true );
					handleMessage( 0, MSG_FIRST_MISSION + i );
				}
			}
		}

	}

	for ( ; i < operationScreen.buttonCount; i++ )
	{
		operationScreen.buttons[i].showGUIWindow( 0 );
	}

	bStop = 0;
}

void MissionSelectionScreen::end()
{
	if ( bMovie )
	{
		bMovie->stop();

		delete bMovie;
		bMovie = NULL;
	}

	beginFadeOut( 0 );
}

int MissionSelectionScreen::handleMessage( unsigned long msg, unsigned long who )
{
	if ( who >= MSG_FIRST_MISSION && who < MSG_FIRST_MISSION + MAX_MISSIONS_IN_GROUP )
	{
		setMission( who - MSG_FIRST_MISSION );
	}
	
	switch( who )
	{
	case MB_MSG_NEXT:
		status = NEXT;
		break;

	case MB_MSG_PREV:
		status = PREVIOUS;
		break;

	case MN_MSG_PLAY:
		if ( bMovie )
			bMovie->restart();
		getButton( MN_MSG_STOP )->press( 0 );
		getButton( MN_MSG_PAUSE )->press( 0 );
		break;
	case MN_MSG_STOP:
		if ( bMovie )
			bMovie->stop();
		bStop = true;
		break;

	case MN_MSG_PAUSE:
		if ( !getButton( who )->isPressed() )
		{
			if ( bMovie )
				bMovie->pause(0);
			getButton( who )->press(false);
		}
		else
		{
			if ( bMovie )
				bMovie->pause(1);
			getButton( who )->press(true);
		}


		break;

	case MB_MSG_MAINMENU:
		status = MAINMENU;
		break;
	}

	return 0;

}

void MissionSelectionScreen::setMission( int whichOne )
{
	LogisticsData::instance->setCurrentMission( missionNames[whichOne] );

	char text[64];
	sprintf( text, "%ld ", LogisticsData::instance->getCBills() );
	textObjects[RP_TEXT].setText( text );
	setDefsElementText( "game.mission_selection.text.cbills_readout", text );


	updateListBox();

}

void MissionSelectionScreen::updateListBox()
{
	missionDescriptionListBox.removeAllItems( true );

	// Leave the legacy aListBox EMPTY (it is auto-rendered by the aObject system,
	// so emptying it is the only reliable way to stop the old text drawing).

	// Pure-ImGui path: route name + description into the converter's pre-generated
	// runtime_text defs elements (runtimeTextBinding = mission.selection.descriptionList).
	const char* mn = LogisticsData::instance->getCurrentMissionFriendlyName();
	const char* md = LogisticsData::instance->getCurrentMissionDescription();
	setDefsElementText( "game.mission_selection.runtime_text.current_mission_name", mn ? mn : "" );
	setDefsElementText( "game.mission_selection.runtime_text.current_mission_blurb", md ? md : "" );







}


//*************************************************************************************************
// end of file ( MissionSelectionScreen.cpp )
