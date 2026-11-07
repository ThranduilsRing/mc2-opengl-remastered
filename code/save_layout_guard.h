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

#include <cstddef>
#include "gameobj.h"
#include "mover.h"
#include "tacordr.h"
#include "dcontact.h"
#include "logisticsvariant.h"
#include "unit_profile_validator.h"
#include "controlgui.h"
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

// LG14 lane 0: records nested inside the pilot/mover records, and the enum-typed fields inside them. save/net layout pinned; LG14 enum-class
// conversion must keep it (every enum below stays 4 bytes at its current offset).
MC2_SAVE_STRUCT_SIZE(TacticalOrder, 332);
MC2_SAVE_STRUCT_SIZE(MoverControl, 48);
MC2_SAVE_STRUCT_SIZE(MoverDynamics, 40);
MC2_SAVE_STRUCT_SIZE(TacOrderMoveParams, 224);
MC2_SAVE_STRUCT_SIZE(TacOrderAttackParams, 36);
MC2_SAVE_STRUCT_SIZE(RadioLog, 24);

#define MC2_ENUM_FIELD(T, F, E, OFF) \
	static_assert(sizeof(E) == 4 && offsetof(T, F) == (OFF), #T "::" #F " (" #E ") is save/net layout: size 4 at a fixed offset")
MC2_ENUM_FIELD(TacticalOrder, origin, OrderOriginType, 20);
MC2_ENUM_FIELD(TacticalOrder, code, TacticalOrderCode, 24);
MC2_ENUM_FIELD(TacOrderMoveParams, mode, SpecialMoveMode, 208);
MC2_ENUM_FIELD(TacOrderAttackParams, type, AttackType, 0);
MC2_ENUM_FIELD(TacOrderAttackParams, method, AttackMethod, 4);
MC2_ENUM_FIELD(TacOrderAttackParams, range, FireRangeType, 8);
MC2_ENUM_FIELD(TacOrderAttackParams, tactic, TacticType, 12);
MC2_ENUM_FIELD(MoverDynamics, type, DynamicsType, 8);
MC2_ENUM_FIELD(MoverControl, type, ControlType, 8);
MC2_ENUM_FIELD(MoverControl, dataType, ControlDataType, 12);
MC2_ENUM_FIELD(RadioLog, lastMessageType, RadioMessageType, 12);
MC2_ENUM_FIELD(GameObjectData, objectClass, ObjectClass, 4);
MC2_ENUM_FIELD(CarnageData, carnageType, CarnageEnumType, 372);
#undef MC2_ENUM_FIELD
static_assert(offsetof(MechWarriorData, radioLog) == 224);
static_assert(offsetof(MoverData, control) == 2952);
static_assert(offsetof(MoverData, dynamics) == 3000);

#undef MC2_SAVE_STRUCT_SIZE
#endif // _MSC_VER && _WIN64

// Enumerator VALUE pins (ABI-independent): these enums are persisted in saves, packed into net packets, or exposed to ABL by number. The
// LG14 enum-class conversion keeps every value; a renumber is a format change and updates this list in the same commit.
#define MC2_PIN(E, N) static_assert(static_cast<int>(E) == (N), #E " value is persisted/ABL/net: do not renumber")
MC2_PIN(MOVE_SPEED_BEST, 0); MC2_PIN(MOVE_SPEED_MAXIMUM, 1); MC2_PIN(MOVE_SPEED_SLOW, 2); MC2_PIN(MOVE_SPEED_MODERATE, 3);
MC2_PIN(MOVE_PATTERN_DIRECT, 0); MC2_PIN(MOVE_PATTERN_ZIGZAG_NARROW, 1); MC2_PIN(MOVE_PATTERN_ZIGZAG_MEDIUM, 2); MC2_PIN(MOVE_PATTERN_ZIGZAG_WIDE, 3);
MC2_PIN(MOVE_PATTERN_RANDOM_NARROW, 4); MC2_PIN(MOVE_PATTERN_RANDOM_MEDIUM, 5); MC2_PIN(MOVE_PATTERN_RANDOM_WIDE, 6);
MC2_PIN(ATTITUDE_CAUTIOUS, 0); MC2_PIN(ATTITUDE_CONSERVATIVE, 1); MC2_PIN(ATTITUDE_NORMAL, 2); MC2_PIN(ATTITUDE_AGGRESSIVE, 3);
MC2_PIN(ATTITUDE_BERSERKER, 4); MC2_PIN(ATTITUDE_SUICIDAL, 5);
MC2_PIN(FIRERANGE_DEFAULT, -4); MC2_PIN(FIRERANGE_RAMMING, -3); MC2_PIN(FIRERANGE_LONGEST, -2); MC2_PIN(FIRERANGE_OPTIMAL, -1);
MC2_PIN(FIRERANGE_SHORT, 0); MC2_PIN(FIRERANGE_MEDIUM, 1); MC2_PIN(FIRERANGE_LONG, 2); MC2_PIN(FIRERANGE_CURRENT, 3); MC2_PIN(NUM_FIRERANGES, 4);
MC2_PIN(ATTACK_NONE, 0); MC2_PIN(ATTACK_TO_DESTROY, 1); MC2_PIN(ATTACK_TO_DISABLE, 2); MC2_PIN(ATTACK_CONSERVING_AMMO, 3); MC2_PIN(NUM_ATTACK_TYPES, 4);
MC2_PIN(ATTACKMETHOD_RANGED, 0); MC2_PIN(ATTACKMETHOD_DFA, 1); MC2_PIN(ATTACKMETHOD_RAMMING, 2); MC2_PIN(NUM_ATTACKMETHODS, 3);
MC2_PIN(FireOddsType::FIREODDS_LUCKY, 0); MC2_PIN(FireOddsType::FIREODDS_BAD, 1); MC2_PIN(FireOddsType::FIREODDS_MEDIUM, 2);
MC2_PIN(FireOddsType::FIREODDS_GOOD, 3); MC2_PIN(FireOddsType::FIREODDS_GREAT, 4); MC2_PIN(FireOddsType::NUM_FIREODDS, 5);
MC2_PIN(TACTIC_NONE, 0); MC2_PIN(TACTIC_FLANK_LEFT, 1); MC2_PIN(TACTIC_FLANK_RIGHT, 2); MC2_PIN(TACTIC_FLANK_REAR, 3); MC2_PIN(TACTIC_STOP_AND_FIRE, 4);
MC2_PIN(TACTIC_TURRET, 5); MC2_PIN(TACTIC_JOUST, 6); MC2_PIN(TACTIC_INDIRECT_FIRE, 7); MC2_PIN(TACTIC_HULL_DOWN, 8); MC2_PIN(TACTIC_FIGHTING_WITHDRAW, 9);
MC2_PIN(TACTIC_PURSUE, 10); MC2_PIN(TACTIC_HIT_AND_RUN, 11); MC2_PIN(NUM_TACTICS, 12);
MC2_PIN(ORDER_ORIGIN_PLAYER, 0); MC2_PIN(ORDER_ORIGIN_COMMANDER, 1); MC2_PIN(ORDER_ORIGIN_SELF, 2);
MC2_PIN(TACTICAL_ORDER_NONE, 0); MC2_PIN(TACTICAL_ORDER_WAIT, 1); MC2_PIN(TACTICAL_ORDER_MOVETO_POINT, 2); MC2_PIN(TACTICAL_ORDER_MOVETO_OBJECT, 3);
MC2_PIN(TACTICAL_ORDER_JUMPTO_POINT, 4); MC2_PIN(TACTICAL_ORDER_JUMPTO_OBJECT, 5); MC2_PIN(TACTICAL_ORDER_TRAVERSE_PATH, 6); MC2_PIN(TACTICAL_ORDER_PATROL_PATH, 7);
MC2_PIN(TACTICAL_ORDER_ESCORT, 8); MC2_PIN(TACTICAL_ORDER_FOLLOW, 9); MC2_PIN(TACTICAL_ORDER_GUARD, 10); MC2_PIN(TACTICAL_ORDER_STOP, 11);
MC2_PIN(TACTICAL_ORDER_POWERUP, 12); MC2_PIN(TACTICAL_ORDER_POWERDOWN, 13); MC2_PIN(TACTICAL_ORDER_WAYPOINTS_DONE, 14); MC2_PIN(TACTICAL_ORDER_EJECT, 15);
MC2_PIN(TACTICAL_ORDER_ATTACK_OBJECT, 16); MC2_PIN(TACTICAL_ORDER_ATTACK_POINT, 17); MC2_PIN(TACTICAL_ORDER_HOLD_FIRE, 18); MC2_PIN(TACTICAL_ORDER_WITHDRAW, 19);
MC2_PIN(TACTICAL_ORDER_SCRAMBLE, 20); MC2_PIN(TACTICAL_ORDER_CAPTURE, 21); MC2_PIN(TACTICAL_ORDER_REFIT, 22); MC2_PIN(TACTICAL_ORDER_GETFIXED, 23);
MC2_PIN(TACTICAL_ORDER_LOAD_INTO_CARRIER, 24); MC2_PIN(TACTICAL_ORDER_DEPLOY_ELEMENTALS, 25); MC2_PIN(TACTICAL_ORDER_RECOVER, 26); MC2_PIN(NUM_TACTICAL_ORDERS, 27);
MC2_PIN(TRAVEL_MODE_INVALID, -1); MC2_PIN(TRAVEL_MODE_SLOW, 0); MC2_PIN(TRAVEL_MODE_FAST, 1); MC2_PIN(TRAVEL_MODE_JUMP, 2); MC2_PIN(NUM_TRAVEL_MODES, 3);
MC2_PIN(MOVE_MODE_NORMAL, 0); MC2_PIN(MOVE_MODE_MINELAYING, 1);
MC2_PIN(CONTACT_SORT_NONE, 0); MC2_PIN(CONTACT_SORT_CV, 1); MC2_PIN(CONTACT_SORT_DISTANCE, 2); MC2_PIN(NUM_CONTACT_SORTS, 3);
MC2_PIN(HEAD, 0); MC2_PIN(CHEST, 1); MC2_PIN(LEGS, 2);
MC2_PIN(UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR, 0); MC2_PIN(UPV_HARDPOINT_DAMAGE_LOCATION_MISSING, 1); MC2_PIN(UPV_HARDPOINT_SOCKET_MISSING, 2);
MC2_PIN(UPV_ANIMATION_FALLBACK_MISSING, 3); MC2_PIN(UPV_HARDPOINT_WEAPON_CLASS_REJECTED, 4); MC2_PIN(UPV_UI_ASSET_MISSING, 5); MC2_PIN(UPV__COUNT, 6);
MC2_PIN(ControlButton::ENABLED, 0); MC2_PIN(ControlButton::PRESSED, 1); MC2_PIN(ControlButton::DISABLED, 2); MC2_PIN(ControlButton::AMBIGUOUS, 3);
MC2_PIN(ControlButton::HIDDEN, 4);
#undef MC2_PIN

#endif // CODE_SAVE_LAYOUT_GUARD_H
