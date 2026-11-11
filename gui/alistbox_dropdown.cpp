//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
// aDropList and aComboBox, split from alistbox.cpp (no behaviour change).

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
///////////////////////////////////////////////////////////////////////////////
// aDropList methods
///////////////////////////////////////////////////////////////////////////////

aDropList::aDropList(void) 
: templateItem(  27333 )
{
	listBoxMaxHeight = 0.0;
	selectionIndex = -1;

	rects = nullptr;
}	

aDropList& aDropList::operator=( const aDropList& src)
{
	aObject::operator =( src );

	
	//Toss the CURRENT listBox
	listBox.destroy();


	//TOss the current rects.  We are about to NEW over them!!
	if ( rects )
	{
		delete [] rects;
		rects = nullptr;
	}

	listBox.removeAllItems( 0 );
	listBoxMaxHeight = src.listBoxMaxHeight;
	selectionIndex = -1;

	rectCount = src.rectCount;

	if ( rectCount )
	{
		rects = new aRect[rectCount];
		for ( int i = 0; i < rectCount; i++  )
		{
			addChild( &rects[i] );
			rects[i] = src.rects[i];
		}
	}
	addChild( &listBox );

	listBox.init( rects[2].globalX(), rects[2].globalY(), rects[2].width(), listBoxMaxHeight );
	listBox.setOrange(true);


	addChild( &expandButton );
	expandButton = src.expandButton;

	textLeft = src.textLeft;
	textTop = src.textTop;

	IsExpanded( 0 );

	return *this;
}


long aDropList::init( FitIniFile* file, const char* blockName )
{
	file->seekBlock( blockName );

	long x, y, width, height;
	x = y = width = height = 0;
	file->readIdLong( "XLocation", x );
	file->readIdLong( "YLocation", y );
	file->readIdLong( "Width", width );
	file->readIdLong( "Height", height );

	expandButton.init(*file, "ExpandButton");

	file->seekBlock( "Rects" );
	file->readIdLong( "Rectcount", rectCount );
	gosASSERT( rectCount > 1 );

	char tmpBlockName[64];
	if ( rectCount )
	{
		rects = new aRect[rectCount];
		for ( int i = 0; i < rectCount; i++ )
		{
			sprintf( tmpBlockName, "Rect%ld", i );
			rects[i].init( file, tmpBlockName );
		}
	}

	listBoxMaxHeight = rects[2].height();
	//rects[2].resize(rects[2].width(), rects[1].height());

	listBox.init( rects[2].globalX(), rects[2].globalY(), rects[2].width(), listBoxMaxHeight );

	rects[2].showGUIWindow(false);
	listBox.showGUIWindow(false);

	if (width < rects[1].width()) {
		width = rects[1].width();
	}
	if (height < rects[1].height()) {
		height = rects[1].height();
	}
	aObject::init(0, 0, width, height);

	textLeft = 0;
	textTop = 0;

	if ( NO_ERR == file->seekBlock( "Text0" ) )
	{
		file->readIdLong( "XLocation", textLeft );
		file->readIdLong( "YLocation", textTop );
	}

	addChild( &rects[1] );
	addChild( &listBox );
	addChild( &rects[2] );
	addChild( &expandButton );
	addChild( &rects[0] );

	expandButton.setPressFX( LOG_VIDEOBUTTONS );
	expandButton.setHighlightFX( LOG_DIGITALHIGHLIGHT );
	expandButton.setDisabledFX( LOG_WRONGBUTTON );

	expandButton.setMessageOnRelease();

	move(x, y);

	selectionIndex = -1;
	templateItem.init( *file );


	return 0;
}

void aDropList::destroy()
{
	removeAllChildren();
	expandButton.destroy();
	listBox.destroy(); // This deletes all the list items as well
	if ( rects )
	{
		delete [] rects;
		rects = nullptr;
	}
	aObject::destroy();
}

void aDropList::specialDestroy()
{
	//Do NOT destroy the listBox.  Heidi's = operator just copies the pointer and base one is gone now.
	removeAllChildren();
	expandButton.destroy();
	if ( rects )
	{
		delete [] rects;
		rects = nullptr;
	}
	aObject::destroy();
}

