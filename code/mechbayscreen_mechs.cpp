// MechBayScreen: mech selection, removal, setMech, drag handling and the deployment-team helpers.
// Declared in the class headers; shared names in mechbayscreen_internal.h.
#include "mc2_czstring.h"
#include "mechbayscreen_internal.h"
#include "mc2fmt.h"
#include "mc2str.h"
#include <tracy/Tracy.hpp>


void MechBayScreen::addSelectedMech()
{
	if ( mechListBox.getCurrentMech() )
	{
		if ( !LogisticsData::instance->canAddMechToForceGroup( mechListBox.getCurrentMech() ) )
			return;

		if ( !LogisticsData::instance->addMechToForceGroup( mechListBox.getCurrentMech(),
			forceGroupCount + 1 ) )
		{
			// its already in the force group, add probably called 2x'2
			return;
		}

		if ( forceGroupCount > 11 )
			return;

		unselectDeploymentTeam();
		forceGroupCount++;
		reinitMechs();

		addWeightAmount = pCurMech->getMaxWeight();
		addWeightAnim.begin();
		removeWeightAnim.end();

		bool bFound = 0;
		for ( int i = 0; i < mechListBox.GetItemCount(); i++ )
		{
			if ( LogisticsData::instance->canAddMechToForceGroup( 
				(static_cast<MechListBoxItem*>(mechListBox.GetItem( i )))->getMech() ) )
			{
				bFound = true;
			}
		}

		if ( !bFound ) // select first thing in FG
		{
			selectFirstFGItem();
			mechListBox.SelectItem( -1 );
		}
	}

}

void MechBayScreen::removeSelectedMech()
{
    int i = 0;
	for (i = 0; i < ICON_COUNT; i++ )
	{
		if ( pIcons[i].isSelected() )
		{
			LogisticsMech* pMech = pIcons[i].getMech();
			if ( !pMech )
				return;
			if ( 0 == LogisticsData::instance->getVariantsInInventory( pMech->getVariant(), 0 ) )
			{
				MechListBoxItem* item = new MechListBoxItem( pMech, 0 );
				mechListBox.AddItem( item );
			}

			LogisticsData::instance->removeMechFromForceGroup( pMech, true );	
			
			forceGroupCount--;
			removeWeightAmount = pMech->getMaxWeight();
			removeWeightAnim.begin();
			addWeightAnim.end();
			if ( i >= forceGroupCount )
			{
				if ( !selectFirstFGItem() )
				{
					selectFirstViableLBMech();
				}
			}
			else
				pIcons[i].select( true );
			break;
		}
	}
	
	for ( ; i < ICON_COUNT - 1; i++ )
	{
		LogisticsData::instance->removeMechFromForceGroup(pIcons[i+1].getMech(), false);		
		LogisticsData::instance->addMechToForceGroup(pIcons[i+1].getMech(), i+1);
	}

	reinitMechs();

	if ( !forceGroupCount )
		selectFirstViableLBMech();


}


void MechBayScreen::end()
{
	mechCamera->setMech( nullptr );
}

