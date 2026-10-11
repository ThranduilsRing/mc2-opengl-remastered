// ControlGui: button, static and rect initialisation from the .fit files and resolution swap.
// Declared in the class headers; shared names in controlgui_internal.h.
#include "controlgui_internal.h"
#include "mc2fmt.h"
#include "../GameOS/gameos/utils/mc2log.h"


void ControlButton::initButtons( FitIniFile& buttonFile, long buttonCount, ControlButton* Buttons, ButtonData* Data, const char* str, aFont* font )
{
	ZoneScopedN("ControlButton::initButtons");
	char path[256];
	for ( int i = 0; i < buttonCount; ++i )
	{
		ZoneScopedN("ControlButton::initButtons button");
		mc2::fmt::to_buf(path, "{}{:d}", mc2::fmt::safe(str), mc2::fmt::as_i32(i));
		Data[i].textureHandle = 0;

		long result;
		{
			ZoneScopedN("ControlButton::initButtons seekBlock");
			result = buttonFile.seekBlock( path );
		}
		if ( result != NO_ERR )
		{
			char errorStr[256];
			mc2::fmt::to_buf(errorStr, "couldn't find button {}", mc2::fmt::safe(path));
			Assert( 0, 0, errorStr );
			continue;
		}

		{
			ZoneScopedN("ControlButton::initButtons metadata");
			buttonFile.readIdLong( "ID", Buttons[i].ID );
			buttonFile.readIdString("FileName", Data[i].fileName, 32 );

			buttonFile.readIdLong( "HelpCaption", Data[i].helpTextHeader );
			buttonFile.readIdLong( "HelpDesc", Data[i].helpTextID );
			buttonFile.readIdLong( "TextID", Data[i].textID );
			buttonFile.readIdLong( "TextNormal", Data[i].textColors[0] );
			buttonFile.readIdLong( "TextPressed", Data[i].textColors[1] );
			buttonFile.readIdLong( "TextDisabled", Data[i].textColors[2] );
			buttonFile.readIdLong( "TextHighlight", Data[i].textColors[3] );
		}
		if ( font )
			Data[i].textFont = *font;

		long x, y, width, height;

		{
			ZoneScopedN("ControlButton::initButtons layout");
			buttonFile.readIdLong( "XLocation", x );
			buttonFile.readIdLong( "YLocation", y );
			
			x += ControlGui::hiResOffsetX;
			y += ControlGui::hiResOffsetY;
			
			buttonFile.readIdLong( "Width", width );
			buttonFile.readIdLong( "Height", height );

			buttonFile.readIdLong( "HelpCaption", Data[i].helpTextHeader );
			buttonFile.readIdLong( "HelpDesc", Data[i].helpTextID );

			buttonFile.readIdBoolean( "texturesRotated", Data[i].textureRotated );
		}


		Buttons[i].data = &Data[i];

        // sebi: initialize state so it is not garbage
		Buttons[i].state = ENABLED;
		
		{
			ZoneScopedN("ControlButton::initButtons setupVertices");
		Buttons[i].location[0].x = Buttons[i].location[1].x = x;
		Buttons[i].location[0].y = Buttons[i].location[3].y = y;
		Buttons[i].location[2].x = Buttons[i].location[3].x = x + width;
		Buttons[i].location[1].y = Buttons[i].location[2].y = y + height;

		for ( int j = 0; j < 4; j++ )
		{
			Buttons[i].location[j].argb = 0xffffffff;
			Buttons[i].location[j].frgb = 0;
			Buttons[i].location[j].rhw = .5;
			Buttons[i].location[j].u = 0.f;
			Buttons[i].location[j].v = 0.f;
			Buttons[i].location[j].z = 0.f;
		}
		}
			
		
		if ( 0 == Buttons[i].data->textureHandle )
		{
			ZoneScopedN("ControlGui::initButtons loadButtonTexture");
			char file[256];
			strcpy( file, artPath );
			strcat( file, Buttons[i].data->fileName );
			strcat( file, ".tga" );
			S_strlwr( file );
			int ID = mcTextureManager->loadTexture( file, gos_Texture_Alpha, 0, 0, 0x2 );
			Buttons[i].data->textureHandle = ID;
			DWORD logicalWidth = 0;
			DWORD logicalHeight = 0;
			if ( mcTextureManager->tryGetTextureLogicalSize( ID, logicalWidth, logicalHeight ) )
			{
				Buttons[i].data->fileWidth = logicalWidth;
				Buttons[i].data->fileHeight = logicalHeight;
			}
			else
			{
				int gosID = mcTextureManager->get_gosTextureHandle( ID );
				TEXTUREPTR textureData;
				{
					ZoneScopedN("ControlGui::initButtons gos_LockTexture");
					gos_LockTexture( gosID, 0, 0, 	&textureData );
				}
				{
					ZoneScopedN("ControlGui::initButtons gos_UnLockTexture");
					gos_UnLockTexture( gosID );
				}

				Buttons[i].data->fileWidth = textureData.Width / mcTextureManager->getUVScale(ID);
				Buttons[i].data->fileHeight = Buttons[i].data->fileWidth;
			}
		}

		{
			ZoneScopedN("ControlButton::initButtons uvRead");
			if ( NO_ERR != buttonFile.readIdLong( "UNormal", Buttons[i].data->stateCoords[0][0] ) )
				Buttons[i].data->stateCoords[0][0] = -1.f;

			if ( NO_ERR != buttonFile.readIdLong( "VNormal", Buttons[i].data->stateCoords[0][1] ) )
				Buttons[i].data->stateCoords[0][1] = -1.f;


			if ( NO_ERR != buttonFile.readIdLong( "UPressed", Buttons[i].data->stateCoords[1][0] ) )
				Buttons[i].data->stateCoords[1][0] = -1.f;

			if ( NO_ERR != buttonFile.readIdLong( "VPressed", Buttons[i].data->stateCoords[1][1] ) )
				Buttons[i].data->stateCoords[1][1] = -1.f;

			if ( NO_ERR != buttonFile.readIdLong( "UDisabled", Buttons[i].data->stateCoords[2][0] ) )
				Buttons[i].data->stateCoords[2][0] = -1.f;

			if ( NO_ERR != buttonFile.readIdLong( "VDisabled", Buttons[i].data->stateCoords[2][1] ) )
				Buttons[i].data->stateCoords[2][1] = -1.f;

			if ( NO_ERR != buttonFile.readIdLong( "UAmbiguous", Buttons[i].data->stateCoords[3][0] ) )
			{
				if ( NO_ERR != buttonFile.readIdLong( "UHighlight", Buttons[i].data->stateCoords[3][0] ) )
					Buttons[i].data->stateCoords[3][0] = -1.f;
			}

			if ( NO_ERR != buttonFile.readIdLong( "VAmbiguous", Buttons[i].data->stateCoords[3][1] ) )
			{
				if ( NO_ERR != buttonFile.readIdLong( "VHighlight", Buttons[i].data->stateCoords[3][1] ) )
					Buttons[i].data->stateCoords[3][1] = -1.f;
			}

			buttonFile.readIdLong( "UWidth", Buttons[i].data->textureWidth );
			buttonFile.readIdLong( "VHeight", Buttons[i].data->textureHeight );
		}

		{
			ZoneScopedN("ControlButton::initButtons stateSetup");
		Buttons[i].disable( 0 );
		Buttons[i].press( 0 );
		}
	}

}


