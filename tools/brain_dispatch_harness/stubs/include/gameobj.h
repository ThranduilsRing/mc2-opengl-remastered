#pragma once
#ifndef GAMEOBJ_H
#define GAMEOBJ_H
// BRAIN-DISPATCH-HARNESS-1: gameobj.h stub
// Provides just enough for brain_special_dispatch.cpp to compile.
// Replaces the engine gameobj.h (which pulls mclib.h and the whole world).

#include <cstdint>
#include "stuff_vector3d.h"

// GameObjectWatchID — mirrors dgameobj.h (int32_t typedef)
typedef int32_t GameObjectWatchID;

// Team stub — brain_special_dispatch.cpp only uses pointer equality (getTeam() == getTeam())
class Team {};
typedef Team* TeamPtr;

// Minimal GameObject stub — only the methods brain_special_dispatch.cpp calls
class GameObject {
public:
    virtual ~GameObject() = default;
    virtual long    getWatchID(bool assign = true) { return 0; }
    virtual TeamPtr getTeam() { return nullptr; }
    // HARNESS-STUB-REPAIR-1: BRAIN-ENGAGE-1 / BRAIN-OPORD-MOVE-1 additions.
    virtual bool    isDisabled() { return false; }
    virtual Stuff::Vector3D getPosition() { return {}; }
};

typedef GameObject* GameObjectPtr;

#endif // GAMEOBJ_H
