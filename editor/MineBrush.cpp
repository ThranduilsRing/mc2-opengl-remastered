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
		if ( !pAction->mineInfoList.empty() )
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
		return false;  // no terrain -> worldToTile/Cell deref NULL (sibling HeightBrush.cpp:100)

	 int tileC;
	 int tileR;

	land->worldToTile( worldPos, tileR, tileC );

	int cellRow;
	int cellCol;
	land->worldToCell(worldPos, cellRow, cellCol);

	// Validate cell coordinates against the cell grid before indexing it;
	// tile coordinates describe a different grid.
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

	for (auto& info : mineInfoList)
	{
		unsigned long lMineState = GameMap->getMine( info.row, info.column );
		GameMap->setMine( info.row, info.column, info.mineState );
		info.mineState = lMineState;
	}

	return true;
}

bool MineBrush::MineAction::redo()
{
	return undo();
}

void MineBrush::MineAction::AddChangedTileMineInfo( CTileMineInfo& info )
{
	for (const auto& existing : mineInfoList)
	{
		if ( info.row == existing.row && info.column == existing.column )
			return;
	}

	mineInfoList.push_back( info );
}




//*************************************************************************************************
// end of file ( MineBrush.cpp )
