#define MISSIONBRIEFINGSCREEN_CPP
//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"missionbriefingscreen.h"
#include "../GuiRuntime/GuiRuntime.h"
#include "gos_crashbundle.h"
#include"mechbayscreen.h"
#include"logisticsdata.h"
#include"inifile.h"
#include"packet.h"
#include "../resource.h"
#include"objective.h"
#include"multplyr.h"
#include"chatwindow.h"
#include"gamesound.h"


#define MAP_INDEX 32
#define BUTTON_TEXT 34
#define VIDEO_SCREEN 33
#define RP_INDEX 1

#define MN_MSG_PLAY 80
#define MN_MSG_STOP 82
#define MN_MSG_PAUSE 81

namespace {
// Legacy briefing strings embed font-control backslashes and use '/' as a hard line
// break; strip the '\' and turn '/' into newlines (mirrors mechlopedia's cleanLegacyDescription).
std::string cleanBriefingText( const char* text )
{
	std::string out;
	for ( const char* p = text; p && *p; ++p )
	{
		if ( *p == '\\' ) continue;
		out.push_back( *p == '/' ? '\n' : *p );
	}
	return out;
}

// Word-wrap `text` to ~maxChars/line, appending each wrapped line (with the item's
// colour) to the parallel lists the defs GuiList renders (its rows are fixed one-line
// height, so long items must be pre-wrapped). Existing '\n' breaks are preserved.
void appendWrapped( std::vector<std::string>& lines, std::vector<unsigned int>& colors,
	const std::string& text, unsigned int color, size_t maxChars )
{
	size_t start = 0;
	for ( ;; )
	{
		size_t nl = text.find( '\n', start );
		std::string seg = text.substr( start, nl == std::string::npos ? std::string::npos : nl - start );
		if ( seg.empty() )
		{
			lines.push_back( std::string() );
			colors.push_back( color );
		}
		size_t p = 0;
		while ( p < seg.size() )
		{
			size_t take = seg.size() - p;
			if ( take > maxChars )
			{
				take = maxChars;
				size_t sp = seg.rfind( ' ', p + take );
				if ( sp != std::string::npos && sp > p )
					take = sp - p;
			}
			lines.push_back( seg.substr( p, take ) );
			colors.push_back( color );
			p += take;
			while ( p < seg.size() && seg[p] == ' ' ) ++p;
		}
		if ( nl == std::string::npos ) break;
		start = nl + 1;
	}
}
} // namespace


MissionBriefingScreen::MissionBriefingScreen(  )
{
	status = RUNNING;
	LogisticsScreen::helpTextArrayID = 4;

	for ( int i = 0; i < MAX_OBJECTIVES; i++ )
		objectiveButtons[i] = NULL;
}

MissionBriefingScreen::~MissionBriefingScreen()
{
	for ( int i = 0; i < MAX_OBJECTIVES; i++ )
	{
		if (objectiveButtons[i])
		{
			delete objectiveButtons[i];
			objectiveButtons[i] = NULL;
		}
	}

	missionListBox.destroy();  //NO Default destructor for aListBox.  Just call destroy.
}

//-------------------------------------------------------------------------------------------------


void MissionBriefingScreen::init( FitIniFile* file )
{
	LogisticsScreen::init( *file, "Static", "Text", "Rect", "Button" );
	defsHelpTextKey = "game.mcl_mn.text.help_text";
	for ( int i= 0; i < buttonCount; i++ )
		buttons[i].setMessageOnRelease();

	missionListBox.init( rects[1].left(), rects[1].top(),
						rects[1].width(), rects[1].height() );

	missionListBox.setPressFX( -1 );
	missionListBox.setHighlightFX( -1 );
	missionListBox.setDisabledFX( -1 );

	getButton( MN_MSG_PLAY )->showGUIWindow( 0 );
	getButton( MN_MSG_STOP )->showGUIWindow( 0 );
	getButton( MN_MSG_PAUSE )->showGUIWindow( 0 );
	camera.init( statics[VIDEO_SCREEN].left(), statics[VIDEO_SCREEN].top(),
		statics[VIDEO_SCREEN].right(), statics[VIDEO_SCREEN].bottom() );


}