void MechBayScreen::setMech( LogisticsMech* pMech,  bool bCommandFromLB )
{
	ZoneScopedN("MechBayScreen::setMech");

	if ( pMech == pCurMech || status != RUNNING)
		return;

	pCurMech = pMech;
	if ( pMech )
	{
		loadoutListBox.setMech( pMech->getVariant() );

		mechCamera->setMech( mc2::str::file_stem( pMech->getFileName() ).c_str(), prefs.baseColor, prefs.highlightColor, prefs.highlightColor );
		mechCamera->setScale( pMech->getVariant()->getChassis()->getScale() );
	}
	else
		loadoutListBox.setMech( nullptr );

	if ( bCommandFromLB ) // clicked on a disabled mech
	{
		unselectDeploymentTeam();
		mechListBox.SelectItem( -1 );
	}

	if ( pCurMech )
	{
		char tmpStr[256];

		// update current text
		char str[64];

		// weight
		cLoadString( IDS_MB_MECH_WEIGHT, tmpStr, 63 );
		sprintf( str, tmpStr, pCurMech->getMaxWeight(), pCurMech->getMechClass().c_str() );
		textObjects[7].setText( str );
		setDefsElementText( "game.mcl_mb_layout.text.mech_info_tonnage", str );

		// firing range (getOptimalRangeString returns a string-table ID)
		uint32_t tmpColor;
		int rangeID = pCurMech->getVariant()->getOptimalRangeString( tmpColor );
		textObjects[14].setText( rangeID );
		textObjects[14].setColor( tmpColor );
		char rangeBuf[128];
		cLoadString( rangeID, rangeBuf, sizeof( rangeBuf ) - 1 );
		setDefsElementText( "game.mcl_mb_layout.text.mech_info_range_text", rangeBuf );

		// armor
		int armor = pCurMech->getArmor();
		mc2::fmt::to_buf(str, "{:d}", mc2::fmt::as_i32(armor));
		textObjects[8].setText( str );
		setDefsElementText( "game.mcl_mb_layout.text.mech_info_armor", str );
		attributeMeters[0].setValue( armor/MAX_ARMOR_RANGE );

		// speed
		int speed = pCurMech->getDisplaySpeed().value_or(-1);
		mc2::fmt::to_buf(str, "{:d}", mc2::fmt::as_i32(speed));
		textObjects[9].setText( str );
		setDefsElementText( "game.mcl_mb_layout.text.mech_info_speed", str );
		attributeMeters[1].setValue( pCurMech->getSpeed()/MAX_SPEED_RANGE );


		// jump
		int jumpRange = pCurMech->getJumpRange();
		mc2::fmt::to_buf(str, "{:d}", mc2::fmt::as_i32(jumpRange * 25));
		textObjects[10].setText( str );
		setDefsElementText( "game.mcl_mb_layout.text.mech_info_jump_dist", str );
		attributeMeters[2].setValue( jumpRange * 25 / MAX_JUMP_RANGE );

		// Both names are text: the chassis name through the shared resolver, the variant from the file.
		const std::string chassisName = pCurMech->getChassisDisplayName();
		textObjects[11].setText( chassisName.c_str() );
		setDefsElementText( "game.mcl_mb_layout.text.mech_name_header", chassisName.c_str() );
		textObjects[12].setText( pCurMech->getName() );
		setDefsElementText( "game.mcl_mb_layout.text.variant_name_header", pCurMech->getName().c_str() );
	}
	else
	{
		textObjects[7].setText( "" );
		textObjects[8].setText( "" );
		textObjects[9].setText( "" );
		textObjects[10].setText( "" );
		textObjects[11].setText( "" );
		textObjects[12].setText( "" );
		textObjects[14].setText( "" );
		setDefsElementText( "game.mcl_mb_layout.text.mech_info_tonnage", "" );
		setDefsElementText( "game.mcl_mb_layout.text.mech_info_armor", "" );
		setDefsElementText( "game.mcl_mb_layout.text.mech_info_speed", "" );
		setDefsElementText( "game.mcl_mb_layout.text.mech_info_jump_dist", "" );
		setDefsElementText( "game.mcl_mb_layout.text.mech_name_header", "" );
		setDefsElementText( "game.mcl_mb_layout.text.variant_name_header", "" );
		setDefsElementText( "game.mcl_mb_layout.text.mech_info_range_text", "" );
		for (auto & attributeMeter : attributeMeters)
		{
			attributeMeter.setValue(0);
		}

	}

	// Mirror the (legacy, occluded) loadout list into the defs GuiList so it renders
	// on the ImGui layer.  loadoutListBox was populated above via setMech(); an empty
	// box (pMech == NULL) pushes an empty list, clearing the defs display.
	syncWeaponLoadout( this, loadoutListBox, "game.mcl_mb_layout.list.loadout" );
}

