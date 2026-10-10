/***************************************************************
* FILENAME: Action.cpp
* DESCRIPTION: Implements Editor action and undo manager behavior.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

//----------------------------------------------------------------------------
//
// Action.cpp - implementation file for the abstract action object, and
//              the mgr that holds action objects. (otherwise known as the
//              undo manager )
//
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_

#include <winsock2.h>
#include <string.h>

#include "terrain.h"
#include "Action.h"
#include "TerrTxm.h"
#include "EditorData.h"

ActionUndoMgr* ActionUndoMgr::instance = NULL;

ActionUndoMgr::ActionUndoMgr()
{
    m_CurrentPos = -1;
    m_PosOfLastSave = -1;
    gosASSERT( !instance );
    instance = this;
}

ActionUndoMgr::~ActionUndoMgr()
{
    Reset();
}

void ActionUndoMgr::AddAction( Action* pAction )
{
    gosASSERT( pAction );

    if ( m_listUndoActions.Count() )
    {
        if (m_PosOfLastSave > m_CurrentPos)
        {
            m_PosOfLastSave = -1;
        }

        ACTION_LIST::EIterator iter = m_listUndoActions.End();
        for ( int i = m_listUndoActions.Count() - 1; i > m_CurrentPos; -- i )
        {
            delete (*iter);
            iter--;
            m_listUndoActions.DeleteTail();
        }
    }

    m_listUndoActions.Append( pAction );
    m_CurrentPos = m_listUndoActions.Count() - 1;
}

void ActionUndoMgr::EmptyUndoList()
{
    for (ACTION_LIST::EIterator pos = m_listUndoActions.Begin(); !pos.IsDone(); pos++ )
    {
        delete (*pos);
    }

    m_listUndoActions.Clear();
    m_CurrentPos = -1;
    m_PosOfLastSave = -1;
}

const char* ActionUndoMgr::GetRedoString()
{
    const char* strRet = NULL;

    if ( HaveRedo() )
    {
        ACTION_POS tmp = m_CurrentPos;
        tmp++;
        ACTION_LIST::EIterator iter = m_listUndoActions.Iterator( tmp );
        return (*iter)->getDescription();
    }

    return strRet;
}

const char* ActionUndoMgr::GetUndoString()
{
    const char* strRet = NULL;

    if ( HaveUndo() )
    {
        ACTION_LIST::EIterator iter = m_listUndoActions.Iterator( m_CurrentPos );
        return (*iter)->getDescription();
    }

    return strRet;
}

bool ActionUndoMgr::HaveRedo() const
{
    return m_CurrentPos + 1 != m_listUndoActions.Count();
}

bool ActionUndoMgr::HaveUndo() const
{
    return m_CurrentPos != -1;
}

bool ActionUndoMgr::Redo()
{
    gosASSERT( HaveRedo() );

    m_CurrentPos++;
    ACTION_LIST::EIterator iter = m_listUndoActions.Iterator( m_CurrentPos );

    return (*iter)->redo();
}

void ActionUndoMgr::Reset()
{
    EmptyUndoList();
}

bool ActionUndoMgr::Undo()
{
    bool bRetVal = false;

    gosASSERT( HaveUndo() );

    ACTION_LIST::EIterator iter = m_listUndoActions.Iterator( m_CurrentPos );
    bRetVal = (*iter)->undo();
    m_CurrentPos --;

    return bRetVal;
}

void ActionUndoMgr::NoteThatASaveHasJustOccurred()
{
    m_PosOfLastSave = m_CurrentPos;
}

bool ActionUndoMgr::ThereHasBeenANetChangeFromWhenLastSaved()
{
    if (m_PosOfLastSave == m_CurrentPos)
    {
        return false;
    }
    else
    {
        return true;
    }
}

int ActionUndoMgr::GetActionCount() const
{
    return (int)m_listUndoActions.Count();
}

//************************************************************************
// Function:    GetActionDescription
// ParamsIn:    index -- 0-based index into the undo list
// ParamsOut:   none
// Returns:     description string of the action at that index, or "" on
//              out-of-range. Never returns NULL.
// Description: Read-only accessor for display panels.
//              Uses the same EIterator/Begin()/IsDone() pattern as the
//              rest of Action.cpp.
//************************************************************************
const char* ActionUndoMgr::GetActionDescription( int index ) const
{
    if ( index < 0 || index >= (int)m_listUndoActions.Count() )
        return "";

    int i = 0;
    for ( ACTION_LIST::EConstIterator iter = m_listUndoActions.Begin();
          !iter.IsDone(); iter++, ++i )
    {
        if ( i == index )
        {
            const char* desc = (*iter)->getDescription();
            return desc ? desc : "";
        }
    }

    return "";
}

int ActionUndoMgr::GetCurrentPosition() const
{
    return (int)m_CurrentPos;
}

bool ActionPaintTile::redo()
{
    return doRedo();
}

bool ActionPaintTile::doRedo()
{
    for ( VERTEX_INFO_LIST::EIterator iter = vertexInfoList.Begin();
        !iter.IsDone(); iter++ )
    {
        int terrain = land->getTerrain( (*iter).row, (*iter).column );
        int texture = land->getTexture( (*iter).row, (*iter).column );
        float elv = land->getTerrainElevation( (*iter).row, (*iter).column );

        Overlays overlay;
        unsigned long offset;

        land->terrainTextures->getOverlayInfoFromHandle( (*iter).textureData, overlay, offset );
        land->setOverlay( (*iter).row, (*iter).column, overlay, offset );
        land->setTerrain( (*iter).row, (*iter).column, (*iter).terrainData );
        land->setVertexHeight( (*iter).row * land->realVerticesMapSide + (*iter).column, (*iter).elevation );

        (*iter).terrainData = terrain;
        (*iter).textureData = texture;
        (*iter).elevation = elv;
    }

    // setOverlay/setTerrain above NULLed the terrain face cache; rebuild so the
    // undone/redone tiles don't render black.
    EditorData::refreshTerrainAfterEdit();
    return true;
}

bool ActionPaintTile::undo()
{
    // actually, undo does the same thing as redo.
    return doRedo();
}

void ActionPaintTile::addVertexInfo( VertexInfo& info )
{
    for( VERTEX_INFO_LIST::EIterator iter = vertexInfoList.Begin();
        !iter.IsDone(); iter++ )
    {
        if ( info.row == (*iter).row && info.column == (*iter).column )
            return;
    }

    vertexInfoList.Append( info );
}

void ActionPaintTile::addChangedVertexInfo( int row, int column )
{
    for( VERTEX_INFO_LIST::EIterator iter = vertexInfoList.Begin();
        !iter.IsDone(); iter++ )
    {
        if ( row == (*iter).row && column == (*iter).column )
            return;
    }

    VertexInfo info( row, column );
    vertexInfoList.Append( info );
}

bool ActionPaintTile::getOldHeight( int row, int column, float& height )
{
    for( VERTEX_INFO_LIST::EIterator iter = vertexInfoList.Begin();
        !iter.IsDone(); iter++ )
    {
        if ( row == (*iter).row && column == (*iter).column )
        {
            height = (*iter).elevation;
            return true;
        }
    }

    return false;
}

VertexInfo::VertexInfo( long newRow, long newColumn )
{
    gosASSERT( newRow > -1 && newColumn > -1 );
    gosASSERT( newRow < land->realVerticesMapSide && newColumn < land->realVerticesMapSide );

    row = newRow;
    column = newColumn;

    elevation = land->getTerrainElevation( row, column );
    terrainData = land->getTerrain( row, column );
    textureData = land->getTexture( row, column );
}

#ifndef EDITOROBJECTMGR_H
#include "EditorObjectMgr.h"
#endif

#include "EditorObjects.h"   // Unit (ModifyUnitOrderAction)

ModifyBuildingAction::EditorAppearanceSnapshot::EditorAppearanceSnapshot()
{
    /*
        Keep this constructor boring and explicit.

        EList constructs/copies node payloads by value. The snapshot must be a
        plain value type so the editor can keep using the original undo-list
        mechanics without requiring ObjectAppearance itself to be concrete.
    */
    lightIntensity = 0.0f;
    rotation = 0.0f;
    selected = 0;
    teamId = 0;
    homeTeamRelationship = 0;
    actualRotation = 0.0f;
    objectNameId = 0;
    damage = 0;
    pilotNameID = 0;
    pilotName[0] = 0;
    paintScheme = 0;
    fadeTable = NULL;
}

