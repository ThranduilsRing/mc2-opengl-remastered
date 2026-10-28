// dispatch wrapper.
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
// The dispatch TU requires mover/dcontact/contact types.
// Stubs must precede engine headers so MOVER_H/DCONTACT_H/CONTACT_H are defined and the
// engine headers (which drag mclib.h) are skipped.
#include "mover.h"      // stub Mover/MoverPtr
#include "dcontact.h"   // MAX_CONTACTS_PER_SENSOR, CONTACT_SORT_*
#include "contact.h"    // guard-only (SensorSystem not referenced by the TU)

// Now include-and-compile the real dispatch TU.
// The #include guards in the engine headers (WARRIOR_H, TACORDR_H, etc.) are now
// already defined by our stubs above, so the engine headers will be skipped.
#include "../../code/brain_special_dispatch.cpp"

// B4: the harness does not link brain_execution_policy.cpp (it needs File + the mission.fit parser); it runs the
// TechBrain policy unconditionally, which is what every fixture here exercises.
namespace mc2::brainpolicy {
Policy g_policy = Policy::TechBrain;
std::uint32_t g_generation = 1;
}  // namespace mc2::brainpolicy