void MissionBriefingScreen::render(int xOffset, int yOffset )
{
	// Match LogisticsScreen::render: while transitioning out (NEXT/BACK) isShowing()
	// goes false and the defs page stops; skip the camera + ImGui-layer markers too so
	// they don't linger on top of the loading screen until the next page loads.
	if ( !isShowing() )
		return;

	// PREVIEW-FBO-FIXED-800x600-1: no defs/ImGui placement rect known for this
	// camera; composite via the same real-resolution ratio used elsewhere
	// (GuiRuntime::GetDisplaySize / Environment.screenWidth,Height), applied to
	// camera.bounds[] (unscaled legacy 800x600 rect from camera.init() above).
	{
		// UI-ASPECT-ANCHOR-1: canvas-aware composite transform (see aObject).
		float sx = 1.f, sy = 1.f, cox = 0.f, coy = 0.f;
		aObject::getCanvasTransform( sx, sy, cox, coy );
		camera.setPreviewOffscreen( true );
		camera.render();
		camera.drawPreviewToPanel(
			camera.bounds[0] * sx + cox, camera.bounds[1] * sy + coy,
			(camera.bounds[2] - camera.bounds[0]) * sx,
			(camera.bounds[3] - camera.bounds[1]) * sy );
	}

	LogisticsScreen::render( xOffset, yOffset );

	// Re-sync objective line colours each frame so the auto-cycling selection (which
	// recolours the hidden listbox items) shows in the defs GuiList.
	syncObjectivesToDefs( false );

	// Draw the objective markers on the defs minimap (ImGui layer, on top of the map
	// image), aligned to the scaled map rect. The legacy gos objectiveButtons are kept
	// only for the cycle/click state + colour; we render the markers here instead of
	// their (0,0-anchored, under-the-defs-map) gos draw.
	// Only overlay the markers when the screen is settled at its home position.
	// During a NEXT/BACK slide the base render scrolls its content by xOffset/yOffset,
	// but these markers are anchored to the defs map at fixed display coords (they can't
	// scroll with it), so drawing them mid-slide snaps them to their final spot and they
	// hang there -- bleeding onto the adjacent screen -- until the animation completes.
	if ( xOffset == 0 && yOffset == 0 )
	{
		float mx, my, mw, mh;
		if ( getDefsElementScreenRect( "game.mcl_mn.image.describes_area_that_mission_map_goes_into", mx, my, mw, mh ) )
		{
			// Marker size proportional to the map (~7% of its width) so it matches the
			// OG at any resolution; resize() only changes the quad extent (keeps the UV).
			const long ms = (long)( mw * 0.07f );
			for ( int i = 0; i < MAX_OBJECTIVES; i++ )
			{
				if ( !objectiveButtons[i] || objMarkerFx[i] < 0.f )
					continue;
				const float px = mx + objMarkerFx[i] * mw;
				const float py = my + objMarkerFy[i] * mh;
				// Position (display space, centred) = also the click hit-box.
				objectiveButtons[i]->moveTo( (long)( px - ms / 2.f ), (long)( py - ms / 2.f ) );
				objectiveButtons[i]->resize( ms, ms );
			}
			if ( !MPlayer && dropZoneFx >= 0.f )
			{
				const float dpx = mx + dropZoneFx * mw;
				const float dpy = my + dropZoneFy * mh;
				dropZoneButton.moveTo( (long)( dpx - ms / 2.f ), (long)( dpy - ms / 2.f ) );
				dropZoneButton.resize( ms, ms );
			}
			// Draw all markers with their REAL gos art (the mcl_mn_4 per-objective /
			// drop-zone icons, tinted by the auto-cycle colour) routed into the ImGui HUD
			// layer on top of the defs map (1:1 bridge — positions are display-space).
			aObject::beginGuiBridge( 1.f, 1.f );
			for ( int i = 0; i < MAX_OBJECTIVES; i++ )
				if ( objectiveButtons[i] && objMarkerFx[i] >= 0.f )
					objectiveButtons[i]->render();
			if ( !MPlayer && dropZoneFx >= 0.f )
				dropZoneButton.render();
			aObject::endGuiBridge();
		}
	}

	// dropZoneButton is drawn with the objective markers (defs-map overlay) above.

	if ( MPlayer && ChatWindow::instance() )
		ChatWindow::instance()->render(xOffset, yOffset);





}

