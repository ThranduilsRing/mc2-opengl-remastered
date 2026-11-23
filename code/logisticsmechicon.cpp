#define LOGISTICSMECHICON_CPP
//===========================================================================//
//LogisticsMechIcon.cpp	: Implementation of the LogisticsMechIcon component. //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"logisticsmechicon.h"
#include "../mclib/texture_assets.h"
#include"mechicon.h"
#include "icon_registry.h"
#include"logisticsmech.h"
#include"logisticspilot.h"
#include "../resource.h"
#include"alistbox.h"
#include"logisticspilotlistbox.h"
#include"gamesound.h"
#include "Gates/gate.h"
#include "mc2fmt.h"

LogisticsMechIcon* LogisticsMechIcon::s_pTemplateIcon = nullptr;


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
		hudArt = src.hudArt;
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



int LogisticsMechIcon::init( FitIniFile& file, const long* slot )
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

		if ( slot )
		{
			// The page's slots are the grid; a card box of another size (MCO: 90 x 80 at x 75) is scaled into slot 1.
			LogisticsMechIcon& t = *s_pTemplateIcon;
			const auto k = unit_icons::cardScale( static_cast<int>(slot[2]), static_cast<int>(slot[3]), static_cast<int>(t.outline.width()), static_cast<int>(t.outline.height()) );
			for ( aObject* o : { &t.iconConnector, &t.icon, &t.pilotIcon } )
				unit_icons::scaleCardChild( *o, static_cast<float>(o->globalX()), static_cast<float>(o->globalY()), k, true );
			for ( aObject* o : { static_cast<aObject*>(&t.pilotName), static_cast<aObject*>(&t.chassisName) } )
				unit_icons::scaleCardChild( *o, static_cast<float>(o->globalX()), static_cast<float>(o->globalY()), k, false );
			t.outline.move( static_cast<float>(slot[0] - t.outline.globalX()), static_cast<float>(slot[1] - t.outline.globalY()) );
			t.outline.resize( slot[2], slot[3] );
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
        hudArt = unit_icons::apply(icon, s_pTemplateIcon->icon, pMech->getIconIndex(),
                                   { pMech->getFileName(), pMech->getVariant()->getChassis()->getAuthoredName() });

		chassisName.setText( pMech->getChassisDisplayName().c_str() );

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

	obj.setColor( unit_icons::logisticsIconColor(&obj == &icon && hudArt, color) );
	obj.render( xOffset, yOffset );
}

void LogisticsMechIcon::update()
{
	bJustSelected = 0;

	if ( !pMech )
		return;

	long x = userInput->getMouseX();
	long y = userInput->getMouseY();

	for (auto & animation : animations)
	{
		animation.update();
	}

	if ( outline.pointInside( x, y ) )
	{
		if ( (userInput->isLeftClick() || userInput->isLeftDoubleClick())
			&& getMech() )
		{
		
			for (auto & animation : animations)
			{
				animation.setState( aAnimGroup::PRESSED );
			}

			bJustSelected = true;
			if ( state != aListItem::SELECTED )
				soundSystem->playDigitalSample( LOG_SELECT );
			state = aListItem::SELECTED;
		}

		else if ( state == aListItem::ENABLED || state == aListItem::HIGHLITE )
		{
			for (auto & animation : animations)
			{
				animation.setState( aAnimGroup::HIGHLIGHT );
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
		for (auto & animation : animations)
		{
			animation.setState( aAnimGroup::NORMAL );
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
		for (auto & animation : animations)
			animation.setState( aAnimGroup::NORMAL );
	}
	else
	{
		state = aListItem::SELECTED;
		for (auto & animation : animations)
			animation.setState( aAnimGroup::PRESSED );

		
		bJustSelected = true;
	}
}

bool LogisticsMechIcon::isSelected()
{
	return state == aListItem::SELECTED;
}

std::optional<std::reference_wrapper<LogisticsPilot>> LogisticsMechIcon::getPilot()
{
	if ( pMech && pMech->getPilot() )
		return *pMech->getPilot();

	return std::nullopt;
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
