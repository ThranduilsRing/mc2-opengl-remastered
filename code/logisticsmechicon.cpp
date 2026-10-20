#define LOGISTICSMECHICON_CPP
//===========================================================================//
//LogisticsMechIcon.cpp	: Implementation of the LogisticsMechIcon component. //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"logisticsmechicon.h"
#include "../mclib/texture_assets.h"
#include"mechicon.h"
#include"icon_atlas_cell.h"
#include "mc2_audit.h"   // META-FIX design B PART 3a: icon_oob marker
#include"logisticsmech.h"
#include"logisticspilot.h"
#include "../resource.h"
#include"alistbox.h"
#include"logisticspilotlistbox.h"
#include"gamesound.h"
#include "Gates/gate.h"
#include "mc2fmt.h"

LogisticsMechIcon*	LogisticsMechIcon::s_pTemplateIcon = nullptr;


LogisticsMechIcon::LogisticsMechIcon( )
{
	if( s_pTemplateIcon )
		*this = *s_pTemplateIcon;

	pMech = nullptr;
	state = 0;
	bJustSelected = 0;
	bDisabled = 0;
}

LogisticsMechIcon& LogisticsMechIcon::operator =( const LogisticsMechIcon& src )
{
	if ( &src != this )
	{
		pilotName = src.pilotName;
		chassisName = src.chassisName;
		iconConnector = src.iconConnector;
		icon = src.icon;
		pilotIcon = src.pilotIcon;
		outline = src.outline;
		for ( int i = 0; i < ICON_ANIM_COUNT; i++ )
		{
			animations[i] = src.animations[i];
		}

		pilotID = src.pilotID;
		chassisNameID = src.chassisNameID;
		iconConnectorID = src.iconConnectorID;
		iconID = src.iconID;
		pilotIconID = src.pilotIconID;
		outlineID = src.outlineID;
		helpID = src.helpID;

	}

	return *this;
}

//-------------------------------------------------------------------------------------------------

LogisticsMechIcon::~LogisticsMechIcon()
{

}



int LogisticsMechIcon::init( FitIniFile& file )
{
	if ( !s_pTemplateIcon )
	{
		s_pTemplateIcon = new LogisticsMechIcon;
		s_pTemplateIcon->pilotName.init( &file, "PilotNameText" );
		assignAnimation( file, s_pTemplateIcon->pilotID );

		s_pTemplateIcon->chassisName.init( &file, "MechNameText" );
		assignAnimation( file, s_pTemplateIcon->chassisNameID );

		s_pTemplateIcon->iconConnector.init( &file, "IconConnector" );
		assignAnimation( file, s_pTemplateIcon->iconConnectorID );

		s_pTemplateIcon->outline.init( &file, "BoxOutline" );
		assignAnimation( file, s_pTemplateIcon->outlineID );

		s_pTemplateIcon->icon.init( &file, "MechEntryIcon" );
		assignAnimation( file, s_pTemplateIcon->iconID );

		s_pTemplateIcon->pilotIcon.init( &file, "PilotIcon" );
		assignAnimation( file, s_pTemplateIcon->pilotIconID );



		char blockName[64];
		for ( int i = 0; i < ICON_ANIM_COUNT; i++ )
		{
			mc2::fmt::to_buf(blockName, "Animation{:d}", mc2::fmt::as_i32(i));
			s_pTemplateIcon->animations[i].init( &file, blockName );
		}
	}
	

	return true;
}

void LogisticsMechIcon::assignAnimation( FitIniFile& file, long& number )
{
	number = -1;
	char buffer[64];
	if ( NO_ERR == file.readIdString( "Animation", buffer, 63 ) )
	{
		for ( int i = 0; i < strlen( buffer ); i++ )
		{
			if ( isdigit( buffer[i] ) )
			{
				buffer[i+1] = 0;
				number = atoi( &buffer[i] );
			}
		}
	}
}

