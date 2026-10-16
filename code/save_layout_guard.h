//============================================================================
// save_layout_guard.h -- the byte size of every struct the game writes into a save file.
//
// WHAT: one static_assert per struct that a Save function writes verbatim into a packet of the save file (and the matching Load reads back).
// WHY: the save format IS the struct layout. The object loader tells a record's class from its byte size (GameObjectManager::Load), the mission
// loader reads every other struct with one readPacket of sizeof(X), and a save written by a build whose struct has another size is a different
// format. Adding, reordering or retyping a field therefore changes the format silently: old saves would mis-read or be refused. This list makes that a
// build error at the edit that does it. A change that is meant to alter the format (a new packet, a version stamp bump, a format extension) updates the
// number here in the same commit and says so.
// The numbers are the 64-bit MSVC layout (these structs use `long`, which is 4 bytes on Windows and 8 elsewhere); on another ABI the guard is off
// rather than wrong.
//============================================================================
#ifndef CODE_SAVE_LAYOUT_GUARD_H
#define CODE_SAVE_LAYOUT_GUARD_H

#include "gameobj.h"
#include "mover.h"
#include "objmgr.h"
#include "terrobj.h"
#include "bldng.h"
#include "turret.h"
#include "gate.h"
#include "artlry.h"
#include "carnage.h"
#include "mech.h"
#include "gvehicl.h"
#include "weaponbolt.h"
#include "warrior.h"
#include "team.h"
#include "comndr.h"
#include "ablexec.h"

#if defined(_MSC_VER) && defined(_WIN64)

#define MC2_SAVE_STRUCT_SIZE(T, N) \
	static_assert(sizeof(T) == (N), #T " is written into save files verbatim: its size is the save format. Update save_layout_guard.h in the same commit if the format change is intended.")

// Object records: one packet per object slot, the class is told from the packet size.
MC2_SAVE_STRUCT_SIZE(GameObjectData, 372);
MC2_SAVE_STRUCT_SIZE(MoverData, 3288);
MC2_SAVE_STRUCT_SIZE(TerrainObjectData, 720);
MC2_SAVE_STRUCT_SIZE(BuildingData, 756);
MC2_SAVE_STRUCT_SIZE(TurretData, 820);
MC2_SAVE_STRUCT_SIZE(GateData, 760);
MC2_SAVE_STRUCT_SIZE(ArtilleryData, 432);
MC2_SAVE_STRUCT_SIZE(CarnageData, 396);
MC2_SAVE_STRUCT_SIZE(MechData, 3992);
MC2_SAVE_STRUCT_SIZE(GroundVehicleData, 3424);
MC2_SAVE_STRUCT_SIZE(WeaponBoltData, 600);

// The object manager header, the pilots, the teams and the commanders: read back by size.
MC2_SAVE_STRUCT_SIZE(ObjectManagerData, 60);
MC2_SAVE_STRUCT_SIZE(StaticMechWarriorData, 40);
MC2_SAVE_STRUCT_SIZE(MechWarriorData, 19864);
MC2_SAVE_STRUCT_SIZE(TeamStaticData, 80);
MC2_SAVE_STRUCT_SIZE(TeamData, 488);
MC2_SAVE_STRUCT_SIZE(StaticCommanderData, 8);
MC2_SAVE_STRUCT_SIZE(CommanderData, 1288);

// The ABL environment writes eternal and static variables one StackItem at a time.
MC2_SAVE_STRUCT_SIZE(StackItem, 8);

#undef MC2_SAVE_STRUCT_SIZE
#endif // _MSC_VER && _WIN64

#endif // CODE_SAVE_LAYOUT_GUARD_H