void aDropList::render()
{
	if ( showWindow )
	{
		for ( int i = 0; i < pNumberOfChildren; i++ )
		{
			pChildren[i]->render();
		}
		if (0 <= selectionIndex)
		{
			aListItem *pListItem = ListBox().GetItem(selectionIndex);
			if ( pListItem )
			{
				float l, t, w, h;
				l = pListItem->globalLeft();
				t = pListItem->globalTop();
				w = pListItem->width();
				h = pListItem->height();
				float availableWidth = (expandButton.globalLeft() - 1) - (globalLeft() + 1);
				if ((0 < availableWidth) && (w > availableWidth))
				{
					pListItem->resize(availableWidth, h);
				}
				bool bShowing = pListItem->isShowing();
				pListItem->moveTo( globalX() + textLeft, globalY() + textTop );
				pListItem->showGUIWindow( true );
				pListItem->render();
				pListItem->moveTo(l, t);
				pListItem->showGUIWindow( bShowing );
				if ((0 < availableWidth) && (w > availableWidth))
				{
					pListItem->resize(w, h);
				}
			}
		}
	}
}

void aDropList::IsExpanded(bool isExpanded)
{
	if (isExpanded)
	{
		listBox.showGUIWindow(true);
		listBox.SelectItem( -1 );
		rects[2].showGUIWindow(true);
	}
	else
	{
		listBox.showGUIWindow(false);
		rects[2].showGUIWindow(false);
	}
}

void aDropList::update()
{
	if ( showWindow )
	{
		aObject::update();
		if (userInput->isLeftClick())
		{
			int cx = userInput->getMouseX();
			int cy = userInput->getMouseY();

			if (expandButton.pointInside(cx, cy)) {
				handleMessage(aMSG_BUTTONCLICKED, &expandButton);
			}

			// lose focus if appropriate
			if ( ListBox().isShowing() && 
				( !pointInside( userInput->getMouseX(), userInput->getMouseY() ) 
				|| ( ListBox().pointInside(userInput->getMouseX(), userInput->getMouseY() )
					&& !ListBox().pointInScrollBar(userInput->getMouseX(), userInput->getMouseY() ) )  ) )
			{
				listBox.showGUIWindow(false);
				rects[2].showGUIWindow(false);
			}
		}
		if (0 <= ListBox().GetSelectedItem())
		{
			selectionIndex = ListBox().GetSelectedItem();
			if (listBox.isShowing())
			{
				IsExpanded(false);
			}
			ListBox().SelectItem(-1);
		}
		else if (listBox.isShowing())
		{
			if (userInput->getKeyDown( KEY_RETURN ) )
			{
				listBox.showGUIWindow(false);
				rects[2].showGUIWindow(false);

			}
		}

	}

	ListBox().setHelpID( helpID );
}


int aDropList::handleMessage( unsigned long message, void* who )
{
	{
		if (&expandButton == who) 
		{
			if (aMSG_BUTTONCLICKED == message)
			{
				if (listBox.isShowing())
				{
					IsExpanded(false);
				}
				else
				{
					IsExpanded(true);
				}
				return 1;
			}
		}
	}

	return 0;
}

bool aDropList::pointInside(long xPos, long yPos) const
{
	bool retval = aObject::pointInside(xPos, yPos);
	if (listBox.isShowing())
	{
		retval = retval || listBox.pointInside(xPos, yPos);
	}
	return retval;
}

long aDropList::AddItem(std::unique_ptr<aListItem> itemString)
{
	const float itemHeight = itemString->height();
	long retval = ListBox().AddItem(std::move(itemString));
	float newHeight = (itemHeight + ListBox().getSpaceBetweenItems()) * ListBox().GetItemCount()+4;
	if (newHeight > listBoxMaxHeight)
	{
		newHeight = listBoxMaxHeight;
	}
	ListBox().resize(ListBox().width(), newHeight);
	rects[2].resize(rects[2].width(), newHeight);
	return retval;
}

