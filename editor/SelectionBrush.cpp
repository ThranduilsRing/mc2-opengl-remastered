/***************************************************************
* FILENAME: SelectionBrush.cpp
* DESCRIPTION: Implements terrain/object selection and selection-drag behavior for the Editor.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Added screen-space single-click object picking while preserving drag-select behavior.
****************************************************************/

#define SELECTIONBRUSH_CPP

#include "stdafx.h"
#include "SelectionBrush.h"

#ifndef CAMERA_H
#include "Camera.h"
#endif

#ifndef EDITOROBJECTMGR_H
#include "EditorObjectMgr.h"
#endif

#ifndef ACTION_H
#include "Action.h"
#endif

#include "utilities.h"

#include "EditorMessages.h"
#include "EditorInterface.h"


SelectionBrush::SelectionBrush( bool Area, int newRadius )
{ 
	bPainting = false; 
	bArea = Area; 
	bDrag = false; 
	pCurAction = nullptr; 
	pCurModifyBuildingAction = nullptr;

	lastPos.x = lastPos.y = lastPos.z = lastPos.w = 0.0f;		//Keep the FPU exception from going off!
	m_lastDragScreen.x = m_lastDragScreen.y = 0.0f;			//Keep the FPU exception from going off!
	m_marqueeFirstScreen.x = m_marqueeFirstScreen.y = m_marqueeFirstScreen.z = m_marqueeFirstScreen.w = 0.0f;
	m_marqueeLastScreen = m_marqueeFirstScreen;
	m_marqueeStarted = false;
	smoothRadius = newRadius;
	bFirstClick = false;

	pDragBuilding = nullptr;
}

SelectionBrush::~SelectionBrush()
{
	if ( EditorObjectMgr::instance() )
			EditorObjectMgr::instance()->unselectAll();
	if ( land )
		land->unselectAll();
}
bool SelectionBrush::beginPaint()
{
	lastPos.x = lastPos.y = 0.0;
	firstWorldPos.x = lastWorldPos.x = 0.f;
	lastWorldPos.y = firstWorldPos.y = 0.f;

	bPainting = true;
	bFirstClick = !bFirstClick;
	m_marqueeStarted = false;   // BUG1: fresh screen-space marquee rect per drag
	return true;
}
Action* SelectionBrush::endPaint()
{
	bPainting = false;
	Action* pRetAction = nullptr;
	if ( pCurAction )
	{
		if ( !pCurAction->vertexInfoList.empty() )
		{
			pRetAction = pCurAction;
			pCurAction = nullptr;
			land->recalcWater();
		}

		else
		{
			delete pCurAction;
			pCurAction = nullptr;
		}
	}
	else if ( pCurModifyBuildingAction )
	{
		if ( pCurModifyBuildingAction->isNotNull() )
		{
			// Undo identifies buildings by their positions, which may have changed.
			pCurModifyBuildingAction->updateNotedObjectPositions();
			pRetAction = pCurModifyBuildingAction;
			pCurModifyBuildingAction = nullptr;
		}

		else
		{
			delete pCurModifyBuildingAction;
			pCurModifyBuildingAction = nullptr;
		}
	}

	// Screen-space corners keep the marquee independent of terrain unprojection.
	if ( m_marqueeStarted &&
		( m_marqueeFirstScreen.x != m_marqueeLastScreen.x ||
		  m_marqueeFirstScreen.y != m_marqueeLastScreen.y ) )
	{
		EditorObjectMgr::instance()->select( m_marqueeLastScreen, m_marqueeFirstScreen );
		land->selectVerticesInRect( m_marqueeLastScreen, m_marqueeFirstScreen, (GetAsyncKeyState( VK_CONTROL )) );
	}

	if (EditorInterface::instance()->ObjectSelectOnlyMode())
	{
		ReleaseCapture();
		EditorInterface::instance()->Team(EditorInterface::instance()->objectivesEditState.alignment);
	}
	
	return pRetAction;
}


