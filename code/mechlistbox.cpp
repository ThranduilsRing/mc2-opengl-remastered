#include "utils/mc2log.h"
#define MECHLISTBOX_CPP
/*************************************************************************************************\
MechListBox.cpp			: Implementation of the MechListBox component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include <cstdio>
#include "../mclib/texture_assets.h"
#include "Gates/gate.h"
#include <cstdlib>
#include"mechlistbox.h"
#include "icon_registry.h"
#include"logisticsmech.h"
#include"paths.h"
#include"inifile.h"
#include"err.h"
#include"userinput.h"
#include"mechbayscreen.h"
#include"logisticsdata.h"
#include"mechpurchasescreen.h"
#include"gamesound.h"

// Interactive cheat gate (set in logisticsdata.cpp; extern here for UI bypass)
// The launcher writes these into launcher_env.json, which main() applies after static initialization, so they
// are read at first use rather than in a namespace-scope initializer.
static bool cheatInfiniteMoney() { static const bool on = mc2::gate::get<"MC2_CHEAT_INFINITE_MONEY">(); return on; }
#include"render_submissions.h"
#include "mc2fmt.h"
#include "mc2str.h"

std::unique_ptr<MechListBoxItem> MechListBoxItem::s_templateItem = nullptr;

bool MechListBox::s_DrawCBills = true;
bool MechListBoxItem::bAddCalledThisFrame = 0;



MechListBox::MechListBox( bool bDel, bool bInclude  )
{
	bDeleteIfNoInventory = bDel;
	bIncludeForceGroup = bInclude;
	bOrange = 0;
	skipAmount = 5;
}

//-------------------------------------------------------------------------------------------------

MechListBox::~MechListBox()
{
	removeAllItems( true );

	MechListBoxItem::s_templateItem.reset();
	MechListBoxItem::s_templateItem = nullptr;
}

void	MechListBox::setScrollBarOrange()
{
	scrollBar->setOrange();
}
void	MechListBox::setScrollBarGreen()
{
	scrollBar->setGreen();
}

void	MechListBox::drawCBills( bool bDraw )
{
	s_DrawCBills = bDraw;
}

void MechListBox::update()
{
	aListBox::update();
	MechListBoxItem::bAddCalledThisFrame = false;

	if ( bDeleteIfNoInventory )
	{
		for ( int i = 0; i < itemCount; i++ )
		{
			if ( (static_cast<MechListBoxItem*>(items[i]))->mechCount == 0 )
			{
				RemoveItem( items[i], true );
				i--;
				disableItemsThatCanNotGoInFG();


				// find better thing to select if necessary
				if ( itemSelected >= itemCount || itemSelected == -1 
					|| items[itemSelected]->getState() == aListItem::DISABLED
				//	|| !LogisticsData::instance->canAddMechToForceGroup( ((MechListBoxItem*)items[itemSelected])->getMech()  )
				)
				{
					if ( itemCount )
					{
						for ( int j = 0; j < itemCount; j++ )
							if ( items[j]->getState() != aListItem::DISABLED )
							{
								SelectItem( j );
								break;
							}
						
					}
					else
						itemSelected = -1;					
				}
				
			}
		}
	}
}

LogisticsMech* MechListBox::getCurrentMech()
{
	if ( itemSelected != -1 )
	{
		return (static_cast<MechListBoxItem*>(items[itemSelected]))->pMech;
	}

	return nullptr;
}



int MechListBox::init()
{
	if ( MechListBoxItem::s_templateItem )
		return 0;


	char path[256];
	strcpy( path, artPath );
	strcat( path, "mcl_gn_availablemechentry.fit" );
	FitIniFile file;
	if ( NO_ERR != file.open( path ) )
	{
		char errorStr[256];
		mc2::fmt::to_buf(errorStr, "couldn't open file {}", mc2::fmt::safe(path));
		Assert( 0, 0, errorStr );
		return -1;
	}

	MechListBoxItem::init( file );
	
	return 0;
}

//-------------------------------------------------------------------------------------------------
bool	MechListBoxItem::pointInside(long xPos, long yPos) const
{

	int minX = location[0].x + outline.globalX();
	int minY = location[0].y + outline.globalY();
	int maxX = location[0].x + outline.globalX() + outline.width();
	int maxY = location[0].y + outline.globalY() + outline.height();

	if ( minX < xPos && xPos < maxX
		&& minY < yPos && yPos < maxY )
		return true;

	return 0;
}
MechListBoxItem::MechListBoxItem( LogisticsMech* pRefMech, long count )
{
	
	state = ENABLED; // sebi: init so will not be garbage

	bIncludeForceGroup = 0;
	bOrange = 0;
	if ( s_templateItem )
	{
		*this = *s_templateItem;
	}

	animTime = 0.f;

	pMech = pRefMech;
	if ( !pMech )
		return;

	aObject::init( 0, outline.top(), outline.width(), outline.height() );
	setColor( 0, 0 );

	chassisName.setText( pMech->getChassisName() );
	
	char text[32];
	mc2::fmt::to_buf(text, "{:d}", mc2::fmt::as_i32(pMech->getCost()));
	costText.setText( text );

	mechCount = LogisticsData::instance->getVariantsInInventory( pRefMech->getVariant(), bIncludeForceGroup );
	mc2::fmt::to_buf(text, "{:d}", mc2::fmt::as_i32(mechCount));
	countText.setText( text );


	MechListBox::initIcon( pRefMech, mechIcon );

	variantName.setText( pMech->getName() );
	
	mc2::fmt::to_buf(text, "{:.0f}", pMech->getMaxWeight());
	weightText.setText( text );

	// ---- MC2_LOG_LOGISTICS mech-list item diagnostic (env-gated, no behavior change) ----
	if ( pMech && false ) {
		char chassisBuf[256] = "";
		cLoadString( pMech->getChassisName(), chassisBuf, sizeof(chassisBuf) );
		bool mechListLog = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info);
		if ( mechListLog ) {
			MC2_LOG(Ui, Info, "[MECHLIST] item chassisNameID={:d} chassisText='{}' variantName='{}' cost={:d} weight={:.0f}", mc2::fmt::as_i32(pMech->getChassisName()), mc2::fmt::safe(chassisBuf), pMech->getName(), mc2::fmt::as_i32(pMech->getCost()), pMech->getMaxWeight());


		}
	}

	addChild( &weightIcon );
	addChild( &mechIcon );
	addChild( &costIcon );

	addChild( &chassisName );
	addChild( &weightText );
	addChild( &countText );
	addChild( &variantName );
	addChild( &costText );

	//	addChild( &line );
	//	addChild( &outline );

	bDim = 0;

}

MechListBoxItem::~MechListBoxItem()
{
	removeAllChildren( false );
}

void MechListBoxItem::init( FitIniFile& file )
{
	if ( !s_templateItem )
	{
		s_templateItem = std::make_unique<MechListBoxItem>( nullptr, 0 );
		file.seekBlock( "MainBox" );

		long width, height;

		file.readIdLong( "Width", width );
		file.readIdLong( "Height", height );

		(static_cast<aObject*>(s_templateItem.get()))->init( 0, 0, width, height );

		memset( s_templateItem->animationIDs, 0, sizeof(long) * 9  );

		// rects
		s_templateItem->line.init( &file, "Rect1" );
		s_templateItem->outline.init( &file, "Rect0" );

		long curAnim = 0;
		// statics
		s_templateItem->weightIcon.init( &file, "Static0" );
		assignAnimation( file, curAnim );
	
		s_templateItem->mechIcon.init( &file, "Static1" );
		assignAnimation( file, curAnim );
		s_templateItem->costIcon.init( &file, "Static2" );
		assignAnimation( file, curAnim );

		// texts
		s_templateItem->chassisName.init( &file, "Text0" );
		assignAnimation( file, curAnim );
		s_templateItem->weightText.init( &file, "Text1" );
		assignAnimation( file, curAnim );
		s_templateItem->countText.init( &file, "Text2" );
		assignAnimation( file, curAnim );
		s_templateItem->variantName.init( &file, "Text3" );
		assignAnimation( file, curAnim );
		s_templateItem->costText.init( &file, "Text4" );
		assignAnimation( file, curAnim );

		char blockName[64];
		for ( int i = 0; i < 4; i++ )
		{
			mc2::fmt::to_buf(blockName, "OrangeAnimation{:d}", mc2::fmt::as_i32(i));
			s_templateItem->animations[1][i].init( &file, blockName );
			mc2::fmt::to_buf(blockName, "Animation{:d}", mc2::fmt::as_i32(i));
			s_templateItem->animations[0][i].init( &file, blockName );
		}
		
	}

}

void MechListBoxItem::assignAnimation( FitIniFile& file, long& curAnim )
{
	char tmpStr[64];

	s_templateItem->animationIDs[curAnim] = -1;
	if ( NO_ERR == file.readIdString( "Animation", tmpStr, 63 ) )
	{
		for ( int j = 0; j < strlen( tmpStr ); j++ )
		{
			if ( isdigit( tmpStr[j] ) )
			{
				tmpStr[j+1] = 0;
				s_templateItem->animationIDs[curAnim] = atoi( &tmpStr[j] );
			}
		}
	}
	curAnim++;
}
MechListBoxItem& MechListBoxItem::operator=( const MechListBoxItem& src )
{
	if ( &src != this )
	{
		chassisName = src.chassisName;
		costIcon = src.costIcon;
		costText = src.costText;
		line = src.line;
		mechIcon = src.mechIcon;
		outline = src.outline;
		variantName = src.variantName;
		weightIcon = src.weightIcon;
		weightText = src.weightText;
		countText = src.countText;
		for ( int i = 0; i < ANIMATION_COUNT; i++ )
		{
			animations[0][i] = src.animations[0][i];
			animations[1][i] = src.animations[1][i];
		}

		for (int i = 0; i < 9; i++ )
		{
			animationIDs[i] = src.animationIDs[i];
		}
	}

	return *this;
}

void MechListBoxItem::update()
{
	char text[32];
	int oldMechCount = mechCount;
	if ( !pMech )
	{
		mechCount = 0;
		return;
	}
	mechCount = LogisticsData::instance->getVariantsInInventory( pMech->getVariant(), bIncludeForceGroup );
	if ( oldMechCount != mechCount )
	{
		animTime = .0001f;
	}
	mc2::fmt::to_buf(text, "{:d}", mc2::fmt::as_i32(mechCount));
	countText.setText( text );
	if ( animTime )
	{
		if ( animTime < .25f 
			|| ( animTime > .5f && animTime <= .75f ) )
		{
			countText.setColor( 0 );
		}
		else 
			countText.setColor( 0xffffffff );

		animTime += frameLength;
		
		if ( animTime > 1.0f )
			animTime = 0.f;
		
	}

	bool isInside = pointInside( userInput->getMouseX(), userInput->getMouseY() );


	for ( int i = 0; i < ANIMATION_COUNT; i++ )
		animations[bOrange][i].update();

	if ( state == aListItem::SELECTED ) 
	{
		for ( int i = 0; i < ANIMATION_COUNT; i++ )
			animations[bOrange][i].setState( aAnimGroup::PRESSED );

	//	if ( userInput->isLeftClick() && isInside )
	//		setMech();
		
		if ( userInput->isLeftDrag() &&
			pointInside( userInput->getMouseDragX(), userInput->getMouseDragY() ) )
			startDrag();

	}
	else if ( state == aListItem::HIGHLITE )
	{
		for ( int i = 0; i < ANIMATION_COUNT; i++ )
			animations[bOrange][i].setState( aAnimGroup::HIGHLIGHT );

	}
	else if ( state == aListItem::DISABLED &&  isShowing() )
	{
		if ( userInput->isLeftClick() && isInside )
		{
			soundSystem->playDigitalSample( LOG_WRONGBUTTON );	
			setMech(); // need to call explicitly
		}

		for ( int i = 0; i < ANIMATION_COUNT; i++ )
			animations[bOrange][i].setState( aAnimGroup::DISABLED );
	}
	else
	{
		for ( int i = 0; i < ANIMATION_COUNT; i++ )
			animations[bOrange][i].setState( aAnimGroup::NORMAL );
	}

	if ( userInput->isLeftDoubleClick() && isInside && state != aListItem::DISABLED && isShowing() )
		doAdd();

	aObject::update();
}



void MechListBoxItem::render()
{
	if ( !MechListBox::s_DrawCBills )
	{
		costText.showGUIWindow( 0 );
		costIcon.showGUIWindow( 0 );
	}
	else
	{
		costText.showGUIWindow( 1 );
		costIcon.showGUIWindow( 1 );

	}


	for ( int i = 0; i < this->pNumberOfChildren; i++ )
	{
		long index = animationIDs[i];
		if ( index != -1 )
		{
			if ( pChildren[i]->isShowing() )
			{
				if ( !animTime || pChildren[i] != &countText )
				{
					long color = animations[bOrange][index].getCurrentColor( animations[bOrange][index].getState());
					pChildren[i]->setColor( color );
				}

				
			}
		}
		pChildren[i]->render();

	}

	if ( bDim )
	{
		mechIcon.setColor( 0xa0000000 );
		mechIcon.render();
	}
		
	outline.setColor(animations[bOrange][2].getCurrentColor(animations[bOrange][2].getState()));
	outline.render( location[0].x, location[0].y );

	line.setColor(animations[bOrange][2].getCurrentColor(animations[bOrange][2].getState()));
	line.render(location[0].x, location[0].y);

}

void MechListBoxItem::setMech()
{
	MechBayScreen::instance()->setMech( pMech );
	MechPurchaseScreen::instance()->setMech( pMech, true );
	
}

void MechListBoxItem::startDrag()
{
	if ( state != DISABLED )
	{
		MechBayScreen::instance()->beginDrag( pMech );
		MechPurchaseScreen::instance()->beginDrag( pMech );
	}
}

void MechListBoxItem::doAdd()
{
	if ( !bAddCalledThisFrame ) // only select one, sometimes we auto scroll, don't want to be selecting each time
	{
		MechBayScreen::instance()->handleMessage( ID, MB_MSG_ADD );
		MechPurchaseScreen::instance()->handleMessage( ID, MB_MSG_ADD );
		bAddCalledThisFrame = true;
	}
}

void MechListBox::initIcon( LogisticsMech* pMech, aObject& mechIcon )
{
	mechIcon = (MechListBoxItem::s_templateItem->mechIcon);

    unit_icons::apply(mechIcon, MechListBoxItem::s_templateItem->mechIcon,
                      pMech->getIconIndex(), pMech->getFileName().c_str());

}

long MechListBox::AddItem(std::unique_ptr<aListItem> itemString)
{
	if (itemCount >= MAX_LIST_ITEMS)
		return TOO_MANY_ITEMS;
	itemString->setID( ID );
	MechListBoxItem* pItem = dynamic_cast<MechListBoxItem*>(itemString.get());
	char tmp[256];
	cLoadString( pItem->getMech()->getChassisName(), tmp, 255 );
	const std::string addedName = tmp;
	
	if ( pItem )
	{
		pItem->bOrange = bOrange;
		pItem->bIncludeForceGroup = bIncludeForceGroup;

		if ( !bDeleteIfNoInventory )
		{
			pItem->countText.setColor( 0 );
			pItem->countText.showGUIWindow( 0 );
		}
	
		// Same-weight mechs order by chassis display name, ignoring ASCII case (the stock mech names order the same as under the former locale collation), then by variant.
		for ( int i = 0; i < itemCount; i++ )
		{

			long ID = (static_cast<MechListBoxItem*>(items[i]))->pMech->getChassisName();
			char tmpChassisName[256];
			cLoadString( ID, tmpChassisName, 255 );
			const std::string chassisName = tmpChassisName;
			if ( (static_cast<MechListBoxItem*>(items[i]))->pMech->getMaxWeight() < pItem->pMech->getMaxWeight() )
			{
				return InsertItem( itemString.release(), i );
				break;
			}
			else if ( (static_cast<MechListBoxItem*>(items[i]))->pMech->getMaxWeight() == pItem->pMech->getMaxWeight()
				&& mc2::str::icompare( chassisName, addedName ) > 0 )
			{
				return InsertItem( itemString.release(), i );
			}
			else if ( (static_cast<MechListBoxItem*>(items[i]))->pMech->getMaxWeight() == pItem->pMech->getMaxWeight()
				&& mc2::str::iequals( chassisName, addedName )
				&& (static_cast<MechListBoxItem*>(itemString.get()))->pMech->getName().find("Prime") != std::string::npos )
			{
				return InsertItem( itemString.release(), i );
			}
			else if ( (static_cast<MechListBoxItem*>(items[i]))->pMech->getMaxWeight() == pItem->pMech->getMaxWeight()
				&& mc2::str::iequals( chassisName, addedName )
				&& ( (static_cast<MechListBoxItem*>(items[i]))->pMech->getName().find("Prime") == std::string::npos )
				// variants of one chassis sort by name, ignoring ASCII case, then by raw bytes
				&& mc2::str::ci_total_less{}( pItem->pMech->getName(), (static_cast<MechListBoxItem*>(items[i]))->pMech->getName() ) )
			{
				return InsertItem( itemString.release(), i );
			}
		}

	}

	
	return aListBox::AddItem( std::move(itemString) );
}

void	MechListBox::dimItem( LogisticsMech* pMech, bool bDim )
{
		for ( int i = 0; i < itemCount; i++ )
		{
			if ( (static_cast<MechListBoxItem*>(items[i]))->pMech == pMech )
			{
				
				(static_cast<MechListBoxItem*>(items[i]))->bDim = bDim;
			}
		}
		
}

void MechListBox::undimAll()
{
	for ( int i = 0; i < itemCount; i++ )
	{
			
			(static_cast<MechListBoxItem*>(items[i]))->bDim = 0;
		
	}
}

void MechListBox::disableItemsThatCostMoreThanRP()
{
	bool bDisabledSel = 0;
	for ( int i = 0; i < itemCount; i++ )
	{
		if ( !cheatInfiniteMoney() && (static_cast<MechListBoxItem*>(items[i]))->pMech->getCost() > LogisticsData::instance->getCBills() )
		{
			items[i]->setState( aListItem::DISABLED );
			if ( itemSelected == i )
				bDisabledSel = true;
		}
		else
		{
			if ( items[i]->getState() == aListItem::DISABLED )
				items[i]->setState( aListItem::ENABLED );
		}
	}

	if ( bDisabledSel )
	{
		for (int i = 0; i < itemCount; i++ )
		{
			if ( items[i]->getState() != aListItem::DISABLED )
			{
				SelectItem( i );
				bDisabledSel = 0;
				break;
			}
		}

		if ( bDisabledSel )
			SelectItem( -1 );
	}
}

void MechListBox::disableItemsThatCanNotGoInFG()
{
	bool bDisabledSel = 0;
	for ( int i = 0; i < itemCount; i++ )
	{
		if ( !LogisticsData::instance->canAddMechToForceGroup( (static_cast<MechListBoxItem*>(items[i]))->pMech ) )
		{
			if ( itemSelected == i )
				bDisabledSel = true;
			items[i]->setState( aListItem::DISABLED );
		}
		else
		{
			if ( items[i]->getState() == aListItem::DISABLED )
				items[i]->setState( aListItem::ENABLED );
		}
	}

	if ( bDisabledSel )
	{
		for (int i = 0; i < itemCount; i++ )
		{
			if ( items[i]->getState() != aListItem::DISABLED )
			{
				SelectItem( i );
				bDisabledSel = 0;
				break;
			}
		}

		if ( bDisabledSel )
			SelectItem( -1 );
	}
}

void MechListBox::setOrange( bool bNewOrange )
{
	bOrange = bNewOrange ? 1 : 0;

	for ( int i= 0; i < itemCount; i++ )
	{
		(static_cast<MechListBoxItem*>(items[i]))->bOrange = bOrange;
	}

	if ( bNewOrange )
		scrollBar->setOrange( );
	else
		scrollBar->setGreen();

}




//*************************************************************************************************
// end of file ( MechListBox.cpp )
