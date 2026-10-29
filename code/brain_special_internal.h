// Shared surface of the brain_special_dispatch*.cpp pieces: the include bundle and the helpers the effect handlers
// of executeSpecialBody_Apply call.
#ifndef CODE_BRAIN_SPECIAL_INTERNAL_H
#define CODE_BRAIN_SPECIAL_INTERNAL_H

#include "brain_special_dispatch.h"
#include "brain_symbol_resolve.h"    // UNITQUERY-SETTARGETPRIORITY-1: symbolic-arg resolver
#include "brain_mission_var_store.h" // TECHSCRIPT-DISPATCH-1D-M: MissionVarStore global
#include "brain_world_snapshot.h" // BRAIN-WORLD-SNAPSHOT-1: BrainWorldSnapshot POD + gate
#include "warrior.h"   // MechWarrior — needed for executeSpecialBody_Apply setGeneralTacOrder call
                       // warrior.h includes mech_brain_runtime.h which defines VarStore + VarScope
#include "tacordr.h"   // TacticalOrder, TACTICAL_ORDER_POWERDOWN, ORDER_ORIGIN_SELF
#include "gameobj.h"   // GameObjectPtr, GameObject::getWatchID/getTeam — DISPATCH-EFFECT-COREATTACK-1
#include "objmgr.h"    // ObjectManager (global) — DISPATCH-EFFECT-COREATTACK-1
#include "mover.h"     // Mover::getContacts / getVehicle MoverPtr — BRAIN-ENGAGE-1
#include "dcontact.h"  // CONTACT_SORT_DISTANCE, MAX_CONTACTS_PER_SENSOR — BRAIN-ENGAGE-1
#include "dteam.h"     // MAX_CONTACTS_PER_TEAM: the length of every getContacts output array
#include "contact.h"   // SensorSystem::getEffectiveRange — BRAIN-ENGAGE-1 detection range
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include "inifile.h"   // FitIniFile — same header used by _ai.fit loader
#include "Gates/gate.h"

// Dispatch helpers the effect handlers call (brain_special_dispatch.cpp).
bool s_tacticWeightsBEnabled();
bool emitBrainIntent(MechBrainRuntime* runtime, int warriorId, TacticalOrderCode orderType, int targetWID,
                     float wx, float wy, float wz, uint32_t sourceBodyId);
bool emitBrainClearMoveIntent(MechBrainRuntime* runtime, int warriorId);
bool s_missionVarGate();
bool emitBrainTargetPolicyIntent(MechBrainRuntime* runtime, int warriorId,
                                 long slot, long type, long p1, long p2, long p3);

struct BrainVerbFrame;

// Effect handlers of executeSpecialBody_Apply (brain_special_apply.cpp).
void applyCorePower(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect);
void applyUnitEject(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect);
void applyCoreGuard(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect);
void applyCoreMoveTo(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect, BrainVerbFrame& f);
void applyCoreAttack(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect, BrainVerbFrame& f);
void applyUnitRetreat(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect);
void applyClearMoveOrders(int wid, MechBrainRuntime* runtime);
void applySetTargetPriority(int wid, MechBrainRuntime* runtime, BrainVerbFrame& f);
void applyCorePatrol(MechWarrior* warrior, int wid, MechBrainRuntime* runtime, bool& appliedEffect, BrainVerbFrame& f);
bool applySetState(MechWarrior* warrior, int wid, BrainVerbFrame& f);
bool applySetStatePrev(MechWarrior* warrior, int wid);
void applyInState(MechWarrior* warrior, int wid, bool& stateGateOpen, BrainVerbFrame& f);
void applyNotInState(MechWarrior* warrior, int wid, bool& stateGateOpen, BrainVerbFrame& f);
bool applySetStateIf(MechWarrior* warrior, int wid, VarStore* varStore, bool varGate, BrainVerbFrame& f);

#endif  // CODE_BRAIN_SPECIAL_INTERNAL_H
