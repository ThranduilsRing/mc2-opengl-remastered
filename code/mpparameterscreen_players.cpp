/* aPlayerParams, aStyle2TextListItem and CFocusManager, split from mpparameterscreen.cpp (no behaviour change). */
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

aPlayerParams::aPlayerParams()
{
	statics = nullptr;
	rects = nullptr;
	textObjects = nullptr;
	staticCount = rectCount = textCount = 0;
	bHasFocus = 0;
}

aPlayerParams::~aPlayerParams()
{
	destroy();
}

aPlayerParams& aPlayerParams::operator=( const aPlayerParams& src )
{
	aObject::operator=( src );

	addChild(&ReadyButton);
	addChild( &CBillsSpinnerDownButton );
	addChild( &CBillsSpinnerUpButton );
	addChild( &edit );

	ReadyButton = src.ReadyButton;
	CBillsSpinnerDownButton = src.CBillsSpinnerDownButton;
	CBillsSpinnerUpButton = src.CBillsSpinnerUpButton;
	edit = src.edit;
	insigniaName = src.insigniaName;
	templateItem = src.templateItem;

	staticCount = src.staticCount;

	if ( staticCount )
	{
		statics = std::make_unique_for_overwrite<aObject[]>(staticCount);

		for ( int i = 0; i < staticCount; i++ )
		{
			addChild( &statics[i] );
			statics[i] = src.statics[i];
		}
	}

	rectCount = src.rectCount;
	if ( rectCount )
	{
		rects = std::make_unique_for_overwrite<aRect[]>(rectCount);

		for ( int i = 0; i < rectCount; i++ )
		{
			addChild( &rects[i] );
			rects[i] = src.rects[i];
		}
	}

	textCount = src.textCount;
	if ( textCount )
	{
		textObjects = std::make_unique_for_overwrite<aText[]>(textCount);

		for ( int i = 0; i < textCount; i++ )
		{
			addChild( &textObjects[i] );
			textObjects[i] = src.textObjects[i];
		}
	}

	addChild(&teamNumberDropList);
	teamNumberDropList = src.teamNumberDropList;

	addChild( &factionDropList );
	factionDropList = src.factionDropList;


	return *this;
}

long aPlayerParams::init(long xPos, long yPos,long w, long h )
{
	long err;
	
	err = aObject::init(xPos,yPos,w,h);
	if (err)
		return err;

	if ( !MPlayer->isHost() )
	{
		CBillsSpinnerDownButton.showGUIWindow( 0 );
		CBillsSpinnerUpButton.showGUIWindow( 0 );
	}

	
	return (NO_ERR);
}