void ControlGui::initStatics( FitIniFile& file )
{
	ZoneScopedN("ControlGui::initStatics");

	if ( infoWnd )
		infoWnd->setUnit( nullptr );

	if ( staticInfos )
		delete [] staticInfos;
	staticInfos = nullptr;

	if ( objectiveInfos )
		delete [] objectiveInfos;

	objectiveInfos = nullptr;

	if ( videoInfos )
		delete [] videoInfos;

	videoInfos = nullptr;

	if ( timerInfos )
		delete [] timerInfos;

	timerInfos = nullptr;

	if ( missionStatusInfos )
		delete [] missionStatusInfos;

	missionStatusInfos = nullptr;

	staticCount = 0;

	{
		ZoneScopedN("ControlGui::initStatics staticsHeader");
		file.seekBlock( "Statics" );
		file.readIdLong( "staticCount", staticCount );	
	}
	if ( !staticCount )
		return;

	staticInfos = new StaticInfo[staticCount];

	for ( int i = 0; i < staticCount; i++ )
	{
		ZoneScopedN("ControlGui::initStatics static");
		char buffer[32];
		mc2::fmt::to_buf(buffer, "Static{:d}", mc2::fmt::as_i32(i));

		staticInfos[i].init( file, buffer ,hiResOffsetX, hiResOffsetY);
	}

	if ( staticCount > 20 )
		staticInfos[20].showGUIWindow( 0 );

	{
		ZoneScopedN("ControlGui::initStatics helpText");
	if ( NO_ERR != file.seekBlock( "HelpText" ) )
	{
		Assert( 0, 0, "couldn't find the help text block in the layout file" );
	}

	file.readIdLong( "HelpTextLeft", 	HELPAREA_LEFT );
	file.readIdLong( "HelpTextBottom", 	HELPAREA_BOTTOM );
	
	HELPAREA_LEFT		+= hiResOffsetX;
	HELPAREA_BOTTOM		+= hiResOffsetY;
	}

	{
		ZoneScopedN("ControlGui::initStatics resourcePoints");
	if ( NO_ERR != file.seekBlock( "ResourcePoints" ) )
	{
		Assert( 0, 0, "couldn't find the help text block in the layout file" );
	}

	file.readIdLong( "RPTextLeft", 	RPLEFT );
	file.readIdLong( "RPTextTop", 	RPTOP );

	RPLEFT	+= hiResOffsetX; 
	RPTOP 	+= hiResOffsetY; 								 

	//TUTORIAL!!
	rpCallout.rect.left = RPLEFT;
	rpCallout.rect.right = RPLEFT;
	rpCallout.rect.top = RPTOP;
	rpCallout.rect.bottom = RPTOP;
	}

	char blockName[64];
	int i = 0;

	{
		ZoneScopedN("ControlGui::initStatics objectives");
	if ( NO_ERR != file.seekBlock( "Objectives" ) )
	{
		Assert( 0, 0, "couldn't find the objective block in the button layout file" );
	}

	file.readIdLong( "ObjStaticCount", objectiveInfoCount );
	file.readIdLong( "ObjectiveLeft", 	OBJECTIVESLEFT );
	
	file.readIdLong( "ObjectiveSkip", 	OBJECTIVESSKIP );
	file.readIdLong( "SecondObjSkip", 	OBJEECTIVESHEADERSKIP );
	file.readIdLong( "totalRight", 		OBJECTIVESTOTALRIGHT );
	
	file.seekBlock( "PrimaryObjHeader" );
	file.readIdLong( "HeaderLeft", 	OBJECTIVEHEADERLEFT );
	file.readIdLong( "HeaderTop", 	OBJECTIVESTOP );

	objectiveInfos = new StaticInfo[objectiveInfoCount + 3];
	i = 0;
	for (; i < objectiveInfoCount; i++ )
	{
		mc2::fmt::to_buf(blockName, "ObjStatic{:d}", mc2::fmt::as_i32(i));
		objectiveInfos[i].init( file, blockName );
	}

	objectiveInfos[i++].init( file, "PrimaryObjBox" );
	file.readIdLong( "objBoxSkip", OBJECTIVEBOXSKIP );
	objectiveInfos[i++].init( file, "PrimaryObjCheck" );
	file.readIdLong( "objBoxSkip", OBJECTIVECHECKSKIP );
	objectiveInfos[i++].init( file, "PrimaryObjX" );

	objectiveInfoCount = i;
	}

	{
		ZoneScopedN("ControlGui::initStatics video");
	if ( NO_ERR != file.seekBlock( "VideoWindow" ) )
	{
		Assert( 0, 0, "couldn't find th videoWindow block in the button layout file" );
	}
	file.readIdLong( "VideoStaticCount", videoInfoCount );
	file.readIdLong( "left", 	videoRect.left );
	file.readIdLong( "right", 	videoRect.right );
	file.readIdLong( "top", 	videoRect.top );
	file.readIdLong( "bottom", 	videoRect.bottom );
	// NOTE: videoRect is stored in layout-file-native (800x600-era)
	// coordinates. hiResOffsetX/Y are NOT added here because
	// initStatics runs before swapResolutions — at this point both
	// offsets are still 0. The real offset is applied lazily at
	// playMovie() time, when swapResolutions has already finalised
	// hiResOffsetX/Y with the +x_correction/+y_correction adjustment.

	file.seekBlock( "VideoTextBox" );
	file.readIdLong( "left", 	videoTextRect.left );
	file.readIdLong( "right", 	videoTextRect.right );
	file.readIdLong( "top", 	videoTextRect.top );
	file.readIdLong( "bottom", 	videoTextRect.bottom );
	// Same timing issue as videoRect above: apply hiResOffset at use site.


	if ( videoInfoCount )
	{
		videoInfos = new StaticInfo[videoInfoCount];
		for ( i = 0; i < videoInfoCount; i++ )
		{
			mc2::fmt::to_buf(blockName, "VideoStatic{:d}", mc2::fmt::as_i32(i));
			videoInfos[i].init( file, blockName );
		}
	}
	}

	{
		ZoneScopedN("ControlGui::initStatics missionTimer");
		file.seekBlock( "MissionTimer" );
		file.readIdLong( "left", 	timerRect.rect.left );
		file.readIdLong( "top", 	timerRect.rect.top );
		file.readIdLong( "color", timerRect.color );
		file.readIdLong( "TimerStaticCount", timerInfoCount );
		timerRect.rect.left += hiResOffsetX;
		timerRect.rect.right += hiResOffsetX;
		timerRect.rect.top += hiResOffsetY;
		timerRect.rect.bottom += hiResOffsetY;

		if ( timerInfoCount )
		{
			timerInfos = new StaticInfo[timerInfoCount];
			for ( i = 0; i < timerInfoCount; i++ )
			{
				mc2::fmt::to_buf(blockName, "TimerStatic{:d}", mc2::fmt::as_i32(i)); 
				timerInfos[i].init( file, blockName, hiResOffsetX, hiResOffsetY );
			}
		}
	}

	{
		ZoneScopedN("ControlGui::initStatics missionResults");
		file.seekBlock( "MissionResults" );
		file.readIdLong( "ResultsStaticCount", missionStatusInfoCount );
		if ( missionStatusInfoCount )
		{
			missionStatusInfos = new StaticInfo[missionStatusInfoCount ];
			for ( i = 0; i < missionStatusInfoCount; i++ )
			{
				mc2::fmt::to_buf(blockName, "ResultsStatic{:d}", mc2::fmt::as_i32(i));
				missionStatusInfos[i].init( file, blockName );

			}
		}

		file.seekBlock( "ResultsTextBox" );
		file.readIdLong( "left", missionStatusRect.rect.left );	
		file.readIdLong( "right", missionStatusRect.rect.right );	
		file.readIdLong( "top", missionStatusRect.rect.top );	
		file.readIdLong( "bottom", missionStatusRect.rect.bottom );	
		file.readIdLong( "ResultsTextColor", missionStatusRect.color );	
	}

	{
		ZoneScopedN("ControlGui::initStatics chatWidgets");
		file.seekBlock( "ChatPlayerNameRect" );
		long left, top, width, height;
		file.readIdLong( "xlocation", left );
		file.readIdLong( "yLocation", top );
		file.readIdLong( "Width", width );
		file.readIdLong( "Height", height );

		((aObject*)&playerNameEdit)->init( left+2, top, width, height );
		playerNameEdit.setFont( IDS_CHAT_PLAYERNAME_FONT );
		timerFont.init( IDS_OUTLINE_CHAT_FONT );
		int fontHeight = playerNameEdit.getFontObject()->height();
		playerNameEdit.resize( playerNameEdit.width(), fontHeight );
		playerNameEdit.setTextColor( 0xffffffff );

		file.seekBlock( "ChatTextRect" );
		file.readIdLong( "xlocation", left );
		file.readIdLong( "yLocation", top );
		file.readIdLong( "Width", width );
		file.readIdLong( "Height", height );

		((aObject*)&chatEdit)->init( left, top, width, height ); 
		chatEdit.font.init( IDS_OUTLINE_CHAT_FONT );
		chatEdit.setColor( 0xffffffff );
		chatEdit.resize( chatEdit.width(), fontHeight );

		((aObject*)&personalEdit)->init( left, top, width, height );
		personalEdit.setFont( IDS_OUTLINE_CHAT_FONT );
		personalEdit.setTextColor( 0xffffffff );
		personalEdit.resize( personalEdit.width(), fontHeight );

		personalEdit.setColor( 0 );// make tranparent
	}
}

