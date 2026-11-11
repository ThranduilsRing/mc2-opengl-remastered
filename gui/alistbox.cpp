//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef ASYSTEM_H
#include"asystem.h"
#endif

#ifndef AFONT_H
#include"afont.h"
#endif

#ifndef ALISTBOX_H
#include"alistbox.h"
#endif

#include"soundsys.h"

#include "sound.h"

#include"mclib.h"
#include "../GuiRuntime/GuiRuntime.h"

///////////////////////////////////////////////////////////////////////////////
// aListBox methods
///////////////////////////////////////////////////////////////////////////////

aListBox::aListBox()
{
	itemCount = 0;
	itemSelected = -1;
	memset( items, 0, sizeof( aListItem*) * MAX_LIST_ITEMS );
	scrollBar = nullptr;
	skipAmount = 1;
	singleCheck = 1;
	topSkip = 0;

	clickSFX = LOG_SELECT;
	highlightSFX = LOG_DIGITALHIGHLIGHT;
	disabledSFX = LOG_WRONGBUTTON;

}

long aListBox::init(long xPos, long yPos,long w, long h )
{
	long err;
	
	err = aObject::init(xPos,yPos,w,h);
	if (err)
		return err;

	//If you call the FitIni INIT and then this init afterwards, we leak one.
	if (scrollBar)
	{
		scrollBar->destroy();
		delete scrollBar;
		scrollBar = nullptr;
	}

	scrollBar = new mcScrollBar;
	scrollBar->init( w - 17, 2, 15, h-4 );
	scrollBar->SetScrollMax( 0 );
	addChild( scrollBar );

	return (NO_ERR);
}

void aListBox::init( FitIniFile* file, const char* blockName )
{
	long x, y, width, height;
	file->readIdLong( "XLocation", x );
	file->readIdLong( "YLocation", y );
	file->readIdLong( "Width", width );
	file->readIdLong( "Height", height );

	file->readIdLong( "HelpCaption", helpHeader );
	file->readIdLong( "HelpDesc", helpID );


	init( x, y, width, height );

}

void aListBox::destroy()
{
	if (scrollBar)
	{
		scrollBar->destroy();
		delete scrollBar;
		scrollBar = nullptr;
	}

	removeAllItems( true );
	
	aObject::destroy();
}

void aListBox::removeAllItems( bool bDelete )
{
    // sebi: WTF??? always true
	//if (items)
	{
		for ( int i = 0; i < itemCount; i++ )
		{
			if ( items[i] && bDelete )
				delete items[i];

			items[i] = nullptr;
		}
	}

	itemCount = 0;
	itemSelected = -1;

	if ( scrollBar )
		scrollBar->SetScroll( 0 );

}