void aPlayerParams::init( FitIniFile* pFile, mc2::czstring blockNameParam )
{

	FitIniFile &file = (*pFile);
	pFile->seekBlock(blockNameParam);

	ReadyButton.init( file, "PlayerParamsReadyButton" );
	CBillsSpinnerDownButton.init( file, "PlayerParamsCBillsSpinnerDownButton" );
	CBillsSpinnerUpButton.init( file, "PlayerParamsCBillsSpinnerUpButton" );
	addChild(&ReadyButton);
	addChild( &CBillsSpinnerDownButton );
	addChild( &CBillsSpinnerUpButton );

	edit.init( pFile, "playerparamEdit0" );
	edit.setNumeric( true );
	edit.limitEntry( 3 );
	edit.allowIME( 0 );
	addChild( &edit );


	CBillsSpinnerDownButton.setPressFX( LOG_VIDEOBUTTONS );
	CBillsSpinnerUpButton.setPressFX( LOG_VIDEOBUTTONS );
	ReadyButton.setPressFX( LOG_VIDEOBUTTONS );

	CBillsSpinnerDownButton.setHighlightFX( LOG_DIGITALHIGHLIGHT );
	CBillsSpinnerUpButton.setHighlightFX( LOG_DIGITALHIGHLIGHT );
	ReadyButton.setHighlightFX( LOG_DIGITALHIGHLIGHT );

	CBillsSpinnerDownButton.setDisabledFX( LOG_WRONGBUTTON );
	CBillsSpinnerUpButton.setDisabledFX( LOG_WRONGBUTTON );
	ReadyButton.setDisabledFX( LOG_WRONGBUTTON );


	const char* staticName = "PlayerParamsStatic";
	const char* textName = "PlayerParamsText";
	const char* rectName = "PlayerParamsRect";

	char blockName[256];

	// init statics
	if ( staticName )
	{
		mc2::fmt::to_buf( blockName, "{}{:c}", mc2::fmt::safe(staticName), mc2::fmt::as_char('s') );
		if ( NO_ERR == file.seekBlock( blockName ) )
		{
			file.readIdLong( "staticCount", staticCount );

			if ( staticCount )
			{
				statics = std::make_unique_for_overwrite<aObject[]>(staticCount);

				char blockName[128];
				for ( int i = 0; i < staticCount; i++ )
				{
					mc2::fmt::to_buf( blockName, "{}{:d}", mc2::fmt::safe(staticName), mc2::fmt::as_i32(i) );
					statics[i].init( &file, blockName );			
					addChild(&(statics[i]));
				}
				
			}
		}
	}

	if ( rectName )
	{
		// init rects
		mc2::fmt::to_buf( blockName, "{}{:c}", mc2::fmt::safe(rectName), mc2::fmt::as_char('s') );
		if ( NO_ERR == file.seekBlock( blockName ) )
		{
			file.readIdLong( "rectCount", rectCount );
			if ( rectCount )
			{
				rects = std::make_unique_for_overwrite<aRect[]>(rectCount);

				char blockName[128];
				for ( int i = 0; i < rectCount; i++ )
				{
					mc2::fmt::to_buf( blockName, "{}{:d}", mc2::fmt::safe(rectName), mc2::fmt::as_i32(i) );
					rects[i].init( &file, blockName );
					addChild(&(rects[i]));
				}
			}
		}
	}

	// init texts
	if ( textName )
	{
		mc2::fmt::to_buf( blockName, "{}{:c}", mc2::fmt::safe(textName), mc2::fmt::as_char('s') );
		if ( NO_ERR == file.seekBlock( blockName ) )
		{
			if ( NO_ERR != file.readIdLong( "TextEntryCount", textCount ) )
				file.readIdLong( "TextCount", textCount );

			if ( textCount )
			{
				textObjects = std::make_unique_for_overwrite<aText[]>(textCount);
				char blockName[64];
				for ( int i = 0; i < textCount; i++ )
				{
					mc2::fmt::to_buf( blockName, "{}{:d}", mc2::fmt::safe(textName), mc2::fmt::as_i32(i) );
					textObjects[i].init( &file, blockName );
					addChild(&(textObjects[i]));
				}
				
			}
		}
	}

	
	

	{
		char path[256];
		strcpy( path, artPath );
		strcat( path, "mcl_mp_param_droplist3.fit" );
		
		FitIniFile PNfile;
		if ( NO_ERR != PNfile.open( path ) )
		{
			char error[256];
			mc2::fmt::to_buf( error, "couldn't open file {}", mc2::fmt::safe(path) );
			Assert( 0, 0, error );
			return;
		}
		teamNumberDropList.init(&PNfile, "TeamNumberDropList");
		teamNumberDropList.ListBox().setOrange( true );

		PNfile.seekBlock( "Text0" );
		templateItem.init(&PNfile, "Text0" );

	
	}
	addChild(&teamNumberDropList);

	{
		char path[256];
		strcpy( path, artPath );
		strcat( path, "mcl_mp_param_droplist4.fit" );
		
		FitIniFile PNfile;
		if ( NO_ERR != PNfile.open( path ) )
		{
			char error[256];
			mc2::fmt::to_buf( error, "couldn't open file {}", mc2::fmt::safe(path) );
			Assert( 0, 0, error );
			return;
		}
		factionDropList.init(&PNfile, "FactionTypeDropList");
		factionDropList.ListBox().setOrange( true );

		int i;
		for (i = 0; i < 4; i += 1)
		{
			auto pTmp2 = std::make_unique<aStyle2TextListItem>();
			*pTmp2 = templateItem;
			pTmp2->setText( IDS_FACTION0 + i );
			pTmp2->resize( factionDropList.width() - 
				factionDropList.ListBox().getScrollBarWidth() - 8,
				pTmp2->height() );
			pTmp2->sizeToText();
			factionDropList.AddItem(pTmp2.release());
		}
		factionDropList.SelectItem(0);
	}
	addChild(&factionDropList);

	// resize after we include drop lists
	if (true)
	{
		float x = 1000000.0;
		float y = 1000000.0;
		float w = 0.0;
		float h = 0.0;
		int i;
		for (i = 0; i < numberOfChildren(); i += 1)
		{
			aObject *pChild = child(i);
			if (x > pChild->globalX())
			{
				x = pChild->globalX();
			}
			if (y > pChild->globalY())
			{
				y = pChild->globalY();
			}
			if (w < pChild->globalRight() )
			{
				w = (pChild->globalRight());
			}
			if (h < pChild->globalBottom() - pChild->globalTop() )
			{
				h = (pChild->globalBottom() - pChild->globalTop());
			}
		}

		resize( w, h );
		moveToNoRecurse( x, y );
	}





}

