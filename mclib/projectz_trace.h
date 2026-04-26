#pragma once
//---------------------------------------------------------------------------
// PROJECTZ v1 diagnostic trace system.
// Spec: docs/superpowers/specs/2026-04-25-projectz-containment-design.md
//
// Env vars (all default OFF):
//   MC2_PROJECTZ_TRACE=1    per-call [PROJECTZ v1] vertex + triangle records
//   MC2_PROJECTZ_HEATMAP=1  per-(callsite,predicate) disagreement counters
//   MC2_PROJECTZ_SUMMARY=1  600-frame + shutdown summary
//   MC2_PROJECTZ_GUARD_PX=N rectGuard expansion pixels (default 64)
//
// Schema versioning: all records carry [PROJECTZ v1] prefix. Future format
// changes bump the version; no backward-compat shims.
//---------------------------------------------------------------------------

#include <stdint.h>

// Forward-declare Stuff vector types (fully defined in stuff/stuff.hpp via camera.h).
namespace Stuff { class Vector3D; class Vector4D; }

//--------------------------------------------------------------------
// Per-vertex predicates: six booleans + perspective flag.
// Parallel-branch calls set isPerspective=false; modern predicates are n/a.
struct ProjectZPredicates {
    bool legacyRect;        // ground truth: current screen-rect test (must == bool return)
    bool legacyRectFinite;  // legacyRect && all screen coords isfinite
    bool homogClip;         // signedW > 0 && |rawClip.xy| <= rawClip.w
    bool rectSignedW;       // legacyRect && signedW > 0
    bool rectNearFar;       // legacyRect && rawClip.z in [0, rawClip.w]
    bool rectGuard;         // legacyRect with viewport expanded by g_pzGuardPx pixels
    bool isPerspective;     // false => modern predicates are n/a for this call
};

//--------------------------------------------------------------------
// Per-quad vertex snapshot used to build per-triangle records (Q2 answer).
// Filled from already-computed locals at each BoolAdmission site; no re-call
// of projectZ needed, and the data is provably the same values submission saw.
struct ProjectZTriVert {
    bool  legacyAccepted; // vertices[i]->clipInfo (what the submission gate actually saw)
    float wx, wy, wz;    // screen.x/.y/.z (vertices[i]->px/py/pz for terrain)
    float vx, vy, vz;    // world position: vx, vy, pVertex->elevation
};

//--------------------------------------------------------------------
// Probe state (read-only after projectz_trace_init()).
extern bool g_pzTrace;      // ANY PROJECTZ env var is on (or overlay enabled) — gates all diagnostic paths
extern bool g_pzDoTrace;    // MC2_PROJECTZ_TRACE=1
extern bool g_pzDoHeatmap;  // MC2_PROJECTZ_HEATMAP=1
extern bool g_pzDoSummary;  // MC2_PROJECTZ_SUMMARY=1
extern int  g_pzGuardPx;    // MC2_PROJECTZ_GUARD_PX (default 64)

// Most-recent per-vertex predicate result. trace_dispatch overwrites this
// after every projectZ call when g_pzTrace is set. The BoolAdmission caller
// (single-threaded terrain submission) reads it immediately after the call
// to capture per-vertex state for the overlay. Diagnostic-only.
extern ProjectZPredicates g_pzLastPredicates;

//--------------------------------------------------------------------
// Per-call callsite globals.
// Set immediately before each priority projectZ call; cleared inside
// projectZ after reading so stale values produce <unknown> at the next
// call rather than silently re-using the previous site's ID.
extern const char* g_projectz_site_id;
extern const char* g_projectz_site_cat;

// Set both globals in one statement at each priority callsite.
#define PROJECTZ_SITE(id, cat) \
    do { g_projectz_site_id = (id); g_projectz_site_cat = (cat); } while (0)

//--------------------------------------------------------------------
// API

// Probe env vars (idempotent). Call once at startup before any projectZ call.
void projectz_trace_init();

// Called from inside the projectZ inline body when g_pzTrace is true.
// Takes raw values directly from projectZ locals; does not feed back.
// siteId/siteCat are the already-read-and-cleared globals.
void projectz_trace_dispatch(
    const char*          siteId,
    const char*          siteCat,
    const Stuff::Vector3D& worldPt,
    const Stuff::Vector4D& rawClip,   // xformCoords pre-divide
    const Stuff::Vector4D& screen,    // post-divide screen vector
    bool                 usePerspective,
    bool                 accepted,    // bool return of projectZ
    float                screenResX,
    float                screenResY);

// Emit a [PROJECTZ v1] tri record at terrain addTriangle sites.
// clusterIdx[3]: three vertex indices into verts[4].
// currentSubmitted: true when the calling path actually called addTriangle.
void projectz_emit_tri(
    const char*              callsiteId,
    int                      tileR,
    int                      tileC,
    const int                clusterIdx[3],
    const ProjectZTriVert    verts[4],
    bool                     currentSubmitted,
    const char*              file,
    int                      line);

// Bump frame counter; emit 600-frame summary when MC2_PROJECTZ_SUMMARY=1.
void projectz_frame_tick();

// Emit final summary and release counters. Call at shutdown.
void projectz_shutdown();
