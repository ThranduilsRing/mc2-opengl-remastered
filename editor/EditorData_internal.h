// Shared surface of the EditorData translation units (EditorData*.cpp).
//
// EditorData.cpp keeps the globals, construction/clear and mission settings; terrain building, saving and the tactical
// map/teams code are split by responsibility.
#ifndef EDITORDATA_INTERNAL_H
#define EDITORDATA_INTERNAL_H

#include "stdafx.h"
#include "EditorData.h"
#include "MCLib.h"
#include "EditorTaskRunner.h"
#include "EditorObjectMgr.h"
#include "mclibresource.h"
#include "resource.h"
#include "action.h"
#include "move.h"
#include "Vertex.h"
#include "terrtxm.h"
#include "editorinterface.h"
#include "mc2fmt.h"
#include "objective_text.h"

bool EditorReadMissionCamera(FitIniFile& file);
bool EditorSaveMissionCamera(FitIniFile& file);
void EditorResetMissionCamera();

#include "../EditorBridge/EditorRenderBridge.h"  // GPU batcher/registry/cull lifecycle (firewall)
#include "../GameOS/gameos/gos_crashbundle.h"
#include "../GameOS/gameos/gos_terrain_indirect.h"
#include "../GameOS/gameos/gos_terrain_water_stream.h"
#include "../GameOS/gameos/gpu_cull_substrate.h"
#include "../GameOS/gameos/gpu_cull_readback.h"
#include "../GameOS/gameos/gos_terrain_lighting.h"
#include <gameos.hpp>  // gos_ResetStaticShadowPriming
#include "../GameAdapters/StaticPropRenderAdapter.h"
#include "../GameAdapters/MechRenderAdapter.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

// From multplyr.h
// Can't include because of redefinition of MissionSettings struct
enum MissionType {
	MISSION_TYPE_ELIMINATION,
	MISSION_TYPE_KING_OF_THE_HILL,
	MISSION_TYPE_CAPTURE_BASE,
	MISSION_TYPE_TERRITORIES,
	MISSION_TYPE_LAST_MAN_STANDING,
	MISSION_TYPE_LAST_MAN_ON_THE_HILL,
	MISSION_TYPE_OTHER,
	NUM_MISSION_TYPES
};

#pragma warning( disable:4201 )
#include "mmsystem.h"
#pragma warning( default:4201 )

#pragma warning( disable:4244 )

#include <objbase.h>

// ARM
#include "../ARM/Microsoft.Xna.Arm.h"
using namespace Microsoft::Xna::Arm;
#include <set>
#include "EditorResourceFallback.h"
#include "EditorVersion.h"

// Globals defined in EditorData.cpp (or elsewhere in the engine) that the split pieces share.
extern bool gEditorDataMoveDataReadyForFullSave;
extern IProviderAsset * mapAsset;
extern IProviderAsset * mechAsset;
extern bool justResaveAllMaps;
extern bool g_cliSuppressModals;
extern char missionScriptName[1024];
extern bool bIsLoading;
extern bool gEditorDataMoveDataReadyForFullSave;
void EditorDataTrace(const char* fmt, ...);

//-------------------------------------------------------------------------------------------------

static long sReadIdBoolean(FitIniFile* missionFile, const char *varName, bool &value) {
	long result = 0;
	bool tmpBool;
	result = missionFile->readIdBoolean((char *)varName, tmpBool);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpBool;
	}
	return result;
}

static long sReadIdWholeNum(FitIniFile* missionFile, const char *varName, int &value) {
	long result = 0;
	unsigned long tmpULong;
	result = missionFile->readIdULong((char *)varName, tmpULong);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpULong;
	}
	return result;
}

// Free functions defined in the split pieces.
bool CreateScaledColorMap(long mapWidth, char *localColorMapName, MemoryPtr tmpRAM, long fileSize);

#endif // EDITORDATA_INTERNAL_H