bool aPlayerParams::hasFocus( )
{
	if ( edit.hasFocus() )
		return true;

	if ( factionDropList.ListBox().isShowing() )
		return true;

	if ( teamNumberDropList.ListBox().isShowing() )
		return true;

	return false;
}

void aPlayerParams::destroy()
{
	removeAllChildren();

	statics.reset();
	rects.reset();
	textObjects.reset();

	aObject::destroy();
}

void aPlayerParams::update()
{

	if ( !MPlayer->isHost() )
	{
		CBillsSpinnerDownButton.showGUIWindow( 0 );
		CBillsSpinnerUpButton.showGUIWindow( 0 );
	}
	if ( userInput->isLeftClick() )
	{
		if ( pointInside( userInput->getMouseX(), userInput->getMouseY() ) 
			|| factionDropList.pointInside( userInput->getMouseX(), userInput->getMouseY()) 
			|| teamNumberDropList.pointInside( userInput->getMouseX(), userInput->getMouseY()))
			bHasFocus = true;
		else
			bHasFocus = false;
	}

	if ( MPlayer->missionSettings.allTech )
	{
		if ( factionDropList.ListBox().GetItemCount() != 1 )
		{
			factionDropList.SelectItem( -1 );
			factionDropList.ListBox().removeAllItems( true );
			auto pTmp2 = std::make_unique<aStyle2TextListItem>();
			*pTmp2 = templateItem;
			pTmp2->setText( IDS_FACTION0 + 5 );
			pTmp2->resize( factionDropList.width() - 
				factionDropList.ListBox().getScrollBarWidth() - 8,
				pTmp2->height() );
			pTmp2->sizeToText();
			factionDropList.AddItem(pTmp2.release());
			factionDropList.SelectItem( 0 );
		}
	}
	else if ( factionDropList.ListBox().GetItemCount() < 4 )
	{
		factionDropList.SelectItem( -1 );
		factionDropList.ListBox().removeAllItems( true );

		for ( int i = 0; i < 4; i += 1)
		{
			auto pTmp2 = std::make_unique<aStyle2TextListItem>();
			*pTmp2 = templateItem;
			pTmp2->setText( IDS_FACTION0 + i );
			pTmp2->resize( factionDropList.width() - 
				factionDropList.ListBox().getScrollBarWidth() - 8,
				pTmp2->height() );
			pTmp2->sizeToText();
			factionDropList.AddItem(pTmp2.release());
			factionDropList.SelectItem( 0 );

		}
	}

	// don't accept any kind of input for anything but your own stuff....
	if ( commanderID != MPlayer->commanderID && !MPlayer->isHost() )
	{
		CBillsSpinnerUpButton.showGUIWindow( 0 );
		CBillsSpinnerDownButton.showGUIWindow( 0 );
		teamNumberDropList.disable( true );
		factionDropList.disable( true );
		rects[2].showGUIWindow( 0 );
		rects[7].showGUIWindow( 0 );
		rects[8].showGUIWindow( 0 );
		rects[1].showGUIWindow( 0 );

		return;
	}
	else
	{
		if ( MPlayer->isHost() )
		{
			CBillsSpinnerUpButton.showGUIWindow( 1 );
			CBillsSpinnerDownButton.showGUIWindow( 1 );
		}
		else
		{
			CBillsSpinnerUpButton.showGUIWindow( 0 );
			CBillsSpinnerDownButton.showGUIWindow( 0 );
		}
		teamNumberDropList.disable( 0 );
		factionDropList.disable( 0 );
		rects[2].showGUIWindow( 1 );
		rects[7].showGUIWindow( 1 );
		rects[8].showGUIWindow( 1 );
		rects[1].showGUIWindow( 1 );


	}

	if ( commanderID == MPlayer->commanderID )
	{

		if ( !MPlayer->isHost() )
		{
			FullPathFileName path;
			path.init( missionPath, MPlayer->missionSettings.map, ".fit" );
			if ( !fileExists( path ) ||
				MPParameterScreen::getGUIDFromFile( MPlayer->missionSettings.map ) != MPlayer->missionSettings.mapGuid )  // disable the ready button if the map isn't around...
			{
				ReadyButton.disable( false );		
				ReadyButton.press( 0 );
				ReadyButton.disable( true );		
			}
			else
				ReadyButton.disable( false );
		}
	}

	if ( !bHasFocus )
		return;



	bool bOldReady = ReadyButton.isPressed();

	if ( bOldReady ) // need to check the ready button if nothing else
	{
		ReadyButton.update();
	}



		// need to check for changes
		int oldSel = teamNumberDropList.GetSelectedItem();
		int oldFaction = factionDropList.GetSelectedItem();
		const char* pText = textObjects[1].text.c_str();
		
		long oldCBills = 0;
		
		if ( pText )
		{
			oldCBills = atoi( pText ) * 1000;
		}
		

	bool bCBillsChanged = 0;
	if ( !bOldReady || ( MPlayer->isHost() ) ) // don't do anything if all ready. 
	{

		if ( (commanderID == MPlayer->commanderID || MPlayer->isHost() ) )
		{
			edit.setReadOnly( 0 );
			bool bHasFocus = edit.hasFocus();
			edit.update();
			if ( bHasFocus && !edit.hasFocus() )
				bCBillsChanged = true;

			if ( commanderID == MPlayer->commanderID )
				ReadyButton.update();
			CBillsSpinnerDownButton.update();
			CBillsSpinnerUpButton.update( );
			factionDropList.update();
			teamNumberDropList.update();		
		}
	}
	else
		edit.setReadOnly( 1 );

		int newSel = teamNumberDropList.GetSelectedItem();
		int newFaction = factionDropList.GetSelectedItem();
		bool bNewReady = ReadyButton.isPressed();
		std::string cBillsText = edit.entry();
		long newCBills = 0;
		if ( !cBillsText.empty() )
		{
			newCBills = atoi( cBillsText.c_str() ) * 1000;
		}

		if (  bCBillsChanged )
		{
		 	MC2Player* pInfo = MPlayer->getPlayerInfo( commanderID );
			pInfo->cBills = newCBills;
			MPlayer->sendPlayerUpdate( nullptr, 5, commanderID );
				
			char text[256];
			mc2::fmt::to_buf(text, "{:d}", mc2::fmt::as_i32(pInfo->cBills/5000 * (5)) ); // need to round to nearest 5000
			edit.setEntry( text );
			MPParameterScreen::resetCheckBoxes();
		}

		else if ( (oldSel != newSel) || (oldFaction != newFaction) || 
			(bOldReady != bNewReady)  )
		{
			// send out new info....
			if ( MPlayer )
			{
			
				MC2Player* pInfo = MPlayer->getPlayerInfo( commanderID );
				pInfo->cBills = newCBills;
				if ( MPlayer->isHost() )
					pInfo->ready = 0;
				else
					pInfo->ready = bNewReady;
				pInfo->faction = newFaction;
				if (MPlayer->isHost())
				{
					if (pInfo->team != newSel)
						MPlayer->setPlayerTeam(commanderID, newSel);
				}
				else {
					pInfo->team = newSel;
					MPlayer->sendPlayerUpdate( nullptr, 5, commanderID );
				}

				
			}

			bHasFocus = 0;



		}

		if ( userInput->getKeyDown( KEY_RETURN ) )
			bHasFocus = false;



}

