// Shared surface of the team.cpp translation units (team.cpp, team_los.cpp).
//
// team.cpp keeps team setup, the roster and contact queries, target orders, the seen-radius marking and save / load;
// the line-of-sight functions are split out (see DOC in the piece).
#ifndef TEAM_INTERNAL_H
#define TEAM_INTERNAL_H

#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifdef USE_MECHS
#ifndef MECH_H
#include<mech.h>
#endif
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifdef USE_ARTILLERY
#ifndef ARTLRY_H
#include<artlry.h>
#endif
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifdef USE_MOVERCONTROL
#ifndef AICTRL_H
#include<aictrl.h>
#endif
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef CONTACT_H
#include"contact.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef MISSIONGUI_H
#include"missiongui.h"
#endif

#ifndef WARRIOR_H
#include"warrior.h"
#endif

// Globals defined in team.cpp (or elsewhere in the engine) that the split pieces share.
extern bool useRealLOS;
extern float maxVisualRange;
extern long	visualRangeTable[];
extern unsigned long MaxTreeLOSCellBlock;

#endif // TEAM_INTERNAL_H
