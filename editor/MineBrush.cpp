#define MINEBRUSH_CPP
/*************************************************************************************************\
MineBrush.cpp			: Implementation of the MineBrush component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "stdafx.h"
#include "MineBrush.h"
#include "EditorObjectMgr.h"
#include "Move.h"


bool MineBrush::beginPaint()
{
	if ( !pAction )
		pAction = new MineAction();

	return true;
}

Action* MineBrush::endPaint()
{
	Action* pRetAction = nullptr;

	if (pAction)
	{
		if ( pAction->mineInfoList.Count() )
			pRetAction =  pAction;
		else
		{
			delete pAction;
		}
	}
	
	pAction = nullptr;
	return pRetAction;
}

bool MineBrush::paint( Stuff::Vector3D& worldPos, int screenX, int screenY  )
{
	if (!GameMap)
		return false;  // MOVE data not built yet; can't place mines without GameMap

	if (!land)
		return false;  // EDITOR-CRASH-HARDENING-1: no terrain -> worldToTile/Cell deref NULL (sibling HeightBrush.cpp:100)

	 int tileC;
	 int tileR;

	land->worldToTile( worldPos, tileR, tileC );

	int cellRow;
	int cellCol;
	land->worldToCell(worldPos, cellRow, cellCol);

	// EDITOR-CRASH-HARDENING-1: original guard was a tautology (always false, never
	// returned) and checked tile coords, not the cell coords actually indexed below.
	// Bound the real cellRow/cellCol against the GameMap cell grid (getMine/setMine
	// also inBounds-guard internally, but reject early here too).
	if (cellRow < 0 || cellRow >= GameMap->getHeight()
		|| cellCol < 0 || cellCol >= GameMap->getWidth()) {
		return false;
	}

	if (1 == GameMap->getMine(cellRow, cellCol)) {
		return true;
	}

	MineAction::CTileMineInfo info;
	info.row = tileR;
	info.column = tileC;
	info.mineState = GameMap->getMine( tileR, tileC);
	pAction->AddChangedTileMineInfo( info );
	GameMap->setMine(cellRow, cellCol, 1);

	return true;
}

bool MineBrush::canPaint( Stuff::Vector3D& worldPos, int screenX, int screenY, int flags )
{
	return true;
}

bool MineBrush::canPaintSelection( )
{
	return land->hasSelection();
}

Action* MineBrush::applyToSelection()
{
	if (!GameMap)
		return nullptr;  // MOVE data not built yet; can't place mines without GameMap

	MineAction* pRetAction = new MineAction();
	for ( int i = 0; i < land->realVerticesMapSide; ++i )
	{
		for ( int j = 0; j < land->realVerticesMapSide; ++j )
		{
			if ( land->isVertexSelected( j, i ) )
			{
				MineAction::CTileMineInfo info;
				info.row = j;
				info.column = i;
				info.mineState = GameMap->getMine( j, i );
				pRetAction->AddChangedTileMineInfo( info );
				GameMap->setMine( j, i, 1 );
			}
		}
	}

	return pRetAction;
}

bool MineBrush::MineAction::undo()
{
	if (!GameMap)
		return true;  // MOVE data gone (e.g. map reload); treat as silent no-op

	for ( MINE_INFO_LIST::EIterator iter = mineInfoList.Begin();
		!iter.IsDone(); iter++ )
	{
		// get current values
		unsigned long lMineState = GameMap->getMine( (*iter).row, (*iter).column );

		// reset to old values
		GameMap->setMine( (*iter).row, (*iter).column, (*iter).mineState );

		// save current valuds
		(*iter).mineState = lMineState;
	}

	return true;
}

bool MineBrush::MineAction::redo()
{
	return undo();
}

void MineBrush::MineAction::AddChangedTileMineInfo( CTileMineInfo& info )
{
	for( MINE_INFO_LIST::EIterator iter = mineInfoList.Begin();
		!iter.IsDone(); iter++ )
		{
			if ( info.row == (*iter).row && info.column == (*iter).column )
				return;
		}

		mineInfoList.Append( info );
}




//*************************************************************************************************
// end of file ( MineBrush.cpp )
