#pragma once
//---------------------------------------------------------------------------
// PROJECTZ v1 debug overlay (commit 4 of containment spec).
// Spec: docs/superpowers/specs/2026-04-25-projectz-containment-design.md
//
// Hotkey RAlt+P cycles overlay mode through:
//   off -> legacyRectFinite -> homogClip -> rectSignedW -> rectNearFar
//       -> rectGuard -> off ...
//
// When enabled, draws a colored alpha-blended triangle per submitted terrain
// triangle on top of the composited scene (after post-process, before HUD).
// Color decision is per spec §"Color-Coded Debug Overlay":
//   green  — legacy and candidate predicate agree on all three vertices
//   yellow — legacy accepts at least one vertex that candidate rejects
//   orange — candidate accepts at least one vertex that legacy rejects
//   red    — submitted triangle contains a legacy-rejected vertex
//            (overrides yellow/orange when both apply)
//   purple — NaN/Inf in projection chain OR projected screen area
//            > 0.5 * viewport area (overrides all other colors)
//
// Hard guard: observation-only. Reads pzVerts captured by commit 3 (no new
// projectZ calls). Does not feed back into any non-debug code path.
//---------------------------------------------------------------------------

#include "projectz_trace.h"

enum ProjectZOverlayMode {
    PZ_OVERLAY_OFF = 0,
    PZ_OVERLAY_LEGACY_RECT_FINITE,
    PZ_OVERLAY_HOMOG_CLIP,
    PZ_OVERLAY_RECT_SIGNED_W,
    PZ_OVERLAY_RECT_NEAR_FAR,
    PZ_OVERLAY_RECT_GUARD,
    PZ_OVERLAY_MODE_COUNT
};

// Read-only state.
extern ProjectZOverlayMode g_pzOverlayMode;
inline bool projectz_overlay_active() { return g_pzOverlayMode != PZ_OVERLAY_OFF; }

// RAlt+P handler. Advances the cycle and toggles g_pzTrace so the per-vertex
// predicate path runs (auto-enables tracing data flow even with no env var set).
void projectz_overlay_advance();

// Returns "off", "legacyRectFinite", etc. — used for legend label.
const char* projectz_overlay_predicate_name();

// Per-frame lifecycle. begin_frame resets the triangle buffer + counts.
// Call once per frame regardless of overlay state (cheap when off).
void projectz_overlay_begin_frame();

// Record one submitted triangle's per-vertex state. Called from the terrain
// addTriangle hook (mclib/quad.cpp pz_emit_terrain_tris). Must NOT call
// projectZ — read commit-3 captured data only.
void projectz_overlay_record_tri(
    const ProjectZTriVert     verts[4],
    const ProjectZPredicates  preds[4],
    const int                 clusterIdx[3],
    float                     screenResX,
    float                     screenResY);

// Draw the overlay. Call after post-process composite, before HUD flush.
// No-op when overlay is off.
void projectz_overlay_render(int viewportW, int viewportH);
