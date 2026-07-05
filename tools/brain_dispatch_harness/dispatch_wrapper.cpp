// BRAIN-DISPATCH-HARNESS-1: dispatch wrapper.
// Includes stub headers BEFORE the real brain_special_dispatch.cpp so that
// stub warrior.h/tacordr.h/gameobj.h/objmgr.h/inifile.h win over the engine
// versions in code/ (which MSVC would otherwise prefer since the .cpp is in code/).
//
// This is compiled FROM tools/brain_dispatch_harness/ so there is no local warrior.h,
// tacordr.h, etc. — the stubs/include/ versions are found first.

// Pull stubs first — these define all engine types the dispatch TU needs.
#include "warrior.h"    // stub MechWarrior
#include "tacordr.h"    // stub TacticalOrder, enums
#include "gameobj.h"    // stub GameObject, GameObjectPtr
#include "objmgr.h"     // stub ObjectManager global
#include "inifile.h"    // stub FitIniFile
// HARNESS-STUB-REPAIR-1: BRAIN-ENGAGE-1 added mover/dcontact/contact includes to the
// dispatch TU. Pull the stubs first so MOVER_H/DCONTACT_H/CONTACT_H are defined and the
// engine headers (which drag mclib.h) are skipped.
#include "mover.h"      // stub Mover/MoverPtr
#include "dcontact.h"   // MAX_CONTACTS_PER_SENSOR, CONTACT_SORT_*
#include "contact.h"    // guard-only (SensorSystem not referenced by the TU)

// Now include-and-compile the real dispatch TU.
// The #include guards in the engine headers (WARRIOR_H, TACORDR_H, etc.) are now
// already defined by our stubs above, so the engine headers will be skipped.
#include "../../code/brain_special_dispatch.cpp"
