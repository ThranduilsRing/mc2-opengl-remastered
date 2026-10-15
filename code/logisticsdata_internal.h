// Shared surface of the logisticsdata.cpp translation units (logisticsdata.cpp, logisticsdata_{init,inventory,save,availability,modify}.cpp).
//
// logisticsdata.cpp keeps the constructor / destructor, the campaign and multiplayer start and the small accessors; the rest of
// LogisticsData is split by responsibility (see DOC in each piece).
#ifndef LOGISTICSDATA_INTERNAL_H
#define LOGISTICSDATA_INTERNAL_H

#include"logisticsdata.h"
#include"mc2str.h"
#include"file.h"
#include"logisticserrors.h"
#include"cmponent.h"
#include"paths.h"
#include"warrior.h"
#include"../resource.h"
#include"malloc.h"
#include"team.h"
#include"mech.h"
#include"logisticsmissioninfo.h"
#include"packet.h"
#include"gamesound.h"
#include"prefs.h"
#include"comndr.h"
#include"missionresults.h"
#include"unitprofile.h"   // EQUIP-ENABLE-WIRE-1: applyEquipmentEnableWireIfEnabled
#include"equipment_registry.h"   // EQUIP-ADD-FILL-1: EquipmentAddFillRow (second consumer)
#include<zlib.h>
#include<vector>
#include<string>
#include"../GameOS/gameos/diagnostic_trace.h"   // LOGISTICS front-end state capture
#include "Gates/gate.h"
#ifndef VIEWER
#include"multplyr.h"
#include"chatwindow.h"
#endif

// Globals defined in logisticsdata.cpp (or elsewhere in the engine) that the split pieces share.
extern CPrefs prefs;
extern long SaveGameVersionNumber;

// Free functions defined in the split pieces.
void decryptFile (char *inputFile, char *outputFile);

#endif // LOGISTICSDATA_INTERNAL_H
