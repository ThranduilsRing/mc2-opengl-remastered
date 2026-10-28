// Shared surface of the group.cpp translation units (group.cpp, group_{goals,orders}.cpp).
//
// group.cpp keeps the group membership and selection methods and the copy / init; the goal-cell search and the
// tactical order handling are split out (see DOC in each piece).
#ifndef GROUP_INTERNAL_H
#define GROUP_INTERNAL_H

#ifndef MCLIB_H
#include"mclib.h"
#endif
#include "warrior.h"

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

#ifndef TACORDR_H
#include"tacordr.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

// Globals defined in group.cpp (or elsewhere in the engine) that the split pieces share.
extern float	DelayedOrderTime;
extern long		tileMulMAPCELL_DIM[MAX_MAP_CELL_WIDTH];
extern char OverlayIsBridge[NUM_OVERLAY_TYPES];

#endif // GROUP_INTERNAL_H
