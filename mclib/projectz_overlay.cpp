//---------------------------------------------------------------------------
// PROJECTZ v1 debug overlay implementation (commit 4).
// Spec: docs/superpowers/specs/2026-04-25-projectz-containment-design.md
//
// Hard guard: observation-only. Reads commit-3 captured pzVerts + predicates;
// never calls projectZ; never feeds back into culling/submission/lighting.
//---------------------------------------------------------------------------

#include "projectz_overlay.h"

#include <gameos.hpp>
#include "../GameOS/gameos/gos_profiler.h"

// pp->endScene() leaves VAO 0 bound; rebind the renderer's VAO before any
// gos_Draw* calls. Defined in GameOS/gameos/gameos_graphics.cpp.
extern void gos_RendererRebindVAO();

#include <stdio.h>
#include <math.h>
#include <float.h>
#include <string.h>

//--------------------------------------------------------------------
// Public state.
ProjectZOverlayMode g_pzOverlayMode = PZ_OVERLAY_OFF;

//--------------------------------------------------------------------
// Per-frame triangle buffer.
// Worst case: tier1 5-frame sample emitted ~16K triangles in 5 frames
// (~3.3K/frame), Wolfman/zoom can spike higher. 32K cap is safe;
// past that we drop and bump a counter.
struct PzOverlayTri {
    float    x0, y0, x1, y1, x2, y2;
    uint32_t argb;          // pre-blended color w/ ~40% alpha
    uint8_t  bucket;        // 0=green 1=yellow 2=orange 3=red 4=purple
};
static const int  MAX_TRIS = 32 * 1024;
static PzOverlayTri s_tris[MAX_TRIS];
static int          s_triCount = 0;
static uint32_t     s_dropped  = 0;
static uint32_t     s_bucketCount[5] = {0, 0, 0, 0, 0};
static const char*  s_bucketNames[5] = {"green", "yellow", "orange", "red", "purple"};

// Colors in gos_VERTEX.argb format (0xAARRGGBB stored little-endian as BGRA).
// glVertexAttribPointer reads GL_UNSIGNED_BYTE as RGBA (byte0=R,byte1=G,byte2=B,byte3=A),
// so the bytes map: GL_R=MC2_B, GL_G=MC2_G, GL_B=MC2_R. R and B must be swapped
// relative to the desired visual RGB to compensate.
// 40% alpha => 0x66 = 102/255.
static const uint32_t COLOR_GREEN  = 0x6600FF00u;  // visual (0,255,0)   — R=B=0, no swap needed
static const uint32_t COLOR_YELLOW = 0x6600FFFFu;  // visual (255,255,0) — was 0x66FFFF00
static const uint32_t COLOR_ORANGE = 0x6600A5FFu;  // visual (255,165,0) — was 0x66FFA500
static const uint32_t COLOR_RED    = 0x660000FFu;  // visual (255,0,0)   — was 0x66FF0000
static const uint32_t COLOR_PURPLE = 0x66FF40B0u;  // visual (176,64,255)— was 0x66B040FF

//--------------------------------------------------------------------
static const char* s_modeNames[PZ_OVERLAY_MODE_COUNT] = {
    "off",
    "legacyRectFinite",
    "homogClip",
    "rectSignedW",
    "rectNearFar",
    "rectGuard",
};

const char* projectz_overlay_predicate_name() {
    return s_modeNames[g_pzOverlayMode];
}

//--------------------------------------------------------------------
// Pull the candidate predicate value out of a ProjectZPredicates struct
// according to the current overlay mode. Returns true if the predicate
// would accept this vertex.
static inline bool pred_value(const ProjectZPredicates& p, ProjectZOverlayMode mode) {
    switch (mode) {
        case PZ_OVERLAY_LEGACY_RECT_FINITE: return p.legacyRectFinite;
        case PZ_OVERLAY_HOMOG_CLIP:         return p.homogClip;
        case PZ_OVERLAY_RECT_SIGNED_W:      return p.rectSignedW;
        case PZ_OVERLAY_RECT_NEAR_FAR:      return p.rectNearFar;
        case PZ_OVERLAY_RECT_GUARD:         return p.rectGuard;
        default:                            return p.legacyRect;
    }
}

static inline bool pz_finite(float v) {
#if defined(_MSC_VER)
    return _finite(v) != 0;
#else
    return isfinite(v);
#endif
}

//--------------------------------------------------------------------
void projectz_overlay_advance() {
    int next = ((int)g_pzOverlayMode + 1) % (int)PZ_OVERLAY_MODE_COUNT;
    g_pzOverlayMode = (ProjectZOverlayMode)next;

    // When overlay is on, force g_pzTrace so projectz_trace_dispatch runs
    // and populates g_pzLastPredicates. Disabling the overlay does NOT
    // clear g_pzTrace if any explicit env var also enabled it (init time
    // is the source of truth for env-var driven trace).
    if (g_pzOverlayMode != PZ_OVERLAY_OFF) {
        g_pzTrace = true;
    }
    fprintf(stderr, "[PROJECTZ overlay] mode=%s\n", s_modeNames[g_pzOverlayMode]);
    fflush(stderr);
}

