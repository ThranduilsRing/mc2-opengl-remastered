#define MPLOADMAP_CPP
/*************************************************************************************************\
MPLoadMap.cpp			: Implementation of the MPLoadMap component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"mploadmap.h"
#include"UiDefs.h"
#include"prefs.h"
#include"inifile.h"
#include"userinput.h"
#include "../resource.h"
#include"platform_windows.h"
#include"missionbriefingscreen.h"

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#define CHECK_BUTTON 200

static int connectionType = 0;

static const int FIRST_BUTTON_ID = 1000010;
static const int OK_BUTTON_ID = 1000001;
static const int CANCEL_BUTTON_ID = 1000002;

// Keys for the v2 mcl_mp_loadmap.fit page text elements (map info panel).
static const char* const kMpMaxPlayersKey  = "game.mcl_mp_loadmap.text.maximum_players";
static const char* const kMpMapNameKey     = "game.mcl_mp_loadmap.text.map_name_header";
static const char* const kMpMissionTypeKey = "game.mcl_mp_loadmap.text.mission_type";
static const char* const kMpMapInfoKey      = "game.mcl_mp_loadmap.text.map_info";
static const char* const kMpMapKey          = "game.mcl_mp_loadmap.image.this_static_is_for_the_tac_map";

MPLoadMap::MPLoadMap()
{
	bDone = 0;

	status = RUNNING;
	helpTextArrayID = 6;
	mapListPage = NULL;
}

MPLoadMap::~MPLoadMap()
{
	mapList.destroy();
	delete mapListPage;
}

int MPLoadMap::indexOfButtonWithID(int id)
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

void MPLoadMap::init(FitIniFile* file)
{
	LogisticsScreen::init( *file, "Static", "Text", "Rect", "Button" );

	if ( buttonCount )
	{
		for ( int i = 0; i < buttonCount; i++ )
		{
			buttons[i].setMessageOnRelease();
			if (buttons[i].getID() == 0)
			{
				buttons[i].setID(FIRST_BUTTON_ID + i);
			}
		}
	}

	file->seekBlock( "EnterAnim" );
	enterAnim.init( file, "" );
	file->seekBlock( "LeaveAnim" );
	exitAnim.init( file, "" );



	{
		char path[256];
		strcpy( path, artPath );
		strcat( path, "mcl_mp_loadmap_list0.fit" );
		
		FitIniFile PNfile;
		if ( NO_ERR != PNfile.open( path ) )
		{
			char error[256];
			sprintf( error, "couldn't open file %s", path );
			Assert( 0, 0, error );
			return;
		}
		PNfile.seekBlock("MapList"); /*for some reason aListBox::init(...) doesn't do the seekBlock itself*/
		mapList.init(&PNfile, "MapList");
		templateItem.init( &PNfile, "Text0" );
	
	}

	mapList.setOrange( true );

	// Mirror mapList into the new ImGui defs renderer (game.mcl_mp_loadmap_list0,
	// converted from mcl_mp_loadmap_list0.fit). See syncMapListPage().
	if ( UiDefs::gameOsUiDefsEnabled() )
	{
		const std::string replacementPath = UiDefs::replacementPathForLegacyFit( "mcl_mp_loadmap_list0.fit" );
		if ( !replacementPath.empty() )
		{
			mapListPage = new UiDefs::GameOSPage();
			if ( !mapListPage->load( replacementPath.c_str() ) )
			{
				delete mapListPage;
				mapListPage = NULL;
			}
		}
	}
}

void MPLoadMap::begin()
{
	// fill up the dialog....
	LogisticsDialog::begin();

	seedDialog( 0 );


	bIsSingle = false;


}

void MPLoadMap::beginSingleMission()
{
	// fill up the dialog....
	LogisticsDialog::begin();

	seedDialog( 1 );
	bIsSingle = true;
}

