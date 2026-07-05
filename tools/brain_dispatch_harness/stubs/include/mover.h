#pragma once
#ifndef MOVER_H
#define MOVER_H
// HARNESS-STUB-REPAIR-1: mover.h stub (BRAIN-ENGAGE-1 / BRAIN-OPORD-MOVE-1 deps).
// Defines MOVER_H so the engine code/mover.h (which pulls mclib.h) is skipped.
// Provides only what brain_special_dispatch.cpp's tick functions reference.
// The harness never enables MC2_BRAIN_ENGAGE and stub getVehicle() returns nullptr,
// so none of these methods execute — they only need to compile + link.

#include "gameobj.h"
#include "stuff_vector3d.h"

class MechWarrior;
typedef MechWarrior* MechWarriorPtr;

class Mover : public GameObject {
public:
    virtual long getContacts(int* /*contactList*/, long /*criteria*/, long /*sort*/) { return 0; }
    virtual float distanceFrom(Stuff::Vector3D /*pos*/) { return 0.0f; }
    virtual bool isEnemy(TeamPtr /*team*/) { return false; }
    virtual MechWarriorPtr getPilot() { return nullptr; }
};

typedef Mover* MoverPtr;

#endif // MOVER_H
