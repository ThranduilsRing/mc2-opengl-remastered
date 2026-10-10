// Shared surface of the logisticsvariant.cpp translation units (logisticsvariant.cpp, logisticsvariant_stats.cpp).
//
// logisticsvariant.cpp keeps LogisticsChassis and the LogisticsVariant construction, init and addComponent; the stat
// getters, component placement and lookup, save and the LogisticsVehicle setup are split out (see DOC in the piece).
#ifndef LOGISTICSVARIANT_INTERNAL_H
#define LOGISTICSVARIANT_INTERNAL_H

#include"logisticsvariant.h"
#include"mclib.h"
#include"logisticsdata.h"
#include"../resource.h"
#include"cmponent.h" 
#include"logisticserrors.h"
#include"ui_profile.h"            // UI-PROFILE-1: UIProfile facet + UI_STRING_ID_UNSET
#include"ui_profile_registry.h"  // UI-PROFILE-1: uiProfileForChassis (additive-merge, gated)
#include"unit_definition.h"      // SINGLE-SOURCE-UNIT-DEFINITION-1: mergeUIProfile (shared UI resolution)
#include"unit_profile_validator.h" // UI-PROFILE-1: UPV_UI_ASSET_MISSING reason code
#include "Gates/gate.h"

// Globals defined in logisticsvariant.cpp (or elsewhere in the engine) that the split pieces share.

#endif // LOGISTICSVARIANT_INTERNAL_H