void aListBox::update()
{
	if ( showWindow )
	{ /* if the listbox is invisible, then we probably don't want to update it either */
		if ( scrollBar )
			scrollBar->update();


		int prevCheck = GetCheckedItem();
		int newCheck = -1;


		int iSel = -1;
		int iHighlight = -1;
		long mouseX = userInput->getMouseX();
		long mouseY = userInput->getMouseY();

		bool bInside = pointInside( mouseX, mouseY );
		if ( bInside && helpID )
		{
			::helpTextID = helpID;
		}

		// Mouse-wheel scrolling when the cursor is over the list.  wheel > 0 =
		// scroll down (matches the defs GuiList).  scroll()'s own guards prevent
		// running past the top/bottom, so the GetScrollMax clamp is a backstop.
		if ( bInside && scrollBar )
		{
			long wheel = userInput->getMouseWheelDelta();
			if ( wheel != 0 )
			{
				const float step = 40.f;
				float newPos = scrollBar->GetScrollPos() + ( wheel > 0 ? step : -step );
				if ( newPos < 0.f )
					newPos = 0.f;
				if ( newPos > scrollBar->GetScrollMax() )
					newPos = scrollBar->GetScrollMax();
				setScrollPos( (int)newPos );
			}
		}

		for ( int i = 0; i < itemCount; i++ )
		{
			if ( bInside &&
				items[i]->pointInside(mouseX, mouseY) && items[i]->isShowing()
				&& items[i]->getState() != aListItem::DISABLED )
			{
				if ( userInput->isLeftClick() )
				{
					iSel = i;
					if ( items[i]->getState() != aListItem::SELECTED )
						mc2::gui::playSound( clickSFX );
				}
				else 
				{
					iHighlight = i;
				}
			}

			if ( items[i]->globalBottom() > globalTop()
				&& items[i]->globalTop() < globalBottom() )
				{
					items[i]->showGUIWindow( 1 );
				}
				else
				{
					items[i]->showGUIWindow( 0 );
				}
		}

		for (int i = 0; i < itemCount; i++ )
		{
			if ( iSel == i )
			{
				items[i]->setState( aListItem::SELECTED );	
				itemSelected = i;
			}
			else if ( iHighlight == i && items[i]->getState() != aListItem::SELECTED )
			{
				if ( items[i]->getState() != aListItem::HIGHLITE )
						mc2::gui::playSound( highlightSFX );
				items[i]->setState( aListItem::HIGHLITE );				
			}
			else if ( (iSel != -1 || items[i]->getState() != aListItem::SELECTED)
				&& items[i]->getState() != aListItem::DISABLED )
				items[i]->setState( aListItem::ENABLED );

			items[i]->update();

		}

		for (int i = 0; i < itemCount; i++ )
		{
			if ( items[i]->isChecked() && i != prevCheck )
			{
				newCheck = i;
			}
		}

		if ( singleCheck && newCheck != -1 && prevCheck != -1)
		{
			items[prevCheck]->setCheck( 0 );
		}
	}

	


}

	
int aListBox::handleMessage( unsigned long message, unsigned long who )
{
	switch ( message )
	{
	case aMSG_SCROLLUP:
		scroll( -1 );
		break;

	case aMSG_SCROLLDOWN:
		scroll( 1 );
		break;

	case aMSG_PAGEUP:
		scroll( 10 );
		break;

	case aMSG_PAGEDOWN:
		scroll( -10 );
		break;

	case aMSG_SCROLLTO:
		{
			scroll( -scrollBar->GetScrollPos() );
			scroll( who );
		}
		break;
	}

	return 0;
}

void aListBox::resize(long w, long h)
{
	aObject::resize(w, h);
	scrollBar->resize(scrollBar->width(), h - 4);
}

void aListBox::setScrollPos( int pos )
{
	scroll( pos - scrollBar->GetScrollPos() );
	scrollBar->SetScroll( pos );
}