float SelectionBrush::calcNewHeight( int vertexRow, int vertexCol, float deltaScreen )
{
	glm::vec3 world;
	glm::vec3 newWorld;
	Stuff::Vector4D screenVertex;
	Stuff::Vector4D screenNewVertex;


	world.y = newWorld.y = land->tileRowToWorldCoord[vertexRow];
	world.x = newWorld.x = land->tileColToWorldCoord[vertexCol];
	world.z = land->getTerrainElevation( vertexRow, vertexCol );	
	newWorld.z = world.z + 1000.0f;

	eye->projectZ( world, screenVertex );
	eye->projectZ( newWorld, screenNewVertex );

	float ratio = 1000.0f/(screenNewVertex.y - screenVertex.y);
	
	return (ratio * deltaScreen);
	
}

bool SelectionBrush::paint( glm::vec3& worldPos, int screenX, int screenY )
{
	if ( !land )
		return false;  // no terrain -> land deref in every paint branch

	Stuff::Vector4D endPos;
	endPos.x = (float)screenX;
	endPos.y = (float)screenY;

	if ( pDragBuilding )
	{
		const EditorObject* pObject = pDragBuilding;
		if ( pObject )
		{
			EditorObject* pMutable = const_cast<EditorObject*>(pObject);
			ObjectAppearance* pApp = pMutable->appearance();
			if ( pApp )
			{
				if ( !pCurModifyBuildingAction )
					pCurModifyBuildingAction = new ModifyBuildingAction();

				// FORWARD-projection drag jacobian, ported from the working object-drag
				// path (EditorInterface.cpp m_pDragObject, ~lines 1719-1797). The hand
				// tool previously assigned pApp->position = inverseProject(cursor), but
				// inverseProject inverts the distorted worldToClipGL (X-collapse) and
				// teleported the object off-map. Instead, forward-project the object's
				// world pos +-D in world X/Y (the well-conditioned matrix the GPU renders
				// with), invert that 2x2 dScreen/dWorld jacobian, and advance the object
				// by J^-1 * (per-frame cursor pixel delta). All cell/link bookkeeping and
				// the invalidate/update/register re-bake sequence below are unchanged.
				auto projGL = [&]( const glm::vec3& wp, float& outSx, float& outSy ) -> bool {
					ModernClipResult r = eye->projectModernClipGL( wp );
					if ( r.clip.w <= 1e-4f )            // at/behind the near plane (signed w)
						return false;
					float vmx = 0.f, vmy = 0.f, vax = 0.f, vay = 0.f;
					gos_GetViewport( &vmx, &vmy, &vax, &vay );
					const float ndcX = r.clip.x / r.clip.w;
					const float ndcY = r.clip.y / r.clip.w;
					outSx = vax + (ndcX * 0.5f + 0.5f) * vmx;
					outSy = vay + (1.0f - (ndcY * 0.5f + 0.5f)) * vmy;  // GL bottom-left -> screen-Y flip
					return true;
				};
				const float D = 50.0f;   // world-unit probe for the forward jacobian
				glm::vec3 w0  = pApp->position;
				glm::vec3 wXp = w0; wXp.x += D;
				glm::vec3 wYp = w0; wYp.y += D;
				float s0x, s0y, sXx, sXy, sYx, sYy;
				// Per-frame cursor screen delta from the dedicated last-screen state.
				const float dsx = (float)screenX - m_lastDragScreen.x;
				const float dsy = (float)screenY - m_lastDragScreen.y;
				m_lastDragScreen.x = (float)screenX;
				m_lastDragScreen.y = (float)screenY;

				if ( !projGL( w0, s0x, s0y ) || !projGL( wXp, sXx, sXy ) || !projGL( wYp, sYx, sYy ) )
				{
					// Object at/behind the near plane this frame: skip the move (do NOT
					// fall back to a broken unproject that would teleport it off-map).
					return true;
				}

				// Forward jacobian J = dScreen/dWorld (rows screenX/Y, cols worldX/Y).
				const float invD = 1.0f / D;
				const float Jxx = ( sXx - s0x ) * invD;   // dScreenX / dWorldX
				const float Jyx = ( sXy - s0y ) * invD;   // dScreenY / dWorldX
				const float Jxy = ( sYx - s0x ) * invD;   // dScreenX / dWorldY
				const float Jyy = ( sYy - s0y ) * invD;   // dScreenY / dWorldY
				const float det = Jxx * Jyy - Jxy * Jyx;
				if ( fabsf( det ) < 1e-9f )
				{
					// Degenerate jacobian: skip the move this frame.
					return true;
				}

				// the jacobian above is computed once at the grabbed
				// ANCHOR (pDragBuilding). Convert this frame's cursor pixel delta into a
				// single WORLD translation, then apply that SAME rigid translation to every
				// selected object so a multi-select group moves together. For a single
				// selection the loop runs once == the previous behavior.
				const float invDet = 1.0f / det;
				const float worldDx = (  Jyy * dsx - Jxy * dsy ) * invDet;
				const float worldDy = ( -Jyx * dsx + Jxx * dsy ) * invDet;

				EditorObjectMgr::EDITOR_OBJECT_LIST sel =
					EditorObjectMgr::instance()->getSelectedObjectList();
				for ( auto it = sel.begin();
					it != sel.end(); it++ )
				{
					EditorObject* pSel = *it;
					if ( !pSel )
						continue;
					ObjectAppearance* pSelApp = pSel->appearance();
					if ( !pSelApp )
						continue;

					glm::vec3 newPos = pSelApp->position;
					newPos.x += worldDx;
					newPos.y += worldDy;
					newPos.z = land->getTerrainElevation( newPos );

					int newCellI, newCellJ;
					land->worldToCell( newPos, newCellJ, newCellI );

					pCurModifyBuildingAction->addBuildingInfo(*pSel);
					// Free (un-snapped) move: moveBuilding keeps cell/link bookkeeping
					// current, then we override its grid-snapped position with the
					// jacobian-advanced free position so objects track the cursor exactly.
					EditorObjectMgr::instance()->moveBuilding( pSel, newCellJ, newCellI );
					pSelApp->position = newPos;
					// Invalidate the baked static recipe BEFORE update() so update()
					// runs UNREGISTERED and re-transforms from the free position; the
					// next render then re-bakes the recipe at the free pose. If
					// invalidate came after update(), update() takes the still-
					// registered PositionsOnly path and never re-bakes -> snap-back.
					pSelApp->invalidateStaticRegistration();
					pSelApp->update();
					// Re-bake the static recipe at the new pose (see single-move note).
					pSelApp->registerStatic();

					if ( pSel == pMutable )
					{
						lastRow = newCellI;
						lastCol = newCellJ;
					}
				}
			}
		}
		return true;
	}
	
	else if ( bDrag ) // if we are dragging vertex heights, do this
	{
		if ( endPos != lastPos )
		{
			if ( !pCurAction )
				pCurAction = new ActionPaintTile();

			if ( smoothRadius != -1 )
			{
				return paintSmooth( worldPos, screenX, screenY, smoothRadius );
			}

			if ( lastPos.x  != 0.0 && lastPos.y != 0.0 )
			{
				float delta = calcNewHeight( lastRow, lastCol, endPos.y - lastPos.y );
				for ( int j = 0; j < land->realVerticesMapSide; ++j )
				{
					for ( int i = 0; i < land->realVerticesMapSide; ++i )
					{
						if ( land->isVertexSelected( j, i ) )
						{
							pCurAction->addChangedVertexInfo( j, i );
							float oldHeight = land->getVertexHeight( j * land->realVerticesMapSide + i);
							land->setVertexHeight( j * land->realVerticesMapSide + i, oldHeight + delta );
						}
					}
				}
			}
		}	

		lastPos = endPos;
		return true;
	}
	
	else //if ( bFirstClick ) // otherwise, do a new area select
	{
		long bShift = GetAsyncKeyState( VK_SHIFT );
		long bCtrl = GetAsyncKeyState( VK_CONTROL );

		// record the marquee corners in SCREEN space (endPos = current cursor).
		// endPaint selects with these directly instead of round-tripping world positions
		// through the broken inverseProject, which collapsed under zoom-in.
		if ( !m_marqueeStarted )
		{
			m_marqueeFirstScreen = endPos;
			m_marqueeStarted = true;
		}
		m_marqueeLastScreen = endPos;

		Stuff::Vector2DOf<long> screenPos;
		screenPos.x = screenX;
		screenPos.y = screenY;

		if ( lastPos.x != 0.0 && lastPos.y != 0.0 )
		{
			if ( !bShift && !bCtrl )
			{
				land->unselectAll();
				EditorObjectMgr::instance()->unselectAll();
			}
			
			eye->inverseProject( screenPos, lastWorldPos );
		
		}
		else
		{
			if ( lastPos != endPos )
			{
				if ( !bShift && !bCtrl )
				{
					land->unselectAll();
					EditorObjectMgr::instance()->unselectAll();			
				}
			}
			lastPos = endPos;
	
			if ( firstWorldPos.x == 0.f && firstWorldPos.y == 0.f )
				eye->inverseProject( screenPos, firstWorldPos );

			eye->inverseProject( screenPos, lastWorldPos );

			// drag-select already works from projected object centers;
			// single-click needs to test the actual mouse screen point so remastered
			// mech appearances do not miss through terrain inverse-project drift.
			const EditorObject* pInfo = EditorObjectMgr::instance()->getObjectAtScreenPosition( screenX, screenY );
			if ( pInfo )
			{
				if ( !bCtrl || (bCtrl && pInfo->isSelected() == false ) )
					(const_cast<EditorObject*>(pInfo))->select( true );
				else
					(const_cast<EditorObject*>(pInfo))->select( false );
			}
			else
			{
				 int tileR, tileC;
				land->worldToTile( worldPos, tileR, tileC );
				if ( tileR > -1 && tileR < land->realVerticesMapSide
					&& tileC > -1 && tileC < land->realVerticesMapSide )
				{
					if ( fabs(worldPos.x - land->tileColToWorldCoord[tileC]) >= land->worldUnitsPerVertex/2 )
						tileC++;

					if ( fabs(worldPos.y - land->tileRowToWorldCoord[tileR]) >= land->worldUnitsPerVertex/2 )
						tileR++;

					// the rounding above can push an edge click from the
					// last valid tile to one-past-the-end.  The original bounds check
					// happened before rounding, so re-check before touching the terrain
					// selection arrays.
					if ( tileR > -1 && tileR < land->realVerticesMapSide
						&& tileC > -1 && tileC < land->realVerticesMapSide )
					{
						if (!bCtrl || (bCtrl && !land->isVertexSelected( tileR, tileC ) ) )
							land->selectVertex( tileR, tileC );

						else // shift key, object is selected
							land->selectVertex( tileR, tileC, false );
					}
				}
			}
		}
	}


	return true;
}