void	aPlayerParams::disableReadyButton()
{
	ReadyButton.disable( true );
}

void	aPlayerParams::setData( const _MC2Player* data)
{
	bool bDisable =  ( data->cBills <= 0 );
	CBillsSpinnerDownButton.disable( bDisable );
	bDisable = ( data->cBills >= 1000000 );
	CBillsSpinnerUpButton.disable( bDisable );

	if ( data->ready )
	{
		if ( data->commanderID != MPlayer->commanderID )
			ReadyButton.disable( false );
		ReadyButton.press( 1 );
	}
	else
	{
		if ( ReadyButton.isPressed() )
			soundSystem->playDigitalSample( LOG_UNREADY );
		ReadyButton.press( 0 );
	}

	commanderID = data->commanderID;


	long textColor = 0xff000000;
	long newColor = MPlayer->colors[data->baseColor[BASECOLOR_TEAM]];
	if ( ((newColor & 0xff) + ( (newColor & 0xff00)>>8 ) + ( (newColor & 0xff0000)>>16 ))/3 < 85 )
		textColor = 0xffffffff;

	if ( textObjects )
	{
		textObjects[0].setText( data->name );
		textObjects[1].setText( data->unitName );
		textObjects[0].setColor( textColor );
		textObjects[1].setColor( textColor );
		char text[256];
		mc2::fmt::to_buf(text, "{:d}", mc2::fmt::as_i32(data->cBills/5000 * 5) );
		if ( !edit.hasFocus() )
			edit.setEntry( text );
	}

	rects[4].setColor( MPlayer->colors[data->stripeColor] );
	rects[6].setColor( MPlayer->colors[data->baseColor[BASECOLOR_TEAM]] );

	// set up the insignia...
	// I really need to store this... really don't want to allocate a texture every time
	const char* pFileName = data->insigniaFile;
	if ( !mc2::str::iequals(mc2::str::from_c(pFileName), insigniaName) )
	{
		FullPathFileName path;
		path.init( "data" PATH_SEPARATOR "multiplayer" PATH_SEPARATOR "insignia" PATH_SEPARATOR, data->insigniaFile, ".tga" );

		if ( fileExists( path ) )
		{
			statics[0].setTexture( path );
			statics[0].setUVs( 0, 0, 32, 32 );
			insigniaName = mc2::str::from_c(pFileName);
			MPlayer->insigniaList[data->commanderID] = 1;

		}
	}


	if ( !teamNumberDropList.IsExpanded() )
	{
		int oldSel = teamNumberDropList.ListBox().GetSelectedItem();
		if ( teamNumberDropList.ListBox().GetItemCount() != MPlayer->missionSettings.maxTeams )
		{
			teamNumberDropList.ListBox().removeAllItems( true );
			for ( int i = 0; i <  MPlayer->missionSettings.maxTeams; i ++ )
			{
				auto pTmp2 = std::make_unique<aStyle2TextListItem>();
				char tmpStr[32];
				mc2::fmt::to_buf(tmpStr, "{:d}", mc2::fmt::as_i32(i+1));
				*pTmp2 = templateItem;
				pTmp2->setText( tmpStr );
				pTmp2->resize( teamNumberDropList.width() - 
					teamNumberDropList.ListBox().getScrollBarWidth() - 8, pTmp2->height() );
				pTmp2->sizeToText();
				teamNumberDropList.AddItem(pTmp2.release());
			}
			if ( oldSel < teamNumberDropList.ListBox().GetItemCount() )
				teamNumberDropList.SelectItem(oldSel);
			else
				teamNumberDropList.SelectItem(0);
		}

		teamNumberDropList.SelectItem( data->team );
	}

	if ( !factionDropList.IsExpanded() )
	{
		factionDropList.SelectItem( data->faction );
	}
	


}


