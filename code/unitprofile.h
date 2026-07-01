#ifndef UNITPROFILE_H
#define UNITPROFILE_H
// UNIT-PROFILE-SEAM-1: static baseline facets (ObjectType) + current runtime
// facets (Mover). Slice 1 facet = capability.
#include "capability.h"

bool mc2_unitprofile_parse_enabled(const char* envValue);   // pure, testable
bool mc2UnitProfileDataEnabled();                            // cached getenv

class UnitProfile {                  // static, on ObjectType (flyweight)
  public:
    CapabilitySet baselineCapabilities;
};
struct UnitRuntimeState {            // mutable, on Mover
    CapabilitySet currentCapabilities;
};

class FitIniFile;                      // fwd; keep heavy I/O header out of unitprofile.h
void populateProfileFromFit(FitIniFile& fit, UnitProfile& profile);

// UNIT-PROFILE-SEAM-1: state-dump witness bridge. debug_state_dump.cpp keeps no
// game headers, so the mover walk lives engine-side (unitprofile_fit.cpp) and is
// exposed as a flat POD array, mirroring gos_getMechTextureNameByNodeIdx.
struct UnitProfileWitnessRow {
    long objectId;
    int  capJumpBaseline;   // 0/1
    int  capJumpCurrent;    // 0/1
    int  canPerformJump;    // 0/1
};
extern "C" int mc2_unitprofile_collect_witness(UnitProfileWitnessRow* out, int maxRows);
#endif // UNITPROFILE_H
