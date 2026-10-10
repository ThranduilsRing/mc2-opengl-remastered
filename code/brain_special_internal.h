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
#include "contact.h"   // SensorSystem::getEffectiveRange — BRAIN-ENGAGE-1 detection range
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include "inifile.h"   // FitIniFile — same header used by _ai.fit loader
#include "Gates/gate.h"

#endif  // CODE_BRAIN_SPECIAL_INTERNAL_H
