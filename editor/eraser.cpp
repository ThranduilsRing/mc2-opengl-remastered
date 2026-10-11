#define ERASER_CPP
/*************************************************************************************************\
eraser.cpp			: Implementation of the eraser component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "stdafx.h"
#include "eraser.h"
#include "MineBrush.h"

#ifndef OBJECTMGR_H
#include "EditorObjectMgr.h"
#endif
#include "EditorData.h"

// EDITOR-BRUSH-SCREENPICK-1: GPU-primary object pick (defined in EditorInterface.cpp),
// same picker the Select tool uses. Avoids pulling the heavy EditorInterface.h in here.
extern EditorObject* EditorPickObjectAtScreen(int screenX, int screenY);

inline bool isCementType (DWORD type)
{
	bool isCement = ((type == BASE_CEMENT_TYPE) ||
					((type >= START_CEMENT_TYPE) && (type <= END_CEMENT_TYPE)));
	return isCement;
}

bool Eraser::beginPaint()
{
	gosASSERT( !pCurAction );
	pCurAction = new EraserAction;

	return true;
}
Action* Eraser::endPaint()
{
	// Erasing terrain overlays/types NULLs the face cache; rebuild so erased cells
	// don't render black.
	EditorData::refreshTerrainAfterEdit();
	Action* pRetAction = NULL;
	if (pCurAction)
	{
		if ( pCurAction->vertexInfoList.Count() || pCurAction->bldgAction.objInfoPtrList.Count() )
		{
			pRetAction = pCurAction;
		}
		else
		{
			delete pCurAction;
		}
	}
	
	pCurAction = NULL;
	return pRetAction;
}

constexpr int DEFAULT_TERRAIN = 2;
bool Eraser::paint( Stuff::Vector3D& worldPos, int screenX, int screenY )
{
	if ( !land )
		return false;  // EDITOR-CRASH-HARDENING-1: no terrain -> worldToTileCell/getOverlay deref NULL (sibling StampBrush.cpp:66)

	// EDITOR-BRUSH-SCREENPICK-1: use the SCREEN-projection picker (same as the Select
	// tool, EditorInterface.cpp:1625) instead of the world-footprint getObjectAtPosition.
	// The world pick matches an object's base against the cursor's GROUND point; for a
	// tall static prop (building/tree) viewed at an angle, clicking the body projects to
	// ground behind the base -> outside the footprint radius -> miss. Mechs are
	// ground-level so they matched, which is why erase worked on units but not props.
	EditorObject* pInfo = EditorPickObjectAtScreen( screenX, screenY );
	if ( pInfo )
	{
		CTeams originalTeams = EditorData::instance->TeamsRef();
		EditorData::instance->handleObjectInvalidation(pInfo);
		if (!(originalTeams == EditorData::instance->TeamsRef())) {
			pCurAction->teamsAction.PreviousTeams(originalTeams);
			pCurAction->teamsActionIsSet = true;
		}

		{
			BuildingLink* pLink = EditorObjectMgr::instance()->getLinkWithParent( pInfo );
			if ( pLink )
			{
				pCurAction->linkAction.AddToListOnce( LinkBrush::LinkInfo( pLink, LinkBrush::LinkInfo::REMOVE ) );
				EditorObjectMgr::instance()->deleteLink( pLink );
			}
			{
				pLink = EditorObjectMgr::instance()->getLinkWithBuilding( pInfo );
				if ( pLink )
				{
					pCurAction->linkAction.AddToListOnce( LinkBrush::LinkInfo( pLink, LinkBrush::LinkInfo::EDIT ) );
					pLink->RemoveObject( pInfo );
				}
			}
		}

		pCurAction->bldgAction.addBuildingInfo( *pInfo ); // undo!
		bool retval = EditorObjectMgr::instance()->deleteObject( pInfo );
		return retval;
	}
	else
	{
	int tileRow, tileCol;
	int cellRow, cellCol;
		land->worldToTileCell( worldPos, tileRow, tileCol, cellRow, cellCol );
		cellRow += tileRow * MAPCELL_DIM;
		cellCol += tileCol * MAPCELL_DIM;
		
		Overlays type;
		unsigned long Offset;
		land->getOverlay( tileRow, tileCol, type, Offset );

		if ( type != INVALID_OVERLAY )
		{
			pCurAction->addChangedVertexInfo( tileRow, tileCol ); // undo!
			land->setOverlay( tileRow, tileCol, INVALID_OVERLAY, 0 );
			return true;
		}

		if ( GameMap && GameMap->getMine( cellRow, cellCol ) )
		{
			GameMap->setMine( cellRow, cellCol, 0 );
		}
		
		long terrainType = land->getTerrain(tileRow, tileCol);
		if (isCementType(terrainType))
		{
			land->setTerrain(tileRow, tileCol, DEFAULT_TERRAIN);
		}
	}

	return false;

}
bool Eraser::canPaint( Stuff::Vector3D& worldPos, int screenX, int screenY, int flags )
{
	if ( EditorPickObjectAtScreen( screenX, screenY ) )  // EDITOR-BRUSH-SCREENPICK-1
		return true;

	if ( !land )
		return false;  // EDITOR-CRASH-HARDENING-1: no terrain -> worldToTileCell/getOverlay deref NULL (sibling StampBrush.cpp:66)

	int tileRow, tileCol;
	int cellRow, cellCol;

	land->worldToTileCell( worldPos, tileRow, tileCol, cellRow, cellCol );
	cellRow += tileRow * MAPCELL_DIM;
	cellCol += tileCol * MAPCELL_DIM;
	
	Overlays type;
	unsigned long Offset;
	land->getOverlay( tileRow, tileCol, type, Offset );

	if ( type != INVALID_OVERLAY )
	{
		return true;
	}

	long terrainType = land->getTerrain(tileRow, tileCol);
	if (isCementType(terrainType))
	{
		return true;
	}
	
	if ( GameMap && GameMap->getMine( cellRow, cellCol ) )
	{
		return true;
	}

	return false;
}

Action* Eraser::applyToSelection()
{
	if ( !land )
		return NULL;  // EDITOR-CRASH-HARDENING-1: no terrain -> realVerticesMapSide loop derefs NULL (sibling StampBrush.cpp:66)

	EraserAction* pRetAction = new EraserAction;

	{
		CTeams originalTeams = EditorData::instance->TeamsRef();
		/*first remove all references to objects that are about to be deleted*/
		EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjectsList = EditorObjectMgr::instance()->getSelectedObjectList();
		EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = selectedObjectsList.Begin();
		while (!it.IsDone())
		{
			const EditorObject* pInfo = (*it);
			if ( pInfo )
			{
				EditorData::instance->handleObjectInvalidation(pInfo);
			}
			it++;
		}
		if (!(originalTeams == EditorData::instance->TeamsRef())) {
			/*record undo info regarding removal of references to deleted objects before we actually
			delete the objects (because we need info from the actual objects before they're deleted)*/
			pRetAction->teamsAction.PreviousTeams(originalTeams);
			pRetAction->teamsActionIsSet = true;
		}
	}

	EditorObjectMgr::EDITOR_OBJECT_LIST selectedObjectsList = EditorObjectMgr::instance()->getSelectedObjectList();
	EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = selectedObjectsList.Begin();
	while (!it.IsDone())
	{
		EditorObject* pInfo = (*it);
		if ( pInfo )
		{
			{
				BuildingLink* pLink = EditorObjectMgr::instance()->getLinkWithParent( pInfo );
				if ( pLink )
				{
					pRetAction->linkAction.AddToListOnce( LinkBrush::LinkInfo( pLink, LinkBrush::LinkInfo::REMOVE ) );
					EditorObjectMgr::instance()->deleteLink( pLink );
				}
				{
					pLink = EditorObjectMgr::instance()->getLinkWithBuilding( pInfo );
					if ( pLink )
					{
						pRetAction->linkAction.AddToListOnce( LinkBrush::LinkInfo( pLink, LinkBrush::LinkInfo::EDIT ) );
						pLink->RemoveObject( pInfo );
					}
				}
			}
			pRetAction->bldgAction.addBuildingInfo( *pInfo ); // undo!
			EditorObjectMgr::instance()->deleteObject( pInfo );
		}
		it++;
	}

	for ( int i = 0; i < land->realVerticesMapSide; ++i )
	{
		for ( int j = 0; j < land->realVerticesMapSide; ++j )
		{
			if ( land->isVertexSelected( j, i ) )
			{
				pRetAction->addChangedVertexInfo( j, i );
				land->setOverlay( j, i, INVALID_OVERLAY, 0 );
				land->setTerrain(j, i, DEFAULT_TERRAIN);

				if (GameMap) {
					for (long icell = 0;icell<MAPCELL_DIM;icell++)
					{
						for (long jcell = 0;jcell<MAPCELL_DIM;jcell++)
						{
							long cellRow = j * MAPCELL_DIM + jcell;
							long cellCol = i * MAPCELL_DIM + icell;
							GameMap->setMine(cellRow,cellCol,0);
						}
					}
				}
			}
		}
	}

	return pRetAction;
}

bool Eraser::EraserAction::undo()
{
	bool bRetVal = bldgAction.redo();
	bRetVal = linkAction.undo() && bRetVal;
	/*teamAction.undo() must occur after bldgAction.redo() (after the deleted buildings have
	been restored)*/
	if (teamsActionIsSet)
	{
		bRetVal = teamsAction.undo() && bRetVal;
	}
	bRetVal = ActionPaintTile::undo() && bRetVal;
	return bRetVal;
}

bool Eraser::EraserAction::redo()
{
	bool bRetVal = true;
	/*teamAction.redo() must occur before bldgAction.undo() (before the buildings are deleted)*/
	if (teamsActionIsSet)
	{
		bRetVal = teamsAction.redo();
	}
	bRetVal = linkAction.redo() && bRetVal;
	bRetVal = bldgAction.undo() && bRetVal;
	bRetVal = ActionPaintTile::undo() && bRetVal;
	return bRetVal;

}
