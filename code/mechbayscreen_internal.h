// Shared surface of the mechbayscreen.cpp translation units (mechbayscreen.cpp, mechbayscreen_{update,mechs}.cpp).
//
// mechbayscreen.cpp keeps construction, init, begin and render; the per-frame update and message handling, and the
// mech selection / drag / loadout handling are split out (see DOC in each piece).
#ifndef MECHBAYSCREEN_INTERNAL_H
#define MECHBAYSCREEN_INTERNAL_H

#include"mechbayscreen.h"
#include"logisticsmech.h"
#include"logisticsdata.h"
#include"logisticsmechicon.h"
#include "../resource.h"
#include"simplecamera.h"
#include"platform_windows.h"
#include"gamesound.h"
#include"multplyr.h"
#include"chatwindow.h"
#include"prefs.h"
#include"../GuiRuntime/GuiRuntime.h"
#include "Gates/gate.h"
#include "gamerulesdialog.h"
#include "net/runtime.h"

// Globals defined in mechbayscreen.cpp (or elsewhere in the engine) that the split pieces share.

#endif // MECHBAYSCREEN_INTERNAL_H
