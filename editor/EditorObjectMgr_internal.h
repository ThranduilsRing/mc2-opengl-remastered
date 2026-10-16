// Shared surface of the EditorObjectMgr translation units (EditorObjectMgr*.cpp).
//
// EditorObjectMgr.cpp keeps construction, init, building add/delete/queries, links and names; picking, update/render,
// save/load and selection/forest editing are split by responsibility.
#ifndef EDITOROBJECTMGR_INTERNAL_H
#define EDITOROBJECTMGR_INTERNAL_H

#define EDITOROBJECTMGR_CPP
#include "stdafx.h"
#include "mc2_diag_verbose.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// Throwaway diagnostic: throttled trace to editor-startup.log. Gated on
// MC2_EDITOR_TRACE env var (same convention as EditorDataTrace in
// EditorData.cpp). ASCII only. Remove with the rest of the editor render
// probes when the object-loop GPU port slice is verified.
#include "../GameOS/gameos/gos_crashbundle.h"

#ifndef BDACTOR_H
#include "bdActor.h"
#endif

#include "EditorObjectMgr.h"
#include "../EditorBridge/EditorRenderBridge.h"  // GPU static-prop batcher/registry (firewall)

#ifndef EDITORINTERFACE_H
#include "EditorInterface.h"  // notifyObjectDeleted (drag-pointer UAF scrub)
#endif


#ifndef FILE_H
#include "File.h"
#endif

#ifndef UTILITIES_H
#include "Utilities.h"
#endif

#ifndef TERRAIN_H
#include "Terrain.h"
#endif

#ifndef ERR_H
#include "Err.h"
#endif

#ifndef CAMERA_H
#include "Camera.h"
#endif

#ifndef PATHS_H
#include "Paths.h"
#endif

#ifndef CIDENT_H
#include "cident.h"
#endif

#ifndef PACKET_H
#include "Packet.h"
#endif

#ifndef CAMERA_H
#include "camera.h"
#endif

#ifndef BDACTOR_H
#include "bdActor.h"
#endif

#ifndef MECH3D_H
#include "Mech3d.h"
#endif

#ifndef MESSAGEBOX_H
#include "MessageBox.h"
#endif

#ifndef APPRTYPE_H
#include "apprtype.h"
#endif

#ifndef TACMAP_H
#include "tacMap.h"
#endif

#ifndef EDITORDATA_H
#include "EditorData.h"
#endif

#include "resource.h"

#include "McLibResource.h"

#include "BuildingLink.h"
#include "Utilities.h"

#include "malloc.h"

#include "assert.h"

// ARM
#include "../ARM/Microsoft.Xna.Arm.h"
#include "EditorResourceFallback.h"
#include "EditorResourceCatalog.h"
#include "../GameAdapters/MechRenderAdapter.h"
using namespace Microsoft::Xna::Arm;

#define TREE_GROUP 1

// Globals defined in EditorObjectMgr.cpp (or elsewhere in the engine) that the split pieces share.
extern HSTRRES gameResourceHandle;
extern bool renderObjects;
extern bool renderTrees;
extern bool justResaveAllMaps;
void EditorObjMgrTrace(const char* fmt, ...);

// Forward decl: GL-correct world->screen projection (defined below). Used by the
// pick helpers, which precede the definition.
bool EditorObjectMgr_ProjectScreenXY_GL(const Stuff::Vector3D& wp, Stuff::Vector4D& sp);

// Free functions defined in the split pieces.
bool EditorObjectMgr_ProjectScreenXY_GL(const Stuff::Vector3D& wp, Stuff::Vector4D& sp);

#endif // EDITOROBJECTMGR_INTERNAL_H