long				aDropList::AddItem( const char* text, unsigned long color )
{
	auto pItem = std::make_unique<aAnimTextListItem>( 27333 );
	*pItem = templateItem;
	pItem->setText( text );
	pItem->setColor( color );
	return AddItem( std::move(pItem) );
}

long aDropList::AddItem( unsigned long textID, unsigned long color )
{
		auto pItem = std::make_unique<aAnimTextListItem>( 27333 );
		*pItem = templateItem;
		pItem->setText( textID );
		pItem->setColor( color );
		return AddItem( std::move(pItem) );
}

long	aDropList::SelectItem(long item)
 { 
	listBox.showGUIWindow(false);
	rects[2].showGUIWindow(false);
	selectionIndex = item;
	return listBox.SelectItem(item);

 }



///////////////////////////////////////////////////////////////////////////////
// aComboBox methods
///////////////////////////////////////////////////////////////////////////////

aComboBox::aComboBox(void) : templateItem( 27333 )
{
	listBoxMaxHeight = 0.0;
	selectionIndex = -1;
	rects = nullptr;
}

aComboBox& aComboBox::operator=(const aComboBox& src )
{
	aObject::operator =( src );

	

	//Toss the CURRENT listBox
	listBox.destroy();

	addChild( &listBox );


	//TOss the current rects.  We are about to NEW over them!!
	if ( rects )
	{
		delete [] rects;
		rects = nullptr;
	}

	listBox = src.listBox;
	listBox.removeAllItems( 0 );
	listBoxMaxHeight = src.listBoxMaxHeight;
	selectionIndex = -1;

	rectCount = src.rectCount;
	if ( rectCount )
	{
		rects = new aRect[rectCount];
		for ( int i = 0; i < rectCount; i++  )
		{
			addChild( &rects[i] );
			rects[i] = src.rects[i];
		}
	}




	addChild( &expandButton );
	expandButton = src.expandButton;





	entry = src.entry;
	addChild( &entry );

	return *this;
}

long aComboBox::init( FitIniFile* file, const char* blockName )
{
	file->seekBlock( blockName );

	long x, y, width, height;
	x = y = width = height = 0;
	file->readIdLong( "XLocation", x );
	file->readIdLong( "YLocation", y );
	file->readIdLong( "Width", width );
	file->readIdLong( "Height", height );

	file->seekBlock( "Rects" );
	file->readIdLong( "Rectcount", rectCount );
	gosASSERT( rectCount > 1 );

	{
		char blockName[64];
		if ( rectCount )
		{
			rects = new aRect[rectCount];
			for ( int i = 0; i < rectCount; i++ )
			{
				sprintf( blockName, "Rect%ld", i );
				rects[i].init( file, blockName );
			}
		}
	}

	// editbox rect = first rect
	// drop list outline = 2nd rect
	entry.init(file, "EditBox");

	expandButton.init(*file, "ExpandButton");

	expandButton.setPressFX( LOG_VIDEOBUTTONS );
	expandButton.setHighlightFX( LOG_DIGITALHIGHLIGHT );
	expandButton.setDisabledFX( LOG_WRONGBUTTON );


	listBoxMaxHeight = rects[2].height();
	//rects[2].resize(rects[2].width(), entry.height());
	listBox.init( rects[2].globalX(), rects[2].globalY(), rects[2].width(), listBoxMaxHeight );

	rects[2].showGUIWindow(false);
	listBox.showGUIWindow(false);

	if (width < entry.width()) {
		width = entry.width();
	}
	if (height < entry.height()) {
		height = entry.height();
	}
	aObject::init(0, 0, width, height);

	addChild( &listBox );
	addChild( &entry );
	addChild( &expandButton );

	for ( int i = 0; i < rectCount; i++ )
		addChild( &rects[i] );

	templateItem.init( *file );
	expandButton.setMessageOnRelease();

	move(x, y);

	return 0;
}

long	aComboBox::SelectItem(long item)
 { 
	listBox.showGUIWindow(false);
	rects[2].showGUIWindow(false);
	selectionIndex = item;
	return listBox.SelectItem(item);

 }