void ControlGui::initRects( FitIniFile& file )
{
	ZoneScopedN("ControlGui::initRects");
	if ( rectInfos )
		delete[] rectInfos;

	rectInfos = nullptr;
	rectCount = 0;
	
	if ( NO_ERR != file.seekBlock( "Rects" ) )
		return;

	{
		ZoneScopedN("ControlGui::initRects header");
		file.readIdLong( "rectCount", rectCount );
	}

	if ( rectCount )
	{
		rectInfos = new RectInfo[rectCount];
		char buffer[32];
		for ( int i = 0; i < rectCount; i++ )
		{
			ZoneScopedN("ControlGui::initRects rect");
			mc2::fmt::to_buf(buffer, "Rect{:d}", mc2::fmt::as_i32(i));
			if ( NO_ERR != file.seekBlock( buffer ) )
			{
				Assert( 0, i, "couldn't find the rect block" );
				return;
			}

			file.readIdLong( "left", 	rectInfos[i].rect.left );
			file.readIdLong( "top", 	rectInfos[i].rect.top );
			file.readIdLong( "right", 	rectInfos[i].rect.right );
			file.readIdLong( "bottom", 	rectInfos[i].rect.bottom );
			file.readIdLong( "color", rectInfos[i].color );
			
			rectInfos[i].rect.left		+= hiResOffsetX;
			rectInfos[i].rect.top		+= hiResOffsetY; 
			rectInfos[i].rect.right     += hiResOffsetX; 
			rectInfos[i].rect.bottom    += hiResOffsetY; 
			
		}

		tacMap.setPos( rectInfos[0].rect );
	}
}