void SelectionBrush::render( int screenX, int screenY )
{
	
	if ( bPainting && !bDrag && !pDragBuilding && m_marqueeStarted )
	{
		//------------------------------------------
		// BUG1: draw the rubber band from the recorded SCREEN-space start corner to the
		// live cursor (was projectZ(firstWorldPos), which drew an empty rect under zoom).
		GUI_RECT rect = { screenX, screenY, (long)m_marqueeFirstScreen.x, (long)m_marqueeFirstScreen.y };
		drawRect( rect, 0x30ffffff );
		drawEmptyRect( rect, 0xff000000, 0xff000000 );
	}


	else if ( lastPos.x != 0.0 && lastPos.y != 0.0 && 
		!GetAsyncKeyState( KEY_LSHIFT ) && !GetAsyncKeyState( KEY_LCONTROL ) )
	{
		
		if ( EditorObjectMgr::instance()->getSelectionCount() >= 1 )
		{
			// BUG2/BUG3: hovering ANY selected object (single OR group) grabs it as the
			// drag ANCHOR and shows the move (hand) cursor. Was `== 1`, which on a group
			// fell through to the terrain-vertex scan below and showed the deform/hills
			// cursor (BUG2) and never let the group move (BUG3). paint() translates the
			// whole selection by the anchor's jacobian delta.
			//
			// Hover-grab pick: use the FORWARD screen-space picker (the same one the
			// working single-click path uses), NOT eye->inverseProject. inverseProject
			// inverts the distorted worldToClipGL (documented X-collapse) so the hovered
			// world point disagreed with what is on screen -> the hand cursor fired on a
			// body-click the forward pick missed, then the inverse-projected position fed
			// the teleporting move. getObjectAtScreenPosition tests the actual cursor
			// pixel against forward-projected object footprints, so the hand cursor and
			// pDragBuilding now agree with the on-screen object.
			const EditorObject* pObject =
				EditorObjectMgr::instance()->getObjectAtScreenPosition( screenX, screenY );

			if ( pObject && pObject->isSelected() )
			{
				EditorInterface::instance()->ChangeCursor( IDC_HAND );
				pDragBuilding = (EditorObject*)pObject;
				pObject->getCells( (long&)lastRow, (long&)lastCol );
				lastPos.x = (float)screenX;
				lastPos.y = (float)screenY;
				// Seed the dedicated last-cursor-screen state for the jacobian move so
				// the first paint() frame produces a zero (not garbage) screen delta.
				m_lastDragScreen.x = (float)screenX;
				m_lastDragScreen.y = (float)screenY;
				return;
			}

		}
		
		glm::vec3 world;
		Stuff::Vector4D screen;

		if ( 	!GetAsyncKeyState( KEY_LSHIFT ) && !GetAsyncKeyState( KEY_LCONTROL ) )
		{
			for ( int i = 0; i < land->realVerticesMapSide; ++i )
			{
				for ( int j = 0; j < land->realVerticesMapSide; ++j )
				{
					if ( land->isVertexSelected( j, i ) )
					{
						world.y = land->tileRowToWorldCoord[j];
						world.x = land->tileColToWorldCoord[i];
						world.z = land->getTerrainElevation( j, i );


						eye->projectZ( world, screen );
						if ( (fabs(screen.x - screenX) < 20 && fabs(screen.y - screenY) < 20) )
						{
							EditorInterface::instance()->ChangeCursor( smoothRadius == -1 ? IDC_UP : IDC_HILLS );
							bDrag = true;
							lastRow = j;
							lastCol = i;
							return;
						}
					}
				}
			}
		}
	}

	if ( !bPainting )
	{
		EditorInterface::instance()->ChangeCursor( IDC_MC2ARROW );
		bDrag = false;
		pDragBuilding = nullptr;
	}
}


