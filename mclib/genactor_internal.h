// Shared surface of the genactor.cpp translation units (genactor.cpp, genactor_frame.cpp).
//
// genactor.cpp keeps the type loader, the appearance setup and the bounds recalculation; render, shadow render and the
// per-frame update are split out (see DOC in the piece).
#ifndef GENACTOR_INTERNAL_H
#define GENACTOR_INTERNAL_H

#ifndef GENACTOR_H
#include"genactor.h"
#include "tgl_load_guard.h"  // META-FIX design B PART 1: required base-shape load guard
#include "cone_glow.h"       // LIGHT-REALISM-1 — beacon ground glow
#endif

#include "gos_object_recon_tracy.h"  // [OBJECT_RECON v1] slice-2 recon-zero
#include "cpu_proj_cost_split.h"      // F3 CPU projection cost-baseline (RAII scope)

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef OBJSTATUS_H
#include"objstatus.h"
#endif

#ifndef UTILITIES_H
#include"utilities.h"
#endif

#ifndef INIFILE_H
#include"inifile.h"
#endif

#ifndef ERR_H
#include"err.h"
#endif

#ifndef TXMMGR_H
#include"render_submissions.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef CELINE_H
#include"celine.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#include "gos_static_prop_batcher.h"
#include "gos_static_prop_killswitch.h"
#include "../RenderCore/axis_convert.h"

// Globals defined in genactor.cpp (or elsewhere in the engine) that the split pieces share.
extern float	worldUnitsPerMeter;
extern bool 	drawTerrainGrid;
extern bool		useFog;
extern int		ObjectTextureSize;
extern bool reloadBounds;

#endif // GENACTOR_INTERNAL_H
