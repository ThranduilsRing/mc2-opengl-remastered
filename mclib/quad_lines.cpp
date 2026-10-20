// TerrainQuad: the passability / LOS / debug-cell line draws and the mine draw.
// Declared in the class headers; shared names in quad_internal.h.
#include "quad_internal.h"


//---------------------------------------------------------------------------
long DrawDebugCells = 0;

void TerrainQuad::drawLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.
	// Reverse-Z (Step 6b): post-divide pz is now reverse-Z [near->1,
	// far->0] with GEQUAL; "just above base tiles" (toward camera) =
	// LARGER z, so the hardcoded -0.002f bias flips to +0.002f. Scene
	// debug-overlay consumer (drawTerrainGrid / selected-quad), not a
	// named-constant site; flipped explicitly.
	long color = XP_WHITE;

	if (uvMode == BOTTOMRIGHT)
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz+0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,vertices[1]->pz+0.002f,1.0f / vertices[1]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[2]->px;
			pos2.y = vertices[2]->py;

			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz + 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz+0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,vertices[2]->pz+0.002f,1.0f / vertices[2]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz + 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz + 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
		}
	}
	else
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,vertices[0]->pz+0.002f,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,vertices[1]->pz+0.002f,1.0f / vertices[1]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;
			pos1.z = vertices[1]->pz + 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz + 0.002f;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;
			pos2.z = vertices[0]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[1]->px,vertices[1]->py,vertices[1]->pz+0.002f,1.0f / vertices[1]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,vertices[2]->pz+0.002f,1.0f / vertices[2]->pw);
			
			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;
			pos1.z = vertices[2]->pz + 0.002f;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;
			pos2.z = vertices[3]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;
			pos1.z = vertices[3]->pz + 0.002f;

			pos2.x = vertices[1]->px;
			pos2.y = vertices[1]->py;
			pos2.z = vertices[1]->pz + 0.002f;

			{
				LineElement newElement(pos1,pos2,color,nullptr);
				newElement.draw();
			}
		}
	}

	//------------------------------------------------------------
	// Draw Movement Map Grid.
	//		Once movement is split up, turn this back on for editor -fs
	// I need ALL cells drawn to check elevation Code
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		// Display the ScenarioMap cell grid, as well, displaying open\blocked
		// states...
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		//cellWidth -= 5.0;
		
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = nullptr;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (!curCell || 
						curCell->getDebug() || 
						!curCell->getPassable() || 
						curCell->getPathlock(0) || 
						curCell->getDeepWater() || 
						curCell->getShallowWater() ||
						curCell->getForest())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;
						
						glm::vec3 thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);
						
						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_0]
						eye->projectForDebugOverlay(thePoint,pos4);

						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_1]
						eye->projectForDebugOverlay(thePoint,pos1);

						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_2]
						eye->projectForDebugOverlay(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_passability_3]
						eye->projectForDebugOverlay(thePoint,pos3);

						pos1.z += 0.002f;
						pos2.z += 0.002f;
						pos3.z += 0.002f;
						pos4.z += 0.002f;

						DWORD color = XP_RED;
						
						if (!curCell)
						{
							color = XP_GREEN;
						}
						else if (curCell->getDebug() && DrawDebugCells)
						{
							color = XP_YELLOW;
						}
						else if (curCell->getPathlock(0))
						{
							color = XP_YELLOW;
						}
						else if (curCell->getForest())
						{
							color = SB_ORANGE;
						}
						else if (!curCell->getPassable())
						{
							color = SB_RED;
						}
						else if (curCell->getDeepWater())
						{
							color = SB_ORANGE;
						}
						else if (curCell->getShallowWater())
						{
							color = XP_BLUE;
						}

						{
							LineElement newElement(pos1,pos2,color,nullptr);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,nullptr);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,nullptr);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,nullptr);
							newElement.draw();
						}
					}
				}
			}
		}
		else if (drawEditorPassability && gEditorNavFlags)
		{
			// EditorNav overlay — one colored outline per terrain tile.
			// No GameMap dependency; works on maps of any size.
			long rowCol = vertices[0]->posTile;
			long tileR  = rowCol >> 16;
			long tileC  = rowCol & 0x0000ffff;

			if (tileR >= 0 && tileR < gEditorNavCellSide &&
			    tileC >= 0 && tileC < gEditorNavCellSide)
			{
				uint8_t f = gEditorNavFlags[tileR * gEditorNavCellSide + tileC];
				DWORD color;
				if      (f & EDITOR_NAV_BLOCKED)          color = SB_RED;
				else if (f & EDITOR_NAV_DEEP_WATER)       color = SB_ORANGE;
				else if (f & EDITOR_NAV_SHALLOW_WATER)    color = XP_BLUE;
				else if (f & EDITOR_NAV_GROUND_PASSABLE)  color = XP_GREEN;
				else                                       color = XP_RED;

				Stuff::Vector4D p0, p1, p2, p3;
				p0.x = vertices[0]->px; p0.y = vertices[0]->py; p0.z = vertices[0]->pz + 0.002f; p0.w = 1.f;
				p1.x = vertices[1]->px; p1.y = vertices[1]->py; p1.z = vertices[1]->pz + 0.002f; p1.w = 1.f;
				p2.x = vertices[2]->px; p2.y = vertices[2]->py; p2.z = vertices[2]->pz + 0.002f; p2.w = 1.f;
				p3.x = vertices[3]->px; p3.y = vertices[3]->py; p3.z = vertices[3]->pz + 0.002f; p3.w = 1.f;

				{ LineElement e(p0, p1, color, nullptr); e.draw(); }
				{ LineElement e(p1, p3, color, nullptr); e.draw(); }
				{ LineElement e(p3, p2, color, nullptr); e.draw(); }
				{ LineElement e(p2, p0, color, nullptr); e.draw(); }
			}
		}
	}

	if (GlobalMoveMap[0]->badLoad)
		return;

	if (clipped1 != 0)
	{
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		cellWidth -= 5.0;
				
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
		
		long cellR = tileR * MAPCELL_DIM;
		long cellC = tileC * MAPCELL_DIM;
		
		for (long currentDoor = 0;currentDoor < GlobalMoveMap[0]->numDoors;currentDoor++)
		{
			if ((GlobalMoveMap[0]->doors[currentDoor].row >= cellR) && 
				(GlobalMoveMap[0]->doors[currentDoor].row < (cellR + MAPCELL_DIM)) &&
				(GlobalMoveMap[0]->doors[currentDoor].col >= cellC) &&
				(GlobalMoveMap[0]->doors[currentDoor].col < (cellC + MAPCELL_DIM)))
			{
				Stuff::Vector4D pos1;
				Stuff::Vector4D pos2;
				Stuff::Vector4D pos3;
				Stuff::Vector4D pos4;
					
				long xLength = 1;
				long yLength = 1;
				
				if (GlobalMoveMap[0]->doors[currentDoor].direction[0] == 1)
				{
					yLength = GlobalMoveMap[0]->doors[currentDoor].length;
				}
				
				if (GlobalMoveMap[0]->doors[currentDoor].direction[0] == 2)
				{
					xLength = GlobalMoveMap[0]->doors[currentDoor].length;
				}

				glm::vec3 thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);
				
				thePoint.x += (GlobalMoveMap[0]->doors[currentDoor].col - cellC) * cellWidth;
				thePoint.y -= (GlobalMoveMap[0]->doors[currentDoor].row - cellR) * cellWidth;

				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_0]
				eye->projectForDebugOverlay(thePoint,pos4);

				thePoint.x += (xLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_1]
				eye->projectForDebugOverlay(thePoint,pos1);

				thePoint.y -= (yLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_2]
				eye->projectForDebugOverlay(thePoint,pos2);

				thePoint.x -= (xLength) * cellWidth;
				thePoint.z = land->getTerrainElevation(thePoint);
				// [PROJECTZ:DebugOnly id=debug_door_outline_3]
				eye->projectForDebugOverlay(thePoint,pos3);

				pos1.z += 0.002f;
				pos2.z += 0.002f;
				pos3.z += 0.002f;
				pos4.z += 0.002f;
				{
					LineElement newElement(pos1,pos2,XP_GREEN,nullptr);
					newElement.draw();
				}	
				
				{
					LineElement newElement(pos2,pos3,XP_GREEN,nullptr);
					newElement.draw();
				}
				
				{
					LineElement newElement(pos3,pos4,XP_GREEN,nullptr);
					newElement.draw();
				}
				
				{
					LineElement newElement(pos1,pos4,XP_GREEN,nullptr);
					newElement.draw();
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------------------------
void TerrainQuad::drawLOSLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.

	//------------------------------------------------------------
	// Draw LOS Map Grid.
	// Draw a color for LOS cell height data based on height.  Draw NOTHING if cell height is ZERO!!
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = nullptr;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (curCell && curCell->getLocalHeight())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;

						glm::vec3 thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);

						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_0]
						eye->projectForDebugOverlay(thePoint,pos4);

						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_1]
						eye->projectForDebugOverlay(thePoint,pos1);

						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_2]
						eye->projectForDebugOverlay(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_los_cell_height_3]
						eye->projectForDebugOverlay(thePoint,pos3);
						
						pos1.z += 0.002f;
						pos2.z += 0.002f;
						pos3.z += 0.002f;
						pos4.z += 0.002f;

						DWORD color = XP_BLACK;
						
						if (curCell->getLocalHeight() < 2)
						{
							color = XP_BLACK;
						}
						else if (curCell->getLocalHeight() < 4)
						{
							color = XP_GRAY;
						}
						else if (curCell->getLocalHeight() < 6)
						{
							color = XP_RED;
						}
						else if (curCell->getLocalHeight() < 8)
						{
							color = XP_ORANGE;
						}
						else if (curCell->getLocalHeight() < 10)
						{
							color = XP_YELLOW;
						}
						else if (curCell->getLocalHeight() < 12)
						{
							color = XP_GREEN;
						}
						else if (curCell->getLocalHeight() < 14)
						{
							color = XP_BLUE;
						}
						else if (curCell->getLocalHeight() <= 16)
						{
							color = XP_WHITE;
						}

						{
							LineElement newElement(pos1,pos2,color,nullptr);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,nullptr);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,nullptr);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,nullptr);
							newElement.draw();
						}
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
void TerrainQuad::drawDebugCellLine (void)
{
	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw the Tile block lines at depth just above base tiles.

	if (uvMode == BOTTOMRIGHT)
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[2]->px;
			pos2.y = vertices[2]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,HUD_DEPTH,1.0f / vertices[2]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
	}
	else
	{
		if (clipped1 != 0)
		{
			Stuff::Vector4D pos1(vertices[0]->px,vertices[0]->py,HUD_DEPTH,1.0f / vertices[0]->pw);
			Stuff::Vector4D pos2(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[1]->px;
			pos1.y = vertices[1]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[0]->px;
			pos2.y = vertices[0]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
		
		if (clipped2 != 0)
		{
			Stuff::Vector4D pos1(vertices[1]->px,vertices[1]->py,HUD_DEPTH,1.0f / vertices[1]->pw);
			Stuff::Vector4D pos2(vertices[2]->px,vertices[2]->py,HUD_DEPTH,1.0f / vertices[2]->pw);
			
			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[2]->px;
			pos1.y = vertices[2]->py;

			pos2.x = vertices[3]->px;
			pos2.y = vertices[3]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
			
			pos1.x = vertices[3]->px;
			pos1.y = vertices[3]->py;

			pos2.x = vertices[1]->px;
			pos2.y = vertices[1]->py;

			{
				//LineElement newElement(pos1,pos2,color,NULL);
				//newElement.draw();
			}
		}
	}

	//------------------------------------------------------------
	// Draw Movement Map Grid.
	//		Once movement is split up, turn this back on for editor -fs
	// I need ALL cells drawn to check elevation Code
	if (clipped1 != 0)
	{
		//--------------------------------------------------------------------
		// Display the ScenarioMap cell grid, as well, displaying open\blocked
		// states...
		float cellWidth = Terrain::worldUnitsPerVertex / MAPCELL_DIM;
		//cellWidth -= 5.0;
		
		long rowCol = vertices[0]->posTile;
		long tileR = rowCol>>16;
		long tileC = rowCol & 0x0000ffff;
				
		if (GameMap)
		{
			for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
			{
				for (long cellC = 0; cellC < MAPCELL_DIM; cellC++) 
				{
					long actualCellRow = tileR * MAPCELL_DIM + cellR;
					long actualCellCol = tileC * MAPCELL_DIM + cellC;
					
					MapCellPtr curCell = nullptr;
					if (GameMap->inBounds(actualCellRow, actualCellCol))
						curCell = GameMap->getCell(actualCellRow, actualCellCol);
					
 					if (!curCell || curCell->getDebug())
					{
						Stuff::Vector4D pos1;
						Stuff::Vector4D pos2;
						Stuff::Vector4D pos3;
						Stuff::Vector4D pos4;

						glm::vec3 thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);

						thePoint.x += (cellC) * cellWidth;
						thePoint.y -= (cellR) * cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_0]
						eye->projectForDebugOverlay(thePoint,pos4);

						thePoint.x += cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_1]
						eye->projectForDebugOverlay(thePoint,pos1);

						thePoint.y -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_2]
						eye->projectForDebugOverlay(thePoint,pos2);

						thePoint.x -= cellWidth;
						thePoint.z = land->getTerrainElevation(thePoint);
						// [PROJECTZ:DebugOnly id=debug_cell_state_3]
						eye->projectForDebugOverlay(thePoint,pos3);

						pos1.z = pos2.z = pos3.z = pos4.z = HUD_DEPTH;

						DWORD color = XP_RED;
						if (!curCell)
						{
							color = XP_GREEN;
						}
						else {
							static DWORD debugColors[4] = {0, XP_RED, XP_WHITE, XP_BLUE};
							DWORD cellDebugValue = curCell->getDebug();
							if (cellDebugValue)
								color = debugColors[cellDebugValue];
						}
/*						else if (curCell->getPathlock())
						{
								color = XP_YELLOW;
						}
						else if (!curCell->getLineOfSight())
						{
					 		color = XP_BLUE;
						}
						else if (!curCell->getPassable())
						{
							color = SB_RED;
						}
*/
						{
							LineElement newElement(pos1,pos2,color,nullptr);
							newElement.draw();
						}	
						
						{
							LineElement newElement(pos2,pos3,color,nullptr);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos3,pos4,color,nullptr);
							newElement.draw();
						}
						
						{
							LineElement newElement(pos1,pos4,color,nullptr);
							newElement.draw();
						}
					}
				}
			}
		}
	}


}

