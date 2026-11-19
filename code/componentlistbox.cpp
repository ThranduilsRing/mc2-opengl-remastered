#define COMPONENTLISTBOX_CPP
/*************************************************************************************************\
ComponentListBox.cpp			: Implementation of the ComponentListBox component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"componentlistbox.h"
#include"logisticsvariant.h"
#include"logisticscomponent.h"
#include "../resource.h"
#include"malloc.h"
#include"mclib.h"
#include"logisticsdata.h"
#include"mechlabscreen.h"
#include"mechlab_loc_layout.h"
#include"gamesound.h"
#include"logisticserrors.h"
#include "mc2fmt.h"
#include "../GameOS/gameos/input_capture_policy.h"
#include "vfs.h"

std::unique_ptr<ComponentListItem> ComponentListItem::s_templateItem = nullptr;

ComponentIconListBox* ComponentIconListBox::s_instance = nullptr;



///////////////////////////////////////////////////////


ComponentListItem::ComponentListItem( LogisticsComponent* pComp )
{

	if ( s_templateItem )
		*this = *s_templateItem;

	pComponent = pComp;


	if ( pComp )
	{
		name.setText( pComponent->getName() );
		char numericText[32];
		mc2::fmt::to_buf(numericText, "{:d}", mc2::fmt::as_i32(pComponent->getCost()));
		costText.setText(  numericText  );
		mc2::fmt::to_buf(numericText, "{:.0f}", pComponent->getHeat());
		heatText.setText( numericText );

		int sizeX = pComponent->getComponentWidth();
		int sizeY = pComponent->getComponentHeight();
		
		const char* pFile = pComponent->getIconFileName();

		FullPathFileName path;
		path.init( artPath, pFile, "tga" );

		icon.setTexture( path );
		icon.setComponent(*pComponent);
		icon.resize( sizeX * LogisticsComponent::XICON_FACTOR, sizeY * LogisticsComponent::YICON_FACTOR);
		icon.setUVs( 0.f, 0.f, sizeX * 48.f, sizeY * 32.f );

		// Rects come from the pure table (mechlab_loc::componentItemRects): wider-than-one-cell icons get a header row
		// for heat/cost above the icon, and the warning is confined to the icon.
		namespace ml = mc2::mechlab_loc;
		const ml::ItemRects R = ml::componentItemRects( sizeX, sizeY );
		const float ox = static_cast<float>(outline.left()), oy = static_cast<float>(outline.top());   // the fit frame starts at the outline
		const long rowDy = static_cast<long>(R.rowDy);
		icon.moveTo( ox + R.icon.x, oy + R.icon.y );
		name.moveTo( ox + R.name.x, oy + R.name.y );
		name.resize( static_cast<long>(R.name.w), static_cast<long>(R.name.h) );
		heatIcon.moveTo( ox + R.heatIcon.x, oy + R.heatIcon.y );
		heatText.moveTo( ox + R.heat.x, oy + R.heat.y );
		costText.moveTo( ox + R.cost.x, oy + R.cost.y );
		costIcon.moveTo( ox + R.costIcon.x, oy + R.costIcon.y );
		if ( rowDy )
			heatText.alignment = 0;

		// outline and icon outline sizes come from the same table (so the layout test pins the real placement)
		outline.resize( static_cast<long>(R.item.w), static_cast<long>(R.item.h) );
		iconOutline.resize( static_cast<long>(R.iconOutline.w), static_cast<long>(R.iconOutline.h) );

		// Warning (HEAT EXCEEDED): small face (agencyfb11), wraps to two lines, confined to the icon outline.
		disabledText.font = name.font;
		disabledText.resize( static_cast<long>(R.warn.w), static_cast<long>(R.warn.h) );
		disabledText.moveTo( ox + R.warn.x, oy + R.warn.y );
		disabledText.alignment = 2;


		aObject::init( outline.left(), outline.top(), outline.width(), outline.height() );

		addChild( &icon );
		addChild( &name );
		addChild( &costText );
		addChild( &heatText );
		addChild( &costIcon );
		addChild( &heatIcon );
		addChild( &disabledText );

		if ( s_templateItem )
		{

			for ( int i = 0; i < 6; i++ )
			{
				for ( int j = 0; j < COMP_ANIMATION_COUNT; j++ )
				{
					if ( s_templateItem->pChildAnims[i] == &s_templateItem->animations[j] )
					{
						pChildAnims[i] = &animations[j];
					}
				}
			}
		}

	}
}

ComponentListItem::~ComponentListItem()
{
}

int ComponentListItem::init( FitIniFile& file )
{
	if ( !s_templateItem )
	{
		s_templateItem = std::make_unique<ComponentListItem>( nullptr );
		char animName[COMP_ANIMATION_COUNT][32];
		for ( int i = 0; i < COMP_ANIMATION_COUNT; i++ )
		{
			mc2::fmt::to_buf(animName[i], "Animation{:d}", mc2::fmt::as_i32(i));
			s_templateItem->animations[i].init( &file, animName[i] );
		
		}
		
		s_templateItem->icon.init( &file, "ComponentIcon" );
		assignAnimation( file, 0, animName, &s_templateItem->icon );

		s_templateItem->iconOutline.init( &file, "ComponentIconOutlineRect" );

		s_templateItem->outline.init( &file, "ComponentEntryBox" );


		s_templateItem->name.init( &file, "ComponentNameText" );
		assignAnimation( file, 1, animName, &s_templateItem->name );

		s_templateItem->costText.init( &file, "ComponentCostText" );
		assignAnimation( file, 2, animName, &s_templateItem->costText );

		s_templateItem->heatText.init( &file, "ComponentHeatText" );
		assignAnimation( file, 3, animName, &s_templateItem->heatText );

		s_templateItem->costIcon.init( &file, "ComponentCostIcon" );
		assignAnimation( file, 4, animName, &s_templateItem->costIcon );

		s_templateItem->heatIcon.init( &file, "ComponentHeatIcon" );
		assignAnimation( file, 5, animName, &s_templateItem->heatIcon );

		s_templateItem->disabledText.init( &file, "ComponentDisabledText" );

		// The stock fit (read directly, not through the defs overlay) is 210 wide; the list is kDx narrower.
		const float dx = mc2::mechlab_loc::kDx;
		auto& t = *s_templateItem;
		t.outline.resize( t.outline.width() - static_cast<long>(dx), t.outline.height() );

	}

	return 0;
}

void ComponentListItem::assignAnimation( FitIniFile& file, int whichChild, char animNames[COMP_ANIMATION_COUNT][32], 
										aObject* object )
{
		s_templateItem->pChildAnims[whichChild] = nullptr;
		char tmpAnimName[32];
  		file.readIdString("Animation", tmpAnimName, 31);
		for ( int i = 0; i < COMP_ANIMATION_COUNT; i++ )
		{
			if ( S_stricmp( animNames[i], tmpAnimName ) == 0 )
			{
				s_templateItem->pChildAnims[whichChild] = &s_templateItem->animations[i];
				break;
			}
		}

		s_templateItem->addChild( object );

}

void ComponentListItem::render( )
{
	aListItem::render();

	// draw rects, these aren't children
	outline.moveTo(globalX(), globalY());
	outline.setColor( animations[0].getCurrentColor( animations[0].getState() ) );
	outline.render();

	if ( pComponent )
	{
		const mc2::mechlab_loc::ItemRects R = mc2::mechlab_loc::componentItemRects( pComponent->getComponentWidth(), pComponent->getComponentHeight() );
		iconOutline.moveTo( globalX() + R.iconOutline.x, globalY() + R.iconOutline.y );
	}
	iconOutline.setColor( animations[0].getCurrentColor( animations[0].getState() ) );
	iconOutline.render();

	for ( int i = 0; i < 6; i++ )
	{
		if ( pChildAnims[i] )
		{
			long color = pChildAnims[i]->getCurrentColor( pChildAnims[i]->getState() );
			child(i)->setColor( color );
		}
	}


	if ( MechLabScreen::instance()->canAddComponent( pComponent ) )
	{
		icon.setColor( 0xff404040 );
	}
	else
		icon.setColor( 0xffffffff );

}

void	ComponentListItem::update()
{

	int bCanAdd = MechLabScreen::instance()->canAddComponent( pComponent );
	for (auto & animation : animations)
	{
		animation.update();
	}
	bool isInside = pointInside( userInput->getMouseX(), userInput->getMouseY() );

	if ( state == aListItem::SELECTED ) 
	{
		if ( (userInput->isLeftClick() && isInside)
			|| ( animations[0].getState() != aAnimGroup::PRESSED )
			&& ComponentIconListBox::s_instance->pointInside( userInput->getMouseX(), userInput->getMouseY() ))
		{
			setComponent();

			if ( bCanAdd )
			{
				soundSystem->playDigitalSample( LOG_WRONGBUTTON );
			}

			::helpTextID = IDS_HELP_COMP0 + pComponent->getID();

		}

		if ( !bCanAdd )
		{
			if ( animations[0].getState() != aAnimGroup::PRESSED )
			{
				for (auto & animation : animations)
				{
					animation.setState( aAnimGroup::PRESSED );
					
				}
			}
			if ( isInside && ComponentIconListBox::s_instance->pointInside( userInput->getMouseX(), userInput->getMouseY() ) ) 
			{
				::helpTextID = IDS_HELP_COMP0 + pComponent->getID();
			}
			disabledText.setText( "" );
		}
		else
		{
			for (auto & animation : animations)
				animation.setState( aAnimGroup::DISABLED );

			if ( COMPONENT_TOO_HOT == bCanAdd )
			{
				disabledText.setText( IDS_MC_COMPONENT_TOO_HOT );
			}
			else if ( NO_MORE_ARMOR == bCanAdd )
			{
				disabledText.setText( IDS_MC_COMPONENT_TOO_MUCH_ARMOR );
			}
			else
				disabledText.setText( "" );


		}
		
	}
	else if ( isInside && !bCanAdd
		&& ComponentIconListBox::s_instance->pointInside( userInput->getMouseX(), userInput->getMouseY() ))
	{
		if ( animations[0].getState() != aAnimGroup::HIGHLIGHT )
		{
			for (auto & animation : animations)
			{
				animation.setState( aAnimGroup::HIGHLIGHT );
			}

			soundSystem->playDigitalSample( LOG_HIGHLIGHTBUTTONS );

		}
		state = aListItem::HIGHLITE;
		::helpTextID = IDS_HELP_COMP0 + pComponent->getID();
	}
	else if ( !bCanAdd )
	{
		state = aListItem::ENABLED;
		for (auto & animation : animations)
			animation.setState( aAnimGroup::NORMAL );

		disabledText.setText( "" );
	}
	else
	{
		state = DISABLED;

		if ( isInside
			&& ComponentIconListBox::s_instance->pointInside( userInput->getMouseX(), userInput->getMouseY() ))
			::helpTextID = IDS_HELP_COMP0 + pComponent->getID();


		if ( (userInput->isLeftClick() && isInside
			&& ComponentIconListBox::s_instance->pointInside( userInput->getMouseX(), userInput->getMouseY() ) ) )
		{
			MechLabScreen::instance()->inspectComponent( pComponent );
			setComponent();
			soundSystem->playDigitalSample( LOG_WRONGBUTTON );
		}

		for (auto & animation : animations)
			animation.setState( aAnimGroup::DISABLED );

		if ( COMPONENT_TOO_HOT == bCanAdd )
		{
			disabledText.setText( IDS_MC_COMPONENT_TOO_HOT );
		}
		else if ( NO_MORE_ARMOR == bCanAdd )
		{
			disabledText.setText( IDS_MC_COMPONENT_TOO_MUCH_ARMOR );
		}
		else
			disabledText.setText( "" );
	}

	if (!bCanAdd && input::capture::startsWidgetDrag(userInput->isLeftDrag(),
		pointInside(userInput->getMouseDragX(), userInput->getMouseDragY()) &&
		ComponentIconListBox::s_instance->pointInside(userInput->getMouseDragX(), userInput->getMouseDragY())))
		startDrag();

	if ( userInput->isLeftDoubleClick() && isInside 
		&& ComponentIconListBox::s_instance->pointInside( userInput->getMouseX(), userInput->getMouseY() ) )
		doAdd();

	aObject::update();
}


void ComponentListItem::doAdd()
{
	long x = -1;
	long y = -1;
	MechLabScreen::instance()->addComponent( pComponent, x, y);
}

void ComponentListItem::setComponent()
{
	MechLabScreen::instance()->setComponent( pComponent, 1 );
}

void ComponentListItem::startDrag()
{
	MechLabScreen::instance()->beginDrag( pComponent );
}
//*************************************************************************************************

void ComponentIconListBox::setType( int newType, int otherNewType, int orThis, int orThis2 )
{
	if ( newType == type && itemCount )
		return;

	type = newType;
	scrollBar->setGreen();

	
	removeAllItems( 0 );

	itemSelected = -1;

	
	if ( !std::ssize(masterComponentList) )
	{
		int count = 256;
		LogisticsComponent* pComp[256];
		LogisticsData::instance->getAllComponents( pComp, count );
		//for ( int j = 0; j < 2048; j++ )
		//{
			for ( int i = 0; i < count; i++ )
			{
				masterComponentList.push_back( std::make_unique<ComponentListItem>( pComp[i] ) );
			}			
	

	}

	for (auto & masterComponentElem : masterComponentList)
		{
			if ( masterComponentElem->getComponent()->isAvailable() )
			{
				if ( masterComponentElem->getComponent()->getType() == type ||
					masterComponentElem->getComponent()->getType() == otherNewType ||
					 masterComponentElem->getComponent()->getType() == orThis ||
					 ( orThis2 != -1 && masterComponentElem->getComponent()->getType() == orThis2 ) )
				{
					// Template items sit at the stock list origin; the list is kDx narrower on its left.
					const long listOriginX = globalX() - static_cast<long>(mc2::mechlab_loc::kDx);
					// A revisited item already sits at globalX() + offset: strip the current origin so AddItem puts it
					// back at the same x every revisit (not +kDx each time).
					if ( masterComponentElem->globalX() >= globalX() )
					{
						masterComponentElem->move( -globalX(), 0 );
					}
					else if ( masterComponentElem->globalX() > listOriginX )
					{
						masterComponentElem->move( -listOriginX, 0 );
					}
					addSortedItem( masterComponentElem.get() );
				}
			}
		}

	selectFirstAvailableComponent();
}

void ComponentIconListBox::update()
{
	aListBox::update();

	if ( itemSelected != -1 )
	{
		if ( items[itemSelected]->getState() == aListItem::DISABLED
			|| MechLabScreen::instance()->canAddComponent( 
			(static_cast<ComponentListItem*>(items[itemSelected]))->getComponent() ) )
		{
			selectFirstAvailableComponent();
		}
	}
} 

int ComponentIconListBox::selectFirstAvailableComponent()
{
	bool bFound = 0;
	for ( int i = 0; i < itemCount; i++ )
	{
		if ( items[i]->getState() != aListItem::DISABLED
			&& !MechLabScreen::instance()->canAddComponent( 
			(static_cast<ComponentListItem*>(items[i]))->getComponent() ))
		{
			SelectItem( i );
			(static_cast<ComponentListItem*>(items[i]))->setComponent();
			bFound = true;
			return i;
		}
	}

	if ( !bFound )
		SelectItem( -1 );

	return -1;
}

ComponentIconListBox::ComponentIconListBox()
{
	skipAmount = 5;
	type = -1;
	s_instance = this;

}

ComponentIconListBox::~ComponentIconListBox()
{
	destroy();

	s_instance = nullptr;

	ComponentListItem::s_templateItem.reset();
	ComponentListItem::s_templateItem = nullptr;
}

void ComponentIconListBox::destroy()
{
	// The listbox only borrows the visible subset; release its views before the owners.
	removeAllItems( false );
	masterComponentList.clear();
	aListBox::destroy();
}

LogisticsComponent* ComponentIconListBox::getComponent()
{
	if ( itemSelected != -1 )
	{
		return (static_cast<ComponentListItem*>(items[itemSelected]))->pComponent;
	}

	return nullptr;
}

void ComponentIconListBox::addSortedItem( ComponentListItem* pItem )
{
	int size = pItem->getComponent()->getComponentHeight() * 
				pItem->getComponent()->getComponentWidth();

	for ( int i = 0; i < itemCount; i++ )
	{
		LogisticsComponent* pTmp = (static_cast<ComponentListItem*>(items[i]))->getComponent();
		long tmpSize = pTmp->getComponentHeight() * pTmp->getComponentWidth();
		if ( size > tmpSize )
		{
			InsertItem( pItem, i );
			return;
		}
		else if ( size == tmpSize && 
			S_stricmp( pItem->getComponent()->getName(), pTmp->getName() ) < 0 )
		{
			InsertItem( pItem, i );
			return;
		}
	}

	
	aListBox::AddItem( pItem );
}



	
//*************************************************************************************************
// end of file ( ComponentListBox.cpp )