void MechBayScreen::beginDrag( LogisticsMech* pMech )
{	
	if ( pDragMech || status != RUNNING )
		return;

	if ( !pMech )
		return;


	// need to set the uv's of the mech icon
	MechListBox::initIcon( pMech, dragIcon );

	dragIcon.moveTo( userInput->getMouseX() - dragIcon.width() / 2, 
			userInput->getMouseY() - dragIcon.height() / 2 );


	pDragMech = pMech;

	// figure out where the mech came from 
	if ( mechListBox.pointInside( userInput->getMouseDragX(), userInput->getMouseDragY() ) )
	{
		if ( pMech->getForceGroup() )
		{
			pMech = LogisticsData::instance->getMechWithoutForceGroup( pMech ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
		}
		pDragMech = pMech;


		if ( !LogisticsData::instance->canAddMechToForceGroup( pMech ) )
		{
			pDragMech = nullptr;
			return;
		}

		LogisticsData::instance->addMechToForceGroup( pMech, forceGroupCount+1 );
		dragLeft = 0;
		
	}
	else
	{
		for ( int i = 0; i < ICON_COUNT; i++ )
		{
			if ( pIcons[i].pointInside( userInput->getMouseDragX(), userInput->getMouseDragY() ) )
			{
				forceGroupCount--;

				// select the next one
				if ( i == forceGroupCount && forceGroupCount )
				{
					pIcons[i].select( 0 );
					pIcons[0].select( true );
				}
				else if ( !forceGroupCount) 
					pIcons[i].select( 0 );
				
				for ( ; i < ICON_COUNT - 1; i++ )
				{		
					LogisticsData::instance->removeMechFromForceGroup( pIcons[i+1].getMech(), false );
					LogisticsData::instance->addMechToForceGroup( pIcons[i+1].getMech(), i+1 );
					pIcons[i].setMech( pIcons[i+1].getMech() );
					pIcons[i].setPilot( pIcons[i+1].getPilot().transform([](auto ref) { return &ref.get(); }).value_or(nullptr) );
				}

				if ( i < ICON_COUNT )
				{
					pIcons[i].setMech( nullptr );
					pIcons[i].setPilot( nullptr );
				}


				break;
			}

		}
		dragLeft = 1;
	}
	
}

void MechBayScreen::reinitMechs()
{
	
	int count = 0;

	
	int maxUnits = 12;

	if ( MPlayer )
	{
		long playerCount;
		MPlayer->getPlayers( playerCount );
		if ( playerCount )
			maxUnits = MAX_MULTIPLAYER_MECHS_IN_LOGISTICS/playerCount;

		maxUnits = std::min(maxUnits, 12);
	}

	for ( int j = 0; j < ICON_COUNT_Y; j++ )
	{
		for ( int i = 0; i < ICON_COUNT_X; i++ )
		{
			pIcons[count].setMech( nullptr );
			if ( count >= maxUnits )
				pIcons[count].disable( true );
			else
				pIcons[count].disable( 0 );

			count++;
		}
	}


	forceGroupCount = 0;

	// initialize both the inventory and icon lists
	auto mechList = LogisticsData::instance->getInventory();
	
	
	for (auto & mechElem : mechList)
		{
			if ( mechElem->getForceGroup() )
			{
				long FG = mechElem->getForceGroup();

				if ( FG <= maxUnits )
				{
				
					pIcons[FG-1].setMech( mechElem );
					pIcons[FG-1].setPilot( mechElem->getPilot() );
					forceGroupCount++;
				}
				else 
					mechElem->setForceGroup( 0 );
				
			}
		}
}

void	MechBayScreen::unselectDeploymentTeam()
{
	int count = 0;
	for ( int j = 0; j < ICON_COUNT_Y; j++ )
	{
		for ( int i = 0; i < ICON_COUNT_X; i++ )
		{
			pIcons[count].select( 0 );
			count++;
		}
	}
}

bool	MechBayScreen::selectFirstFGItem()
{
	bool bRetVal = false;
	for ( int i = 0; i < ICON_COUNT_X * ICON_COUNT_Y; i++ )
	{
		if ( pIcons[i].getMech() && !bRetVal )
		{
			pIcons[i].select( true );
			bRetVal = true;
		}
		else
			pIcons[i].select( false );
	}

	return bRetVal;
}
bool	MechBayScreen::selectFirstViableLBMech()
{
	for ( int i = 0; i < mechListBox.GetItemCount(); i++ )
	{
		if ( mechListBox.GetItem( i )->getState() != aListItem::DISABLED )
		{
			mechListBox.SelectItem( i );
			return true;
		}
	}

	return 0;
}

LogisticsMech* MechBayScreen::getFGSelMech()
{
	for ( int i = 0; i < ICON_COUNT_X * ICON_COUNT_Y; i++ )
	{
		if ( pIcons[i].getMech() && pIcons[i].isSelected()  )
			return pIcons[i].getMech();
	}

	return nullptr;
}
