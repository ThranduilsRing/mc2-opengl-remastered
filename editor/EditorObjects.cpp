/***************************************************************
* FILENAME: EditorObjects.cpp
* DESCRIPTION: Implements Editor object wrappers and Editor object behavior.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

#define EDITOROBJECTS_CPP

#include "EditorObjects_internal.h"



// EDITOR-STATIC-DEDRAW: free an editor object's appearance, invalidating its static-prop
// RENDER recipe FIRST. Appearance::destroy() is virtual -> Bldg/TreeAppearance::destroy()
// call invalidateStaticRegistration() (clears the GpuStaticPropRegistry RecipeRange). A
// bare `delete appearance` runs only the (empty) ~ObjectAppearance and NEVER calls
// destroy(), so a deleted building/tree kept rendering as a ghost once placed props began
// registering static recipes (EDITOR-STATIC-TEXTURE-PREWARM-1). destroy() nulls what it
// frees, so the following delete is a safe memory free with no double-free.
static void EditorFreeObjectAppearance( ObjectAppearance*& app )
{
	if ( app )
	{
		app->destroy();
		delete app;
		app = nullptr;
	}
}


Pilot::PilotInfo Pilot::s_BadPilots[MAX_PILOT] = { nullptr };
Pilot::PilotInfo Pilot::s_GoodPilots[MAX_PILOT] = { nullptr };
long	Pilot::goodCount = 0;
long	Pilot::badCount = 0;

void *EditorObject::operator new (size_t mySize)
{
	void *result = nullptr;
	result = systemHeap->Malloc(mySize);
	
	return(result);
}

void EditorObject::operator delete (void *us)
{
	systemHeap->Free(us);
}

EditorObject::EditorObject()
{
	appearInfo = new AppearanceInfo();
	appearInfo->appearance = nullptr;
	appearInfo->refCount = 1;
	cellColumn = cellRow = id = 0;
	forestId = -1;
	scale = 1.0;
}
EditorObject::EditorObject( const EditorObject& src )
{
	if ( &src != this )
	{
		appearInfo = src.appearInfo;
		appearInfo->refCount++;
		cellColumn = src.cellColumn;
		cellRow = src.cellRow;
		id = src.id;
		forestId = src.forestId;
		scale = src.scale;
	}

}

EditorObject& EditorObject::operator=( const EditorObject& src )
{
	if ( &src != this )
	{
		/* "destruct" the current object first */
		if (appearInfo)
		{
			gosASSERT(appearInfo->refCount != 0);

			appearInfo->refCount --;
			if ( appearInfo->refCount < 1 )
			{
				EditorFreeObjectAppearance(appearInfo->appearance);
				appearInfo->appearance = nullptr;

				delete appearInfo;
				appearInfo = nullptr;
			}
		}

		appearInfo = src.appearInfo;
		appearInfo->refCount++;
		cellColumn = src.cellColumn;
		cellRow = src.cellRow;
		id = src.id;
		forestId = src.forestId;
		scale = src.scale;
	}

	return *this;
}

EditorObject::~EditorObject()
{ 
	if (appearInfo)
	{
		gosASSERT(appearInfo->refCount != 0);

		appearInfo->refCount --;
		if ( appearInfo->refCount < 1 )
		{
			EditorFreeObjectAppearance(appearInfo->appearance);
			appearInfo->appearance = nullptr;

			delete appearInfo;
			appearInfo = nullptr;
		}
	}
}

void EditorObject::setDamage( bool bDamage )
{
	appearance()->setDamage( bDamage );
//	appearance()->setDamageLvl( 1000000 );
}

bool EditorObject::getDamage( ) const
{
	return appearance()->damage ? true : false;
}

const char* EditorObject::getDisplayName() const
{
	return EditorObjectMgr::instance()->getObjectName( id );
}

void EditorObject::setAlignment( int align )
{
	if ( appearance()->teamId != align )
	{
		if (align == 8)			//New magical force Neutral buttons
			align = -1;
		appearance()->teamId = align;
		BuildingLink* pLink = EditorObjectMgr::instance()->getLinkWithParent( this );

		if ( pLink )
		{
			pLink->SetParentAlignment( appearance()->teamId );
		}
	}
}

int EditorObject::getIndexInGroup() const
{
	return EditorObjectMgr::getIndexInGroup( id );
}

int EditorObject::getGroup() const
{
	return EditorObjectMgr::getGroup( id );
}

void EditorObject::setAppearance( int Group, int indexInGroup )
{
	if ( Group != EditorObjectMgr::getGroup( id ) || indexInGroup != EditorObjectMgr::getIndexInGroup( id ) )
	{
		
		AppearanceInfo* appearInfo2 = new AppearanceInfo();
		appearInfo2->appearance = EditorObjectMgr::instance()->getAppearance( Group, indexInGroup );
		appearInfo2->refCount = 1;
		static long homeRelations[9] = {0, 0, 2, 1, 1, 1, 1, 1, 1};
		gosASSERT((8 > appearance()->teamId) && (-1 <= appearance()->teamId));
		appearInfo2->appearance->setObjectParameters( appearance()->position, appearance()->rotation, appearance()->selected, appearance()->teamId, homeRelations[appearance()->teamId+1]);
		appearInfo2->appearance->setDamage(appearance()->damage);
		id = EditorObjectMgr::instance()->getID( Group, indexInGroup );

		if (appearInfo)
		{
			gosASSERT(appearInfo->refCount != 0);

			appearInfo->refCount --;
			if ( appearInfo->refCount < 1 )
			{
				EditorFreeObjectAppearance(appearInfo->appearance);
				appearInfo->appearance = nullptr;
			
				delete appearInfo;
				appearInfo = nullptr;
			

			}
		}

		appearInfo = appearInfo2;
	}
}

void EditorObject::select( bool bSelect )
{ 
	EditorObjectMgr::instance()->select(*this, bSelect);
}

unsigned long EditorObject::getColor() const
{
	switch( appearInfo->appearance->teamId )
	{
	case EDITOR_TEAMNONE:
		return 0;
	case EDITOR_TEAM1:
		return SB_GREEN;
		break;
	case EDITOR_TEAM2:
		return SB_RED;
		break;
	case EDITOR_TEAM3:
		return SB_BLUE;
	case EDITOR_TEAM4:
		return SB_ORANGE;
	case EDITOR_TEAM5:
		return SB_WHITE;
	case EDITOR_TEAM6:
		return SB_GRAY;
	case EDITOR_TEAM7:
		return SB_BLACK;
	case EDITOR_TEAM8:
		return SB_YELLOW;
		break;
	
	default:
		gosASSERT( false );

	}

	return 0xffffffff;
}

void EditorObject::getCells( long& row, long& col ) const
{
	row = cellRow;
	col = cellColumn;
}

EditorObject::AppearanceInfo& EditorObject::AppearanceInfo::operator=( const AppearanceInfo& src )
{
	if ( &src != this )
	{
		// assuming ref count stays the same, since that refers to the graphic...
		appearance->rotation = src.appearance->rotation;
		appearance->position = src.appearance->position;
		appearance->barStatus = src.appearance->barStatus;
		appearance->damage = src.appearance->damage;
		appearance->fadeTable = src.appearance->fadeTable;
		appearance->paintScheme = src.appearance->paintScheme;
		appearance->selected = src.appearance->selected;
	}

	return *this;
}

int	EditorObject::getSpecialType() const
{
	return EditorObjectMgr::instance()->getSpecialType( getID() );
}

NavMarker::NavMarker()
{
}