void MissionBriefingScreen::update()
{

	if ( MPlayer || LogisticsData::instance->isSingleMission() )
	{
		getButton( MB_MSG_PREV )->disable( true );

	}
	else
		getButton( MB_MSG_PREV )->disable( false );

	int oldSel = missionListBox.GetSelectedItem();
	missionListBox.update();

	if ( userInput->isLeftClick() )
	{
		bClicked = true;
		for ( int i = 0; i < MAX_OBJECTIVES; i++ )
		{
			if ( objectiveButtons[i] &&
				objectiveButtons[i]->pointInside( userInput->getMouseX(), userInput->getMouseY () ) )
			{
				// find the item that has this objective
				for ( int j = 0; j < missionListBox.GetItemCount(); j++ )
				{
					if ( missionListBox.GetItem( j )->getID() == i )
					{
						missionListBox.SelectItem( j );
						break;
					}
				}
			}

		}
	}

	runTime += frameLength;

	long selItem = missionListBox.GetSelectedItem( );
	int ID = -1;

	if ( selItem != -1 )
		ID = missionListBox.GetItem( selItem )->getID();

	if ( selItem != -1 && oldSel != selItem )
	{
		bClicked = true;


		// set old selections back to white
		for ( int i= 0; i < missionListBox.GetItemCount(); i++ )
		{
			if ( missionListBox.GetItem( i )->getID() != -1 )
			{
				missionListBox.GetItem( i )->setColor( 0xffffffff );
				if ( objectiveButtons[missionListBox.GetItem( i )->getID()] )
					objectiveButtons[missionListBox.GetItem( i )->getID()]->setColor( 0xffffffff );
			}
		}

		if ( ID != -1  )
		{
			if ( objectiveModels[ID].Length() )
			{
				camera.setObject( objectiveModels[ID], modelTypes[ID], modelColors[ID][0],
					modelColors[ID][1], modelColors[ID][2] );

				camera.setScale( modelScales[ID] );
				soundSystem->playDigitalSample( LOG_VIDEOBUTTONS );
				statics[35].showGUIWindow( 0 );
			}
			else
			{
				camera.setObject( NULL, -1 );

				statics[35].showGUIWindow( true );
			}

			missionListBox.GetItem( selItem )->setColor( 0xffff0000 );

			if ( objectiveButtons[ID] )
				objectiveButtons[ID]->setColor( 0xffff0000 );
		}


	}


	if ( !bClicked && runTime > 3.0 ) // every second switch selection until user clicks
	{
		runTime = 0;

		// turn old sel back into white
		if ( selItem != -1 && ID != -1 )
		{
			missionListBox.GetItem( selItem )->setColor( 0xffffffff );
			if ( objectiveButtons[ID] )
				objectiveButtons[ID]->setColor( 0xffffffff );
		}

		selItem++;

		// wrap if necessary
		if ( selItem >= missionListBox.GetItemCount() )
			selItem = 0;

		// find next viable item
		while( true )
		{
			if ( selItem >= missionListBox.GetItemCount() )
			{
				selItem = -1;
				break;
			}
			else if ( missionListBox.GetItem(selItem)->getID() == -1 )
				selItem++;

			else
				break;
		}

		missionListBox.SelectItem( selItem );


		if ( selItem != -1 )
		{
			int ID = missionListBox.GetItem(selItem)->getID();
			if ( ID != -1 )
			{
				missionListBox.GetItem( selItem )->setColor( 0xffff0000 );
				if ( objectiveButtons[ID] )
					objectiveButtons[ID]->setColor( 0xffff0000 );
				camera.setObject( objectiveModels[ID], modelTypes[ID], modelColors[ID][0],
					modelColors[ID][1], modelColors[ID][2]);
				camera.setScale( modelScales[ID] );
				if ( objectiveModels[ID].Length() )
					statics[35].showGUIWindow( 0 );
				else
					statics[35].showGUIWindow( 1 );
			}
			else
			{
				camera.setObject( NULL, -1 );

				statics[35].showGUIWindow( 1 );
			}
		}
	}

	camera.update();

	if ( !MPlayer || !ChatWindow::instance()->pointInside(userInput->getMouseX(), userInput->getMouseY()) )
		LogisticsScreen::update();

	 if ( MPlayer && ChatWindow::instance() )
	 {
		 if ( ChatWindow::instance()->pointInside(userInput->getMouseX(), userInput->getMouseY()) )
			textObjects[helpTextArrayID].setText( "" );

		ChatWindow::instance()->update();
	 }




}