int aPlayerParams::handleMessage( unsigned long message, unsigned long who )
{
	float increment = 5000;
	if ( message == aMSG_LEFTMOUSEHELD )
	{
		increment = 1000;
	}

		switch( who )
		{
		
		case MP_INCREMENT_PLAYER_CBILLS:
			{
				
			 	MC2Player* pInfo = MPlayer->getPlayerInfo( commanderID );
				pInfo->cBills += increment;
				
				char text[256];
				mc2::fmt::to_buf(text, "{:d}", mc2::fmt::as_i32(pInfo->cBills/5000 * (5)) ); // need to round to nearest 5000
				edit.setEntry( text );
				MPParameterScreen::resetCheckBoxes();
				


				break;
			}
		case MP_DECREMENT_PLAYER_CBILLS:
			{

				MC2Player* pInfo = MPlayer->getPlayerInfo( commanderID );
				pInfo->cBills -= increment;

				char text[256];
				mc2::fmt::to_buf(text, "{:d}", mc2::fmt::as_i32(pInfo->cBills/5000 * (5)) );
				edit.setEntry( text );
				MPParameterScreen::resetCheckBoxes();



			}
			break;
		}

	return 0;
}

void aPlayerParams::render()
{
	aObject::render();
}

void aPlayerParams::move( float offsetX, float offsetY )
{
	aObject::move( offsetX, offsetY );
}