ModifyBuildingAction::EditorAppearanceSnapshot::EditorAppearanceSnapshot(const ObjectAppearance& src)
{
    captureFrom(src);
}

void ModifyBuildingAction::EditorAppearanceSnapshot::captureFrom(const ObjectAppearance& src)
{
    /*
        Editor migration note:

        Original editor code used:
            ObjectAppearance savedAppearance = *object->appearance();

        Remastered's appearance hierarchy no longer permits that value-copy
        path. This method captures the public state the editor actually swaps
        during ModifyBuildingAction undo/redo.

        This is deliberately local to Editor/Action.*. We are not changing the
        renderer branch's ObjectAppearance hierarchy, and we are not changing
        EList's storage model globally.
    */
    lightIntensity = src.lightIntensity;

    shapeMin = src.shapeMin;
    shapeMax = src.shapeMax;

    position = src.position;
    rotation = src.rotation;
    selected = src.selected;
    teamId = src.teamId;
    homeTeamRelationship = src.homeTeamRelationship;
    actualRotation = src.actualRotation;
    objectNameId = src.objectNameId;
    damage = src.damage;
    pilotNameID = src.pilotNameID;

    strncpy(pilotName, src.pilotName, sizeof(pilotName));
    pilotName[sizeof(pilotName) - 1] = 0;

    paintScheme = src.paintScheme;

    /*
        This intentionally preserves the old shallow-copy behavior.

        The previous ObjectAppearance value copy would have copied fadeTable as
        a pointer-like MemoryPtr member. We do the same here rather than trying
        to invent ownership semantics inside an undo snapshot.
    */
    fadeTable = src.fadeTable;
}

