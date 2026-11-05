#define TACMAP_CPP
/*************************************************************************************************\
TacMap.cpp			: Implementation of the TacMap component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"tacmap.h"

#include <algorithm>
#include"tgainfo.h"
#include"gameos.hpp"

extern unsigned char godMode;

TacMap::TacMap()
{
}


void TacMap::worldToTacMap( glm::vec3& world, int xOffset, int yOffset, int xSize, int ySize, gos_VERTEX& tac )
{
	int tacX;
	int tacY;

	land->worldToCell( world, tacY, tacX );	
	tacX = std::max(tacX, 0);
	tacY = std::max(tacY, 0);
	tacX = std::min<long>(tacX, land->realVerticesMapSide * MAPCELL_DIM);
	tacY = std::min<long>(tacY, land->realVerticesMapSide * MAPCELL_DIM);
	
	tac.x = static_cast<float>(tacX) * static_cast<float>(xSize) /(static_cast<float>(land->realVerticesMapSide) * MAPCELL_DIM);
	tac.y = static_cast<float>(tacY) * static_cast<float>(ySize)/(static_cast<float>(land->realVerticesMapSide) * MAPCELL_DIM);

	tac.x += xOffset;
	tac.y += yOffset;

}

void TacMap::tacMapToWorld( const Stuff::Vector2DOf<long>& screen, int xSize, int ySize,  glm::vec3& world )
{
	// The editor can fire tac-map clicks before any mission/terrain is
	// loaded, when the global `land` is still NULL; the cell math below
	// dereferences it unconditionally. Mirror the existing out-of-range
	// fallback rather than null-deref. (Game callers only show the
	// tac-map in-mission, so land is non-NULL there - unchanged.)
	if (!land)
	{
		if (eye) world = eye->getPosition();
		else { world.x = world.y = world.z = 0.0f; }
		return;
	}

	// turn screen into cells
	long cellX = screen.x /static_cast<float>(xSize) * (static_cast<float>(land->realVerticesMapSide) * MAPCELL_DIM);
	long cellY = screen.y /static_cast<float>(ySize) * (static_cast<float>(land->realVerticesMapSide) * MAPCELL_DIM);

	if ((cellX < 0) || (cellX >= land->realVerticesMapSide * MAPCELL_DIM) ||
		(cellY < 0) || (cellY >= land->realVerticesMapSide * MAPCELL_DIM))
	{
		world = eye->getPosition();
	}
	else
	{
		world.x = land->cellColToWorldCoord[cellX];
		world.y = land->cellRowToWorldCoord[cellY];

		world.z = land->getTerrainElevation( world );
	}
}