long aStyle2TextListItem::init( FitIniFile* file, mc2::czstring blockName )
{
	file->seekBlock( blockName );

	long fontResID = 0;
	file->readIdLong( "Font", fontResID );
	long textID = 0;
	file->readIdLong( "TextID", textID );
	aTextListItem::init(fontResID);
	setText(textID);
	long color = 0xff808080;
	file->readIdLong( "Color", color );
	normalColor = color;
	setColor(color);

	char tmpStr[64];
	strcpy(tmpStr, "");
	file->readIdString( "Animation", tmpStr, 63 );
	if (0 == strcmp("", tmpStr))
	{
		hasAnimation = false;
	}
	else
	{
		hasAnimation = true;
		animGroup.init(file, tmpStr);
	}

	return 0;
}

void aStyle2TextListItem::render()
{
	long color = normalColor;
	if ( hasAnimation )
	{
		if ( animGroup.getState() != getState() )
			animGroup.setState( static_cast<aAnimGroup::STATE>(static_cast<long>(getState())) );
		animGroup.update();
		color = animGroup.getCurrentColor( animGroup.getState() );
	}

/*	if (aListItem::SELECTED == getState())
	{
		color = 0.33 * ((unsigned long)normalColor) + 0.67 * ((unsigned long)0xffffffff);
	}
	else if (aListItem::HIGHLITE == getState())
	{
		color = 0.67 * ((unsigned long)normalColor) + 0.33 * ((unsigned long)0xffffffff);
	}
	else
	{
		color = normalColor;
	}*/
	aTextListItem::setColor(static_cast<unsigned long>(color));

	aTextListItem::render();
}


CFocusManager::CFocusManager()
{
	clear();
}

void CFocusManager::clear()
{
	speciesOfTheControlWhichHasTheFocus = CS_NONE;

	pDropListThatHasTheFocus = nullptr;
	listOfDropListPointers.clear();
}

void *CFocusManager::registerDropList(aDropList &DropList)
{
	listOfDropListPointers.push_back(&DropList);
	return (static_cast<void *>(&DropList));
}

void CFocusManager::unregisterDropList(aDropList &DropList)
{
	const auto found = std::find(listOfDropListPointers.begin(), listOfDropListPointers.end(), &DropList);
	if (found != listOfDropListPointers.end()) listOfDropListPointers.erase(found);
}

void CFocusManager::update()
{
	if (userInput->isLeftClick())
	{
		if (nullptr != pControlThatHasTheFocus())
		{
			if (pControlThatHasTheFocus()->pointInside(userInput->getMouseX(), userInput->getMouseY()))
			{
				/* A click inside the focused control preserves focus. */
				return;
			}
		}
		switch (speciesOfTheControlWhichHasTheFocus)
		{
		case CS_DROPLIST:
			pDropListThatHasTheFocus = nullptr;
			break;
		}
		speciesOfTheControlWhichHasTheFocus = CS_NONE;
		auto DropListIter = listOfDropListPointers.begin();
		while (DropListIter != listOfDropListPointers.end())
		{
			if ((*DropListIter)->pointInside(userInput->getMouseX(), userInput->getMouseY()))
			{
				pDropListThatHasTheFocus = (*DropListIter);
				speciesOfTheControlWhichHasTheFocus = CS_DROPLIST;
				break;
			}
			DropListIter++;
		}

		if (CS_NONE == speciesOfTheControlWhichHasTheFocus)
		{
			/*No droplist got the focus. Check the other control species.*/
		}
		DropListIter = listOfDropListPointers.begin();
		while (DropListIter != listOfDropListPointers.end())
		{
			if ((CS_DROPLIST != speciesOfTheControlWhichHasTheFocus)
				|| ((*DropListIter) != pDropListThatHasTheFocus))
			{
				(*DropListIter)->IsExpanded(false);
			}
			DropListIter++;
		}
	}
}

bool CFocusManager::somebodyHasTheFocus()
{
	if (CS_NONE == speciesOfTheControlWhichHasTheFocus)
	{
		return false;
	}
	else
	{
		return true;
	}
}

aObject *CFocusManager::pControlThatHasTheFocus()
{
	if (CS_NONE != speciesOfTheControlWhichHasTheFocus)
	{
		switch(speciesOfTheControlWhichHasTheFocus) 
		{
		case CS_DROPLIST:
			return pDropListThatHasTheFocus;
			break;
		}
		assert(false);
	}
	return nullptr;
}


//////////////////////////////////////////////



//*************************************************************************************************
// end of file ( MPParameterScreen.cpp )