void ModifyBuildingAction::EditorAppearanceSnapshot::applyTo(ObjectAppearance& dst) const
{
    /*
        Apply in the same spirit as the old ObjectAppearance assignment.

        Damage is applied through setDamage() because the editor-facing setter
        also updates the status/bar state that visual refresh code depends on.
    */
    dst.lightIntensity = lightIntensity;

    dst.shapeMin = shapeMin;
    dst.shapeMax = shapeMax;

    dst.position = position;
    dst.rotation = rotation;
    dst.selected = selected;
    dst.teamId = teamId;
    dst.homeTeamRelationship = homeTeamRelationship;
    dst.actualRotation = actualRotation;
    dst.objectNameId = objectNameId;
    dst.pilotNameID = pilotNameID;

    strncpy(dst.pilotName, pilotName, sizeof(dst.pilotName));
    dst.pilotName[sizeof(dst.pilotName) - 1] = 0;

    dst.paintScheme = paintScheme;
    dst.fadeTable = fadeTable;

    dst.setDamage(damage);
}

ModifyBuildingAction::~ModifyBuildingAction()
{
    for ( OBJ_INFO_PTR_LIST::EIterator iter = buildingCopyPtrs.Begin(); !iter.IsDone(); iter++)
    {
        delete (*iter);
    }
}

