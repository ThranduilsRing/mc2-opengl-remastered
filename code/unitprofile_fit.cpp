// code/unitprofile_fit.cpp  (engine ONLY — never in mc2_tests)
// UNIT-PROFILE-SEAM-1: optional, additive .fit [UnitProfile] data read.
// inifile.h drags file.h -> stdio/fcntl/heap (real file I/O), which the
// doctest target forbids, so this populator lives in its own engine-only TU.
#include "unitprofile.h"
#include "inifile.h"                    // resolves via mclib/ on the engine include path
#include "objmgr.h"
#include "mover.h"
#include "objtype.h"
#include <cstdlib>                       // getenv (throwaway test hook)

void populateProfileFromFit(FitIniFile& fit, UnitProfile& p) {
    // Optional, additive (grant-only, never clears). Stock .fits have no
    // [UnitProfile] block -> no-op for all shipped data. A FAILED seekBlock
    // returns BLOCK_NOT_FOUND without mutating cursor state, so an absent
    // block is side-effect-free -> stock parity clean.
    if (fit.seekBlock("UnitProfile") == NO_ERR) {
        bool jump = false;
        if (fit.readIdBoolean("Jump", jump) == NO_ERR && jump)
            p.baselineCapabilities.set(CAP_JUMP, true);
        // LOCOMOTION-FACET-1 (slice 4): optional data override of the locomotion
        // model. Absent key -> p.locomotion keeps whatever the type-load default
        // set (walker for mechs, wheeled otherwise), so stock .fits are parity.
        char loco[64] = {0};
        if (fit.readIdString("Locomotion", loco, sizeof(loco)) == NO_ERR)
            p.locomotion = parseLocomotionModel(loco, p.locomotion);
    }
}

// UNIT-PROFILE-SEAM-1: fill the state-dump witness array from live movers.
// Returns the number of rows written (capped at maxRows). Read-only.
extern "C" int mc2_unitprofile_collect_witness(UnitProfileWitnessRow* out, int maxRows) {
    if (!out || maxRows <= 0 || !ObjectManager)
        return 0;
    int n = 0;
    const long numMovers = ObjectManager->getNumMovers();
    for (long i = 0; i < numMovers && n < maxRows; ++i) {
        MoverPtr m = ObjectManager->getMover(i);
        if (!m)
            continue;
        const bool baseline = m->getObjectType()
            ? m->getObjectType()->profile().baselineCapabilities.has(CAP_JUMP) : false;
        out[n].objectId        = m->getPartId();
        out[n].capJumpBaseline = baseline ? 1 : 0;
        out[n].capJumpCurrent  = m->hasCapability(CAP_JUMP) ? 1 : 0;
        out[n].canPerformJump  = m->canPerform(UnitAction::Jump) ? 1 : 0;
        ++n;
    }
    return n;
}