long	MissionBriefingScreen::getMissionTGA( const char* missionName, bool swizzleForImGui )
{
	if ( !missionName )
		return 0;

	// do I need to open the file?  I guess so, if this proves too slow,
	// it could be done when a stage is completed
	FullPathFileName path;
	path.init( missionPath, missionName, ".pak" );

    // CreateFile lock-check removed: it used the raw path and bypassed the mod overlay,
    // causing preview to fail for campaign mod .pak files. PacketFile::open handles open failure.

	// read the tga out of the pak file
	PacketFile file;
	if ( NO_ERR == file.open( path ) ) // in case file has just been created
	{
		if ( file.getNumPackets() > 3 )
		{

			file.seekPacket(3);
			long size = file.getPacketSize( );

			// sebi 2026-04-21: Wolfman MC2X mission .pak files store something
			// other than a TGA in packet 3 (often ~8 bytes of metadata). Guard
			// against too-small packets before treating the buffer as a header
			// — previously the width/height read overran and we invoked
			// textureFromMemory with width=0, crashing at the next get_gosTextureHandle.
			if (size < (long)sizeof(TGAFileHeader))
			{
				{
					char _cbbuf[256];
					snprintf(_cbbuf, sizeof(_cbbuf),
						"[MISSION] skipping thumbnail for '%s': packet3 size=%ld < TGA header",
						missionName, size);
					puts(_cbbuf); fflush(stdout); crashbundle_append(_cbbuf);
				}
				return 0;
			}

			BYTE* mem = new BYTE[size];

			file.readPacket( 3, mem );

			TGAFileHeader* pHeader = (TGAFileHeader*)mem;
			long bmpWidth = pHeader->width;
			long bmpHeight = pHeader->height;

			if (bmpWidth <= 0 || bmpHeight <= 0)
			{
				{
					char _cbbuf[256];
					snprintf(_cbbuf, sizeof(_cbbuf),
						"[MISSION] skipping thumbnail for '%s': invalid dims %ldx%ld",
						missionName, bmpWidth, bmpHeight);
					puts(_cbbuf); fflush(stdout); crashbundle_append(_cbbuf);
				}
				delete[] mem;
				return 0;
			}

			flipTopToBottom( (BYTE*)(pHeader + 1), pHeader->pixel_depth, bmpWidth, bmpHeight );

			// MERGE-CONFLICT-UI-PHASE1: both sides independently added alpha-opaque
			// stamping for the same root cause (baked tacmap water/cement cells carry
			// alpha=0, which the legacy GameOS Solid path ignores but which renders as
			// solid black / transparent black through other consumers).
			//   ours  (engine, gos_terrain/HUD tacmap parity): unconditionally OR's
			//     0xff000000 into every 32-bit pixel (DWORD-wise), independent of
			//     channel order, mirroring makeKindaSolid so the briefing minimap
			//     doesn't render black water like the HUD tacmap used to.
			//   theirs (ImGui bridge): only forces alpha (byte index 3) when
			//     swizzleForImGui is requested, reasoning that pak TGA channel order
			//     already matches ImGui's .rgba sampling and only alpha needs fixing.
			// Keeping ours' unconditional stamp (byte-identical outcome for the
			// alpha channel either way -- forcing 0xff via OR into the top byte vs.
			// forcing byte[3]=0xff are equivalent for a 32bpp DWORD pixel) so the
			// legacy non-ImGui render path (swizzleForImGui=false) also gets the fix,
			// which theirs' version does not cover.
			if ( pHeader->pixel_depth == 32 )
			{
				DWORD* px = (DWORD*)(pHeader + 1);
				const long pxCount = (long)bmpWidth * (long)bmpHeight;
				for ( long p = 0; p < pxCount; ++p )
					px[p] |= 0xff000000;
			}

			// set up the texture
			long tmpMapTextureHandle = mcTextureManager->textureFromMemory( (DWORD*)(pHeader+1), gos_Texture_Solid, 0, bmpWidth );

			delete[] mem;

			return tmpMapTextureHandle;
		}
	}

	return 0;


}