bool ModifyBuildingAction::doRedo()
{
    bool bRetVal = true;

    OBJ_INFO_PTR_LIST::EIterator iter2 = buildingPtrs.Begin();
    OBJ_APPEAR_LIST::EIterator iter3 = buildingAppearanceCopies.Begin();
    OBJ_ID_LIST::EIterator iter4 = buildingIDs.Begin();

    for ( OBJ_INFO_PTR_LIST::EIterator iter = buildingCopyPtrs.Begin();
        !iter.IsDone(); iter++)
    {
        EditorObject *pBuilding = EditorObjectMgr::instance()->getObjectAtLocation((*iter4).x, (*iter4).y);

        if (pBuilding)
        {
            EditorObject *pBuildingSwap = (*iter)->Clone();

            /*
                Editor migration note:

                This is the old swap algorithm, but using a local snapshot type
                instead of ObjectAppearance-by-value.

                Old behavior:
                    1. Copy saved appearance to temporary.
                    2. Replace saved appearance with current appearance.
                    3. Replace current appearance with temporary.

                New behavior:
                    1. Copy saved snapshot to temporary.
                    2. Capture current appearance into saved snapshot.
                    3. Apply temporary snapshot back onto the live appearance.

                That keeps undo and redo symmetric while avoiding construction
                of ObjectAppearance itself.
            */
            EditorAppearanceSnapshot appearanceSwap = (*iter3);

            (*iter)->CastAndCopy(*pBuilding);
            (*iter3).captureFrom(*(pBuilding->appearance()));

            (*pBuilding).CastAndCopy(*pBuildingSwap);
            appearanceSwap.applyTo(*(pBuilding->appearance()));

            delete pBuildingSwap;

            {
                /*this is just to make sure the visuals are up-to-date*/
                bool d = pBuilding->getDamage();
                pBuilding->setDamage(!d);
                pBuilding->setDamage(d);
            }

            long row, column;
            pBuilding->getCells(row, column);
            // Preserve the snapshot's (possibly free, sub-cell) position across the
            // cell/link bookkeeping moveBuilding, so undo/redo of a freely-placed
            // building does not snap it back to the grid.
            ObjectAppearance* pUndoApp = pBuilding->appearance();
            Stuff::Vector3D undoFreePos;
            if (pUndoApp)
                undoFreePos = pUndoApp->position;
            EditorObjectMgr::instance()->moveBuilding(pBuilding, row, column);
            if (pUndoApp)
            {
                pUndoApp->position = undoFreePos;
                pUndoApp->invalidateStaticRegistration();
                pUndoApp->update();
                pUndoApp->registerStatic();
            }

            (*iter4).x = pBuilding->getPosition().x;
            (*iter4).y = pBuilding->getPosition().y;
        }
        else
        {
            gosASSERT(false);
        }

        iter2++;
        iter3++;
        iter4++;
    }

    return bRetVal;
}

bool ModifyBuildingAction::redo()
{
    return doRedo();
}

bool ModifyBuildingAction::undo()
{
    // actually, undo does the same thing as redo.
    return doRedo();
}

void ModifyBuildingAction::addBuildingInfo(EditorObject& info)
{
    if ((0 < buildingPtrs.Count()) && (OBJ_INFO_PTR_LIST::INVALID_ITERATOR != buildingPtrs.Find(&info)))
    {
        return;
    }

    EditorObject *pInfoCopy = info.Clone();
    buildingCopyPtrs.Append( pInfoCopy );

    /*
        Store an editor-owned appearance snapshot instead of trying to append
        ObjectAppearance by value. Remastered owns the polymorphic appearance
        model; the editor only needs undo/redo state.
    */
    EditorAppearanceSnapshot appearanceSnapshot(*(info.appearance()));
    buildingAppearanceCopies.Append(appearanceSnapshot);

    buildingPtrs.Append( &info );

    CObjectID id;
    id.x = info.getPosition().x;
    id.y = info.getPosition().y;
    buildingIDs.Append(id);
}


#ifndef FOREST_H
#include "Forest.h"
#endif

ForestAction::ForestAction( const Forest& capturedForest )
    : Action( "Place Forest" )
    , m_forest( new Forest( capturedForest ) )
{
    // m_forest is a heap copy of the Forest that was just created (held by
    // pointer so Action.h can forward-declare Forest and avoid pulling in
    // Forest.h's winsock-tainted include chain).  Its ID is the handle
    // EditorObjectMgr assigned; we use it in undo() to find and remove the same
    // forest, and pass the whole struct to createForest() in redo() so it can
    // be reproduced faithfully.
    //
    // AddAction() does NOT call redo() — the initial create has already
    // happened at the call site before AddAction is invoked.  redo() is
    // therefore only ever called by ActionUndoMgr::Redo() (i.e. after a
    // preceding undo), so there is no double-create risk.
}

