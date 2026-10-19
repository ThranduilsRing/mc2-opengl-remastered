#define BUILDINGBRUSH_CPP
/*************************************************************************************************\
BuildingBrush.cpp	: Implementation of the BuildingBrush component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "stdafx.h"
#include <malloc.h> // _msize: capacity of the standard CRT terrain allocation
#include "BuildingBrush.h"

#ifndef EDITOROBJECTMGR_H
#include "EditorObjectMgr.h"
#endif

#ifndef TERRAIN_H
#include "Terrain.h"
#endif

#ifndef EDITOROBJECTS_H
#include "EditorObjects.h"
#endif

#ifndef EDITORINTERFACE_H
#include "EditorInterface.h"
#endif

#include "resource.h"

//---------------------------------------------------------------------------
// Editor-lane crash guard for the building-brush preview snap.
//
// The brush snaps its preview cursor to the terrain cell grid every frame:
//   worldToCell(pos) -> getCellPos(cr,cc,pos) -> getTerrainElevation(cellPos)
//   -> MapData::terrainElevation(), which indexes the mesh array MapData::blocks.
// EditorInterface::update() runs this per-frame off the mouse-move path
// (ForwardMouseToEditor), so `pos` can be ANY unprojected cursor point.
//
// terrainElevation() is NOT safe for arbitrary positions:
//   (1) It only bounds-checks the HIGH side (meshOffset >= verticesMapSide-1);
//       there is NO low-side check, so a NEGATIVE meshOffset indexes blocks[]
//       out of bounds -> READ violation 0xC0000005.
//   (2) It centers meshOffset on verticesMapSide (= verticesBlockSide*blocksMapSide,
//       always a multiple of 20) while the valid-position extent is sized from
//       realVerticesMapSide. For the standard MC2 grid (realVerticesMapSide =
//       N*20 + 1, e.g. 61) those differ by ~half a vertex, so a cursor at the
//       top/left map EDGE yields meshOffset == -1 -> the unguarded OOB read.
// (Surfaced 2026-06-12 via run_editor_smoke --case asset_browser. Pre-existing
// terrain/editor-lane bug. The engine terrainElevation() is shared with the game
// and is deliberately left untouched; we guard entirely on the editor side.)
//
// editorTerrainMeshReady(): the mesh array is live and large enough to index.
// MapData::destroy now clears blocks on teardown. The CRT owns the allocation;
// retain the capacity guard without the retired HeapManager interface.
static inline bool editorTerrainMeshReady()
{
	if ( land == nullptr || Terrain::mapData == nullptr )
		return false;

	if ( Terrain::mapData->getBlocks() == nullptr )
		return false;

	if ( Terrain::realVerticesMapSide <= 0 )
		return false;

	const size_t neededBytes = (size_t)Terrain::realVerticesMapSide
	                         * (size_t)Terrain::realVerticesMapSide
	                         * sizeof(PostcompVertex);
	return _msize(Terrain::mapData->getBlocks()) >= neededBytes;
}

// editorElevationSampleSafe(): mirror MapData::terrainElevation()'s meshOffset
// computation for `samplePos` and return true ONLY when the sampled vertex AND its
// +1 neighbors (terrainElevation reads blocks[meshOffset], [+1], [+stride],
// [+1+stride]) fall inside the array bounds that terrainElevation actually honors,
// i.e. meshOffset in [0, verticesMapSide-2] on both axes. This adds the low-side
// bounds check the engine lacks, without touching the engine.
static inline bool editorElevationSampleSafe( const glm::vec3& samplePos )
{
	const long verticesMapSide = Terrain::verticesBlockSide * Terrain::blocksMapSide;
	if ( verticesMapSide < 2 )
		return false;

	const double oneOverWUPV = Terrain::oneOverWorldUnitsPerVertex;
	const double WUPV        = Terrain::worldUnitsPerVertex;

	// Replicate terrainElevation()'s upperLeft -> meshOffset math (mapdata.cpp).
	double upperLeftX = floor( samplePos.x * oneOverWUPV ) * WUPV;
	double upperLeftY = floor( samplePos.y * oneOverWUPV );
	if ( (float)( samplePos.y * oneOverWUPV ) != (float)upperLeftY )
		upperLeftY += 1.0;
	upperLeftY *= WUPV;

	long mx = (long)floor( upperLeftX * oneOverWUPV ) + (verticesMapSide >> 1);
	long my = (verticesMapSide >> 1) - (long)floor( upperLeftY * oneOverWUPV );

	return ( mx >= 0 && my >= 0
	      && mx <= (verticesMapSide - 2)
	      && my <= (verticesMapSide - 2) );
}

// editorSnapToCellSafe(): perform the WYSIWYG cell snap in place, but only when it
// is provably crash-free. Computes the cell (worldToCell) and the cell-center world
// position getCellPos() would query, verifies that position is safely sample-able,
// and only then performs the real getCellPos() (which re-derives it + the elevation
// lookup). If anything is unsafe, `pos` is left at the raw unprojected point.
static inline void editorSnapToCellSafe( glm::vec3& pos )
{
	if ( !editorTerrainMeshReady() )
		return;

	int cr = 0, cc = 0;
	// Round-to-nearest vertex so the visible preview snaps to the cell under the
	// cursor (worldToCell floors -> up-left half-cell bias). Editor-scoped only.
	land->worldToCellNearest( pos, cr, cc );

	// Reconstruct getCellPos()'s cell-center XY (terrain.h getCellPos(), minus the
	// elevation line) so we test the exact position terrainElevation() will index.
	glm::vec3 cellPos;
	cellPos.x = ( cc * (Terrain::worldUnitsPerVertex / 3.0) ) + (Terrain::worldUnitsPerVertex / 6.0);
	cellPos.y = ( cr * (Terrain::worldUnitsPerVertex / 3.0) ) + (Terrain::worldUnitsPerVertex / 6.0);
	cellPos.x += land->mapTopLeft3d.x;
	cellPos.y  = land->mapTopLeft3d.y - cellPos.y;

	if ( !editorElevationSampleSafe( cellPos ) )
		return;

	land->getCellPos( cr, cc, pos );
}

BuildingBrush::BuildingBrush( int Group, int IndexInGroup, int Alignment )
{
	group = Group;
	indexInGroup = IndexInGroup;
	pAction = nullptr;
	pCursor = EditorObjectMgr::instance()->getAppearance( Group, IndexInGroup );
	pCursor->teamId = Alignment;
	pCursor->setInView(true);
	pCursor->setVisibility(true,true);
	pCursor->position = eye->getPosition();
	pCursor->update();
	// GL port: placed buildings render through the GPU static-prop batcher, which
	// only draws appearances that own a recipe (registered at mission load via
	// EditorObjectMgr::registerStaticPropsForMissionLoad -> app->registerStatic()).
	// The preview cursor is created AFTER mission load and was never registered,
	// so its per-frame appearance()->render() submit produced no geometry -> no
	// placement ghost. Register it on menu-select so the ghost (and its live
	// mouse-follow position + wheel rotation) renders like a placed object.
	pCursor->registerStatic();
	alignment = Alignment;
}

BuildingBrush::BuildingBrush()
{
	pCursor = nullptr;
	group = indexInGroup = -1;
	pAction = nullptr;
}

BuildingBrush::~BuildingBrush()
{
	if ( pCursor )
		delete pCursor;
}

bool BuildingBrush::canPaint( glm::vec3& worldPos, int screenX, int screenY, int flags )
{
	if ( !EditorObjectMgr::instance()->canAddBuilding( worldPos, pCursor->rotation, group, indexInGroup ) )
	{
		pCursor->setHighlightColor( 0x007f0000 );
		return false; // no two things on top of each other
	}

	pCursor->setHighlightColor( 0x00007f00 );
	return true;
}

bool BuildingBrush::beginPaint()
{
	gosASSERT( !pAction );

	pAction = new BuildingAction;
	
	return true; // need to set up undo here
}

bool BuildingBrush::paint( glm::vec3& worldPos, int screenX, int screenY )
{
	// Patch: Editor placement must not be blocked by the legacy appearance-heap
	// warning threshold.  New/editor-created maps can report very low remaining
	// appearance heap even when the actual addBuilding path can still allocate
	// and place the object.  The old dialog prevented placement before the real
	// operation was attempted, which also blocked save testing.
	EditorObject* pInfo = EditorObjectMgr::instance()->addBuilding( worldPos, group, indexInGroup, alignment, pCursor->rotation );
	if ( pInfo && pAction )
		pAction->addBuildingInfo( *pInfo );

	return (pInfo != nullptr);
}

Action* BuildingBrush::endPaint( )
{
	if (pAction)
	{
		if ( pAction->objInfoPtrList.empty() )
		{
			delete pAction;
			pAction = nullptr;
		}
	}
	Action* pRetAction = pAction;
	pAction = nullptr;
	return pRetAction;
}

BuildingBrush::BuildingAction::~BuildingAction()
{
	for (EditorObject* copy : objInfoPtrList)
		delete copy;
}

bool BuildingBrush::BuildingAction::undo()
{
	bool bRetVal = true;

	for (auto iter = objInfoPtrList.begin(); iter != objInfoPtrList.end(); ++iter)
	{
		ObjectAppearance *pAppearance = (*(*iter)).appearance();
		EditorObject* pObj = EditorObjectMgr::instance()->getObjectAtLocation(pAppearance->position.x, pAppearance->position.y);
		if (pObj)
		{
			bRetVal = EditorObjectMgr::instance()->deleteBuilding( pObj ) && bRetVal;
		}
		else
		{
			gosASSERT(false);
		}
	}

	return bRetVal;
}

bool BuildingBrush::BuildingAction::redo()
{
	bool bRetVal = true;

	for (auto iter = objInfoPtrList.begin(); iter != objInfoPtrList.end(); ++iter)
	{
		ObjectAppearance *pAppearance = (*(*iter)).appearance();
		EditorObject* pObj = EditorObjectMgr::instance()->addBuilding( pAppearance->position, EditorObjectMgr::instance()->getGroup( (*(*iter)).getID() ), EditorObjectMgr::instance()->getIndexInGroup( (*(*iter)).getID() ), pAppearance->teamId, pAppearance->rotation );
		if (pObj)
		{
			(*pObj).CastAndCopy((*(*iter)));
		}
		else
		{
			gosASSERT(false);
			bRetVal = false;
		}
	}

	return bRetVal;

}

void BuildingBrush::BuildingAction::addBuildingInfo(EditorObject& info)
{
	EditorObject *pCopy = info.Clone();
	gosASSERT(pCopy);
	objInfoPtrList.push_back(pCopy);
}

// WYSIWYG placement snap: convert a world position to the cell addBuilding()
// will commit to, then back to that cell's centre. worldToCell() is UNCLAMPED:
// a cursor that projects off the map (especially west/north of it) yields
// negative cell indices, and getCellPos()->getTerrainElevation()->terrainElevation()
// has NO lower-bound guard (only an upper one), so it reads blocks[negative]
// out of bounds -> 0xC0000005. Clamp to the valid cell grid before snapping.
// Shared by update() and the -smoke-place-oob regression test so both exercise
// the identical guard.
void BuildingBrush::snapToTerrainCell( Terrain* terr, glm::vec3& pos )
{
	if ( !terr )
		return;
	int cr = 0, cc = 0;
	// Round-to-nearest vertex so the placed object snaps to the cell under the
	// cursor (worldToCell floors -> up-left half-cell bias). Editor-scoped only.
	terr->worldToCellNearest( pos, cr, cc );
	const int maxCell = (int)( ( Terrain::realVerticesMapSide - 1 ) * 3 ) - 1;
	if ( maxCell < 0 )
		return;
	if ( cr < 0 ) cr = 0; else if ( cr > maxCell ) cr = maxCell;
	if ( cc < 0 ) cc = 0; else if ( cc > maxCell ) cc = maxCell;
	terr->getCellPos( cr, cc, pos );
}

void BuildingBrush::update( int ScreenMouseX, int ScreenMouseY )
{
	if ( !pCursor )
		return;
	
	glm::vec3 pos;
	Stuff::Vector2DOf<long> pt;
	pt.x = ScreenMouseX;
	pt.y = ScreenMouseY;
	eye->inverseProject( pt, pos );

	// WYSIWYG: snap to the cell addBuilding() will commit to (see render()).
	// Guard: skip the cell snap (leave pos at the raw unprojected point) when the
	// terrain isn't safely sample-able -- getCellPos() would index blocks[] OOB.
	editorSnapToCellSafe( pos );

	if ( !EditorObjectMgr::instance()->canAddBuilding( pos, pCursor->rotation, group, indexInGroup ) )
		pCursor->setHighlightColor( 0x00400000 );
	else
		pCursor->setHighlightColor( 0x00004000 );

	pCursor->position = pos;
	pCursor->recalcBounds();
	pCursor->update();			//Safe tp call here now because we run the first update in the constructor which caches in texture
								//NOT TRUE WITH RIA CODE!!!!!  Must have a separate update or NO Triangles get added!!!
	pCursor->setVisibility( true, true );
}

void BuildingBrush::render( int ScreenMouseX, int ScreenMouseY )
{
	if ( !pCursor )
		return;

	// Position the preview cursor at the mouse's ground point EVERY frame. This was
	// commented out historically ("may cause cursor to lag") -- but with the static-
	// prop draw on the live builder, a cursor left at a stale position renders as a
	// persistent DUPLICATE prop "elsewhere on the map". Updating position + bounds +
	// visibility per frame makes the cursor track the mouse instead of ghosting.
	glm::vec3 pos;
	Stuff::Vector2DOf<long> pt;
	pt.x = ScreenMouseX;
	pt.y = ScreenMouseY;
	eye->inverseProject( pt, pos );

	// WYSIWYG: addBuilding() commits the object SNAPPED to the cell grid
	// (realPos = getCellPos(worldToCell(pos)), bSnapToCell default true). The preview
	// cursor was drawn at the RAW unprojected point, so the highlight sat up to a
	// tile off the cell the object actually lands on. Snap the preview to the same
	// cell so what you see is what you place.
	// Guard: skip the cell snap when the terrain isn't safely sample-able (see update()).
	editorSnapToCellSafe( pos );

	if ( !EditorObjectMgr::instance()->canAddBuilding( pos, pCursor->rotation, group, indexInGroup ) )
		pCursor->setHighlightColor( 0x00400000 );
	else
		pCursor->setHighlightColor( 0x00004000 );

	pCursor->position = pos;
	pCursor->recalcBounds();
	pCursor->update();
	pCursor->setVisibility( true, true );
	pCursor->render();
}

void BuildingBrush::rotateBrush( int direction )
{
	int ID = EditorObjectMgr::instance()->getID( group, indexInGroup );
	int fitID = EditorObjectMgr::instance()->getFitID(ID);
	float step = ((EditorObjectMgr::WALL == EditorObjectMgr::instance()->getSpecialType(ID)) || (33/*repair bay*/ == fitID)) ? 90.0f : 45.0f;
	addRotationDegrees( direction * step );
}

// Continuous rotation entry point used by the mouse wheel (EditorInterface::OnMouseWheel).
// rotateBrush() above remains the discrete [/] keyboard 45/90 stepper.
void BuildingBrush::addRotationDegrees( float deg )
{
	if ( pCursor )
		pCursor->rotation += deg;
}