void LogisticsMechIcon::setMech( LogisticsMech* pNewMech )
{
	pMech = pNewMech;

	if ( pMech )
	{
		// need to set the uv's of the mech icon
		// Icon atlas (mcui_gn_mechicons.tga) uses 25x30px cells at setFileWidth(256).
		// See MC2_LOG_MECH_ICON env var for per-mech UV diagnostics.
		long index = pMech->getIconIndex();

		float width = icon.width();
		float height = icon.height();

		// Atlas geometry is DERIVED from the loaded texture, not hardcoded.
		// Retail mech atlas = 256 wide (10 cols of 25px, mechs only, idx 0-79).
		// MC2X mc2x_mechicons = 512 wide (20 cols), holding vehicle/infantry
		// icons at high indices (Ambulance=118, APC=142, Infantry=28). The old
		// hardcoded "% 10 / setFileWidth(256)" scrambled those high indices.
		DWORD atlasW = 0, atlasH = 0;
		float fileW = 256.f, fileH = 256.f;
		if ( mcTextureManager &&
		     mc2::textures::logicalSize( icon.getTextureHandle(), atlasW, atlasH ) )
		{
			if ( atlasW > 0 ) fileW = static_cast<float>(atlasW);
			if ( atlasH > 0 ) fileH = static_cast<float>(atlasH);
		}
		// Cell/UV math shared with mechlistbox.cpp via code/icon_atlas_cell.h
		// (ICON-ATLAS-CELL-EXTRACT-1). FLOOR column count + epsilon + 10-col
		// fallback; see that header for the MCO 1024/40 phantom-column rationale.
		// META-FIX design B PART 3a: checked variant — legacy cell returned
		// UNCHANGED on OOB (wrap is load-bearing), one icon_oob line emitted.
		bool iconOob = false;
		const icon_atlas::Cell cell = icon_atlas::cellForIndexChecked( index, width, height, fileW, fileH, &iconOob );
		if ( iconOob && mc2AuditOnce( "ICON_OOB", 0x1000000ull + static_cast<unsigned long long>(static_cast<unsigned long>(index)) ) )
			mc2AuditLinef( "[ASSET_LOAD v1] event=icon_oob site=logisticsmechicon index=%ld cell=%.0fx%.0f atlas=%.0fx%.0f",
				index, width, height, fileW, fileH );
		const long cols = cell.cols;
		const long xIndex = cell.col;
		const long yIndex = cell.row;
		const float u = cell.u;
		const float v = cell.v;
		const float u2 = cell.u2;
		const float v2 = cell.v2;

		icon.setFileWidth( fileW );
		icon.setFileHeight( fileH );
		icon.setUVs( u, v, u2, v2 );

		if ( mc2::gate::get<"MC2_LOG_MECH_ICON">() )
		{
			mc2::fmt::print_out("[mechicon-logi] mech=ID:{:d} iconIndex={:d} cols={:d} row={:d} col={:d} widgetW={:.0f} widgetH={:.0f} u=[{:.1f},{:.1f}] v=[{:.1f},{:.1f}] fileWidth={:.0f} fileHeight={:.0f} uvX=[{:.3f},{:.3f}] uvY=[{:.3f},{:.3f}]\n", mc2::fmt::as_i32(pMech->getChassisName()), mc2::fmt::as_i32(index), mc2::fmt::as_i32(cols), mc2::fmt::as_i32(yIndex), mc2::fmt::as_i32(xIndex), width, height, u, u2, v, v2, fileW, fileH, u/fileW, u2/fileW, v/fileH, v2/fileH);
			fflush(stdout);
		}

		chassisName.setText( pMech->getChassisName() );

		iconConnector.showGUIWindow( true );

		if ( pMech->getPilot() )
		{
			pilotName.setText( pMech->getPilot()->getName() );
			
		}
		else
		{
			pilotName.setText( IDS_NOPILOT );
			
		}
	}
	else
		iconConnector.showGUIWindow( 0 );

		

}

void LogisticsMechIcon::render(long xOffset, long yOffset )
{

	if ( bDisabled )
	{
		GUI_RECT tmprect = { static_cast<long>((outline.left() + xOffset)), static_cast<long>((outline.top() + yOffset)),
			static_cast<long>((outline.right() + xOffset)), static_cast<long>((outline.bottom() + yOffset)) };

		drawRect( tmprect, 0xff000000 );

		return;

	}

	if ( !pMech )
		return;


	long color = animations[outlineID].getCurrentColor(animations[outlineID].getState());

	outline.setColor( color );
	outline.render(xOffset, yOffset);	

	xOffset += outline.globalX();
	yOffset += outline.globalY();

	
	renderObject( icon, iconID, xOffset, yOffset );
	renderObject( chassisName, chassisNameID, xOffset, yOffset );
	renderObject( pilotName, pilotID, xOffset, yOffset );
	renderObject( iconConnector, iconConnectorID, xOffset, yOffset );
	renderObject( icon, iconID, xOffset, yOffset );

	if ( pMech && pMech->getPilot() )
		pilotIcon.render(xOffset, yOffset );
	
}