ForestAction::~ForestAction()
{
    delete m_forest;
}

bool ForestAction::undo()
{
    // removeForest() identifies the entry by forest.getID() and deletes all
    // associated building objects.  selectForest() is for visual highlighting
    // only and is not required for removal.
    if ( m_forest )
        EditorObjectMgr::instance()->removeForest( *m_forest );
    return true;
}

bool ForestAction::redo()
{
    // Re-create using the captured params.  createForest() assigns a new
    // internal ID each time; we must update m_forest.ID so the next undo()
    // targets the right entry.
    //
    // Forest::ID is private with no public setter and ForestAction is not a
    // friend of Forest (we cannot edit Forest.h per task scope).  Work around
    // by fetching the live Forest pointer back from EditorObjectMgr after
    // creation and doing a full copy — getForests fills a caller-supplied
    // pointer array with up to `count` entries.  Forests are few (dozens at
    // most), so a small stack array is safe; we size conservatively at 256.
    if ( !m_forest )
        return true;
    long newID = EditorObjectMgr::instance()->createForest( *m_forest );

    const long kMaxForests = 256;
    Forest* forestPtrs[kMaxForests];
    long count = kMaxForests;
    long got = EditorObjectMgr::instance()->getForests( forestPtrs, count );
    if ( got > 0 )
    {
        for ( long i = 0; i < got; ++i )
        {
            if ( forestPtrs[i] && forestPtrs[i]->getID() == newID )
            {
                *m_forest = *forestPtrs[i]; // copies ID + all params
                break;
            }
        }
    }
    return true;
}

void ModifyBuildingAction::updateNotedObjectPositions()
{
    OBJ_ID_LIST::EIterator iter4 = buildingIDs.Begin();

    for ( OBJ_INFO_PTR_LIST::EIterator iter = buildingPtrs.Begin(); !iter.IsDone(); iter++)
    {
        EditorObject *pBuilding = (*iter);

        if (pBuilding)
        {
            (*iter4).x = pBuilding->getPosition().x;
            (*iter4).y = pBuilding->getPosition().y;
        }
        else
        {
            gosASSERT(false);
        }

        iter4++;
    }
}


static void EditorOrderSnapFromUnit( Unit* unit, int& orderType, int& stance,
	std::vector<Stuff::Vector3D>& waypoints )
{
	orderType = unit->getOrderType();
	stance    = unit->getStance();
	waypoints = unit->getWaypoints();
}

void ModifyUnitOrderAction::capture( Unit* unit )
{
	if ( !unit )
		return;
	m_unitId = unit->getID();
	EditorOrderSnapFromUnit( unit, m_before.orderType, m_before.stance, m_before.waypoints );
}

void ModifyUnitOrderAction::commit( Unit* unit )
{
	if ( !unit )
		return;
	EditorOrderSnapFromUnit( unit, m_after.orderType, m_after.stance, m_after.waypoints );
}

bool ModifyUnitOrderAction::apply( const Snap& s )
{
	// Re-find the unit by editor id so a since-deleted unit is a safe no-op (no
	// stale pointer). getUnits() returns a copy of the live unit list.
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if ( !mgr )
		return false;
	EditorObjectMgr::UNIT_LIST list = mgr->getUnits();
	for ( EditorObjectMgr::UNIT_LIST::EIterator it = list.Begin(); !it.IsDone(); it++ )
	{
		Unit* u = *it;
		if ( !u || u->getID() != m_unitId )
			continue;
		u->setOrderType( s.orderType );
		u->setStance( s.stance );
		u->clearWaypoints();
		for ( size_t i = 0; i < s.waypoints.size(); ++i )
			u->addWaypoint( s.waypoints[i] );
		return true;
	}
	return false;
}

bool ModifyUnitOrderAction::redo() { return apply( m_after ); }
bool ModifyUnitOrderAction::undo() { return apply( m_before ); }