void projectz_overlay_begin_frame() {
    s_triCount = 0;
    s_dropped  = 0;
    for (int i = 0; i < 5; i++) s_bucketCount[i] = 0;
}

//--------------------------------------------------------------------
void projectz_overlay_record_tri(
    const ProjectZTriVert    verts[4],
    const ProjectZPredicates preds[4],
    const int                clusterIdx[3],
    float                    screenResX,
    float                    screenResY)
{
    if (g_pzOverlayMode == PZ_OVERLAY_OFF) return;

    const ProjectZTriVert& v0 = verts[clusterIdx[0]];
    const ProjectZTriVert& v1 = verts[clusterIdx[1]];
    const ProjectZTriVert& v2 = verts[clusterIdx[2]];
    const ProjectZPredicates& p0 = preds[clusterIdx[0]];
    const ProjectZPredicates& p1 = preds[clusterIdx[1]];
    const ProjectZPredicates& p2 = preds[clusterIdx[2]];

    // Purple condition #1: NaN/Inf in screen coords.
    bool purple = !pz_finite(v0.wx) || !pz_finite(v0.wy) ||
                  !pz_finite(v1.wx) || !pz_finite(v1.wy) ||
                  !pz_finite(v2.wx) || !pz_finite(v2.wy);

    // Screen-space area via cross-product magnitude of the two edges.
    float ax = v1.wx - v0.wx, ay = v1.wy - v0.wy;
    float bx = v2.wx - v0.wx, by = v2.wy - v0.wy;
    float area = fabsf(ax * by - ay * bx) * 0.5f;
    float viewportArea = screenResX * screenResY;

    // Purple condition #2: triangle covers > 50% of viewport.
    if (!purple && viewportArea > 0.0f && area > 0.5f * viewportArea) {
        purple = true;
    }

    bool legacyAccept0 = v0.legacyAccepted;
    bool legacyAccept1 = v1.legacyAccepted;
    bool legacyAccept2 = v2.legacyAccepted;
    bool hasLegacyReject = !legacyAccept0 || !legacyAccept1 || !legacyAccept2;

    bool cand0 = pred_value(p0, g_pzOverlayMode);
    bool cand1 = pred_value(p1, g_pzOverlayMode);
    bool cand2 = pred_value(p2, g_pzOverlayMode);

    bool yellow = (legacyAccept0 && !cand0) ||
                  (legacyAccept1 && !cand1) ||
                  (legacyAccept2 && !cand2);
    bool orange = (!legacyAccept0 && cand0) ||
                  (!legacyAccept1 && cand1) ||
                  (!legacyAccept2 && cand2);

    // Priority: purple > red > orange/yellow > green.
    uint32_t color;
    uint8_t  bucket;
    if (purple)             { color = COLOR_PURPLE; bucket = 4; }
    else if (hasLegacyReject){color = COLOR_RED;    bucket = 3; }
    else if (orange)        { color = COLOR_ORANGE; bucket = 2; }  // unreachable in practice (orange ⊆ red)
    else if (yellow)        { color = COLOR_YELLOW; bucket = 1; }
    else                    { color = COLOR_GREEN;  bucket = 0; }

    s_bucketCount[bucket]++;

    if (s_triCount >= MAX_TRIS) { s_dropped++; return; }
    PzOverlayTri& t = s_tris[s_triCount++];
    t.x0 = v0.wx; t.y0 = v0.wy;
    t.x1 = v1.wx; t.y1 = v1.wy;
    t.x2 = v2.wx; t.y2 = v2.wy;
    t.argb   = color;
    t.bucket = bucket;
}

//--------------------------------------------------------------------
// Fill one gos_VERTEX with screen-space pixel coords + a flat color.
static inline void fill_vert(gos_VERTEX& v, float x, float y, uint32_t argb) {
    v.x    = x;
    v.y    = y;
    v.z    = 0.0f;
    v.rhw  = 0.5f;
    v.argb = argb;
    v.frgb = 0;
    v.u    = 0.0f;
    v.v    = 0.0f;
}

// Draw a solid-color quad as two triangles (gos_DrawQuads is globally disabled).
static void draw_quad_as_tris(float x0, float y0, float x1, float y1, uint32_t argb) {
    gos_VERTEX v[6];
    fill_vert(v[0], x0, y0, argb);
    fill_vert(v[1], x1, y0, argb);
    fill_vert(v[2], x1, y1, argb);
    fill_vert(v[3], x0, y0, argb);
    fill_vert(v[4], x1, y1, argb);
    fill_vert(v[5], x0, y1, argb);
    gos_DrawTriangles(v, 6);
}

