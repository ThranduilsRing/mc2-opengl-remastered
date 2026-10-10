#define DAMAGEBRUSH_CPP
/*************************************************************************************************\
DamageBrush.cpp			: Implementation of the DamageBrush component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "stdafx.h"
#include "DamageBrush.h"
#include "EditorObjectMgr.h"

// EDITOR-BRUSH-SCREENPICK-1: GPU-primary object pick (defined in EditorInterface.cpp),
// same picker the Select tool uses. Avoids pulling the heavy EditorInterface.h in here.
extern EditorObject* EditorPickObjectAtScreen(int screenX, int screenY);

bool DamageBrush::beginPaint()
{
	if ( !pAction )
		pAction = new ModifyBuildingAction();

	return true;
}

Action* DamageBrush::endPaint()
{
	Action* pRetAction = nullptr;

	if (pAction)
	{
		if ( pAction->isNotNull() )
			pRetAction =  pAction;
		else
		{
			delete pAction;
		}
	}
	
	pAction = nullptr;
	return pRetAction;
}

bool DamageBrush::paint( Stuff::Vector3D& worldPos, int screenX, int screenY  )
{
	// EDITOR-BRUSH-SCREENPICK-1: screen-projection pick (like Select) so tall static
	// props are hittable; world-footprint pick missed them (cursor ground point != base).
	EditorObject* pObject = EditorPickObjectAtScreen( screenX, screenY );

	if ( pObject )
	{
		pAction->addBuildingInfo( *pObject );
		pObject->setDamage( damage );
	}

	return true;
}

bool DamageBrush::canPaint( Stuff::Vector3D& worldPos, int screenX, int screenY, int flags )
{
	const EditorObject* pObject = EditorPickObjectAtScreen( screenX, screenY );  // EDITOR-BRUSH-SCREENPICK-1

	return pObject ? true : false;

}

bool DamageBrush::canPaintSelection( )
{
	return EditorObjectMgr::instance()->hasSelection();
}

Action* DamageBrush::applyToSelection()
{
	ModifyBuildingAction* pRetAction = new ModifyBuildingAction;

	EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjectsList = EditorObjectMgr::instance()->getSelectedObjectList();
	EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = selectedObjectsList.Begin();
	while (!it.IsDone())
	{
		EditorObject* pInfo = (*it);
		if ( pInfo )
		{
			pRetAction->addBuildingInfo( *pInfo );
			pInfo->setDamage( damage );
		}
		it++;
	}

	if (!(pRetAction->isNotNull()))
	{
		delete pRetAction; pRetAction = nullptr;
	}

	return pRetAction;
}


//*************************************************************************************************
// end of file ( DamageBrush.cpp )
