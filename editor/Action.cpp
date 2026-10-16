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
#include <algorithm>

#include "terrain.h"
#include "Action.h"
#include "TerrTxm.h"
#include "EditorData.h"

ActionUndoMgr* ActionUndoMgr::instance = nullptr;

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

    if ( !m_listUndoActions.empty() )
    {
        if (m_PosOfLastSave > m_CurrentPos)
        {
            m_PosOfLastSave = -1;
        }

        while (static_cast<long>(m_listUndoActions.size()) - 1 > m_CurrentPos)
        {
            delete m_listUndoActions.back();
            m_listUndoActions.pop_back();
        }
    }

    m_listUndoActions.push_back( pAction );
    m_CurrentPos = static_cast<long>(m_listUndoActions.size()) - 1;
}

void ActionUndoMgr::EmptyUndoList()
{
    for (Action* action : m_listUndoActions)
    {
        delete action;
    }

    m_listUndoActions.clear();
    m_CurrentPos = -1;
    m_PosOfLastSave = -1;
}

const char* ActionUndoMgr::GetRedoString()
{
    const char* strRet = nullptr;

    if ( HaveRedo() )
    {
        ACTION_POS tmp = m_CurrentPos;
        tmp++;
        return m_listUndoActions[tmp]->getDescription();
    }

    return strRet;
}

const char* ActionUndoMgr::GetUndoString()
{
    const char* strRet = nullptr;

    if ( HaveUndo() )
    {
        return m_listUndoActions[m_CurrentPos]->getDescription();
    }

    return strRet;
}

bool ActionUndoMgr::HaveRedo() const
{
    return m_CurrentPos + 1 != static_cast<long>(m_listUndoActions.size());
}

bool ActionUndoMgr::HaveUndo() const
{
    return m_CurrentPos != -1;
}

bool ActionUndoMgr::Redo()
{
    gosASSERT( HaveRedo() );

    m_CurrentPos++;
    return m_listUndoActions[m_CurrentPos]->redo();
}

void ActionUndoMgr::Reset()
{
    EmptyUndoList();
}

bool ActionUndoMgr::Undo()
{
    bool bRetVal = false;

    gosASSERT( HaveUndo() );

    bRetVal = m_listUndoActions[m_CurrentPos]->undo();
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
    return (int)m_listUndoActions.size();
}

const char* ActionUndoMgr::GetActionDescription( int index ) const
{
    if ( index < 0 || index >= (int)m_listUndoActions.size() )
        return "";

    const char* desc = m_listUndoActions[index]->getDescription();
    return desc ? desc : "";
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
    for (auto iter = vertexInfoList.begin(); iter != vertexInfoList.end(); ++iter)
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
    for (auto iter = vertexInfoList.begin(); iter != vertexInfoList.end(); ++iter)
    {
        if ( info.row == (*iter).row && info.column == (*iter).column )
            return;
    }

    vertexInfoList.push_back( info );
}

void ActionPaintTile::addChangedVertexInfo( int row, int column )
{
    for (auto iter = vertexInfoList.begin(); iter != vertexInfoList.end(); ++iter)
    {
        if ( row == (*iter).row && column == (*iter).column )
            return;
    }

    VertexInfo info( row, column );
    vertexInfoList.push_back( info );
}

bool ActionPaintTile::getOldHeight( int row, int column, float& height )
{
    for (auto iter = vertexInfoList.begin(); iter != vertexInfoList.end(); ++iter)
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
    fadeTable = nullptr;
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
    for (EditorObject* copy : buildingCopyPtrs)
    {
        delete copy;
    }
}

bool ModifyBuildingAction::doRedo()
{
    bool bRetVal = true;

    auto iter3 = buildingAppearanceCopies.begin();
    auto iter4 = buildingIDs.begin();

    for (auto iter = buildingCopyPtrs.begin(); iter != buildingCopyPtrs.end(); ++iter)
    {
        EditorObject *pBuilding = EditorObjectMgr::instance()->getObjectAtLocation((*iter4).x, (*iter4).y);

        if (pBuilding)
        {
            EditorObject *pBuildingSwap = (*iter)->Clone();

            // Swapping the saved and live states makes undo and redo symmetric.
            EditorAppearanceSnapshot appearanceSwap = (*iter3);

            (*iter)->CastAndCopy(*pBuilding);
            (*iter3).captureFrom(*(pBuilding->appearance()));

            (*pBuilding).CastAndCopy(*pBuildingSwap);
            appearanceSwap.applyTo(*(pBuilding->appearance()));

            delete pBuildingSwap;

            {
                // Damage transitions refresh the visuals.
                bool d = pBuilding->getDamage();
                pBuilding->setDamage(!d);
                pBuilding->setDamage(d);
            }

            long row, column;
            pBuilding->getCells(row, column);
            // Preserve free placement across the cell/link bookkeeping update.
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
    if (std::find(buildingPtrs.begin(), buildingPtrs.end(), &info) != buildingPtrs.end())
    {
        return;
    }

    EditorObject *pInfoCopy = info.Clone();
    buildingCopyPtrs.push_back( pInfoCopy );

    EditorAppearanceSnapshot appearanceSnapshot(*(info.appearance()));
    buildingAppearanceCopies.push_back(appearanceSnapshot);

    buildingPtrs.push_back( &info );

    CObjectID id;
    id.x = info.getPosition().x;
    id.y = info.getPosition().y;
    buildingIDs.push_back(id);
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
    auto iter4 = buildingIDs.begin();

    for (auto iter = buildingPtrs.begin(); iter != buildingPtrs.end(); ++iter)
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

//*************************************************************************************************
// ModifyUnitOrderAction — undo/redo for patrol/move order authoring (UnitBrainPanel)
//*************************************************************************************************

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
	m_before.authored = unit->isOrderAuthored();
}

void ModifyUnitOrderAction::commit( Unit* unit )
{
	if ( !unit )
		return;
	EditorOrderSnapFromUnit( unit, m_after.orderType, m_after.stance, m_after.waypoints );
	m_after.authored = unit->isOrderAuthored();
}

bool ModifyUnitOrderAction::apply( const Snap& s )
{
	// Re-find the unit by editor id so a since-deleted unit is a safe no-op (no
	// stale pointer). getUnits() returns a copy of the live unit list.
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if ( !mgr )
		return false;
	EditorObjectMgr::UNIT_LIST list = mgr->getUnits();
	for ( auto it = list.begin(); it != list.end(); it++ )
	{
		Unit* u = *it;
		if ( !u || u->getID() != m_unitId )
			continue;
		u->setOrderType( s.orderType );
		u->setStance( s.stance );
		u->clearWaypoints();
		for ( size_t i = 0; i < s.waypoints.size(); ++i )
			u->addWaypoint( s.waypoints[i] );
		// Undo must return ownership to the brain importer when the order was not authored.
		u->setOrderAuthored( s.authored );
		return true;
	}
	return false;
}

bool ModifyUnitOrderAction::redo() { return apply( m_after ); }
bool ModifyUnitOrderAction::undo() { return apply( m_before ); }
