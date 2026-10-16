#pragma once
#ifndef OBJMGR_H
#define OBJMGR_H
// objmgr.h stub
// Provides ObjectManager global pointer + getByWatchID.
// No engine header dependencies.

#include "gameobj.h"
#include "mover.h"

class GameObjectManager {
public:
    virtual ~GameObjectManager() = default;
    virtual GameObjectPtr getByWatchID(long watchID) { return nullptr; }
    // Handle lookup and mover iteration.
    virtual GameObjectPtr get(long /*handle*/) { return nullptr; }
    virtual long          getNumMovers() { return 0; }
    virtual MoverPtr      getMover(long /*index*/) { return nullptr; }
};

typedef GameObjectManager* GameObjectManagerPtr;

// Global singleton pointer — defined in stubs/objmgr_stub.cpp
extern GameObjectManagerPtr ObjectManager;

#endif // OBJMGR_H
