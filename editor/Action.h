/***************************************************************
* FILENAME: Action.h
* DESCRIPTION: Declares Editor action and undo manager interfaces.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

#ifndef ACTION_H
#define ACTION_H
//
// Actions.h - header file for the Action Base object and the Action Mgr.
//              Actions are things that can be undone
//
#include <string.h>

#define MAX_DESC_LENGTH 256

class ActionUndoMgr;
class Action;

#ifndef ELIST_H
#include "Elist.h"
#endif

#ifndef OBJECTAPPEARANCE_H
#include "objectappearance.h"
#endif

#include <vector>

class Action
{
public:

    virtual ~Action(){}
    virtual bool redo() = 0;
    virtual bool undo() = 0;
    Action& operator=( const Action& src );
    const char* getDescription(){  return m_strDescription; }

    char  m_strDescription[MAX_DESC_LENGTH];

protected:

    Action( const char* pStr )
    {
        gosASSERT( strlen( pStr ) < MAX_DESC_LENGTH );
        strcpy( m_strDescription, pStr );
    }

    // if you call this, make sure you set the description
    Action(){ m_strDescription[0] = 0; }
};

struct VertexInfo
{
    VertexInfo( long row, long column );
    ~VertexInfo(){}
    int row;
    int column;
    int terrainData;
    int textureData;
    float elevation;

private:
    // make sure the list class doesn't try and use this
    VertexInfo& operator=( const VertexInfo& );
};

typedef EList< VertexInfo, const VertexInfo& > VERTEX_INFO_LIST;

// for undo redo buffer, since it deals with
// the same stuff as the tile brush, I put
// it here
class ActionPaintTile : public Action
{
public:

    ActionPaintTile(){}

    virtual bool redo();
    virtual bool undo();

    bool doRedo(); // so we don't go through virtual functions

    ActionPaintTile( const char* pStr )
        : Action( pStr ){}

    void addChangedVertexInfo( int row, int column );
    void addVertexInfo( VertexInfo& );
    bool getOldHeight( int row, int column, float& oldHeight );

    VERTEX_INFO_LIST vertexInfoList;
};

#ifndef EDITOROBJECTS_H
#include "EditorObjects.h"
#endif

class ModifyBuildingAction : public Action
{
public:

    virtual ~ModifyBuildingAction();
    virtual bool redo();
    virtual bool undo();
    bool doRedo(); // so we don't go through virtual functions
    virtual void addBuildingInfo(EditorObject& info);
    virtual bool isNotNull()
    {
        return (0 < buildingCopyPtrs.Count());
    }
    virtual void updateNotedObjectPositions();

private:
    typedef EList< EditorObject *, EditorObject *> OBJ_INFO_PTR_LIST;

    /*
        Editor migration note:

        The original editor stored ObjectAppearance by value here:

            typedef EList< ObjectAppearance, ObjectAppearance& > OBJ_APPEAR_LIST;

        That no longer works against the Remastered renderer branch because
        ObjectAppearance is now a polymorphic appearance base with a protected
        constructor and abstract inherited behavior. EList stores its payload
        directly as `T m_Data`, so EList<ObjectAppearance, ...> forces the
        compiler to construct/copy ObjectAppearance itself.

        We intentionally do NOT patch Remastered's EList implementation.
        EList is engine-wide infrastructure and the Remastered code owns it.

        Instead, this editor action stores a narrow value snapshot of the
        public ObjectAppearance state it needs for undo/redo. The migration
        stays local to Editor/Action.*, preserves the old swap-style undo
        behavior, and avoids forcing an abstract renderer base through value
        semantics.
    */
    struct EditorAppearanceSnapshot
    {
        EditorAppearanceSnapshot();
        EditorAppearanceSnapshot(const ObjectAppearance& src);

        void captureFrom(const ObjectAppearance& src);
        void applyTo(ObjectAppearance& dst) const;

        float lightIntensity;

        Stuff::Vector2DOf<long> shapeMin;
        Stuff::Vector2DOf<long> shapeMax;

        Stuff::Vector3D position;
        float rotation;
        long selected;
        long teamId;
        long homeTeamRelationship;
        float actualRotation;
        long objectNameId;
        long damage;
        long pilotNameID;
        char pilotName[32];

        long paintScheme;
        MemoryPtr fadeTable;
    };

    typedef EList< EditorAppearanceSnapshot, const EditorAppearanceSnapshot& > OBJ_APPEAR_LIST;

    struct CObjectID
    {
        float x;
        float y;
    };
    typedef EList<CObjectID, CObjectID&> OBJ_ID_LIST;

    OBJ_INFO_PTR_LIST buildingCopyPtrs;
    OBJ_APPEAR_LIST buildingAppearanceCopies;

    OBJ_INFO_PTR_LIST buildingPtrs;
    OBJ_ID_LIST buildingIDs;
};