void projectz_overlay_render(int viewportW, int viewportH) {
    if (g_pzOverlayMode == PZ_OVERLAY_OFF) return;
    ZoneScopedN("projectz_overlay_render");

    // One-shot entry log: confirms render path is reached and shows how many
    // triangles were recorded this frame. Expected on first RAlt+P press.
    {
        static bool s_firstEntry = true;
        if (s_firstEntry) {
            s_firstEntry = false;
            fprintf(stderr, "[PROJECTZ overlay] first render entry: mode=%s triCount=%d\n",
                    s_modeNames[g_pzOverlayMode], s_triCount);
            fflush(stderr);
        }
    }

    // pp->endScene() leaves VAO 0 bound; rebind so attribute 0 is active
    // (AMD silently drops draws when attribute 0 is unbound).
    gos_RendererRebindVAO();

    // Alpha-blended, no-depth-test, no-texture state.
    gos_SetRenderState(gos_State_Texture,        0);
    gos_SetRenderState(gos_State_AlphaMode,      gos_Alpha_AlphaInvAlpha);
    gos_SetRenderState(gos_State_AlphaTest,      0);
    gos_SetRenderState(gos_State_ZCompare,       0);
    gos_SetRenderState(gos_State_ZWrite,         0);
    gos_SetRenderState(gos_State_Filter,         gos_FilterNone);
    gos_SetRenderState(gos_State_Clipping,       1);
    gos_SetRenderState(gos_State_ShadeMode,      gos_ShadeFlat);
    gos_SetRenderState(gos_State_Culling,        gos_Cull_None);
    gos_SetRenderState(gos_State_TextureMapBlend,gos_BlendModulateAlpha);

    gos_VERTEX v[3];
    for (int i = 0; i < s_triCount; i++) {
        const PzOverlayTri& t = s_tris[i];
        fill_vert(v[0], t.x0, t.y0, t.argb);
        fill_vert(v[1], t.x1, t.y1, t.argb);
        fill_vert(v[2], t.x2, t.y2, t.argb);
        gos_DrawTriangles(v, 3);
    }

    // Legend: color-bar + count strip in the top-left corner.
    // gos_DrawQuads is globally disabled, so we emit two triangles per quad.
    {
        const float barX = 10.0f, barY = 10.0f, barH = 14.0f;
        const float barW = 300.0f;
        uint32_t total = 0;
        for (int i = 0; i < 5; i++) total += s_bucketCount[i];

        // Semi-opaque black background so the color bar stands out on terrain.
        draw_quad_as_tris(barX - 2, barY - 2, barX + barW + 2, barY + barH + 2, 0x80000000u);

        if (total > 0) {
            // Same R↔B swap as the triangle constants above (GL_RGBA byte order).
            const uint32_t colors[5] = {
                0xFF00FF00u,  // green  (0,255,0)   — unchanged
                0xFF00FFFFu,  // yellow (255,255,0) — was 0xFFFFFF00
                0xFF00A5FFu,  // orange (255,165,0) — was 0xFFFFA500
                0xFF0000FFu,  // red    (255,0,0)   — was 0xFFFF0000
                0xFFFF40B0u,  // purple (176,64,255)— was 0xFFB040FF
            };
            float cursor = barX;
            for (int i = 0; i < 5; i++) {
                if (s_bucketCount[i] == 0) continue;
                float w = barW * (float)s_bucketCount[i] / (float)total;
                draw_quad_as_tris(cursor, barY, cursor + w, barY + barH, colors[i]);
                cursor += w;
            }
        }
    }

    // Drop counter (printf only when triggered, not per-frame spam).
    if (s_dropped > 0) {
        static uint32_t s_lastDroppedReport = 0;
        if (s_dropped != s_lastDroppedReport) {
            fprintf(stderr,
                "[PROJECTZ overlay] dropped %u triangles this frame (cap=%d)\n",
                s_dropped, MAX_TRIS);
            s_lastDroppedReport = s_dropped;
        }
    }

    // Per-frame stderr legend, throttled to once per ~60 frames so the
    // console isn't flooded but counts are visible.
    {
        static int s_throttle = 0;
        if (++s_throttle >= 60) {
            s_throttle = 0;
            fprintf(stderr,
                "[PROJECTZ overlay] %s | green=%u yellow=%u orange=%u red=%u purple=%u\n",
                s_modeNames[g_pzOverlayMode],
                s_bucketCount[0], s_bucketCount[1], s_bucketCount[2],
                s_bucketCount[3], s_bucketCount[4]);
        }
    }

    (void)viewportW; (void)viewportH;
}
