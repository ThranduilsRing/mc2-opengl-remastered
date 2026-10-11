// Shared surface of the EditorObjects.cpp translation units (EditorObjects.cpp, EditorObjects_{unit,pilot}.cpp).
//
// EditorObjects.cpp keeps the EditorObject base class and NavMarker; Unit (with its brain patrol import) and the DropZone / Brain /
// Pilot save and load are split out (see DOC in each piece).
#ifndef EDITOROBJECTS_INTERNAL_H
#define EDITOROBJECTS_INTERNAL_H

#include "stdafx.h"
#include "EditorObjects.h"

#ifndef INIFILE_H
#include "IniFile.h"
#endif

#ifndef OBJECTAPPEARANCE_H
#include "Objectappearance.h"
#endif

#ifndef TERRAIN_H
#include "Terrain.h"
#endif

#ifndef EDITOROBJECTMGR_H
#include "EditorObjectMgr.h"
#endif

#ifndef BUILDINGLINK_H
#include "BuildingLink.h"
#endif

#ifndef EDITORDATA_H
#include "EditorData.h"
#endif

#include "MissionFitBrainSource.h"   // TECHBRAIN-PATROL-VIEWER-1: declarative Brain{} patrol import

#include "resource.h"

#include "PilotProfilePreserve.h"


#include "../ARM/Microsoft.Xna.Arm.h"

#include "EditorResourceFallback.h"
#include "Gates/gate.h"
using namespace Microsoft::Xna::Arm;
extern IProviderEngine* armProvider;


extern HSTRRES gameResourceHandle;

// Globals defined in EditorObjects.cpp (or elsewhere in the engine) that the split pieces share.

#endif // EDITOROBJECTS_INTERNAL_H