//---------------------------------------------------------------------------
void TerrainQuad::drawMine (void)
{
	// PR2c Stage 0c — bracket entire function. Counter once per call (per-quad).
	CostSplitMineDrawScope _csMineDraw;
	gos_terrain_indirect::Counters_AddLegacyMineDrawQuad();

	long clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
	long clipped2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;

	if (uvMode == BOTTOMLEFT)
	{
		clipped1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
		clipped2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
	}

	//------------------------------------------------------------
	// Draw Mines.
	// All mines are visible all the time!
	if ((clipped1 != 0) || (clipped2 != 0))
	{
		long cellPos = 0;
		float cellWidth = Terrain::worldUnitsPerCell;
		for (long cellR = 0; cellR < MAPCELL_DIM; cellR++)
		{
			for (long cellC = 0; cellC < MAPCELL_DIM; cellC++,cellPos++) 
			{
				//--------------------------------------------------------------------
				bool drawMine = false;
				bool drawBlownMine = false;
				
				if (mineResult.getMine(cellPos) == 1)
					drawMine = true;
								
				if (mineResult.getMine(cellPos) == 2)
					drawBlownMine = true;
								
				if (drawMine || drawBlownMine)
				{
					Stuff::Vector4D pos1;
					Stuff::Vector4D pos2;
					Stuff::Vector4D pos3;
					Stuff::Vector4D pos4;

					//------------------------------------------------------------------------------------
					// Dig the actual Vertex information out of the projected vertices already done.
					// In this way, the draw requires only interpolation and not Giant Matrix multiplies.
					glm::vec3 thePoint(vertices[0]->vx,vertices[0]->vy,vertices[0]->pVertex->elevation);

					thePoint.x += (cellC) * cellWidth;
					thePoint.y -= (cellR) * cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner0]
					eye->projectForScreenXY(thePoint,pos4);

					thePoint.x += cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner1]
					eye->projectForScreenXY(thePoint,pos1);
					
					thePoint.y -= cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner2]
					eye->projectForScreenXY(thePoint,pos2);

					thePoint.x -= cellWidth;
					thePoint.z = land->getTerrainElevation(thePoint);
					// [PROJECTZ:ScreenXYOracle id=mine_cell_corner3]
					eye->projectForScreenXY(thePoint,pos3);

					//------------------------------------
					// Replace with New RIA code
					gos_VERTEX gVertex[3];
					gos_VERTEX sVertex[3];
		
					gVertex[0].x        = sVertex[0].x		= pos1.x;
					gVertex[0].y        = sVertex[0].y		= pos1.y;
					gVertex[0].z        = sVertex[0].z		= pos1.z;
					gVertex[0].rhw      = sVertex[0].rhw	= pos1.w;
					gVertex[0].u        = sVertex[0].u		= 0.0f;
					gVertex[0].v        = sVertex[0].v		= 0.0f;
					gVertex[0].argb     = sVertex[0].argb	= vertices[0]->lightRGB;
					gVertex[0].frgb     = sVertex[0].frgb	= vertices[0]->fogRGB;
					{
						BYTE matIdx = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
						gVertex[0].frgb = (gVertex[0].frgb & 0xFFFFFF00) | matIdx;
						sVertex[0].frgb = (sVertex[0].frgb & 0xFFFFFF00) | matIdx;
					}

					gVertex[1].x		= pos2.x;
					gVertex[1].y		= pos2.y;
					gVertex[1].z		= pos2.z;
					gVertex[1].rhw		= pos2.w;
					gVertex[1].u		= 0.999999999f;
					gVertex[1].v		= 0.0f;
					gVertex[1].argb		= vertices[1]->lightRGB;
					gVertex[1].frgb		= vertices[1]->fogRGB;
					gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

					gVertex[2].x        = sVertex[1].x		= pos3.x;
					gVertex[2].y        = sVertex[1].y		= pos3.y;
					gVertex[2].z        = sVertex[1].z		= pos3.z;
					gVertex[2].rhw      = sVertex[1].rhw	= pos3.w;
					gVertex[2].u        = sVertex[1].u		= 0.999999999f;
					gVertex[2].v        = sVertex[1].v		= 0.999999999f;
					gVertex[2].argb     = sVertex[1].argb	= vertices[2]->lightRGB;
					gVertex[2].frgb     = sVertex[1].frgb	= vertices[2]->fogRGB;
					{
						BYTE matIdx = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
						gVertex[2].frgb = (gVertex[2].frgb & 0xFFFFFF00) | matIdx;
						sVertex[1].frgb = (sVertex[1].frgb & 0xFFFFFF00) | matIdx;
					}

					sVertex[2].x		= pos4.x;
					sVertex[2].y		= pos4.y;
					sVertex[2].z		= pos4.z;
					sVertex[2].rhw		= pos4.w;
					sVertex[2].u		= 0.0f;
					sVertex[2].v		= 0.999999999f;
					sVertex[2].argb		= vertices[3]->lightRGB;
					sVertex[2].frgb		= vertices[3]->fogRGB;
					sVertex[2].frgb		= (sVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
		
					if ((gVertex[0].z >= 0.0f) &&
						(gVertex[0].z < 1.0f) &&
						(gVertex[1].z >= 0.0f) &&  
						(gVertex[1].z < 1.0f) && 
						(gVertex[2].z >= 0.0f) &&  
						(gVertex[2].z < 1.0f) &&
						(sVertex[0].z >= 0.0f) &&
						(sVertex[0].z < 1.0f) &&
						(sVertex[1].z >= 0.0f) &&  
						(sVertex[1].z < 1.0f) && 
						(sVertex[2].z >= 0.0f) &&  
						(sVertex[2].z < 1.0f))
					{
						if (drawBlownMine)
						{
							mc2::renderSubmissions().addVertices(blownTextureHandle,gVertex,MC2_DRAWALPHA);
							mc2::renderSubmissions().addVertices(blownTextureHandle,sVertex,MC2_DRAWALPHA);
						}
						else
						{
							mc2::renderSubmissions().addVertices(mineTextureHandle,gVertex,MC2_DRAWALPHA);
							mc2::renderSubmissions().addVertices(mineTextureHandle,sVertex,MC2_DRAWALPHA);
						}
					}
				}
			}
		}
	}
}