bool SelectionBrush::paintSmooth( glm::vec3& worldPos, int screenX, int screenY, int radius )
{
	int minI = INT_MAX;
	int maxI = 0;
	int minJ = INT_MAX;
	int maxJ = 0;
	for ( int j = 0; j < land->realVerticesMapSide; ++j )
	{
		for ( int i = 0; i < land->realVerticesMapSide; ++i )
		{
			if ( land->isVertexSelected( j, i ) )
			{
				if ( i > maxI )
					maxI = i;
				if ( i < minI )
					minI = i;
				if ( j > maxJ )
					maxJ = j;
				if ( j < minJ )
					minJ = j;
			}
		}
	}

	if ( maxI == minI && maxJ == minJ )
	{
		return paintSmoothArea( worldPos, screenX, screenY, (float)radius, (float)radius, minJ, minI );
	}
	else 
		return paintSmoothArea( worldPos, screenX, screenY, float((maxJ - minJ + 1)>>1), float((maxI - minI + 1)>>1),
							minJ + ((maxJ - minJ)/2), minI + ((maxI - minI)/2) );

	return false;
}

bool   SelectionBrush::paintSmoothArea( glm::vec3& worldPos, int screenX, int screenY, float radY, float radX,
									   int j, int i)
{
	Stuff::Vector4D endPos;
	endPos.x = (float)screenX;
	endPos.y = (float)screenY;
	endPos.z = endPos.w = 0.0f;

	float radiusX = radX;
	float radiusY = radY;

	if ( endPos != lastPos )
	{
		if ( !pCurAction )
			pCurAction = new ActionPaintTile();

		if ( lastPos.x  != 0.0 && lastPos.y != 0.0 )
		{
			float delta = calcNewHeight( lastRow, lastCol, endPos.y - lastPos.y );
			pCurAction->addChangedVertexInfo( j, i );
			float oldHeight = land->getVertexHeight( j * land->realVerticesMapSide + i);
			float newHeight = oldHeight + delta;
			land->setVertexHeight( j * land->realVerticesMapSide + i, newHeight );

			float midX = land->tileColToWorldCoord[i];
			float midY = land->tileRowToWorldCoord[j];

			if ( radiusX == 0 )
				radiusX = .5;
			if ( radiusY == 0 )
				radiusY = .5;
			
			float a = radiusX * radiusX * land->worldUnitsPerVertex * land->worldUnitsPerVertex;
			float b = radiusY * radiusY * land->worldUnitsPerVertex * land->worldUnitsPerVertex;
	
			for ( int k = j - (long)radiusY; k < j + (long)radiusY + 1; ++k )
			{
				if ( k > -1 && k < land->realVerticesMapSide )
				{
					for ( int l = i - (long)radiusX; l < i + (long)radiusX + 1; ++l )
					{
						if ( l > -1 && l < land->realVerticesMapSide )
						{
							if ( radX == radY || ( land->isVertexSelected( k, l ) ) )
							{

								float deltaY = ( (k - j) ) * -land->worldUnitsPerVertex;
								float deltaX = ( (l - i) ) * land->worldUnitsPerVertex;

								if ( (deltaX * deltaX)/a + (deltaY * deltaY)/b <= 1 )
								{
									glm::vec3 edge;

									if ( deltaX == 0 )
									{
										edge.x = 0.f;
										edge.y = deltaY < 0 ? -radY * land->worldUnitsPerVertex : 
										radY * land->worldUnitsPerVertex;
									}
									else if ( deltaY == 0 )
									{
										edge.y = 0.f;
										edge.x = deltaX < 0 ? -radX * land->worldUnitsPerVertex : 
										radX * land->worldUnitsPerVertex;

									}
									else
									{

										float tangent = deltaY/deltaX;
										
										float tmp = 1/a + tangent * tangent/b;
										
									
										edge.x = sqrt( 1/tmp );

										if ( deltaX < 0 && edge.x > 0 )
										{
											edge.x = -edge.x;
										}
										
										edge.y = edge.x * tangent;

										if ( deltaY < 0 && edge.y > 0 || deltaY > 0 && edge.y < 0 )
										{
											edge.y = -edge.y;
										}
									}
									
															
									float r = sqrt( edge.x * edge.x + edge.y * edge.y );
									float delta = sqrt( deltaX * deltaX + deltaY * deltaY );
									
									
									edge.x += midX;
									edge.y += midY;

									edge.z = land->getTerrainElevation( edge );							
									
									
									float deltaZ = newHeight - edge.z;

									if ( deltaZ > .1 || deltaZ < -.1 )
									{					
										float z = fabs(deltaZ/2) + fabs(deltaZ/2) * cos( PI * delta/r );
										if ( fabs( z ) > .1f )
										{
											if ( deltaZ <= 0 )
												z = -z;
											pCurAction->addChangedVertexInfo( k, l );
											z += edge.z;
											
											land->setVertexHeight( k * land->realVerticesMapSide + l, z );
										}
									}
								}
							}
						}
					}
				}
			}
			
			lastPos = endPos;
			return true;

		}
	}	

	lastPos = endPos;
	return true;
}