void aListBox::render()
{
	if ( showWindow )
	{
		bool bTop  =0;
		bool bBottom = 0;

		gos_SetRenderState( gos_State_Texture, 0 );
		gos_SetRenderState(gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
		gos_SetRenderState( gos_State_ZCompare, 0 );
		gos_SetRenderState( gos_State_ZWrite, 0 );

		gos_DrawQuads( location, 4 );

		long topHeight = 0;
		long bottomHeight = 0;

		// when this list renders through the GUI bridge (ImGui
		// layer over a defs page), partially-scrolled edge items must clip at the
		// box bounds. The legacy technique below -- painting black cover rects
		// OVER the spill -- would black out defs page art, so bridge mode uses a
		// real clip rect instead and skips the covers.
		float bSx = 1.f, bSy = 1.f, bOx = 0.f, bOy = 0.f;
		const bool bBridged = aObject::getGuiBridgeTransform( bSx, bSy, bOx, bOy );
		bool bClipPushed = false;
		if ( bBridged )
			bClipPushed = GuiRuntime::PushUiClipRect(
				globalX() * bSx + bOx, globalY() * bSy + bOy,
				width() * bSx, height() * bSy );

		bool bItemOutOfRange = 0;
		for ( int i = 0; i < itemCount; i++ )
		{
			if ( items[i]->globalBottom() > globalTop()
				&& items[i]->globalTop() < globalBottom() )
			{
				items[i]->render();

				if ( items[i]->globalTop() < globalTop() )
				{
					topHeight = items[i]->height();
					bTop = 1;
					bItemOutOfRange = true;
				}
				if ( items[i]->globalBottom() >= globalBottom() )
				{
					bBottom = 1;
					if ( !bottomHeight )
						bottomHeight = items[i]->height();

					bItemOutOfRange = true;
				}
			}
			else 
			{
				bItemOutOfRange = true;
			}
		}

		if ( bClipPushed )
			GuiRuntime::PopUiClipRect();

		// draw black box above this
		if ( !bBridged && ( bTop || bBottom ) )
		{
			GUI_RECT rect = { globalX(), globalY() - topHeight, (globalX() + (long)width()), globalY() };
			if ( bTop )
				drawRect( rect, 0xff000000 ); 
			rect.top = globalY() + height()+1;
			rect.bottom =globalY() + height() + bottomHeight+2;
			if ( bBottom )
				drawRect( rect, 0xff000000 );
		}
		if ( scrollBar )
		{
			if ( bItemOutOfRange )
			{
				scrollBar->Enable( 1 );
			}

			else 
				scrollBar->Enable( 0 );

			scrollBar->render();
		}

			
	}
}

void aListBox::move( float offsetX, float offsetY )
{
	aObject::move( offsetX, offsetY );
	for ( int i = 0; i < itemCount; i++ )
	{
		items[i]->move( offsetX, offsetY );
	}
}

long aListBox::AddItem(aListItem* add)
{
	if (itemCount >= MAX_LIST_ITEMS)
		return TOO_MANY_ITEMS;

	float lastX = globalX() + add->globalX();
	float lastY = globalY() + topSkip;
	
	if ( itemCount )
	{
		lastY = items[itemCount-1]->globalY() + items[itemCount-1]->height() + skipAmount;
	}

	items[itemCount++] = add;
	add->moveTo( lastX, lastY );

	if ( add->globalRight() > globalRight() ) // can't have it hanging over the edge
	{
		float shrink = globalRight() - add->globalRight();
		add->resize( add->width() +  shrink, add->height() );
	}



	if ( scrollBar )
	{
		int itemsTotalHeight = 0;

        // sebi: WTF??? always true
        // putting assert here, because otherwise items[itemCount-1] does bad stuff
        gosASSERT(itemCount>0);
		if ( itemCount>0 )
			itemsTotalHeight = items[itemCount-1]->bottom() - items[0]->top() + skipAmount;

			if ( itemsTotalHeight > scrollBar->height() )
				scrollBar->SetScrollMax( itemsTotalHeight - scrollBar->height() );
			else
			{
				scrollBar->SetScroll( 0 );
				scrollBar->SetScrollMax( 0 );
			}
			 
	}

	return itemCount - 1;
}

long	aListBox::InsertItem(aListItem* itemString, long where)
{
	if ( itemCount >= MAX_LIST_ITEMS )
		return -1;
	gosASSERT( itemString );
	if ( where >= itemCount )
		where = itemCount;
	for ( int i = itemCount - 1; i > where - 1; i-- )
	{
		items[i+1] = items[i];
		items[i+1]->move( 0, itemString->height() + skipAmount );
	}

	float lastX = x() + itemString->x();
	float lastY = y();

	items[where] = itemString;

	if ( itemCount > 0 )
	{
		if ( where - 1 > -1 )
			lastY = items[where-1]->bottom() + skipAmount;
		else
			lastY = items[where+1]->top() - skipAmount - itemString->height();
	}

	itemString->moveTo( lastX, lastY );
	itemString->showGUIWindow( true );

	if ( itemString->globalRight() > globalRight() ) // can't have it hanging over the edge
	{
		float shrink = globalRight() - itemString->globalRight();
		itemString->resize( itemString->width() +  shrink, itemString->height() );
	}

	itemCount++;


	if ( scrollBar )
	{
		int itemsTotalHeight = 0;
        // sebi: WTF??? always true
        // putting assert here, because otherwise items[itemCount-1] does bad stuff
        gosASSERT(itemCount>0);
		if ( itemCount>0 )
			itemsTotalHeight = items[itemCount-1]->bottom() - items[0]->top();

			if ( itemsTotalHeight > scrollBar->height() )
				scrollBar->SetScrollMax( itemsTotalHeight - scrollBar->height() );
			else
				scrollBar->SetScrollMax( 0 );
			
	}


	return where;
}

long	aListBox::RemoveItem( aListItem* item, bool bDelete )
{
	if ( !item )
		return -1;

	float height = item->height();

	bool bFound = false;
	for ( int i = 0; i < itemCount; i++ )
	{
		if ( items[i] == item )
		{
			bFound = true;
			if ( bDelete )
				delete items[i];
			if ( i < itemCount - 1 )
			{
				items[i] = items[i+1];
				items[i]->move( 0, -height - skipAmount );
			}
			else
			{
				items[i] = nullptr;
				if ( itemSelected == i )
					itemSelected = -1;
			}
		}
		else if ( bFound )
		{
			if ( i < itemCount - 1 )
			{
				items[i] = items[i+1];
				items[i]->move( 0, -height - skipAmount );
			}
			else
				items[i] = nullptr;
		}
	}

	if ( bFound )
	{
		itemCount --;
		if ( itemSelected != -1 )
			SelectItem( itemSelected );
	}

	else
		return -1;

	return 0;
}

long aListBox::SelectItem(long itemNumber)
{
	if (itemNumber >= itemCount)
		return ITEM_OUT_OF_RANGE;
	
	for ( int i = 0; i < itemCount; i++ )
	{
		items[i]->deselect();
	}
	
	
	itemSelected = itemNumber;
	if ( itemSelected > -1 )
	{
		aListItem* pItem = GetItem( itemNumber );
		if ( pItem )
		{
			pItem->setState( aListItem::SELECTED );

			if ( !pItem->isShowing() || pItem->globalY() + pItem->height() > globalY() + height() 
				|| pItem->globalY() + pItem->height() < globalY() )
			{
				int newPos = (pItem->globalY() + pItem->height() - globalY()) - scrollBar->height() + scrollBar->GetScrollPos();

				scrollBar->SetScroll( newPos  );//				float itemY = pItem->globalY() + pItem->height();
			}
		}
	}

	
	return NO_ERR;
}

void	aListBox::enableAllItems()
{
	for ( int i = 0; i < itemCount; i++ )
	{
		if ( items[i]->getState() == aListItem::DISABLED )
			items[i]->setState( aListItem::ENABLED );
	}
}


aListItem* aListBox::GetItem(long itemNumber)
{
	if (itemNumber >= itemCount)
		return nullptr;

	return items[itemNumber];
	
}	

void aListBox::scroll( int amount )
{
    // sebi: !items has no sense as will always return false
	//if ( !items || !itemCount )
	if ( !itemCount )
		return;

	if ( amount < 0 && items[0]->globalY() > globalY() ) // no need to scroll
		return;

	else if ( amount > 0 && (items[itemCount-1]->globalY() + items[itemCount-1]->height()) < (globalY() + height() ) )
		return;

	for ( int i = 0; i < itemCount; i++ )
	{
		items[i]->move( 0, -amount );
	}
}

long aListBox::GetCheckedItem() const
{
	int ret = -1;
    // sebi: always coca cola...
	//if ( items )
	{
		for ( int i = 0; i < itemCount; i++ )
		{
			if ( items[i] && items[i]->isChecked() )
				return i;
		}
	}

	return ret;
}

long	aListBox::getScrollBarWidth()
{
	if ( scrollBar )
		return scrollBar->width();

	return 0;
}

void	aListBox::setOrange(bool bOrange)
{
	scrollBar->setOrange();
}

bool				aListBox::pointInScrollBar( long mouseX, long mouseY )
{
	if ( scrollBar )
	{
		return scrollBar->pointInside( mouseX, mouseY );
	}

	return 0;
}



///////////////////////////////////////////////////////////////////////////////
// aTextListItem methods
///////////////////////////////////////////////////////////////////////////////
aTextListItem::aTextListItem( const aFont& newFont )
{
	font = newFont;
	DWORD height;
	DWORD width;
	font.getSize( width, height, "> ");
	aListItem::init( width, 0, Environment.screenWidth, ((float)height*1.25) );
	state = ENABLED;
	alignment = 0;
	bForceToTop = 0;
}
aTextListItem::aTextListItem(long newFontResID)
{
	font.init( newFontResID );
	DWORD height;
	DWORD width;
	font.getSize( width, height, "> ");
	aListItem::init( width, 0, Environment.screenWidth, ((float)height*1.25) );
	state = ENABLED;
	alignment = 0;
	bForceToTop = 0;
}

void aTextListItem::init(long newFontResID)
{
	font.init( newFontResID );
	DWORD height;
	DWORD width;
	font.getSize( width, height, "> ");
	aListItem::init( width, 0, Environment.screenWidth, ((float)height*1.25) );
	state = ENABLED;
	alignment = 0;
}

aTextListItem::~aTextListItem()
{
}

void		aTextListItem::setText( std::string_view newText )
{
	text = newText.substr(0, newText.find('\0'));
}
const char* aTextListItem::getText() const
{
	return text.c_str();
}

void aTextListItem::render()
{
	if ( !isShowing() )
		return;

	// Text bridge: route the row label to crisp TTF (matches the data/defs UI)
	// when a legacy widget is being drawn over a defs page.
	if ( aObject::renderTextBridged( font, text.c_str(),
		location[0].x, location[0].y, location[2].x, location[2].y,
		location[0].argb, alignment ) )
		return;

	float y = location[2].y - location[0].y;
	float tmpHeight = font.height();
	if ( y > tmpHeight && font.height( text.c_str(), width() ) <= tmpHeight
		&& !bForceToTop )
	{
		// Centered branch — use visual ink bounds so the visible text
		// sits at true vertical center of the row, not the line-skip
		// midpoint. Top-aligned (else) branch left untouched.
		DWORD w;
		int   visTop, visBot;
		gos_TextSetAttributes( font.getTempHandle(), location[0].argb, font.getSize(), true, true, false, false, alignment );
		gos_TextVisualBounds( &w, &visTop, &visBot, text.c_str() );
		y = (location[2].y + location[0].y)/2.f - (visTop + visBot + 1)/2.f;
	}
	else
		y = location[0].y + tmpHeight/4.f;

	font.render( text.c_str(), location[0].x, y,
		location[2].x - location[0].x, location[2].y - location[0].y,
		location[0].argb, 0, alignment );
}

void aTextListItem::setText( long resID )
{
	text = cLoadString(resID, 4095).c_str();
}

void aTextListItem::sizeToText()
{
	// assume the width is right and we need to make the height different
	unsigned long height = font.height( text.c_str(), width() );
	resize( width(), height );

	
}

void aTextListItem::init( FitIniFile& file, const char* blockName )
{
	file.seekBlock( blockName );

	long x, y, width, height;

	file.readIdLong( "XLocation", x );
	file.readIdLong( "YLocation", y );
	file.readIdLong( "Width", width );
	file.readIdLong( "Height", height );

	aObject::init( x, y, width, height );

	long color;
	file.readIdLong( "Color", color );
	setColor( color );
}


//////////////////////////////////////////////////////////////////

aAnimTextListItem::aAnimTextListItem( const aAnimTextListItem& src )
{
	CopyData( src );
}
aAnimTextListItem& aAnimTextListItem::operator=( const aAnimTextListItem& src )
{
	if ( &src != this )
	{
		CopyData( src );
	}

	return *this;
}

void aAnimTextListItem::init( FitIniFile& file, const char* blockName )
{
	file.seekBlock( blockName );

	long x, y, width, height;

	file.readIdLong( "XLocation", x );
	file.readIdLong( "YLocation", y );
	file.readIdLong( "Width", width );
	file.readIdLong( "Height", height );

	aObject::init( x, y, width, height );

	long color;
	file.readIdLong( "Color", color );
	setColor( color );

	// I could read the font here

	char animationSt[256];
	if ( NO_ERR == file.readIdString( "Animation", animationSt, 255 ) )
		animInfo.init( &file, animationSt );
}


void aAnimTextListItem::CopyData( const aAnimTextListItem& src )
{
	animInfo = src.animInfo;
	font = src.font;
	text = src.text;
	state = src.state;

	aObject::operator=( src );
}

void aAnimTextListItem::render()
{
	animInfo.setState( (aAnimGroup::STATE)state );
	long color = animInfo.getCurrentColor( (aAnimGroup::STATE)state );
	setColor( color );

	aTextListItem::render();
	
}

void aAnimTextListItem::update()
{
	animInfo.update();

	aObject::update();
}

aLocalizedListItem::aLocalizedListItem() : aAnimTextListItem( 27663 )
{ 
}
long aLocalizedListItem::init( FitIniFile* file, const char* blockName )
{
	file->seekBlock( blockName );

	long x = 0;
	long y = 0;
	file->readIdLong( "XLocation", x );
	file->readIdLong( "YLocation", y );

	long fontResID = 0;
	file->readIdLong( "Font", fontResID );
	long textID = 0;
	file->readIdLong( "TextID", textID );
	aTextListItem::init(fontResID);
	setText(textID);
	long color = 0xff808080;
	file->readIdLong( "Color", color );

	char tmpStr[64];
	strcpy(tmpStr, "");
	file->readIdString( "Animation", tmpStr, 63 );
	if (0 != strcmp("", tmpStr))
	{
		animInfo.init(file, tmpStr);
	}

	moveTo(x, y);
	return 0;
}

void aLocalizedListItem::render()
{
	long color = 0xffffffff;
	if ( animInfo.getState() != getState() )
		animInfo.setState( (aAnimGroup::STATE)(long)getState() );
	color = animInfo.getCurrentColor( animInfo.getState() );
	aTextListItem::setColor((unsigned long)color);
	aTextListItem::render();
}




