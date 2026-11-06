#include "userinput.h"
#define LogisticsPilotListBox_CPP
/*************************************************************************************************\
LogisticsPilotListBox.cpp			: Implementation of the LogisticsPilotListBox component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"logisticspilotlistbox.h"
#include "../mclib/texture_assets.h"
#include"logisticspilot.h"
#include "../resource.h"
#include"pilotreadyscreen.h"
#include"mc2str.h"
#include "memfunc.h"

std::unique_ptr<LogisticsPilotListBoxItem> LogisticsPilotListBoxItem::s_templateItem = nullptr;

LogisticsPilotListBox::LogisticsPilotListBox( )
{
	skipAmount = 5;
}

LogisticsPilotListBox::~LogisticsPilotListBox()
{
	LogisticsPilotListBoxItem::s_templateItem.reset();
	LogisticsPilotListBoxItem::s_templateItem = nullptr;

	aListBox::destroy();
}

void LogisticsPilotListBox::update()
{
	aListBox::update();

	for ( int i = 0; i < itemCount; i++ )
	{
		if ( (static_cast<LogisticsPilotListBoxItem*>(items[i]))->getPilot() &&
			(static_cast<LogisticsPilotListBoxItem*>(items[i]))->getPilot()->isUsed() )
		{
			RemoveItem( items[i], true );
			i--;
		}
	}

}

//-------------------------------------------------------------------------------------------------

LogisticsPilotListBoxItem::~LogisticsPilotListBoxItem()
{
	removeAllChildren( 0 );
}




LogisticsPilotListBoxItem::LogisticsPilotListBoxItem( LogisticsPilot* pNewPilot )
{
	pPilot = pNewPilot;

	state = ENABLED;
	if ( !s_templateItem )
		return;

	aObject::init( 0,0, s_templateItem->outline.width(), s_templateItem->outline.height() );

	outline = s_templateItem->outline;
	line = s_templateItem->line;
	nameText = s_templateItem->nameText;
	rankText = s_templateItem->rankText;
	rankIcon = s_templateItem->rankIcon;
	icon = s_templateItem->icon;

	for ( int i= 0; i < PILOT_LIST_BOX_CHILD_COUNT; i++ )
	{
		pChildAnimations[i] = s_templateItem->pChildAnimations[i];
	}

	for (int i = 0; i < 3; i++ )
		animations[i] = s_templateItem->animations[i];

	addChild( &nameText );
	addChild( &rankText );
	addChild( &icon );
	addChild( &rankIcon );
	addChild( &outline );
	addChild( &line );
	addChild( &pilotOutline );

	nameText.setText( mc2::str::ascii_upper( pPilot->getName() ) );

	//ACE not continguous with other ranks.  Added too late!
	if (pPilot->getRank() != 4)
		rankText.setText( pPilot->getRank() + IDS_GREEN );
	else
		rankText.setText( IDS_ACE );

	int rank = pPilot->getRank();
	// PILOT-ICON-UV: the rank-insignia UVs are PIXEL coords (rank*15 @ y96) normalized by the
	// icon's fileWidth/Height. The fit's authored size can disagree with the actual resolved
	// atlas (e.g. fit=256 but base mcl_pr_pilotskillicons.tga is 128) -> insignia sampled at
	// the wrong scale. Derive the divisor from the real texture (mirrors the mech-icon fix).
	{
		DWORD aw = 0, ah = 0;
		if ( mc2::textures::ready() &&
		     mc2::textures::logicalSize( rankIcon.getTextureHandle(), aw, ah ) )
		{
			if ( aw > 0 ) rankIcon.setFileWidth( static_cast<float>(aw) );
			if ( ah > 0 ) rankIcon.setFileHeight( static_cast<float>(ah) );
		}
	}
	rankIcon.setUVs( rank * 15, 96, rank * 15 + 15, 96 + 15 );
	
	LogisticsPilotListBox::makeUVs( pPilot, icon );


	
	
}
int LogisticsPilotListBoxItem::init( FitIniFile* file )
{
	if ( !s_templateItem )
		s_templateItem = std::make_unique<LogisticsPilotListBoxItem>( nullptr );

	MemSet(s_templateItem->pChildAnimations, 0);

	s_templateItem->nameText.init( file, "Text0" );
	setAnimation( *file, 0 );

	s_templateItem->rankText.init( file, "Text1" );
	setAnimation( *file, 1 );

	s_templateItem->icon.init( file, "Static0" );
	setAnimation( *file, 2 );

	s_templateItem->rankIcon.init( file, "Static1" );
	setAnimation( *file, 3 );

	s_templateItem->animations[0].init( file, "Animation0" );
	s_templateItem->animations[1].init( file, "Animation1" );
	s_templateItem->animations[2].init( file, "Animation2" );

	s_templateItem->outline.init( file, "Rect0" );
	setAnimation( *file, 4 );

	s_templateItem->line.init( file, "Rect2" );
	setAnimation( *file, 6 );

	s_templateItem->pilotOutline.init( file, "Rect1" );
	setAnimation( *file, 5 );


	return 0;
}

void LogisticsPilotListBoxItem::setAnimation( FitIniFile& file, int whichOne )
{
	char animationText[64];
	if ( NO_ERR == file.readIdString( "Animation", animationText, 63 ) )
	{
		for ( int i = 0; i < strlen( animationText ); i++ )
		{
			if ( isdigit( animationText[i] ) )
			{
				animationText[i+1] = '\0';
				s_templateItem->pChildAnimations[whichOne] = atoi( &animationText[i] );
				return;
			}
				
		}
	}

	s_templateItem->pChildAnimations[whichOne] = -1;


}

void LogisticsPilotListBoxItem::render()
{

	for ( int i = 0; i < this->pNumberOfChildren; i++ )
	{
		uint32_t newColor = 0xffffffff;
		if ( pChildAnimations[i] != -1 )
			newColor = animations[pChildAnimations[i]].getCurrentColor((aAnimGroup::STATE)state );
		pChildren[i]->setColor( newColor, 1 );
		pChildren[i]->render();
	}
		

/*	long newColor = animations[pChildAnimations[4]].getCurrentColor( (aAnimGroup::STATE)state );		
	outline.setColor(newColor);
	outline.render( location[0].x, location[0].y );

	
	newColor = animations[pChildAnimations[5]].getCurrentColor( (aAnimGroup::STATE)state );
	line.setColor( newColor );
	line.render( location[0].x, location[0].y );*/
	

}