void MissionBriefingScreen::begin()
{
	missionListBox.removeAllItems( true );

	runTime = 0;
	bClicked = 0;

	statics[VIDEO_SCREEN].setColor( 0 );

	memset( objectiveButtons, 0, sizeof ( aObject* ) * MAX_OBJECTIVES );
	for ( int mi = 0; mi < MAX_OBJECTIVES; mi++ )
		objMarkerFx[mi] = objMarkerFy[mi] = -1.f;
	dropZoneFx = dropZoneFy = -1.f;
	// need to set up all pertinent mission info
	EString missionName = LogisticsData::instance->getCurrentMission();


	long tmpMapTextureHandle = getMissionTGA( missionName, true );
	statics[MAP_INDEX].setTexture( tmpMapTextureHandle );
	statics[MAP_INDEX].setUVs( 0, 127, 127, 0 );
	statics[MAP_INDEX].setColor( 0xffffffff );
	// The legacy statics[MAP_INDEX] is not drawn on the defs page; route the minimap
	// into the defs map image. getMissionTGA returns an MC_TextureManager node (not a
	// raw gos handle), so inject it as a texture NODE: drawUiImageElement resolves +
	// GL-uploads it each frame via get_gosTextureHandle (setElementTextureNode also
	// handles the pak TGA's V-flip). A raw gos-override skips the upload -> no GL name.
	setDefsElementTextureNode( "game.mcl_mn.image.describes_area_that_mission_map_goes_into", tmpMapTextureHandle );


	// need to get all the objectives and stuff
	FullPathFileName fitPath;
	fitPath.init( missionPath, missionName, ".fit" );
	FitIniFile fitFile;
	fitFile.open( fitPath );

	// put initial divider in list box
	addItem(IDS_MN_DIVIDER, 0xff005392, -1);

	fitFile.seekBlock( "MissionSettings" );

	long result = fitFile.seekBlock( "MissionSettings" );
	Assert( result == NO_ERR, 0, "Coudln't find the mission settings block in the mission file" );

	bool bRes;
	result = fitFile.readIdBoolean( "MissionNameUseResourceString", bRes );
	Assert( result == NO_ERR, 0, "couldn't find the MissionNameUseResourceString" );
	if ( bRes )
	{
		unsigned long ulRes;
		result = fitFile.readIdULong( "MissionNameResourceStringID", ulRes );
		Assert( result == NO_ERR, 0, "couldn't find the MissionNameResourceStringID" );
		addItem(ulRes, 0xff005392, -1);
	}
	else
	{
		char missionName[256];
		fitFile.readIdString( "MissionName", missionName, 255 );
		addLBItem( missionName, 0xff005392, -1 );

	}

	addItem(IDS_MN_DIVIDER, 0xff005392, -1);
	addItem( IDS_MN_MISSION_OBJECTIVES, 0xff005392, -1 );
	addItem(IDS_MN_DIVIDER, 0xff005392, -1);

	// put in primary objectives
	fitFile.seekBlock( "Team0Objectives" );
	unsigned long objectiveCount;
	fitFile.readIdULong( "NumObjectives", objectiveCount );
	bool bHasSecondary = 0;
	int count = 0;

	fitFile.seekBlock( "Terrain" );
	float terrainExtentX;
	float terrainExtentY;
	fitFile.readIdFloat( "TerrainMinX", terrainExtentX );
	if ( !terrainExtentX )
		terrainExtentX = 120 * 128;
	fitFile.readIdFloat( "TerrainMinY", terrainExtentY );
	if ( !terrainExtentY )
		terrainExtentY = 120 * 128;

	CObjectives Objectives(0/*alignment*/);
	/*Note that ObjectManager is probably NULL as these objectives are read, so it's not
	cool to call any of the Status() functions of this instance of objectives (access violation
	may ensue).*/
	Objectives.Read(&fitFile);

	gosASSERT( Objectives.Count() < MAX_OBJECTIVES );

	int buttonCount = 0;

	for ( int j = 1; j < 3; j++ )
	{
		CObjectives::EIterator it = Objectives.Begin();
		buttonCount = 0;
		for ( int i = 0; i < Objectives.Count(); i++, it++ )
		{
			CObjective *pObjective = (*it);
			if ( (!pObjective->IsHiddenTrigger()) && (pObjective->IsActive()) )
			{

				if ( pObjective->Priority() == j )
				{
					addObjectiveButton( pObjective->MarkerX(), pObjective->MarkerY(), buttonCount,pObjective->Priority(), fabs(terrainExtentX),
									fabs(terrainExtentY), pObjective->DisplayMarker());

					if ( j == 0 )
					{
						bHasSecondary = true;
						if ( i == 0 )
							addItem( IDS_MN_DIVIDER, 0xff005392, -1 );
					}

					addLBItem( (pObjective->LocalizedDescription()).Data(), 0xffffffff, count );

					objectiveModels[count] = (pObjective->ModelName()).Data();
					modelTypes[count] = pObjective->ModelType();

					modelColors[count][0] = pObjective->ModelBaseColor();
					modelColors[count][1] = pObjective->ModelHighlightColor();
					modelColors[count][2] = pObjective->ModelHighlightColor2();
					modelScales[count] = pObjective->ModelScale();

					count++;
					buttonCount++;
				}
			}
		}
	}

	addItem( IDS_MN_DIVIDER, 0xff005392, -1 );

	fitFile.seekBlock( "MissionSettings" );

	char blurb[4096];
	result = fitFile.readIdString("Blurb", blurb, 4095 );

	bool tmpBool = false;
	result = fitFile.readIdBoolean("BlurbUseResourceString", tmpBool);
	if (NO_ERR == result && tmpBool )
	{
		unsigned long tmpInt = 0;
		result = fitFile.readIdULong("BlurbResourceStringID", tmpInt);
		if (NO_ERR == result)
		{
			cLoadString( tmpInt, blurb, 2047 );
		}
	}


	addLBItem( blurb, 0xff005392, -1 );

	// Route the objectives into the defs GuiList, then keep the listbox populated (the
	// auto-cycle highlight logic reads its items) but move it off-screen so the legacy
	// aListBox doesn't render over the defs list.
	syncObjectivesToDefs( true );
	missionListBox.moveTo( -9000, -9000 );

	int RP = LogisticsData::instance->getCBills();
	char text[32];
	sprintf( text, "%ld ", RP );
	textObjects[RP_INDEX].setText( text );
	setDefsElementText( "game.mcl_mn.text.cbills_text", text );

	// need to find a drop zone, because our designers were never convinced to place
	// 'em explicitly, we need to do it for them
	int i = 1;
	while( true )
	{
		char blockName[32];
		sprintf( blockName, "Part%ld", i );
		i++;
		if ( NO_ERR != fitFile.seekBlock( blockName ) )
			break;

		bool bPlayer = 0;
		fitFile.readIdBoolean( "PlayerPart", bPlayer );

		if ( bPlayer )
		{
			float fX;
			float fY;

			fitFile.readIdFloat( "PositionX", fX );
			fitFile.readIdFloat( "PositionY", fY );

			setupDropZone( fX, fY, fabs(terrainExtentX), fabs(terrainExtentY) );

			break;
		}
	}

	statics[BUTTON_TEXT].showGUIWindow( 0 );
	status = RUNNING;

}

