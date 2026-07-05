#define LOGISTICSMECHDISPLAY_CPP
/*************************************************************************************************\
LogisticsMechDisplay.cpp			: Implementation of the LogisticsMechDisplay component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"logisticsmechdisplay.h"
#include"logisticsmech.h"
#include"mclib.h"
#include "../resource.h"
#include"prefs.h"

// Mirror a populated component list into the defs GuiList, preserving the per-item
// range colours -- so the Mech Loadout matches the Mech Bay screen 100% (defs-page
// GuiList, TTF) instead of the legacy gos list.  (Same helper as mechbayscreen.cpp;
// mechlopedia's copy lives in an anon namespace and isn't linkable across TUs.)
namespace {
void syncWeaponLoadout( LogisticsScreen* screen, ComponentListBox& box, const char* listKey )
{
	std::vector<std::string>  items;
	std::vector<unsigned int> colors;
	for ( int i = 0; i < box.GetItemCount(); i++ )
	{
		aTextListItem* pItem = (aTextListItem*)box.GetItem( i );
		if ( pItem )
		{
			items.push_back( pItem->getText() );
			colors.push_back( (unsigned int)pItem->getColor() );
		}
	}
	screen->setDefsListItems( listKey, items );
	screen->setDefsListItemColors( listKey, colors );
}
} // namespace

LogisticsMechDisplay::LogisticsMechDisplay(  )
{
	pCurMech = NULL;

	helpTextArrayID = 7;
}

//-------------------------------------------------------------------------------------------------


//-------------------------------------------------------------------------------------------------


//-------------------------------------------------------------------------------------------------

void LogisticsMechDisplay::update()
{
	componentListBox.update();
	LogisticsScreen::update();


	mechCamera.update();


}

//-------------------------------------------------------------------------------------------------
LogisticsMechDisplay::~LogisticsMechDisplay()
{

}

//-------------------------------------------------------------------------------------------------

void LogisticsMechDisplay::render(int xOffset, int yOffset)
{

	componentListBox.move(xOffset, yOffset);
	componentListBox.render();
	componentListBox.move(-xOffset, -yOffset);

	if ( xOffset == 0 && yOffset == 0 ) // don't draw until we're done animating in or out
	{
		// PREVIEW-FBO-FIXED-800x600-1: mcl_mechinfo.fit has a real placement rect
		// for this ("game.mcl_mechinfo.rect.rect_to_fit_mech_animation_into"), so
		// render offscreen (fixed 800x600, matches this class's existing
		// mechCamera.init()/bounds[] math unchanged) and composite there via
		// ImGui, same technique as Mechlopedia/Mech Bay. Shared by
		// mechpurchasescreen.cpp and pilotreadyscreen.cpp (both embed this class).
		mechCamera.setPreviewOffscreen( true );
		mechCamera.render();
		float px = 0, py = 0, pw = 0, ph = 0;
		if ( getDefsElementScreenRect( "game.mcl_mechinfo.rect.rect_to_fit_mech_animation_into", px, py, pw, ph ) )
			mechCamera.drawPreviewToPanel( px, py, pw, ph );
	}

	for ( int i = 0; i < 3; i++ )
	{
		attributeMeters[i].render( xOffset, yOffset );
	}
	
	LogisticsScreen::render( xOffset, yOffset );




}

//-------------------------------------------------------------------------------------------------


int	LogisticsMechDisplay::init( )
{
	FitIniFile file;
	FullPathFileName path;
	path.init( artPath, "mcl_mechinfo", ".fit" );
	if ( NO_ERR !=  file.open( path ) )
	{
		Assert( 0, 0, "Couldn't open mcl_mechinfo.fit" );
		return 0;
	}

	LogisticsScreen::init( file, "Static", "Text", "Rect", "Button" );
	
	mechCamera.init( rects[1].left(), rects[2].top(),
			rects[2].left(), rects[2].bottom() );

	char blockName[64];
	for ( int i = 0; i < 3; i++ )
	{
		sprintf( blockName, "AttributeMeter%ld", i );
		attributeMeters[i].init( &file, blockName );
	}

	
	componentListBox.init( rects[0].left(), rects[0].top(),
		rects[0].width(), rects[0].height() );


	return 1;


}

//-------------------------------------------------------------------------------------------------

void LogisticsMechDisplay::setMech( LogisticsMech* pMech, bool bFromLB )
{
		if ( pMech != pCurMech )
		{
			pCurMech = pMech;
			if ( pCurMech )
			{
				// chassis name is a string-table ID; variant name is an EString.
				textObjects[0].setText( pCurMech->getChassisName() );
				textObjects[1].setText( pCurMech->getName() );
				char chassisBuf[128];
				cLoadString( pCurMech->getChassisName(), chassisBuf, sizeof( chassisBuf ) - 1 );
				setDefsElementText( "game.mcl_mechinfo.text.mech_info_name_header_above_mech_animation", chassisBuf );
				setDefsElementText( "game.mcl_mechinfo.text.variant_name_header_above_weapon_rect", (const char*)pCurMech->getName() );

				char text[64];
				char tmpStr[64];
				cLoadString( IDS_MB_MECH_WEIGHT, tmpStr, 63 );
				sprintf( text, tmpStr, pCurMech->getMaxWeight(), (const char*)pCurMech->getMechClass() );
				textObjects[3].setText( text );
				setDefsElementText( "game.mcl_mechinfo.text.for_mech_weight", text );

				uint32_t tmpColor;
				int rangeID = pCurMech->getVariant()->getOptimalRangeString( tmpColor );
				textObjects[2].setText( rangeID );
				textObjects[2].setColor( tmpColor );
				char rangeBuf[128];
				cLoadString( rangeID, rangeBuf, sizeof( rangeBuf ) - 1 );
				setDefsElementText( "game.mcl_mechinfo.text.for_mech_range", rangeBuf );

				sprintf( text, "%ld", pCurMech->getArmor() );
				textObjects[4].setText( text );
				setDefsElementText( "game.mcl_mechinfo.text.for_mech_armor", text );

				sprintf( text, "%ld", pCurMech->getDisplaySpeed() );
				textObjects[5].setText( text );
				setDefsElementText( "game.mcl_mechinfo.text.for_mech_speed", text );

				sprintf( text, "%ld", pCurMech->getJumpRange() * 25);
				textObjects[6].setText( text );
				setDefsElementText( "game.mcl_mechinfo.text.for_mech_jump_range", text );

				attributeMeters[0].setValue( pCurMech->getArmor()/MAX_ARMOR_RANGE);
				attributeMeters[1].setValue( pCurMech->getSpeed()/MAX_SPEED_RANGE);
				attributeMeters[2].setValue( pCurMech->getJumpRange() * 25 / MAX_JUMP_RANGE);

				
				EString fileName = pMech->getFileName();
				int index = fileName.Find( '.' );
				fileName = fileName.Left( index );
				index = fileName.ReverseFind( PATH_SEPARATOR_AS_CHAR );
				fileName = fileName.Right( fileName.Length() - index - 1 );
				mechCamera.setMech( fileName, prefs.baseColor, prefs.highlightColor, prefs.highlightColor );	
				mechCamera.setScale( pMech->getVariant()->getChassis()->getScale() );
				
				componentListBox.setMech( pCurMech->getVariant() );
				syncWeaponLoadout( this, componentListBox, "game.mcl_mechinfo.list.loadout" );

			}
			else
			{
				for ( int i = 0; i < 6; i++ )
					textObjects[i].setText( "" );
				setDefsElementText( "game.mcl_mechinfo.text.mech_info_name_header_above_mech_animation", "" );
				setDefsElementText( "game.mcl_mechinfo.text.variant_name_header_above_weapon_rect", "" );
				setDefsElementText( "game.mcl_mechinfo.text.for_mech_range", "" );
				setDefsElementText( "game.mcl_mechinfo.text.for_mech_weight", "" );
				setDefsElementText( "game.mcl_mechinfo.text.for_mech_armor", "" );
				setDefsElementText( "game.mcl_mechinfo.text.for_mech_speed", "" );
				setDefsElementText( "game.mcl_mechinfo.text.for_mech_jump_range", "" );

				attributeMeters[0].setValue( 0 );
				attributeMeters[1].setValue( 0 );
				attributeMeters[2].setValue( 0 );

				componentListBox.setMech( 0 );
				syncWeaponLoadout( this, componentListBox, "game.mcl_mechinfo.list.loadout" );
				mechCamera.setMech( NULL );
			}
		}
}


//*************************************************************************************************
// end of file ( LogisticsMechDisplay.cpp )
