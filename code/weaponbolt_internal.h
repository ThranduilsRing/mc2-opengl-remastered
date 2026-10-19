#include "Gates/preferences_legacy.h"
// Shared surface of the WeaponBolt translation units (weaponbolt*.cpp).
//
// weaponbolt.cpp keeps WeaponBoltType, init/destroy, state accessors and Save/Load; the per-frame update and the
// render each get their own file.
#ifndef WEAPONBOLT_INTERNAL_H
#define WEAPONBOLT_INTERNAL_H


//---------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef WEAPONBOLT_H
#include"weaponbolt.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#include <cstdlib>

#ifndef GOS_PROFILER_H
#include"gos_profiler.h"
#endif

#include<gameos.hpp>
#include "../mclib/particles/batcher.h"
#include "vk_umbrella_light.h"  // VK-FIELD-VFX-2: umbrella-route immediate-draw for bolts

inline constexpr float BASE_EFFECT_SCALAR = 1.0f;
inline constexpr float EFFECT_LEFT_TIME = 1.0f;
inline constexpr float WEAPON_ARC_FACTOR = -20.0f;
inline constexpr float WEAPON_ARC_HEIGHT = 150.0f;
inline constexpr float ARC_THRESHOLD = 0.0f;

// Globals defined in weaponbolt.cpp (or elsewhere in the engine) that the split pieces share.
extern bool hasGuardBand;
extern MidLevelRenderer::MLRClipper * theClipper;
extern bool drawOldWay;
extern float BaseHeadShotElevation;
extern bool MLRVertexLimitReached;

#endif // WEAPONBOLT_INTERNAL_H