// Forward-declared only — do NOT #include "Forest.h" here. Forest.h transitively
// pulls <winsock2.h> after <windows.h>, and Action.h is included by every brush
// TU (FlattenBrush.h etc.), so including it here breaks their winsock ordering.
// The full Forest type is needed only in Action.cpp, which includes Forest.h.
class Forest;
class Unit;

// Undo/redo for the patrol/move order authoring (UnitBrainPanel). Snapshots a
// unit's order state (type/stance/waypoints) before and after an edit. undo/redo
// re-find the unit by its editor id (scan EditorObjectMgr units) and apply the
// snapshot, so an order edit on a since-deleted unit is a safe no-op. Pushing the
// action also marks the mission dirty (ActionUndoMgr net-change tracking).
class ModifyUnitOrderAction : public Action
{
public:
    ModifyUnitOrderAction() : Action( "Modify Unit Order" ), m_unitId( 0 ) {}
    virtual ~ModifyUnitOrderAction() {}
    virtual bool redo();
    virtual bool undo();
    void capture( Unit* unit );   // call BEFORE the edit: records id + before-state
    void commit( Unit* unit );    // call AFTER the edit: records after-state
private:
    struct Snap { int orderType; int stance; std::vector<Stuff::Vector3D> waypoints; };
    bool apply( const Snap& s );
    long m_unitId;
    Snap m_before;
    Snap m_after;
};

// Undo/redo action for EditorObjectMgr::createForest().
// undo()  -- removes the forest that was just created (by captured ID).
// redo()  -- re-creates it from the captured Forest params.
// NOTE: foliage-layer (FoliageRender PCG) undo is out of scope and deferred.
class ForestAction : public Action
{
public:
    // capturedForest must be the Forest value AFTER createForest() returns
    // (so its ID field is the one assigned by EditorObjectMgr).
    explicit ForestAction( const Forest& capturedForest );
    virtual ~ForestAction();

    virtual bool redo();
    virtual bool undo();

private:
    Forest* m_forest; // heap copy of the snapshot (ID + all placement params)
};

class ActionUndoMgr
{
public:
    ActionUndoMgr( );
    ~ActionUndoMgr();

    void AddAction( Action* pAction );
    bool Redo();
    bool Undo();
    void Reset();

    bool HaveUndo() const;
    bool HaveRedo() const;

    const char* GetUndoString();
    const char* GetRedoString();

    void NoteThatASaveHasJustOccurred();
    bool ThereHasBeenANetChangeFromWhenLastSaved();

    // Read-only accessors for display panels (e.g. UndoHistoryPanel).
    // These are const and side-effect-free; they do NOT alter undo/redo state.
    int GetActionCount() const;
    const char* GetActionDescription( int index ) const;
    int GetCurrentPosition() const;

    static ActionUndoMgr* instance;

private:

    typedef EList< Action*, Action* > ACTION_LIST;
    typedef long ACTION_POS;

    void EmptyUndoList();
    ACTION_LIST m_listUndoActions;
    ACTION_POS m_CurrentPos;
    ACTION_POS m_PosOfLastSave;
}; // class SActionMgr

#endif // ACTION_H