void LogisticsPilotListBoxItem::update()
{
	bool isInside = pointInside( userInput->getMouseX(), userInput->getMouseY() );

	for (auto & animation : animations)
		animation.update();

	if ( state == aListItem::SELECTED ) 
	{
		if ( animations[0].getState() != aAnimGroup::PRESSED )
		{
			for (auto & animation : animations)
				animation.setState( aAnimGroup::PRESSED );
		}

		if ( userInput->isLeftDoubleClick() && isInside )
		{
			PilotReadyScreen::instance()->setPilot( pPilot );
			PilotReadyScreen::instance()->addSelectedPilot( );
		}
		if ( userInput->isLeftClick() && isInside )
			PilotReadyScreen::instance()->setPilot( pPilot );
		
		if ( userInput->isLeftDrag() && isInside &&
			pointInside( userInput->getMouseDragX(), userInput->getMouseDragY() ) )
		{
			PilotReadyScreen::instance()->beginDrag( pPilot );
			icon.setColor( 0x7f000000 );
		}
		else if ( !userInput->isLeftDrag() )
		{
			icon.setColor( 0xffffffff );
		}
	}
	else if ( state == aListItem::HIGHLITE )
	{
		if ( animations[0].getState() != aAnimGroup::HIGHLIGHT )
		{
			for (auto & animation : animations)
				animation.setState( aAnimGroup::HIGHLIGHT );
		}
	}
	else
	{
		if ( animations[0].getState() != aAnimGroup::NORMAL )
		{
			for (auto & animation : animations)
				animation.setState( aAnimGroup::NORMAL );
		}
	}

	aObject::update();
}


void LogisticsPilotListBox::makeUVs( LogisticsPilot* pPilot, aObject& icon )
{
	icon = LogisticsPilotListBoxItem::s_templateItem->icon;
	// need to set the UV's
	int index = pPilot->getPhotoIndex();

	long xIndex = index % 12;
	long yIndex = index / 12;

	float fX = xIndex;
	float fY = yIndex;

	float width = icon.width();
	float height = icon.height();

	float u = (fX * width);
	float v = (fY * height);

	fX += 1.f;
	fY += 1.f;
	
	float u2 = (fX * width);
	float v2 = (fY * height);

	// PILOT-ICON-UV: derive the photo-atlas width from the real texture instead of a
	// hardcoded 256 (a campaign/compat photo atlas may differ); mirrors the mech-icon fix.
	{
		DWORD aw = 0, ah = 0;
		float fw = 256.f;
		if ( mc2::textures::ready() &&
		     mc2::textures::logicalSize( icon.getTextureHandle(), aw, ah ) && aw > 0 )
			fw = static_cast<float>(aw);
		icon.setFileWidth( fw );
	}
	icon.setUVs( u, v, u2, v2 );

}

long LogisticsPilotListBox::AddItem( std::unique_ptr<aListItem> pNewItem )
{
	if (itemCount >= MAX_LIST_ITEMS)
		return TOO_MANY_ITEMS;

	scrollBar->setOrange();
	LogisticsPilotListBoxItem* pItem = dynamic_cast<LogisticsPilotListBoxItem*>(pNewItem.get());

	if ( pItem )
	{
		LogisticsPilot* pPilot = pItem->getPilot();
		for ( int i = 0; i < itemCount; i++ )
		{
				
			LogisticsPilotListBoxItem* pTmpItem = dynamic_cast<LogisticsPilotListBoxItem*>(items[i]);
			if ( pTmpItem )
			{

				// do not put in twice.
				if ( mc2::str::iequals( pPilot->getName(), pTmpItem->getPilot()->getName() ) )
				{
					return -1;
				}

				if ( pPilot->getRank() > pTmpItem->getPilot()->getRank() )
				{
					return InsertItem( pNewItem.release(), i );
				}
				else if ( pPilot->getRank() == pTmpItem->getPilot()->getRank() )
				{
					if ( pPilot->getGunnery() > pTmpItem->getPilot()->getGunnery() )
					{
						return InsertItem( pNewItem.release(), i );
					}
					else if ( pPilot->getGunnery() == pTmpItem->getPilot()->getGunnery() &&
						// the order of pilots equal in rank and gunnery: name, ignoring ASCII case, then raw bytes
						mc2::str::ci_total_less{}( pPilot->getName(), pTmpItem->getPilot()->getName() ) )
					{
						return InsertItem( pNewItem.release(), i );
					}
				}
			}

		} 
	}

	// if we got here
	return aListBox::AddItem( std::move(pNewItem) );

}

void LogisticsPilotListBox::removePilot( LogisticsPilot* pPilot )
{

	for ( int i = 0; i < itemCount;i++ )
	{
		LogisticsPilotListBoxItem* pItem = dynamic_cast<LogisticsPilotListBoxItem*>(items[i]);
		if ( pItem && pItem->getPilot() == pPilot )
		{
			RemoveItem( pItem, true );
			return;
		}
	}
}

//*************************************************************************************************
// end of file ( LogisticsPilotListBox.cpp )