void MPLoadMap::seedDialog( bool bSeedSingle )
{
	mapList.removeAllItems( true );

	// need to add items to the save game list
	char findString[512];
	sprintf(findString,"%s*.fit",missionPath);


	WIN32_FIND_DATA	findResult;
	HANDLE searchHandle = FindFirstFile(findString,&findResult); 
	if ( searchHandle != INVALID_HANDLE_VALUE )
	{
		do
		{
			if ((findResult.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			{
				addFile( findResult.cFileName, bSeedSingle );
			}
		} while (FindNextFile(searchHandle,&findResult) != 0);
	}

	FindClose(searchHandle);

	if ( bSeedSingle )
	{
		seedFromCampaign();

	}
	else 
	{
		seedFromFile( "Multi" );
	}

	setDefsElementTextureNode( kMpMapKey, 0 );
	if ( bSeedSingle )
		mapList.SelectItem( 0);
	else
		mapList.SelectItem( 1 );
	updateMapInfo();
}

void MPLoadMap::addFile( const char* pFileName, bool bSeedSingle )
{

	FitIniFile tmp;
	FullPathFileName path;
	path.init( missionPath, pFileName, ".fit" );
	if ( NO_ERR == tmp.open( path ) )
	{
		if ( NO_ERR == tmp.seekBlock( "MissionSettings" ) )
		{
			unsigned long bSingle;
			long result = tmp.readIdULong( "IsSinglePlayer", bSingle );
			bool bSingleResult = (bSingle != 0);
			if ( (result == NO_ERR) && (bSingleResult == bSeedSingle) )
			{

				char* pExt = (char*)strstr( pFileName, ".fit" );
				if ( !pExt  )
				{
					pExt = (char*)(strstr( pFileName, ".FIT" ) );
				}
				if ( pExt )
					*pExt = '\0';

								
				
				aLocalizedListItem* pEntry = new aLocalizedListItem();
				*pEntry = templateItem;
				pEntry->resize( mapList.width() - mapList.getScrollBarWidth() - 30, pEntry->height());
				pEntry->setHiddenText( pFileName );
				char missionDisplayName[256];
				strcpy(missionDisplayName, "");
				getMapNameFromFile(pFileName, missionDisplayName, 255 );
				if (0 == strcmp("", missionDisplayName))
				{
					strcpy(missionDisplayName, pFileName);
				}
				pEntry->setText( missionDisplayName );
				pEntry->sizeToText();

				if ( !bSingle )
				{
					unsigned long type = 0;
					tmp.readIdULong( "MissionType", type );

					bool bFound = 0;
					// now go looking for the appropriate header
					for ( int i = 0; i < mapList.GetItemCount(); i++ )
					{
						if ( mapList.GetItem( i )->getID()  - IDS_MP_LM_TYPE0 == type )
						{
							pEntry->move( 10, 0 );
							mapList.InsertItem( pEntry, i+1 );
							bFound = true;
						}
					
					}

					if ( !bFound )
					{
						aLocalizedListItem* pHeaderEntry = new aLocalizedListItem();
						*pHeaderEntry = templateItem;
						pHeaderEntry->setText( 	IDS_MP_LM_TYPE0 + type );
						pHeaderEntry->resize( mapList.width() - mapList.getScrollBarWidth() - 30, pHeaderEntry->height());
						pHeaderEntry->sizeToText();
						pHeaderEntry->setID( IDS_MP_LM_TYPE0 + type );
						pHeaderEntry->setState( aListItem::DISABLED );
						mapList.AddItem( pHeaderEntry );
						pEntry->move( 10, 0 );
						mapList.AddItem( pEntry );

					}
				}
				else
					mapList.AddItem( pEntry );
			}
		}
	}
}

void MPLoadMap::seedFromFile( const char* pFileName )
{
	FullPathFileName path;
	path.init( missionPath, pFileName, ".csv" );

	CSVFile file;
	if ( NO_ERR != file.open( path ) )
	{
		Assert( 0, 0, "couldn't open multiplayer mission .csv file" );
		return;
	}

	int i = 1; 
	char fileName[255];
	while( true )
	{
		if ( NO_ERR != file.readString( i, 1, fileName, 255 ) )
			break;

		path.init( missionPath, fileName, ".fit" );
		if ( fileExists( path ) )
		{		
			addFile( fileName, false );
		}

		i++;
	}
}

void MPLoadMap::seedFromCampaign()
{
		char searchStr[255];
		cLoadString( IDS_AUTOSAVE_NAME, searchStr, 255 );
		EString finalStr;
		finalStr =  "*.fit";
		FullPathFileName findPath;
		findPath.init( savePath, finalStr, ".fit" );

		EString newestFile;
		long	groupCount = -1;
		long	missionCount = -1;
		FitIniFile tmpFile;

		WIN32_FIND_DATA	findResult;
		HANDLE searchHandle = FindFirstFile(findPath,&findResult); 
		 if ( searchHandle != INVALID_HANDLE_VALUE )
		 {
			do
			{
				if ((findResult.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				{
					FullPathFileName path;
					path.init( savePath, findResult.cFileName, ".fit" );
 					tmpFile.open( path  );

					 if ( NO_ERR == tmpFile.seekBlock( "General" ) )
					 {
						 long group, missions;

						tmpFile.readIdLong( "Group ", group );
						if ( group > groupCount )
						{
							groupCount = group;
							tmpFile.readIdLong( "CompletedMissions", missions );
							missionCount = missions;
							newestFile = findResult.cFileName;
						}
						else if ( group == groupCount )
						{
							tmpFile.readIdLong( "CompletedMissions", missions );
							if ( missions > missionCount )
								missionCount = missions;
							newestFile = findResult.cFileName;
						}
					 }

					tmpFile.close();


				}
			} while (FindNextFile(searchHandle,&findResult) != 0);
		 }

		 FindClose(searchHandle);

		 if ( newestFile.Length() )
		 {
			 findPath.init( savePath, newestFile, ".fit" );
			 FitIniFile file;
			 long group;
			 long missions;
			 char campaignFileName[256];
			 campaignFileName[0] = 0;
			 if ( NO_ERR == file.open( findPath ) )
			 {
				 if ( NO_ERR == file.seekBlock( "General" ) )
				 {
					file.readIdLong( "Group ", group );
					file.readIdLong( "CompletedMissions", missions );
					file.readIdString( "CampaignFile", campaignFileName, 255 );
				 }
			 }

			 if ( strlen( campaignFileName ) && ( group || missions ) )
			 {
				FitIniFile campaignFile;
				if ( NO_ERR == campaignFile.open( campaignFileName ) )
				{
					for ( int i = 0; i < group+1; i++ )
					{
						char blockName[64];
						sprintf( blockName,  "Group%ld", i );
						if ( NO_ERR == campaignFile.seekBlock( blockName ) )
						{
							long count = missions;
							if ( i < group )
							{
								campaignFile.readIdLong( "MissionCount", count );
							}

							for ( int j = 0; j < count; j++ )
							{
								sprintf( blockName, "Group%ldMission%ld", i, j );
								if ( NO_ERR == campaignFile.seekBlock( blockName ) )
								{
									char tmpFileName[255];
									campaignFile.readIdString( "FileName", tmpFileName, 255 );

									aLocalizedListItem* pEntry = new aLocalizedListItem();
									*pEntry = templateItem;
									pEntry->resize( mapList.width() - mapList.getScrollBarWidth() - 20, pEntry->height());
									pEntry->setHiddenText( tmpFileName );
									char displayName[256];
									getMapNameFromFile( tmpFileName, displayName, 255 );
									pEntry->setText( displayName );
									pEntry->sizeToText();
									mapList.AddItem( pEntry );
								}
							}
						}
					}
				}
			 }
		 }
}


void MPLoadMap::end()
{
	LogisticsDialog::end();
	setDefsElementTextureNode( kMpMapKey, 0 );
	setDefsElementTextureNode( kMpMapKey, 0 );
}

void MPLoadMap::render(int, int )
{
	float color = 0x7f000000;

	if ( enterAnim.isAnimating() && !enterAnim.isDone() )
	{
		float time = enterAnim.getCurrentTime();
		float endTime = enterAnim.getMaxTime();
		if ( endTime )
		{
			color = interpolateColor( 0x00000000, 0x7f000000, time/endTime );
			
		}
	}

	else if ( exitAnim.isAnimating() && !exitAnim.isDone() )
	{
		float time = exitAnim.getCurrentTime();
		float endTime = exitAnim.getMaxTime();
		if ( endTime )
		{
			color = interpolateColor( 0x7f000000, 0x00000000, time/endTime );
			
			
		}
	}

	
	GUI_RECT rect = { 0, 0, Environment.screenWidth, Environment.screenHeight };
	drawRect( rect, color );

	// The ImGui mapListPage (rendered below) owns the visible list when loaded;
	// the legacy aListBox stays only for input/selection, so don't draw it here
	// or its offset selection box shows under the ImGui list.
	if ((!enterAnim.isAnimating() || enterAnim.isDone() ) && !exitAnim.isAnimating()
		&& !(mapListPage && mapListPage->isLoaded()) )
	{
		mapList.render();
	}
	
	float xOffset = 0;
	float yOffset = 0 ;
	if ( enterAnim.isAnimating() )
	{
		xOffset = enterAnim.getXDelta();
		yOffset = enterAnim.getYDelta();
	}
	else if ( exitAnim.isAnimating() )
	{
		xOffset = exitAnim.getXDelta();
		yOffset = exitAnim.getYDelta();
	}

	LogisticsScreen::render( (int)xOffset, (int)yOffset );

	if ( mapListPage && mapListPage->isLoaded() )
	{
		syncMapListPage();
		mapListPage->render( globalX() + (int)xOffset, globalY() + (int)yOffset );
	}

}

void MPLoadMap::render()
{
	render(0, 0);
}

int	MPLoadMap::handleMessage( unsigned long message, unsigned long who)
{

	status = who;
	end();
	exitAnim.begin();
	enterAnim.end();

	if ( status == YES )
	{
	}

	if ( RUNNING == status )
	{
		switch ( who )
		{
		case FIRST_BUTTON_ID+2:
			{
				getButton( FIRST_BUTTON_ID+2 )->press( 0 );
				connectionType = 0;
				buttons[indexOfButtonWithID(FIRST_BUTTON_ID+2)].press(!((1 == connectionType) || (2 == connectionType) || (3 == connectionType)));
				buttons[indexOfButtonWithID(FIRST_BUTTON_ID+3)].press(1 == connectionType);
				buttons[indexOfButtonWithID(FIRST_BUTTON_ID+4)].press(2 == connectionType);
				buttons[indexOfButtonWithID(FIRST_BUTTON_ID+5)].press(3 == connectionType);
				return 1;
			}
			break;
		}
	}

	return 0;

}

bool MPLoadMap::isDone()
{
	return bDone;
}

void MPLoadMap::update()
{
	LogisticsDialog::update();
	int oldSel = mapList.GetSelectedItem();
	if ( mapListPage && mapListPage->isLoaded() )
	{
		// Clicks land on the visible ImGui list, so drive selection from it and
		// mirror into the legacy list (still the source of truth for map data).
		// The legacy list must NOT also hit-test, or it selects a different row
		// (its geometry differs from the displayed ImGui list).
		mapListPage->update( this, globalX(), globalY() );
		const int sel = mapListPage->getListSelection( "game.mcl_mp_loadmap_list0.list.map" );
		if ( sel >= 0 && sel < mapList.GetItemCount() )
			mapList.SelectItem( sel );
	}
	else
	{
		mapList.update();
	}
	int newSel = mapList.GetSelectedItem();
	if ( oldSel != newSel )
		updateMapInfo();

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

void MPLoadMap::updateMapInfo()
{
	int sel = mapList.GetSelectedItem();
	if ( sel != -1 )
	{

		FitIniFile file;
		FullPathFileName path;
		const char* fileName = ((aTextListItem*)mapList.GetItem( sel ))->getText();
		selMapName = ((aLocalizedListItem*)mapList.GetItem(sel))->getHiddenText();
		path.init( missionPath, selMapName, ".fit" );

		if ( NO_ERR == file.open( path ) )
		{
			
			char missionName[256];
			missionName[0] = 0;
			bool bRes = 0;

			char text[1024];
			char text2[1024];

			file.seekBlock( "MissionSettings" );
			file.readIdBoolean( "MissionNameUseResourceString", bRes );
			if ( bRes )
			{
				unsigned long lRes;
				file.readIdULong( "MissionNameResourceStringID", lRes );
				cLoadString( lRes, missionName, 255 );
			}
			else
			{
				file.readIdString( "MissionName", missionName, 255 );
			}

			long textureHandle = MissionBriefingScreen::getMissionTGA( selMapName, true );
			setDefsElementTextureNode( kMpMapKey, textureHandle );
 
			cLoadString( IDS_MP_LM_MAP_LIST_MAP_NAME, text, 255 );
			sprintf( text2, text, missionName );
			setDefsElementText( kMpMapNameKey, text2 );

			if ( !bIsSingle )
			{
				unsigned long type = 0;
				file.readIdULong( "MissionType", type );
				cLoadString( IDS_MP_LM_MAP_LIST_TYPE, text, 255 );
				char mType[128];
				cLoadString( IDS_MP_LM_TYPE0 + type, mType, 127 );
				
				sprintf( text2, text, mType );
				setDefsElementText( kMpMissionTypeKey, text2 );
			
	
				unsigned long numPlayers = 0;
				file.readIdULong( "MaximumNumberOfPlayers", numPlayers );

				cLoadString( IDS_MP_LM_MAP_LIST_MAX_PLAYERS, text, 255 );
				sprintf( text2, text, numPlayers );
				setDefsElementText( kMpMaxPlayersKey, text2 );
			}
			else
			{
				textObjects[4].setText( "" );
				textObjects[2].setText( "" );
			}

			char blurb[1024];
			blurb[0] = 0;
			long result = file.readIdString("Blurb2", blurb, 1023 );

			bool tmpBool = false;
			result = file.readIdBoolean("Blurb2UseResourceString", tmpBool);
			if (NO_ERR == result && tmpBool )
			{
				unsigned long tmpInt = 0;
				result = file.readIdULong("Blurb2ResourceStringID", tmpInt);
				if (NO_ERR == result)
				{
					cLoadString( tmpInt, blurb, 1024 );
				}
			}

			setDefsElementText( kMpMapInfoKey, blurb );

  

		}

	}
	else
	{
		textObjects[4].setText( "" );
		textObjects[3].setText( "" );
		textObjects[2].setText( "" );
		textObjects[5].setText( "" );
		setDefsElementTextureNode( kMpMapKey, 0 );


	}
}

// Mirrors mapList's current items and selection into the new ImGui defs
// renderer's List element (game.mcl_mp_loadmap_list0.list.map). mapList
// items are always aLocalizedListItem (an aTextListItem), including the
// disabled "type" header rows -- those get mirrored too, just as plain
// rows for now; mapList remains the source of truth for what is
// selectable, this is display-only.
void MPLoadMap::syncMapListPage()
{
	if ( !mapListPage || !mapListPage->isLoaded() )
		return;

	std::vector<std::string> items;
	const long count = mapList.GetItemCount();
	items.reserve( count > 0 ? count : 0 );
	for ( long i = 0; i < count; i++ )
	{
		aListItem* item = mapList.GetItem( i );
		const aTextListItem* textItem = static_cast<const aTextListItem*>( item );
		const char* text = textItem ? textItem->getText() : NULL;
		items.push_back( text ? text : "" );
	}

	mapListPage->setListItems( "game.mcl_mp_loadmap_list0.list.map", items );
	mapListPage->setListSelection( "game.mcl_mp_loadmap_list0.list.map", (int)mapList.GetSelectedItem() );
}

void MPLoadMap::getMapNameFromFile( const char* pFileName, char* missionName, long bufferLength )
{
	FullPathFileName path;
	path.init( missionPath, pFileName, ".fit" );

	FitIniFile file;

	if ( NO_ERR != file.open( (const char*)path ) )
	{
		char errorStr[256];
		sprintf( errorStr, "couldn't open file %s", (const char*)path );
		Assert( 0, 0, errorStr );
	}

	
	long result = file.seekBlock( "MissionSettings" );
	Assert( result == NO_ERR, 0, "Coudln't find the mission settings block in the mission file" );

	missionName[0] = 0;
	bool bRes = 0;

	result = file.readIdBoolean( "MissionNameUseResourceString", bRes );
	//Assert( result == NO_ERR, 0, "couldn't find the MissionNameUseResourceString" );
	if ( bRes )
	{
		unsigned long lRes;
		result = file.readIdULong( "MissionNameResourceStringID", lRes );
		Assert( result == NO_ERR, 0, "couldn't find the MissionNameResourceStringID" );
		cLoadString( lRes, missionName, bufferLength );
	}
	else
	{
		result = file.readIdString( "MissionName", missionName, bufferLength );
		Assert( result == NO_ERR, 0, "couldn't find the missionName" );
	}
}




//////////////////////////////////////////////



//*************************************************************************************************
// end of file ( MPLoadMap.cpp )
