// code/unitprofile_fit.cpp  (engine ONLY — never in mc2_tests)
// UNIT-PROFILE-SEAM-1: optional, additive .fit [UnitProfile] data read.
// inifile.h drags file.h -> stdio/fcntl/heap (real file I/O), which the
// doctest target forbids, so this populator lives in its own engine-only TU.
#include "unitprofile.h"
#include "unit_profile_validator.h"      // UNITPROFILE-VALIDATOR-1: pure validator + facts
#include "action_executor_registry.h"    // theActionExecutorRegistry()
#include "inifile.h"                    // resolves via mclib/ on the engine include path
#include "objmgr.h"
#include "mover.h"
#include "objtype.h"
#include <cstdio>                        // printf (report-only finding log)
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
        // DAMAGE-SCHEMA-FACET-1 (slice 5): optional data override of the damage
        // schema (hit-location model). Absent key -> p.damageSchema keeps whatever
        // the type-load default set (mech-8part for mechs, vehicle-facings
        // otherwise), so stock .fits are parity.
        char dmg[64] = {0};
        if (fit.readIdString("DamageSchema", dmg, sizeof(dmg)) == NO_ERR)
            p.damageSchema = parseDamageSchema(dmg, p.damageSchema);
    }
}

// UNITPROFILE-VALIDATOR-1 (slice 6): run the pure construction-rules validator
// over a type's populated facets and log each finding. GATED + REPORT-ONLY:
//   * gate OFF -> returns immediately, no output (behavioral parity).
//   * gate ON, coherent unit (stock walker+jump, stock vehicle no-jump) ->
//     validator returns no findings, so still no output.
//   * gate ON, incoherent authored profile (e.g. vehicle granted CAP_JUMP) ->
//     one "[UnitProfile:VALIDATE]" line per finding. NO spawn rejection.
// The validator consults the SAME theActionExecutorRegistry() gameplay queries
// use, so its verdict can never diverge from the runtime executor resolution.
void validateAndReportUnitProfile(const UnitProfile& profile, const char* typeName) {
    if (!mc2UnitProfileDataEnabled())
        return;   // gate OFF: no validation, no output -> parity with legacy

    UnitProfileFacts facts;
    facts.grantsJump = profile.baselineCapabilities.has(CAP_JUMP);
    facts.locomotion = profile.locomotion;

    const std::vector<UnitProfileFinding> findings =
        validateUnitProfile(facts, theActionExecutorRegistry());

    const char* label = (typeName && typeName[0]) ? typeName : "<unknown>";
    for (const UnitProfileFinding& f : findings) {
        // Report-only: describe the incoherence in modder terms; do not reject.
        printf("[UnitProfile:VALIDATE] %s: %s (reason=%d)\n",
               label, f.message, static_cast<int>(f.code));
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