void MissionBriefingScreen::setupDropZone( float fX, float fY, float mapWidth, float mapHeight )
{
	dropZoneButton = statics[BUTTON_TEXT];

	float bmpX = statics[MAP_INDEX].width();
	float bmpY = statics[MAP_INDEX].height();

	// in terms of map, this is where the button goes
	float xLoc = fX/mapWidth * bmpX/2.f;
	float yLoc = -fY/mapHeight * bmpY/2.f;

	// offset by the map...
	xLoc += statics[MAP_INDEX].globalX() + bmpX/2.f;
	yLoc += statics[MAP_INDEX].globalY() + bmpY/2.f;

	dropZoneButton.resize( 18, 18 );
	dropZoneButton.setUVs( 100, 100, 118, 118);

	dropZoneButton.moveTo( xLoc, yLoc );
	dropZoneButton.showGUIWindow( true );
	dropZoneFx = ( fX / mapWidth + 1.f ) * 0.5f;
	dropZoneFy = ( -fY / mapHeight + 1.f ) * 0.5f;


}

void MissionBriefingScreen::syncObjectivesToDefs( bool setItems )
{
	std::vector<std::string>  objLines;
	std::vector<unsigned int> objColors;
	for ( long i = 0; i < missionListBox.GetItemCount(); ++i )
	{
		aTextListItem* pItem = (aTextListItem*)missionListBox.GetItem( i );
		if ( !pItem ) continue;
		appendWrapped( objLines, objColors, cleanBriefingText( pItem->getText() ),
			(unsigned int)pItem->getColor(), 88 );
	}
	if ( setItems )
		setDefsListItems( "game.mcl_mn.list.mission_objectives", objLines );
	setDefsListItemColors( "game.mcl_mn.list.mission_objectives", objColors );
}

