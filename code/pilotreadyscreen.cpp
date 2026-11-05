#include <memory>
/*************************************************************************************************\
PilotReadyScreen.cpp			: Implementation of the PilotReadyScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"pilotreadyscreen.h"
#include"logisticsmech.h"
#include"logisticsdata.h"
#include"logisticsmechicon.h"
#include "../resource.h"
#include"mechbayscreen.h"
#include"warrior.h"
#include"gamesound.h"
#include"mc2str.h"
#include <format>
#include"multplyr.h"
#include"chatwindow.h"
#include"../GuiRuntime/GuiRuntime.h"
#include "mc2fmt.h"

PilotReadyScreen* PilotReadyScreen::s_instance = nullptr;


constexpr int PR_MSG_ADD = 62;
constexpr int PR_MSG_REMOVE = 63;
constexpr int PR_MSG_MAIN = 64;

PilotReadyScreen::PilotReadyScreen()
{
	pCurPilot = nullptr;
	pIcons = nullptr;
	status = LogisticsScreen::RUNNING;
	s_instance = this;
	forceGroupCount = 0;
	
	pDragPilot = nullptr;
	dragLeft = 0;

	status = PAUSED;
	helpTextArrayID = 13;
	mechSelected = 0;

}

PilotReadyScreen::~PilotReadyScreen()
{
	if ( pIcons )
		pIcons.reset();

	pIcons = nullptr;

	LogisticsScreen::clear();
}

void PilotReadyScreen::init(FitIniFile* file)
{
	mechDisplay.init();
	// init button, texts, statics, rects
	setLayoutNeeds( { "mcl_pr_layout.fit", 74, 16, 5, 0, 14, false } ); // stock element counts; see LayoutNeeds
	LogisticsScreen::init( *file, "PilotReadyStatic", "PilotReadyTextEntry", "PilotReadyRect", "PilotReadyButton" );
	defsHelpTextKey = "game.mcl_pr_layout.text.help_text";

	// initialize little icons
	FitIniFile iconFile;
	char path[256];
	strcpy( path, artPath );
	strcat( path, "mcl_gn_deploymentteams.fit" );
	S_strlwr( path );
	if ( NO_ERR != iconFile.open( path ) )
	{
		gosASSERT( "couldn't open the MCL_GN_DevploymentTeams file" );
		return;
	}
	LogisticsMechIcon::init(iconFile);
	pIcons = std::make_unique_for_overwrite<LogisticsMechIcon[]>(ICON_COUNT);

	int count = 0;

	
	long x = 0;
	long y = 0;

	for ( int j = 0; j < ICON_COUNT_Y; j++ )
	{
        int i = 0;
		for (; i < ICON_COUNT_X; i++ )
		{
			pIcons[count].setHelpID( IDS_HELP_DEPLOYTEAMMW );
			pIcons[count].setMech( nullptr );
			pIcons[count].move( x, y );
			x+= pIcons[count].width();
			x+=4;

			count++;
		}

		y+= pIcons[i].height();
		y += 5;
		x = 0;
	}
	
	int mechCount[256];
	memset( mechCount, 0, sizeof ( int ) * 256 );


	// initialize the attribute meeters
	attributeMeters[0].init( file, "AttributeMeter0" );
	attributeMeters[1].init( file, "AttributeMeter1" );

	for ( int i= 0; i < buttonCount; i++ )
		buttons[i].setMessageOnRelease();

	pilotListBox.init( rects[0].left(), rects[0].top(),
						rects[0].width(), rects[0].height() );


	FullPathFileName tmpPath;
	tmpPath.init( artPath, "mcl_pr_availablepilotentry", ".fit" );
	FitIniFile tmpFile;
	if ( NO_ERR != tmpFile.open( tmpPath ) )
	{
		Assert( 0, 0, "couldn't open MCL_PR_AvailablePilotEntry.fit" );
	}
	LogisticsPilotListBoxItem::init( &tmpFile );

	char blockName[32];
	for (int i = 0; i < 4; i++ )
	{
		(*std::format_to_n( blockName, sizeof(blockName) - 1, "PilotSpecialtySkillIcon{:d}", mc2::fmt::as_i32(i) ).out = '\0');
		specialtySkillIcons[i].init( file, blockName );
	}

	for (int i = 0; i < 4; i++ )
	{
		(*std::format_to_n( blockName, sizeof(blockName) - 1, "PilotSpecialtySkill{:d}", mc2::fmt::as_i32(i) ).out = '\0');
		file->seekBlock( blockName );
		file->readIdLong( "left", skillLocations[i].left );
		file->readIdLong( "top", skillLocations[i].top );
		file->readIdLong( "bottom", skillLocations[i].bottom );
		file->readIdLong( "right", skillLocations[i].right );
	}

	for (int i = 0; i < 5; i++ )
	{
		(*std::format_to_n( blockName, sizeof(blockName) - 1, "PilotReadyRankIcon{:d}", mc2::fmt::as_i32(i) ).out = '\0');
		rankIcons[i].init( file, blockName );
	}

	for (int i = 0; i < 16; i++ )
	{
		(*std::format_to_n( blockName, sizeof(blockName) - 1, "PilotMedal{:d}", mc2::fmt::as_i32(i) ).out = '\0');
		medalIcons[i].init( file, blockName );
	}

	for (int i = 0; i < 16; i++ )
	{
		(*std::format_to_n( blockName, sizeof(blockName) - 1, "PilotMedalPlacement{:d}", mc2::fmt::as_i32(i) ).out = '\0');
		file->seekBlock( blockName );
		file->readIdLong( "left", medalLocations[i].left );
		file->readIdLong( "right", medalLocations[i].right );
		file->readIdLong( "top", medalLocations[i].top );
		file->readIdLong( "bottom", medalLocations[i].bottom );
	}


}

void PilotReadyScreen::begin()
{
	getButton( MB_MSG_PREV )->disable( false );
	getButton( MB_MSG_MAINMENU )->disable( false );
	launchFadeTime = 0;
	// initialize both the inventory and icon lists
	auto mechList = LogisticsData::instance->getInventory();

	// reset force group
	forceGroupCount = 0;
	
	int maxUnits = 12;

	if ( MPlayer )
	{
		long playerCount;
		MPlayer->getPlayers( playerCount );
		if ( playerCount )
			maxUnits = MAX_MULTIPLAYER_MECHS_IN_LOGISTICS/playerCount;

		if ( maxUnits > 12 )
			maxUnits = 12;
	}

	for ( int i = 0; i < 12; i++ )
	{
		pIcons[i].setMech( nullptr );
		pIcons[i].select( 0 );

		if ( i >= maxUnits )
			pIcons[i].disable( 1 );
		else
			pIcons[i].disable( 0 );

	}
	
	if ( getButton( MB_MSG_MAINMENU ) )
		getButton( MB_MSG_MAINMENU )->setPressFX( -1 );

	int bHasPilot = -1;
	

		// update pilot use here... things can get screwed up.
	

		pilotListBox.removeAllItems( true );

		long count = 256;
		LogisticsPilot* pilots[256];
		LogisticsData::instance->getPilots( pilots, count );

		for (int i = 0; i < count; i++ )
		{
			pilots[i]->setUsed(0);
		}


		for (auto & iter : mechList)
		{
			if ( iter->getForceGroup() )
			{
				long FG = iter->getForceGroup();
				pIcons[FG-1].setMech( iter );
				if ( iter->getPilot() )
				{
					iter->getPilot()->setUsed( true );
					pIcons[FG-1].setPilot( iter->getPilot() );
					bHasPilot = FG - 1;
				}
				else
				{
					// VEHICLE-AUTO-PILOT-1: vehicles/support units auto-get a standard
					// vehicle pilot so they are launch-ready without a roster mech-pilot.
					bool isVeh = LogisticsData::instance->isVehicleUnit( iter );
					LogisticsPilot* vp = isVeh ? LogisticsData::instance->getVehiclePilot() : nullptr;
					if ( isVeh && vp )
					{
						iter->setPilot( vp );
						pIcons[FG-1].setPilot( vp );
					}
				}

				forceGroupCount++;
			}
		}

		for (int i = 0; i < count; i++ )
		{
			if ( !pilots[i]->isUsed() && pilots[i]->isAvailable() )
			{
				pilotListBox.AddItem( std::make_unique<LogisticsPilotListBoxItem>( pilots[i] ).release() );
			}
		}



		status = RUNNING;

		if ( bHasPilot != -1 )
		{
			pIcons[bHasPilot].select( true );
			setPilot( pIcons[bHasPilot].getPilot() );
		}
		else
		{
			LogisticsPilotListBoxItem* pItem = static_cast<LogisticsPilotListBoxItem*>(pilotListBox.GetItem( 0 ));
			if ( pItem )
			{
				pilotListBox.SelectItem( 0 );
				setPilot( pItem->getPilot() );
			}

			pIcons[0].select( true );
		}

	
}

void PilotReadyScreen::end()
{
	mechDisplay.setMech( nullptr );
}

void PilotReadyScreen::render(int xOffset, int yOffset )
{
	// Legacy->display scale for the text bridge (crisp TTF pilot names on the pilot
	// list + deployment icons, matching the data/defs UI layer).
	// UI-ASPECT-ANCHOR-1: canvas-aware transform (scale + pad origin) so
	// bridged icons/text and preview composites line up with the defs page
	// at every aspect; full-stretch fallback when no canvas is active.
	float tbSx = 1.f, tbSy = 1.f, tbOx = 0.f, tbOy = 0.f;
	aObject::getCanvasTransform( tbSx, tbSy, tbOx, tbOy );

	// Pilot names get a slight font boost (1.3x) so they read larger in the list.
	// MECH-ICON-BLANK-1: full gui bridge (quads too, not just text) — same fix as
	// the Mech Bay deployment icons: with the defs replacement page active, legacy
	// gos_DrawQuads are buried under the ImGui page, so pilot photos/icons in the
	// list must draw on the ImGui HUD layer at display scale.
	aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );
	aObject::beginTextBridge( tbSx, tbSy, 1.3f, tbOx, tbOy );
	pilotListBox.move( xOffset, yOffset );
	pilotListBox.render();
	pilotListBox.move( -xOffset, -yOffset );
	aObject::endTextBridge();
	aObject::endGuiBridge();


	if ( !xOffset && !yOffset )
	{
		if ( !MPlayer && !LogisticsData::instance->isSingleMission() && LogisticsData::instance->newPilotsAvailable() )
		{
			soundSystem->playBettySample( BETTY_NEW_PILOTS );
			LogisticsData::instance->setNewPilotsAcknowledged();
		}
	}

	for (auto & attributeMeter : attributeMeters)
	{
		attributeMeter.render( xOffset, yOffset );
	}

	// MECH-ICON-BLANK-1: rank/skill/medal icons are textured quads too.
	aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );
	if ( pCurPilot )
		rankIcons[pCurPilot->getRank()].render(xOffset, yOffset);


	for (auto & skillIcon : skillIcons)
		skillIcon.render(xOffset, yOffset);

	for (auto & medalIcon : medalIcons)
		medalIcon.render(xOffset, yOffset);
	aObject::endGuiBridge();


	LogisticsScreen::render( xOffset, yOffset );
	// MECH-ICON-BLANK-1: deployment-slot mech icons + pilot photos (same as bay).
	aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );
	aObject::beginTextBridge( tbSx, tbSy, 1.0f, tbOx, tbOy );
	for (int i = 0; i < ICON_COUNT; i++ )
	{
		pIcons[i].render( xOffset, yOffset );
	}
	aObject::endTextBridge();
	aObject::endGuiBridge();


	if ( mechSelected )
	{
		// hack, cover up pilot stuff.
		GUI_RECT rect = { 77 + xOffset, 317 + yOffset, 720+ xOffset, 515 + yOffset };
		drawRect( rect, 0xff000000 );
		// MECH-ICON-BLANK-1: mech display widgets on the ImGui HUD layer too.
		aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );
		mechDisplay.render( xOffset, yOffset );
		// hack, cover up list box overrruns.
		statics[27].render( xOffset, yOffset );
		statics[28].render( xOffset, yOffset );
		aObject::endGuiBridge();
	}

	if ( launchFadeTime )
	{
		launchFadeTime += frameLength;
		long color = interpolateColor( 0x00000000, 0x7f000000, launchFadeTime/.5f );
		GUI_RECT rect = { 0, 0, Environment.screenWidth, Environment.screenHeight };
		drawRect( rect, color );
	}

	if ( MPlayer && ChatWindow::instance() )
		ChatWindow::instance()->render(xOffset, yOffset);

	if ( pDragPilot )
		dragIcon.render();

}


void PilotReadyScreen::update()
{
	if ( MPlayer )
	{
		if ( MPlayer->playersReadyToLoad() )
			status = NEXT;
	}

	if ( getButton( MB_MSG_PREV )->isEnabled() ) // this is disabled if the user has already pressed launch )
	{
		// update current text
		char str[64];

		// RP
		(*std::format_to_n( str, sizeof(str) - 1, "{:d} ", mc2::fmt::as_i32(LogisticsData::instance->getCBills()) ).out = '\0');
		textObjects[1].setText( str );
		setDefsElementText( "game.mcl_pr_layout.text.cbills_text", str );

		for (auto & skillIcon : skillIcons)
			skillIcon.update();
		

		// desel icons if necessary
		bool bAllFull = 1;
		int newSel = -1;
		int oldSel = -1;
		bool bHasMech = 0;
		int newRightSel = -1;
		for (int i = 0; i < ICON_COUNT; i++ )
		{
			pIcons[i].update();
			if ( pIcons[i].justSelected() )
				newSel = i;
			else if ( pIcons[i].isSelected() )
				oldSel = i;

			bool bInside = pIcons[i].pointInside( userInput->getMouseX(), userInput->getMouseY() ); 

			
			if ( userInput->isLeftDoubleClick() && bInside && pIcons[i].getMech() )
			{
				removeSelectedPilot();
			}
			if ( userInput->isRightClick() && bInside && pIcons[i].getMech() )
			{
				newRightSel = i;
			}
			else if ( userInput->isLeftDrag() 
				&& bInside
				&& pIcons[i].pointInside( userInput->getMouseDragX(), userInput->getMouseDragY() )
				&& pIcons[i].getMech() )
			{
				beginDrag( pIcons[i].getMech()->getPilot() );
				pIcons[i].dimPilot( true );
				pIcons[i].setPilot( nullptr );
			}

			// VEHICLE-AUTO-PILOT-1: vehicles/support units (incl. copters) do not consume a
			// roster mech-pilot, so an un-piloted vehicle must NOT block launch readiness.
			if ( pIcons[i].getMech() && !pIcons[i].getPilot()
				&& !LogisticsData::instance->isVehicleUnit( pIcons[i].getMech() ) )
				bAllFull = 0;

			if ( pIcons[i].getMech() )
				bHasMech = true;
		}

		if ( newSel != -1 )
		{
			if ( oldSel != -1 )
				pIcons[oldSel].select( 0 );

			if ( pIcons[newSel].getPilot() )
			{
				setPilot( pIcons[newSel].getPilot() );
			}
			else
				setMech( pIcons[newSel].getMech() );
		}

		if ( newRightSel != -1 )
		{
			setMech( pIcons[newRightSel].getMech() );
		}

		long curSel = newSel == -1 ? oldSel : newSel;

	
		if ( !MPlayer || !ChatWindow::instance()->pointInside(userInput->getMouseX(), userInput->getMouseY()) )
			LogisticsScreen::update();

		if ( mechSelected && 
			( !MPlayer || !ChatWindow::instance()->pointInside(userInput->getMouseX(), userInput->getMouseY()) ))
		{
			if ( userInput->getMouseY() > 317 )
			{
				helpID = 0;
				textObjects[helpTextArrayID].setText( "" );
			}
			mechDisplay.update();
		}

		
		if ( !bAllFull ) // disable the next button until all have pilots
		{
			buttons[1].disable( 1 );

			if ( curSel == -1 || pIcons[curSel].getPilot() )
				buttons[2].disable( 1 );
			else
				buttons[2].disable( 0 );
		}
		else
		{
			if ( bHasMech )
				buttons[1].disable( 0 );
			else
				buttons[1].disable( 1 );

			buttons[2].disable( 1 );
		}


		if ( ( newSel == -1 && oldSel == -1 )|| !pIcons[curSel].getPilot())
		{
			// disable the remove button
			buttons[3].disable(1);
		}
		else
			buttons[3].disable(0);


		for (auto & medalIcon : medalIcons) // so we get rollovers
			medalIcon.update();

		
		// update drag and drop
		if ( pDragPilot )
		{
			dragIcon.moveTo( userInput->getMouseX() - dragIcon.width() / 2, 
				userInput->getMouseY() - dragIcon.height() / 2 );

			LogisticsMechIcon* pSelIcon = nullptr;
			
			for ( int i = 0; i < ICON_COUNT; i++ )
			{
				if ( pIcons[i].pointInside( userInput->getMouseX(), userInput->getMouseY() )
					&& pIcons[i].getMech() )
				{
					pSelIcon = &pIcons[i];
					break;
				}
			}
		

			if ( userInput->leftMouseReleased(  ) )
			{
				endDrag(pSelIcon);	
			}

		}

		pilotListBox.update();

	
	}

	if ( MPlayer && ChatWindow::instance() )
		ChatWindow::instance()->update();



}

int	PilotReadyScreen::handleMessage( unsigned long message, unsigned long who )
{

	switch( who )
	{
	case MB_MSG_NEXT:
		if ( MPlayer )
		{
			MPlayer->sendMissionSetup(nullptr, 7, nullptr);
			getButton( MB_MSG_PREV )->disable( true );
			getButton( MB_MSG_NEXT )->disable( true );
			getButton( MB_MSG_MAINMENU )->disable( true );
			MC2Player* pInfo = MPlayer->getPlayerInfo( MPlayer->commanderID );
			if ( pInfo )
			{
				const char* name = pInfo->name;
				MPlayer->sendPlayerActionChat(nullptr, name, IDS_MP_PLAYER_READY );
			}

			pilotListBox.SelectItem( -1 );

			launchFadeTime = .0001f;

		}
		else
			status = NEXT;
		break;

	case MB_MSG_PREV:
		status = PREVIOUS;
		break;

	case PR_MSG_ADD:		
		addSelectedPilot();
		
		break;

	case PR_MSG_REMOVE:
		removeSelectedPilot();
		break;
	
	case MB_MSG_MAINMENU:
		status = MAINMENU;
		break;
	}


	return 0;
}

void PilotReadyScreen::addSelectedPilot()
{
	aListItem* pItem = nullptr;
	bool bFound = 0;

	int index = pilotListBox.GetSelectedItem();
	if ( index != -1 )
		pItem = pilotListBox.GetItem( index );
	if ( pItem )
	{
		LogisticsPilot* pPilot = (static_cast<LogisticsPilotListBoxItem*>(pItem))->getPilot();
		if ( pPilot )
		{
			for ( int i = 0; i < ICON_COUNT; i++ )
			{
				if ( pIcons[i].isSelected() )
				{
					if ( !pIcons[i].getPilot() && pIcons[i].getMech() )
					{
						pIcons[i].setPilot( pPilot );
						pIcons[i].select( 0 );

						i++;
						if ( i >= ICON_COUNT || !pIcons[i].getMech() )
							i = 0;
						
						pIcons[i].select( true );
						bFound = true;
						break;
					}

				}
			}

			if ( !bFound )
			{

				// if we got here, there was nothing selected
				for (int i = 0; i < ICON_COUNT; i++ )
				{
					if ( pIcons[i].getMech() && !pIcons[i].getPilot() )
					{
						pIcons[i].setPilot( pPilot );
						bFound = true;
						break;
					}
				}
			}

		}
	}

	if ( index != -1 )
		pItem = pilotListBox.GetItem( index+1 );
	if ( pItem && bFound )
	{
		LogisticsPilot* pPilot = (static_cast<LogisticsPilotListBoxItem*>(pItem))->getPilot();
		if ( pPilot )
		{
			setPilot( pPilot );
		}
	}


}

void PilotReadyScreen::removeSelectedPilot()
{
	for ( int i = 0; i < ICON_COUNT; i++ )
	{
		if ( pIcons[i].isSelected() )
		{
			if ( pIcons[i].getPilot() )
			{
				// add the pilot back into the list
				int oldSel = pilotListBox.GetSelectedItem();
				auto pNewItem = std::make_unique<LogisticsPilotListBoxItem>( pIcons[i].getPilot() );
				int newIndex = pilotListBox.AddItem( pNewItem.release() );
				pIcons[i].setPilot( nullptr );
				pIcons[i].select( 0 );
				i++;
				if ( i >= ICON_COUNT || !pIcons[i].getMech() )
					i = 0;

				pIcons[i].select( true );
				if ( pIcons[i].getPilot() )
				{
					if ( newIndex <= oldSel )
					{
						pilotListBox.SelectItem( oldSel+1 );
					}
					setPilot( pIcons[i].getPilot() );
				}
				else
				{
					pilotListBox.SelectItem( newIndex );
					int index = pilotListBox.GetSelectedItem();
					if ( index != -1 )
					{
						LogisticsPilotListBoxItem* pItem = static_cast<LogisticsPilotListBoxItem*>(pilotListBox.GetItem( index ));
						if ( pItem )
						{
							LogisticsPilot* pPilot = (pItem)->getPilot();
							if ( pPilot )
							{
								setPilot( pPilot );
							}
						}
					}

				}

			}

			
			return;
		}
	}

}

void PilotReadyScreen::beginDrag( LogisticsPilot* pPilot )
{

	if ( !pDragPilot )
		pDragPilot = pPilot;

	if ( pPilot )
		LogisticsPilotListBox::makeUVs( pPilot, dragIcon );

	launchFadeTime = 0.f;

}

void PilotReadyScreen::endDrag( LogisticsMechIcon* pIcon  )
{
	if ( pIcon )
	{
		LogisticsPilot* pOldPilot = pIcon->getPilot();
		if ( pOldPilot )
			putBackPilot( pOldPilot );
		pIcon->setPilot( pDragPilot );

		pilotListBox.removePilot( pDragPilot );

	}
	else
	{
		putBackPilot( pDragPilot );
	}

	pDragPilot = nullptr;

}


void PilotReadyScreen::putBackPilot( LogisticsPilot* pPilot )
{
	for ( int i = 0; i < ICON_COUNT; i++ )
	{
		if ( pIcons[i].getPilot() == pPilot )
		{
			pIcons[i].setPilot( nullptr );
			pIcons[i].dimPilot( false );
		}
	}
	auto pItem = std::make_unique<LogisticsPilotListBoxItem>( pPilot );
	pilotListBox.AddItem( pItem.release() );

}

// PILOTREADY-MECH-INFO-OVERLAP-1: the pilot panel is a v2 defs page drawn on the
// ImGui layer, which sits above the gos black "cover up pilot stuff" rect, so
// with a mech selected the pilot icons, borders, placement rects (magenta
// placeholders), name and skill texts bled through the mech info panel. Hide
// the pilot-panel widgets of the defs page while a mech is selected.
static void pilotPanelDefsVisibility( LogisticsScreen* scr, bool show )
{
	static const char* const images[] = {
		"pilot_info_targeting_icon", "pilot_info_piloting_icon", "pilot_info_kills_icon",
		"small_border_piece_for_medals", "small_right_border_piece_for_medals",
		"mid_size_mid_border_piece_for_medals", "large_size_bottom_border_piece_for_medals",
		"left_vertical_line_1", "left_vertical_line_2", "right_vertical_line_1", "right_vertical_line_2",
		"left_vertical_line_for_spec_skills", "right_vertical_line_for_spec_skills",
		"small_bottom_border_piece_for_spec_skills", "small_top_border_piece_for_spec_skills",
		"this_rect_defines_the_placement", "corner_left_of_pilot_name", "corner_right_of_pilot_name",
		"corner_right_top_of_spec_skills", "corner_right_bottom_of_spec_skills",
		"corner_right_bottom_of_pilot_pic", "corner_left_bottom_of_pilot_pic",
		"green", "regular", "veteran", "elite", "ace" };
	static const char* const rectsAndMeters[] = {
		"rect.pilot_name", "rect.pilot_specialty",
		"rect.help_rect_for_currently_selected_pilot_name_and_pic",
		"rect.help_rect_for_gunnery_box", "rect.help_rect_for_piloting_skill_box",
		"rect.help_rect_for_kills_box", "rect.help_rect_for_ranks_box",
		"meter.for_targeting", "meter.for_piloting",
		"rect.placement_for_pilot_s_first_specialty_skill", "rect.placement_for_pilot_s_second_specialty_skill",
		"rect.placement_for_pilot_s_third_specialty_skill", "rect.placement_for_pilot_s_fourth_specialty_skill",
		"rect.placement_for_pilot_s_first_medal", "rect.placement_for_pilot_s_second_medal",
		"rect.placement_for_pilot_s_third_medal", "rect.placement_for_pilot_s_fourth_medal",
		"rect.placement_for_pilot_s_fifth_medal", "rect.placement_for_pilot_s_sixth_medal",
		"rect.placement_for_pilot_s_seventh_medal", "rect.placement_for_pilot_s_eighth_medal",
		"rect.placement_for_pilot_s_ninth_medal", "rect.placement_for_pilot_s_tenth_medal",
		"rect.placement_for_pilot_s_eleventh_medal", "rect.placement_for_pilot_s_twelfth_medal",
		"rect.placement_for_pilot_s_thirteenth_medal" };
	for ( const char* k : images )
		scr->setDefsElementVisible( std::string( "game.mcl_pr_layout.image." ) + k, show );
	for ( const char* k : rectsAndMeters )
		scr->setDefsElementVisible( std::string( "game.mcl_pr_layout." ) + k, show );
}

void PilotReadyScreen::setPilot( LogisticsPilot* pPilot )
{
	if ( mechSelected )
	{
		// leaving mech view: restore the pilot panel and force a full refresh
		pilotPanelDefsVisibility( this, true );
		pCurPilot = nullptr;
	}
	mechSelected = 0;
	if ( pPilot != pCurPilot )
	{
		pCurPilot = pPilot;

		// set up the big bitmap
		if ( !pCurPilot )
		{
			statics[staticCount-1].setColor( 0 );
			return;
		}

		const std::string str = mc2::str::ascii_lower( std::format( "{}MCL_PR_{}.tga", mc2::str::from_c( artPath ), pPilot->getName() ) );
		statics[67].setTexture( str.c_str() );
		statics[67].setColor( 0xffffffff );
		statics[67].setUVs( 0, 0, 92, 128 );
		// Route the per-pilot portrait TGA into the defs image (the legacy static is
		// suppressed by the defs page).  Same pattern as the encyclopedia personnel
		// picture. PILOT-PHOTO-UV-1: the photo is a 92x128 crop inside a padded
		// (colorkeyed) texture -- pass the sub-rect explicitly, mirroring the
		// legacy statics[67].setUVs(0,0,92,128); dst = the fit-authored rect.
		setDefsElementImageRegion( "game.mcl_pr_layout.image.this_rect_defines_the_placement",
			str, 0, 0, 92, 128, 398, 344, 92, 128,
			/*legacyUvSpace*/ true );
	}

	// update pilot specific stuff
	if ( pCurPilot )
	{
		char str[256];
		//ACE not continguous with other ranks.  Added too late!
		long rankID = ( pCurPilot->getRank() != 4 ) ? ( IDS_GREEN + pCurPilot->getRank() ) : IDS_ACE;
		textObjects[4].setText( rankID );
		char rankBuf[128];
		cLoadString( rankID, rankBuf, sizeof( rankBuf ) - 1 );
		setDefsElementText( "game.mcl_pr_layout.text.pilot_info_rank", rankBuf );

		(*std::format_to_n( str, sizeof(str) - 1, "{:d}", mc2::fmt::as_i32(static_cast<long>(pCurPilot->getGunnery())) ).out = '\0');
		textObjects[5].setText( str );
		setDefsElementText( "game.mcl_pr_layout.text.pilot_info_targeting_number", str );
		attributeMeters[0].setValue( (pCurPilot->getGunnery())/80.f );

		char number[64];
		(*std::format_to_n( number, sizeof(number) - 1, "{:d}", mc2::fmt::as_i32(static_cast<long>(pCurPilot->getPiloting())) ).out = '\0');
		textObjects[6].setText( number );
		setDefsElementText( "game.mcl_pr_layout.text.pilot_info_piloting_number", number );
		attributeMeters[1].setValue( pCurPilot->getPiloting()/80.f );

		(*std::format_to_n( number, sizeof(number) - 1, "{:d}", mc2::fmt::as_i32(pCurPilot->getMechKills()) ).out = '\0');
		textObjects[7].setText( number );
		setDefsElementText( "game.mcl_pr_layout.text.pilot_info_kills_number", number );

		const std::string name = mc2::str::ascii_upper( pCurPilot->getName() );
		textObjects[8].setText( name );
		setDefsElementText( "game.mcl_pr_layout.text.pilot_name_header", name.c_str() );

		int count = 32;
		const char* specialtySkills[32];
		int skillIDs[32];
 		pCurPilot->getSpecialtySkills( specialtySkills, count );
		count = 32;
		pCurPilot->getSpecialtySkills( skillIDs, count );
        int i = 0;
		for (; i < 4; i++ )
		{
			char skillKey[64];
			(*std::format_to_n( skillKey, sizeof(skillKey) - 1, "game.mcl_pr_layout.text.special_skill_{:d}", mc2::fmt::as_i32(i + 1) ).out = '\0');
			if ( i < count )
			{
				textObjects[9+i].setText( specialtySkills[i] );
				setDefsElementText( skillKey, specialtySkills[i] );
				textObjects[9+i].resize( textObjects[9+i].width(), textObjects[9+i].font.height() );
				textObjects[9+i].setHelpID( skillIDs[i] + IDS_SPECIALTY_HELP1 );
				skillIcons[i] = specialtySkillIcons[MechWarrior::skillTypes[skillIDs[i]]];
				skillIcons[i].resize( skillLocations[i].right - skillLocations[i].left,
									  skillLocations[i].bottom - skillLocations[i].top );
				skillIcons[i].moveTo( skillLocations[i].left, skillLocations[i].top );

				skillIcons[i].setColor( 0xffffffff );
				skillIcons[i].setHelpID( skillIDs[i] + IDS_SPECIALTY_HELP1 );


			}
			else
			{
				textObjects[9+i].setText( "" );
				setDefsElementText( skillKey, "" );
				skillIcons[i].setColor( 0 );
				skillIcons[i].setHelpID( 0 );
			}

		}

		for( ; i < 4; i++ )
			skillIcons[i].setColor( 0 );

		const bool* medals = pCurPilot->getMedalArray( );
		long medalCount = 0;
		for ( i = 0; i < MAX_MEDAL; i++ )
		{
			if ( medals[i] )
			{
				medalIcons[i].moveTo( medalLocations[medalCount].left, medalLocations[medalCount].top );
				medalIcons[i].resize( medalLocations[medalCount].right - medalLocations[medalCount].left,
								medalLocations[medalCount].bottom - medalLocations[medalCount].top );
									
				medalIcons[i].setColor( 0xffffffff );
				medalCount++;
				medalIcons[i].showGUIWindow( true );
				medalIcons[i].setHelpID( IDS_HELP_MEDAL1 + i );
			}
			else
			{
				medalIcons[i].showGUIWindow( 0 );
			}
		}


	}
	else
	{
		for ( int i = 4; i < 13; i++ )
			textObjects[i].setText( "" );
		setDefsElementText( "game.mcl_pr_layout.text.pilot_info_rank", "" );
		setDefsElementText( "game.mcl_pr_layout.text.pilot_info_targeting_number", "" );
		setDefsElementText( "game.mcl_pr_layout.text.pilot_info_piloting_number", "" );
		setDefsElementText( "game.mcl_pr_layout.text.pilot_info_kills_number", "" );
		setDefsElementText( "game.mcl_pr_layout.text.pilot_name_header", "" );
		setDefsElementText( "game.mcl_pr_layout.text.special_skill_1", "" );
		setDefsElementText( "game.mcl_pr_layout.text.special_skill_2", "" );
		setDefsElementText( "game.mcl_pr_layout.text.special_skill_3", "" );
		setDefsElementText( "game.mcl_pr_layout.text.special_skill_4", "" );

		statics[staticCount-1].setColor( 0 );

	}
}

void PilotReadyScreen::setMech( LogisticsMech* pMech )
{
	mechSelected = true;
	pilotPanelDefsVisibility( this, false );
	static const char* const texts[] = { "pilot_info_rank", "pilot_info_targeting_number",
		"pilot_info_piloting_number", "pilot_info_kills_number", "pilot_name_header",
		"special_skill_1", "special_skill_2", "special_skill_3", "special_skill_4" };
	for ( const char* k : texts )
		setDefsElementText( std::string( "game.mcl_pr_layout.text." ) + k, "" );
	mechDisplay.setMech( pMech, 0 );
}