void aComboBox::destroy()
{
	removeAllChildren();
	entry.destroy();
	expandButton.destroy();
	listBox.destroy(); // This deletes all the list items as well
	aObject::destroy();

	delete [] rects;
	rects = nullptr;
}

void aComboBox::render()
{
	if ( showWindow )
	{
		for ( int i = 0; i < pNumberOfChildren; i++ )
		{
			pChildren[i]->render();
		}
	}
}

void aComboBox::update()
{
	if ( showWindow )
	{
		aObject::update();
		
		if (userInput->isLeftClick())
		{
			int cx = userInput->getMouseX();
			int cy = userInput->getMouseY();

			if (expandButton.pointInside(cx, cy)) {
				handleMessage(aMSG_BUTTONCLICKED, &expandButton);
			}

			// lose focus if appropriate
			else if ( ListBox().isShowing() && 
				( !pointInside( userInput->getMouseX(), userInput->getMouseY() )
				|| (  ListBox().pointInside(userInput->getMouseX(), userInput->getMouseY() ) )
				&& !ListBox().pointInScrollBar(userInput->getMouseX(), userInput->getMouseY()) ) )
			{
				listBox.showGUIWindow(false);
				rects[2].showGUIWindow(false);
			}
		}
		if (0 <= ListBox().GetSelectedItem())
		{
			selectionIndex = ListBox().GetSelectedItem();

			aListItem *pListItem = ListBox().GetItem(selectionIndex);
			aTextListItem *pTextListItem = dynamic_cast<aTextListItem *>(pListItem);
			if (nullptr != pTextListItem)
			{
				entry.setEntry(pTextListItem->getText());
			}
			if ( pListItem )
			{
				
				if (listBox.isShowing())
				{
					listBox.showGUIWindow(false);
					rects[2].showGUIWindow(false);
				}

				if ( getParent() )
					getParent()->handleMessage( aMSG_SELCHANGED, ID );
			}
			ListBox().SelectItem(-1);
		}
		else if (listBox.isShowing())
		{
			if (userInput->getKeyDown( KEY_RETURN ) )
			{
				listBox.showGUIWindow(false);
				rects[2].showGUIWindow(false);
			}
		}
	}

	ListBox().setHelpID( helpID );
}


int aComboBox::handleMessage( unsigned long message, const void* who )
{
	{
		if (&expandButton == who) 
		{
			if (aMSG_BUTTONCLICKED == message)
			{
				if (listBox.isShowing())
				{
					listBox.showGUIWindow(false);
					rects[2].showGUIWindow(false);
				}
				else
				{
					listBox.showGUIWindow(true);
					rects[2].showGUIWindow(true);
				}
				return 1;
			}
		}
	}

	return 0;
}

bool aComboBox::pointInside(long xPos, long yPos) const
{
	bool retval = aObject::pointInside(xPos, yPos);
	if (listBox.isShowing())
	{
		retval = retval || listBox.pointInside(xPos, yPos);
	}
	return retval;
}

long aComboBox::AddItem(std::unique_ptr<aListItem> itemString)
{
	float newHeight = (itemString->height() + (float)ListBox().getSpaceBetweenItems()) * ((float)ListBox().GetItemCount()+1)+4;
	if (newHeight > listBoxMaxHeight)
	{
		newHeight = listBoxMaxHeight;
	}
	ListBox().resize(ListBox().width(), newHeight);
	rects[2].resize(rects[2].width(), newHeight);
	long retval = ListBox().AddItem(std::move(itemString));
	return retval;
}

long aComboBox::AddItem( unsigned long textID, unsigned long color )
{
		auto pItem = std::make_unique<aAnimTextListItem>( 27333 );
		*pItem = templateItem;
		pItem->setText( textID );
		pItem->setColor( color );
		return AddItem( std::move(pItem) );
}

long				aComboBox::AddItem( const char* text, unsigned long color )
{
	auto pItem = std::make_unique<aAnimTextListItem>( 27333 );
	*pItem = templateItem;
	pItem->setText( text );
	pItem->setColor( color );
	return AddItem( std::move(pItem) );
}