void ControlGui::swapResolutions( int resolutionX, int resolutionY )
{
	FitIniFile buttonFile;
	char path[256];
	strcpy( path, artPath );
	
	char fileName[32];

    int y_correction = 0;
    int x_correction = 0;

	if ( resolutionX == 1920) {
		strcpy( fileName, "buttonlayout1920.fit" );
    } else if ( resolutionX == 1600) {
		strcpy( fileName, "buttonlayout1600.fit" );
    } else if ( resolutionX == 1280) {
		strcpy( fileName, "buttonlayout1280.fit" );
    } else if ( resolutionX == 1024) {
		strcpy( fileName, "buttonlayout1024.fit" );
    } else if ( resolutionX == 800 ) {
		strcpy( fileName, "buttonlayout800.fit" );
    } else if ( resolutionX == 640 ) {
		strcpy( fileName, "buttonlayout640.fit" );
    } else {
		strcpy( fileName, "buttonlayout1920.fit" );
    }

	strcat( path, fileName );
	if ( NO_ERR != buttonFile.open( path ) )
	{
		// Old MC2X campaigns (pre-1920 era) may lack high-res layouts.
		// Walk DOWN the resolution ladder and use the best fit that exists.
		static const int fallbackWidths[] = { 1920, 1680, 1600, 1440, 1366, 1280, 1024, 800, 640 };
		bool opened = false;
		for ( int fi = 0; fi < (int)(sizeof(fallbackWidths)/sizeof(fallbackWidths[0])); fi++ )
		{
			if ( fallbackWidths[fi] >= resolutionX )
				continue; // only try LOWER (or equal-lower) candidates
			char fallbackName[32];
			mc2::fmt::to_buf(fallbackName, "buttonlayout{:d}.fit", mc2::fmt::as_i32(fallbackWidths[fi]));
			char fallbackPath[256];
			strcpy( fallbackPath, artPath );
			strcat( fallbackPath, fallbackName );
			if ( NO_ERR == buttonFile.open( fallbackPath ) )
			{
				MC2_LOG(Core, Debug, "controlgui: buttonlayout{:d}.fit missing, fell back to {}",
				       mc2::fmt::as_i32(resolutionX), mc2::fmt::safe(fallbackName));
				strcpy( path, fallbackPath );
				strcpy( fileName, fallbackName );
				opened = true;
				break;
			}
		}
		if ( !opened )
		{
			char error[256];
			mc2::fmt::to_buf(error, "couldn't find the file {} (no fallback found)", mc2::fmt::safe(path));
			Assert( 0, 0, error );
		}
	}

	//------------------------------------
	//Get the Resolution Centering Values
	long result = buttonFile.seekBlock("HiresOffsets");
	if (result == NO_ERR)
	{
		result = buttonFile.readIdLong("xOffset",hiResOffsetX);
		if (result != NO_ERR)
			hiResOffsetX = 0;
			
		result = buttonFile.readIdLong("yOffset",hiResOffsetY);
		if (result != NO_ERR)
			hiResOffsetY = 0;
	}
	else
	{
		hiResOffsetX = hiResOffsetY = 0;
	}

	if ( resolutionX == 1920) {
        y_correction = resolutionY - (768 + hiResOffsetY);
        x_correction = (resolutionX - (1600 + hiResOffsetX)) / 2; //sebi: need to correct X as well, because buttonlayout1920 is actually same as 1600, do we ned then separate file for 1920?
    } else if ( resolutionX == 1600) {
        y_correction = resolutionY - (768 + hiResOffsetY);
    } else if ( resolutionX == 1280) {
        y_correction = resolutionY - (768 + hiResOffsetY);
    } else if ( resolutionX == 1024) {
        y_correction = resolutionY - (768 + hiResOffsetY);
    } else if ( resolutionX == 800 ) {
        y_correction = resolutionY - (600 + hiResOffsetY);
    } else if ( resolutionX == 640 ) {
        y_correction = resolutionY - (480 + hiResOffsetY);
    } else {
        y_correction = resolutionY - (768 + hiResOffsetY);
        x_correction = (resolutionX - (1600 + hiResOffsetX)) / 2;
    }
	
    // add corrections
	hiResOffsetY += y_correction;
	hiResOffsetX += x_correction;
		
	infoWnd->init( buttonFile );


	if ( NO_ERR != buttonFile.seekBlock( "Fonts" ) )
		Assert( 0, 0, "couldn't find the font block" );


	long fontID;
	buttonFile.readIdLong( "HelpFont", fontID );
	helpFont.init( fontID );

	buttonFile.readIdLong( "ObjectiveFont", fontID );
	guiFont.init( fontID );
	
	buttonFile.readIdLong( "vehicleFont", fontID );
	vehicleFont.init( fontID );

	buttonFile.readIdLong( "MissionResultsFont", fontID );
	missionResultsFont.init( fontID );

	result = buttonFile.seekBlock( "ControlButtons" );
	if ( result != NO_ERR )
	{
		Assert( 0, 0, "Control button block not found" );
	}

	long buttonCount;
	buttonFile.readIdLong( "ButtonCount", buttonCount );
	Assert( buttonCount == 20, buttonCount, "wrong number of control buttons" );

	// clean up old texture memeory
	if ( buttonData )
	{
		for ( int i = 0; i < buttonCount; i++ )
		{
			unsigned long gosID = mcTextureManager->get_gosTextureHandle( buttonData[i].textureHandle );
			mcTextureManager->removeTexture( gosID );
		}
	}

	if ( vehicleData )
	{
		for ( int i = 0; i < LAST_VEHICLE; i++ )
		{
			unsigned long gosID = mcTextureManager->get_gosTextureHandle( vehicleData[i].textureHandle );
			mcTextureManager->removeTexture( gosID );
		}
	}

	if ( !buttons )
		buttons = new ControlButton[buttonCount];
	if ( !buttonData )
		buttonData = new ButtonData[buttonCount];
	
	memset( buttonData, 0, buttonCount * sizeof( ButtonData ) );

	ControlButton::initButtons( buttonFile, buttonCount, buttons, buttonData, "Button" );

	//getButton( REPAIR )->setColor( 0 );
	getButton( SALVAGE )->setColor( 0 );
	
	 result = buttonFile.seekBlock( "VehicleButtons" );
	 if ( result != NO_ERR )
	 {
		 Assert( 0, 0, "No Vehicle Buttons" );
	 }

	 buttonFile.readIdLong( "ButtonCount", buttonCount );
	 Assert( buttonCount == 8, buttonCount, "wrong number of vehicle buttons" );

	 if ( !vehicleButtons )
		vehicleButtons = new ControlButton[buttonCount];
	 if ( !vehicleData )
		vehicleData = new ButtonData[buttonCount];
	 memset( vehicleData, 0, buttonCount * sizeof( ButtonData ) );

	ControlButton::initButtons( buttonFile, buttonCount, vehicleButtons, vehicleData, "Vehicle" );
	
	getButton( TACMAP_TAB )->press( true );

	// slice C site 8 (default tab): (re)build the registry whenever the button
	// set is (re)built — swapResolutions re-enters here. Registration is
	// unconditional (cheap, inert when the gate is off); the registry is only
	// CONSULTED under MC2_HUD_IMGUI_TABS. Active index resets to 0 ("tacmap"),
	// matching the legacy default press above.
	initHudTabRegistry();

	initStatics( buttonFile );
	initRects( buttonFile );

	forceGroupBar.init( buttonFile, &staticInfos[20] );

	staticInfos[20].showGUIWindow( 0 );

	
	pauseWnd->init( buttonFile );
}
