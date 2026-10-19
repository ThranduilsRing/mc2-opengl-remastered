// Shared surface of the multplyr.cpp translation units (multplyr.cpp, multplyr_{send,handle}.cpp).
//
// multplyr.cpp keeps the networking entry points, the message packet types and the session / colour setup; the roster,
// chunk and send methods and the message handlers are split out (see DOC in each piece).
#ifndef MULTPLYR_INTERNAL_H
#define MULTPLYR_INTERNAL_H


#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef LINUX_BUILD
#include"crtdbg.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef LOGISTICSDATA_H
#include"logisticsdata.h"
#endif

#ifndef BLDNG_H
#include"bldng.h"
#endif

#ifndef TERROBJ_H
#include"terrobj.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef CARNAGE_H
#include"carnage.h"
#endif

#ifndef ARTLRY_H
#include"artlry.h"
#endif

#ifndef CONTROLGUI_H
#include"controlgui.h"
#endif

#ifndef LOGISTICS_H
#include"logistics.h"
#endif

#include"missionbegin.h"

#ifndef PREFS_H
#include"prefs.h"
#endif

#ifndef MISSIONGUI_H
#include"missiongui.h"
#endif

#ifndef GAMELOG_H
#include"gamelog.h"
#endif

#ifndef LINUX_BUILD
#include"dplay8.h"
#endif

#include"mpparameterscreen.h"

//sebi: commented include
//#ifndef VERSION_H
//#include"version.h"
//#endif

#include "../resource.h"


#ifdef USE_LOGISTICS
extern bool whackTimer;
void CancelBool(long value);
#endif

#ifndef MPPREFS_H
#include"mpprefs.h"
#endif

#ifdef USE_STRING_RESOURCES
extern HINSTANCE thisInstance;
long cLoadString (HINSTANCE hInstance,  UINT uID, LPTSTR lpBuffer, int nBufferMax );
#endif

#include"gamesound.h"

inline constexpr int MAX_MSG_SIZE = 10240;

// Globals defined in multplyr.cpp (or elsewhere in the engine) that the split pieces share.
extern CPrefs prefs;
extern bool quitGame;
extern float loadProgress;
extern bool aborted;
extern DWORD ServerPlayerNum;
extern float ResourceBuildingRefreshRate;
extern void* MC2NetLib;
extern bool OnLAN;
extern NETMESSAGE ReceiveMsg;
extern GameLog* CombatLog;
extern GameLog* NetLog;
extern bool LaunchedFromLobby;
extern char* startupPakFile;
extern void SortMoverList (long numMovers, MoverPtr* moverList, glm::vec3 dest);
extern void killTheGame(void);
extern MultiPlayer* MPlayer;

#endif // MULTPLYR_INTERNAL_H