void LogisticsMechIcon::renderObject( aObject& obj, long animIndex, long xOffset, long yOffset )
{
	long color = 0xffffffff;

	if ( animIndex != -1 )
	{
		color = animations[animIndex].getCurrentColor(animations[animIndex].getState());
	}

	obj.setColor( color );
	obj.render( xOffset, yOffset );
}

void LogisticsMechIcon::update()
{
	bJustSelected = 0;

	if ( !pMech )
		return;

	long x = userInput->getMouseX();
	long y = userInput->getMouseY();

	for ( int i = 0; i < ICON_ANIM_COUNT; i++ )
	{
		animations[i].update();
	}

	if ( outline.pointInside( x, y ) )
	{
		if ( (userInput->isLeftClick() || userInput->isLeftDoubleClick())
			&& getMech() )
		{
		
			for ( int i = 0; i < ICON_ANIM_COUNT; i++ )
			{
				animations[i].setState( aAnimGroup::PRESSED );
			}

			bJustSelected = true;
			if ( state != aListItem::SELECTED )
				soundSystem->playDigitalSample( LOG_SELECT );
			state = aListItem::SELECTED;
		}

		else if ( state == aListItem::ENABLED || state == aListItem::HIGHLITE )
		{
			for ( int i = 0; i < ICON_ANIM_COUNT; i++ )
			{
				animations[i].setState( aAnimGroup::HIGHLIGHT );
			}
			if ( state != aListItem::HIGHLITE )
			{
				soundSystem->playDigitalSample( LOG_HIGHLIGHTBUTTONS );
			}
			state = aListItem::HIGHLITE;
		}

		::helpTextID = helpID;

	}
	else if ( state != aListItem::SELECTED )
	{
		for ( int i = 0; i < ICON_ANIM_COUNT; i++ )
		{
			animations[i].setState( aAnimGroup::NORMAL );
		}

		state = aListItem::ENABLED;
	}


	
}

void LogisticsMechIcon::move( long x, long y )
{
	outline.move(x, y);	
}

void LogisticsMechIcon::select( bool bSelect )
{
	if ( !bSelect || bDisabled )
	{
		state = aListItem::ENABLED;
		for ( int i = 0; i < ICON_ANIM_COUNT; i++ )
			animations[i].setState( aAnimGroup::NORMAL );
	}
	else
	{
		state = aListItem::SELECTED;
		for ( int i = 0; i < ICON_ANIM_COUNT; i++ )
			animations[i].setState( aAnimGroup::PRESSED );

		
		bJustSelected = true;
	}
}

bool LogisticsMechIcon::isSelected()
{
	return state == aListItem::SELECTED;
}

LogisticsPilot* LogisticsMechIcon::getPilot()
{
	if ( pMech )
		return pMech->getPilot();

	return nullptr;
}

void LogisticsMechIcon::setPilot( LogisticsPilot* pPilot )
{
	if ( pMech )
	{
		pMech->setPilot( pPilot );
		
		if ( pPilot )
		{
			long x = pilotIcon.globalX();
			long y = pilotIcon.globalY();
			LogisticsPilotListBox::makeUVs( pPilot, pilotIcon );
			pilotIcon.moveTo( x, y );
			pilotName.setText( pPilot->getName() );
			pPilot->setUsed( true );

		}
		else
		{
			pilotIcon.setUVs( 0, 0, 0, 0 );
			pilotName.setText( IDS_NOPILOT );
		}
	}
	else
		pilotIcon.setUVs( 0, 0, 0, 0 );
}

void LogisticsMechIcon::dimPilot( bool bDim )
{
	if ( bDim )
	{
		pilotIcon.setColor( 0x7f000000 );
	}
	else
		pilotIcon.setColor( 0xffffffff );
}

//*************************************************************************************************
// end of file ( LogisticsMechIcon.cpp )
