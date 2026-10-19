#include "Gates/preferences_legacy.h"
// Shared surface of the logistics.cpp translation units (logistics.cpp, logistics_begin.cpp).
//
// logistics.cpp keeps the Logistics start / stop / update / render; beginMission and the mission start helpers are split
// out (see DOC in the piece).
#ifndef LOGISTICS_INTERNAL_H
#define LOGISTICS_INTERNAL_H

// Include Files
#ifndef LOGISTICS_H
#include"logistics.h"
#endif

// VPL-#11: campaign .fit resume spawns the player force-group via
// mission->addMover() AFTER mission->init() finalized the mech batcher;
// finalizePending() rebuilds shared geometry so those mechs are visible.
#include "gos_mech_batcher.h"

#ifndef TEST_SHELL
#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef MISSIONGUI_H
#include"missiongui.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef PREFS_H
#include"prefs.h"
#endif
extern CPrefs prefs;

#ifndef GAMECAM_H
#include"gamecam.h"
#endif

#endif /*TEST_SHELL*/

#ifndef SOUNDS_H
#include"sounds.h"
#endif

#ifndef MISSIONBEGIN_H
#include"missionbegin.h"
#endif

#ifndef MECHICON_H
#include"mechicon.h"
#endif

#include"missionresults.h"
#include"paths.h"
#include"../GameOS/gameos/gos_profiler.h"

extern bool quitGame;
extern bool justStartMission;
extern float loadProgress;

extern bool aborted;

#include "../resource.h"
#include "Gates/gate.h"

// Globals defined in logistics.cpp (or elsewhere in the engine) that the split pieces share.
extern Logistics *logistics;

#endif // LOGISTICS_INTERNAL_H