void MissionBriefingScreen::addObjectiveButton( float fX, float fY, int count, int priority,
											   float mapWidth, float mapHeight, bool display )
{
	float lineOffset = 0;
	if ( priority == 1 )
	{
		count += 12;
	}



	float bmpX = statics[MAP_INDEX].width();
	float bmpY = statics[MAP_INDEX].height();

	// in terms of map, this is where the button goes
	float xLoc = fX/mapWidth * bmpX/2.f;
	float yLoc = -fY/mapHeight * bmpY/2.f;

	// offset by the map...
	xLoc += statics[MAP_INDEX].globalX() + bmpX/2.f;
	yLoc += statics[MAP_INDEX].globalY() + bmpY/2.f;

	aObject* pButtonText = new aObject;
	*pButtonText = statics[BUTTON_TEXT];
	if (display)
		pButtonText->showGUIWindow( true );
	else
		pButtonText->showGUIWindow( false );

	// need to reset the uv's based on count....
	float textWidth = pButtonText->width();
	float textHeight = pButtonText->height();

	lineOffset *= textHeight;

	int itemsPerLine = 128/textWidth;

	int iIndex = count % itemsPerLine;
	int jIndex = count / itemsPerLine;

	pButtonText->setUVs( iIndex * textWidth, jIndex * textHeight + lineOffset,
						 (iIndex+1) * textWidth, (jIndex+1) * textHeight + lineOffset );

	pButtonText->moveTo( xLoc, yLoc );
	pButtonText->setColor( 0xffffffff );
	int i = 0;
	for (; i < MAX_OBJECTIVES; i++ )
	{
		if ( !objectiveButtons[i])
		{
			objectiveButtons[i] = pButtonText;
			// map-fraction (0..1) of this marker, for drawing on the scaled defs map.
			objMarkerFx[i] = ( fX / mapWidth + 1.f ) * 0.5f;
			objMarkerFy[i] = ( -fY / mapHeight + 1.f ) * 0.5f;
			break;
		}
	}

	//If there are too many objectives, don't leave the RAM lying around!
	if (i >= MAX_OBJECTIVES)
	{
		delete pButtonText;
		pButtonText = NULL;
	}
}

/*int MissionBriefingScreen::addLBItem( FitIniFile& file, const char* itemName, unsigned long color, int ID)
{
	char buffer[1024];
	file.readIdString( itemName, buffer, 1023 );
	return addLBItem( buffer, color, ID);
}*/

int MissionBriefingScreen::addLBItem( const char* text, unsigned long color, int ID)
{
	aTextListItem* pEntry = new aTextListItem( IDS_MN_LB_FONT );
	pEntry->setID( ID );
	pEntry->resize( missionListBox.width() - missionListBox.getScrollBarWidth() - 10,
		pEntry->height() );
	pEntry->setText( text );
	pEntry->setColor( color );
	pEntry->sizeToText();
	pEntry->forceToTop(true);
	return missionListBox.AddItem( pEntry );

}

int  MissionBriefingScreen::addItem( int ID, unsigned long color, int LBid)
{

	aTextListItem* pEntry = new aTextListItem( IDS_MN_LB_FONT );
	pEntry->setID( LBid );
	pEntry->resize( missionListBox.width() - missionListBox.getScrollBarWidth() - 10,
		pEntry->height() );
	pEntry->setText( ID );
	pEntry->setColor( color );
	return missionListBox.AddItem( pEntry );

}


void MissionBriefingScreen::end()
{
//	statics[MAP_INDEX].setTexture( (long)0 );
// 	statics[MAP_INDEX].setColor( 0 );

	camera.setMech( NULL );

}

int MissionBriefingScreen::handleMessage( unsigned long msg, unsigned long who )
{
	switch( who )
	{
	case MB_MSG_NEXT:
		status = NEXT;
		break;

	case MB_MSG_PREV:
		status = PREVIOUS;
		break;

	case MN_MSG_PLAY:
		break;
	case MN_MSG_STOP:
		break;

	case MN_MSG_PAUSE:
		break;

	case MB_MSG_MAINMENU:
		status = MAINMENU;
		break;
	}

	return 0;

}




//*************************************************************************************************
// end of file ( MissionBriefingScreen.cpp )
