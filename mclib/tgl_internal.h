#include "Gates/preferences_legacy.h"
// Shared surface of the TG shape translation units (tgl*.cpp).
//
// tgl.cpp keeps the globals; shape types and ASE parsing are split by responsibility. tgl*.cpp are compiled together by the game, asset_viewer and tg_import_dump targets.
#ifndef TGL_INTERNAL_H
#define TGL_INTERNAL_H

#ifndef TGL_H
#include"tgl.h"
#include "gos_crashbundle.h"
#include "../GameOS/gameos/utils/mc2log.h"  // shape loaders reject malformed counts with a log line
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "render_contract.h"  // [RENDER_PASS v1] noteRenderPass
#include "gos_object_recon_tracy.h"  // [OBJECT_RECON v1] slice-2 recon-zero accumulators
#endif

#ifndef CLIP_H
#include"clip.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef MC2_RENDER_SUBMISSIONS_H
#include"render_submissions.h"
#endif

#include "platform_str.h"
#include <stddef.h> // linux offsetof()
#include <stdlib.h>
#include <stdio.h>
#include <cstdlib>
#include <cstring>
#include <new> // for placement new

//-------------------------------------------------------------------------------
// Include Files
#include<toolos.hpp>

#define USE_ASSEMBLY

#include "platform_intrinsics.h"
#include "../GameOS/gameos/gos_terrain_indirect.h"  // LightCostSplit_* ([LIGHT_COST_SPLIT v1])
#include "cone_glow.h"  // GHOST-CONE-VERIFY-4: cone_geom_hidden_by_alpha

// Globals defined in tgl.cpp (or elsewhere in the engine) that the split pieces share.

namespace {
// [LIGHT_COST_SPLIT v1] RAII cycle bracket. Cached-bool early-out; no Tracy
// zone by design (per-leaf hot loop -- a Tracy zone would bust the 100ns floor).
struct LcsBucket { enum E { C2, C6, C5 }; };
template<int K> struct LcsScope {
    bool on; unsigned long long t0;
    LcsScope(): on(gos_terrain_indirect::IsLightCostSplitEnabled()) { if (on) t0 = __rdtsc(); }
    ~LcsScope() {
        if (!on) return;
        unsigned long long d = __rdtsc() - t0;
        if (K==LcsBucket::C2){ gos_terrain_indirect::LightCostSplit_AddC2DirectCycles(d);   gos_terrain_indirect::LightCostSplit_AddC2DirectCall(); }
        if (K==LcsBucket::C6){ gos_terrain_indirect::LightCostSplit_AddC6ResubmitCycles(d); gos_terrain_indirect::LightCostSplit_AddC6ResubmitCall(); }
        if (K==LcsBucket::C5){ gos_terrain_indirect::LightCostSplit_AddC5PerActorCycles(d); gos_terrain_indirect::LightCostSplit_AddC5PerActorCall(); }
    }
};
} // namespace

extern bool g_previewCaptureBounds;
extern float g_previewBoundsMinX;
extern float g_previewBoundsMaxX;
extern float g_previewBoundsMinY;
extern float g_previewBoundsMaxY;
extern int g_previewDrawTris;
extern bool hasGuardBand;
extern bool useFog;
extern DWORD BaseVertexColor;
extern bool drawOldWay;
extern int g_mechPreviewRenderDepth;
extern bool renderTGLShapes;
extern bool silentMode;


// Free functions defined in the split pieces.
void GetNumberData (char *rawData, char *result);
void GetWordData (char *rawData, char *result);
void GetNameData (char *rawData, char *result);

#endif // TGL_INTERNAL_H
