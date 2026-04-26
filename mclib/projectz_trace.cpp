//---------------------------------------------------------------------------
// PROJECTZ v1 diagnostic trace system implementation.
// Spec: docs/superpowers/specs/2026-04-25-projectz-containment-design.md
//
// Hard guard: this file must not feed any computed value back into culling,
// submission, projection, selection, lighting, timing, or rendered output.
// All code here is observation-only: printf, counter increment, summary emit.
//---------------------------------------------------------------------------

#include "projectz_trace.h"

#ifndef CAMERA_H
#include "camera.h"   // LegacyProjectionResult, Stuff::Vector3D/Vector4D full defs
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

//--------------------------------------------------------------------
// Global definitions (declared extern in projectz_trace.h)
bool        g_pzTrace      = false;
bool        g_pzDoTrace    = false;
bool        g_pzDoHeatmap  = false;
bool        g_pzDoSummary  = false;
int         g_pzGuardPx    = 0;
const char* g_projectz_site_id  = nullptr;
const char* g_projectz_site_cat = nullptr;

//--------------------------------------------------------------------
// Internal statics

static bool     s_initialized  = false;
static uint32_t s_frameCount   = 0;
static uint32_t s_perspCalls   = 0;   // perspective-branch calls only
static FILE*    s_traceFile    = nullptr;  // vertex/tri records; null when g_pzDoTrace=false

// ── per-category counters ────────────────────────────────────────────────────
static const int CAT_COUNT = 8;
static const char* const s_catNames[CAT_COUNT] = {
    "BoolAdmission", "Both", "LightingShadow", "SelectionPicking",
    "InverseProjectionPair", "ScreenXYOracle", "DebugOnly", "unknown"
};
struct CatStat { uint32_t calls, parallel, rejected; };
static CatStat s_catStat[CAT_COUNT] = {};

static int cat_index(const char* cat) {
    if (!cat) return CAT_COUNT - 1;
    for (int i = 0; i < CAT_COUNT - 1; i++)
        if (strcmp(cat, s_catNames[i]) == 0) return i;
    return CAT_COUNT - 1;
}

// ── per-predicate global disagreement counters (perspective only) ────────────
// Indices: 0=legacyRectFinite 1=homogClip 2=rectSignedW 3=rectNearFar 4=rectGuard
static const int PRED_COUNT = 5;
static const char* const s_predNames[PRED_COUNT] = {
    "legacyRectFinite", "homogClip", "rectSignedW", "rectNearFar", "rectGuard"
};
struct PredStat {
    uint32_t agree;
    uint32_t disagree_perm;   // predicate accepts, legacy rejects
    uint32_t disagree_restr;  // legacy accepts, predicate rejects
    uint32_t finite_viol;     // legacy accepts, screen is non-finite
};
static PredStat s_predStat[PRED_COUNT] = {};

// ── per-callsite heatmap ─────────────────────────────────────────────────────
#define MAX_HM_ENTRIES 128
struct HeatmapEntry {
    const char* id;
    uint32_t    pred_agree[PRED_COUNT];
    uint32_t    pred_dperm[PRED_COUNT];   // disagree-permissive
    uint32_t    pred_drestr[PRED_COUNT];  // disagree-restrictive
    uint32_t    tri_submitted;
    uint32_t    tri_contains_rejected;
};
static HeatmapEntry s_hm[MAX_HM_ENTRIES];
static int          s_hmCount = 0;

// ── triangle outcome counters ────────────────────────────────────────────────
static uint32_t s_triTotal            = 0;
static uint32_t s_triContainsRejected = 0;
static uint32_t s_triAreaExceeded     = 0;   // area > half-viewport (purple class)

//--------------------------------------------------------------------
// Helpers

static bool pz_isfinite(float v) {
#if defined(_MSC_VER)
    return _finite(v) != 0;
#else
    return isfinite(v);
#endif
}

static int hm_index(const char* id) {
    // Pointer-equality fast path for known string literals.
    for (int i = 0; i < s_hmCount; i++) {
        if (s_hm[i].id == id) return i;
        if (id && s_hm[i].id && strcmp(s_hm[i].id, id) == 0) return i;
    }
    if (s_hmCount < MAX_HM_ENTRIES) {
        memset(&s_hm[s_hmCount], 0, sizeof(HeatmapEntry));
        s_hm[s_hmCount].id = id;
        return s_hmCount++;
    }
    return 0;  // overflow: accumulate into first slot
}

static bool is_consumes_z(const char* id) {
    if (!id) return false;
    // weaponbolt beam block + actor vfx depth — flagged in inventory Observations O2.
    if (strncmp(id, "weaponbolt_beam_", 16) == 0) return true;
    if (strcmp(id, "actor_vfx_top_depth") == 0)   return true;
    return false;
}

// Compute the six per-vertex predicates from the raw projectZ inputs.
static ProjectZPredicates compute_predicates(
    const Stuff::Vector4D& rawClip,
    const Stuff::Vector4D& screen,
    bool usePerspective,
    bool accepted,
    float screenResX, float screenResY,
    int guardPx)
{
    ProjectZPredicates p = {};
    p.isPerspective = usePerspective;
    p.legacyRect    = accepted;

    if (!usePerspective) {
        // Parallel branch: modern predicates are n/a — leave false/zero.
        return p;
    }

    // legacyRectFinite
    p.legacyRectFinite = accepted &&
        pz_isfinite(screen.x) && pz_isfinite(screen.y) &&
        pz_isfinite(screen.z) && pz_isfinite(screen.w);

    // homogClip: W > 0 and |xy| within W
    p.homogClip = (rawClip.w > 0.0f) &&
                  (fabsf(rawClip.x) <= rawClip.w) &&
                  (fabsf(rawClip.y) <= rawClip.w);

    // rectSignedW: legacy rect AND W positive
    p.rectSignedW = accepted && (rawClip.w > 0.0f);

    // rectNearFar: legacy rect AND clip.z in [0, clip.w]
    p.rectNearFar = accepted && (rawClip.z >= 0.0f) && (rawClip.z <= rawClip.w);

    // rectGuard: expanded viewport
    float gx = (float)guardPx, gy = (float)guardPx;
    p.rectGuard = (screen.x >= -gx) && (screen.y >= -gy) &&
                  (screen.x <= screenResX + gx) && (screen.y <= screenResY + gy);

    return p;
}

//--------------------------------------------------------------------
// Public API

void projectz_trace_init() {
    if (s_initialized) return;
    s_initialized  = true;
    g_pzDoTrace    = (getenv("MC2_PROJECTZ_TRACE")   != nullptr);
    g_pzDoHeatmap  = (getenv("MC2_PROJECTZ_HEATMAP") != nullptr);
    g_pzDoSummary  = (getenv("MC2_PROJECTZ_SUMMARY") != nullptr);
    const char* gp = getenv("MC2_PROJECTZ_GUARD_PX");
    if (gp && atoi(gp) > 0) g_pzGuardPx = atoi(gp);
    g_pzTrace = g_pzDoTrace || g_pzDoHeatmap || g_pzDoSummary;
    if (g_pzDoTrace) {
        s_traceFile = fopen("mc2_projectz.log", "w");
        if (s_traceFile)
            // MC2_PROJECTZ_TRACE=1 writes per-vertex records to this file.
            // Each record formats ~15 floats via fprintf (~3 us/vertex on MSVC CRT).
            // At ~3300 perspective vertices/frame the frame budget increases by ~10 ms,
            // reducing normal 150+ FPS to roughly 35 FPS. This is expected and by design;
            // use MC2_PROJECTZ_SUMMARY=1 alone for full-speed statistical capture.
            puts("[PROJECTZ v1] trace file: mc2_projectz.log"
                 " -- NOTE: MC2_PROJECTZ_TRACE=1 costs ~3us/vertex (float formatting);"
                 " expect ~35 FPS vs normal 150+ FPS. Use SUMMARY=1 alone for full-speed stats.");
        else
            puts("[PROJECTZ v1] WARNING: failed to open mc2_projectz.log -- vertex/tri records suppressed");
    }
}

static void emit_summary(uint32_t frames) {
    printf("[PROJECTZ v1] summary frames=%u\n", frames);
    // Per-category
    for (int i = 0; i < CAT_COUNT; i++) {
        if (s_catStat[i].calls == 0) continue;
        printf("  category=%-22s calls=%u parallel=%u rejected=%u\n",
               s_catNames[i],
               s_catStat[i].calls,
               s_catStat[i].parallel,
               s_catStat[i].rejected);
    }
    // Per-predicate disagreement vs legacyRect (perspective calls only)
    if (s_perspCalls > 0) {
        printf("  perspective_calls=%u\n", s_perspCalls);
        for (int p = 0; p < PRED_COUNT; p++) {
            uint32_t total_dis = s_predStat[p].disagree_perm + s_predStat[p].disagree_restr;
            float pct = (s_perspCalls > 0) ? 100.0f * (float)total_dis / (float)s_perspCalls : 0.0f;
            printf("  predicate=%-18s agree=%u disagree_perm=%u disagree_restr=%u finite_viol=%u (%.2f%% of persp)\n",
                   s_predNames[p],
                   s_predStat[p].agree,
                   s_predStat[p].disagree_perm,
                   s_predStat[p].disagree_restr,
                   s_predStat[p].finite_viol,
                   pct);
        }
    }
    // Per-callsite outliers: top 10 by total disagreement
    if (s_hmCount > 0) {
        // Find top 10 by total disagreement across all predicates
        int order[MAX_HM_ENTRIES];
        for (int i = 0; i < s_hmCount; i++) order[i] = i;
        // Simple selection sort for top-10
        for (int i = 0; i < (s_hmCount < 10 ? s_hmCount : 10); i++) {
            uint32_t best = 0;
            int bestIdx = i;
            for (int j = i; j < s_hmCount; j++) {
                uint32_t tot = 0;
                for (int p = 0; p < PRED_COUNT; p++)
                    tot += s_hm[order[j]].pred_dperm[p] + s_hm[order[j]].pred_drestr[p];
                if (tot > best) { best = tot; bestIdx = j; }
            }
            int tmp = order[i]; order[i] = order[bestIdx]; order[bestIdx] = tmp;
            int idx = order[i];
            if (best == 0) break;
            printf("  outlier callsiteId=%-30s homogClip_dis=%u rectSignedW_dis=%u contains_rej_tri=%u\n",
                   s_hm[idx].id ? s_hm[idx].id : "<unknown>",
                   s_hm[idx].pred_dperm[1] + s_hm[idx].pred_drestr[1],
                   s_hm[idx].pred_dperm[2] + s_hm[idx].pred_drestr[2],
                   s_hm[idx].tri_contains_rejected);
        }
    }
    // Triangle outcomes
    printf("  triangles total=%u red(contains_rejected)=%u purple(area_exceeded)=%u\n",
           s_triTotal, s_triContainsRejected, s_triAreaExceeded);
    fflush(stdout);
}

void projectz_trace_dispatch(
    const char*            siteId,
    const char*            siteCat,
    const Stuff::Vector3D& worldPt,
    const Stuff::Vector4D& rawClip,
    const Stuff::Vector4D& screen,
    bool                   usePerspective,
    bool                   accepted,
    float                  screenResX,
    float                  screenResY)
{
    ProjectZPredicates preds = compute_predicates(
        rawClip, screen, usePerspective, accepted,
        screenResX, screenResY, g_pzGuardPx);

    // ── per-vertex trace record ──────────────────────────────────────────────
    if (g_pzDoTrace && s_traceFile) {
        float trueRhw = (rawClip.w != 0.0f) ? (1.0f / rawClip.w) : FLT_MAX;  // ±Inf-ish when 0
        fprintf(s_traceFile,
            "[PROJECTZ v1] vertex callsiteId=%s\n"
            "  file=<inline> line=0 cat=%s branch=%s\n"
            "  point=(%.4f,%.4f,%.4f)\n"
            "  signedW=%.6f legacyRhw=%.6f trueSignedRhw=%.6f"
            " rawClip=(%.4f,%.4f,%.4f,%.4f)\n"
            "  screen=(%.2f,%.2f,%.4f,%.6f) legacyAccepted=%s\n"
            "  predicates: legacyRect=%s legacyRectFinite=%s"
            " homogClip=%s rectSignedW=%s rectNearFar=%s rectGuard=%s\n"
            "  consumes_z=%s\n",
            siteId  ? siteId  : "unknown",
            siteCat ? siteCat : "unknown",
            usePerspective ? "perspective" : "parallel",
            worldPt.x, worldPt.y, worldPt.z,
            rawClip.w,
            (rawClip.w != 0.0f) ? (1.0f / rawClip.w) : 1.0f,
            trueRhw,
            rawClip.x, rawClip.y, rawClip.z, rawClip.w,
            screen.x, screen.y, screen.z, screen.w,
            accepted ? "true" : "false",
            preds.legacyRect        ? "T" : "F",
            preds.legacyRectFinite  ? "T" : "F",
            usePerspective ? (preds.homogClip   ? "T" : "F") : "n/a",
            usePerspective ? (preds.rectSignedW ? "T" : "F") : "n/a",
            usePerspective ? (preds.rectNearFar ? "T" : "F") : "n/a",
            preds.rectGuard         ? "T" : "F",
            is_consumes_z(siteId)   ? "true" : "false");
    }

    // ── summary + heatmap counters ───────────────────────────────────────────
    if (g_pzDoSummary || g_pzDoHeatmap) {
        // Category
        int ci = cat_index(siteCat);
        s_catStat[ci].calls++;
        if (!usePerspective) s_catStat[ci].parallel++;
        if (!accepted)       s_catStat[ci].rejected++;

        if (usePerspective) {
            s_perspCalls++;

            // Global per-predicate disagreement (5 predicates, excluding legacyRect itself)
            bool ref = preds.legacyRect;
            bool pred_vals[PRED_COUNT] = {
                preds.legacyRectFinite,
                preds.homogClip,
                preds.rectSignedW,
                preds.rectNearFar,
                preds.rectGuard
            };
            for (int p = 0; p < PRED_COUNT; p++) {
                if (pred_vals[p] == ref)       s_predStat[p].agree++;
                else if (pred_vals[p] && !ref) s_predStat[p].disagree_perm++;
                else                           s_predStat[p].disagree_restr++;
                // finite violation: legacy accepted but screen is non-finite
                if (p == 0 && ref && !preds.legacyRectFinite)
                    s_predStat[p].finite_viol++;
            }

            // Per-callsite heatmap
            if (g_pzDoHeatmap) {
                int hi = hm_index(siteId);
                for (int p = 0; p < PRED_COUNT; p++) {
                    if (pred_vals[p] == ref)       s_hm[hi].pred_agree[p]++;
                    else if (pred_vals[p] && !ref) s_hm[hi].pred_dperm[p]++;
                    else                           s_hm[hi].pred_drestr[p]++;
                }
            }
        }
    }
}

void projectz_emit_tri(
    const char*           callsiteId,
    int                   tileR,
    int                   tileC,
    const int             clusterIdx[3],
    const ProjectZTriVert verts[4],
    bool                  currentSubmitted,
    const char*           file,
    int                   line)
{
    const ProjectZTriVert& v0 = verts[clusterIdx[0]];
    const ProjectZTriVert& v1 = verts[clusterIdx[1]];
    const ProjectZTriVert& v2 = verts[clusterIdx[2]];

    bool containsRejected = !v0.legacyAccepted || !v1.legacyAccepted || !v2.legacyAccepted;

    // Screen-space triangle area (post-divide cross-product magnitude)
    float ax = v1.wx - v0.wx, ay = v1.wy - v0.wy;
    float bx = v2.wx - v0.wx, by = v2.wy - v0.wy;
    float area = fabsf(ax * by - ay * bx) * 0.5f;

    // Triangle policy observers (not active — observe only)
    bool pol_any = v0.legacyAccepted || v1.legacyAccepted || v2.legacyAccepted;
    bool pol_all = v0.legacyAccepted && v1.legacyAccepted && v2.legacyAccepted;
    int  accepted_count = (int)v0.legacyAccepted + (int)v1.legacyAccepted + (int)v2.legacyAccepted;
    bool pol_maj = (accepted_count >= 2);

    if (g_pzDoTrace && s_traceFile) {
        fprintf(s_traceFile,
            "[PROJECTZ v1] tri callsiteId=%s\n"
            "  quad=(%d,%d) cluster=%d,%d,%d file=%s line=%d\n"
            "  v0.legacy=%s v1.legacy=%s v2.legacy=%s\n"
            "  trianglePolicy_any=%s trianglePolicy_all=%s trianglePolicy_majority=%s\n"
            "  currentSubmitted=%s containsLegacyRejectedVertex=%s screenAreaPixels=%.1f\n",
            callsiteId ? callsiteId : "unknown",
            tileR, tileC,
            clusterIdx[0], clusterIdx[1], clusterIdx[2],
            file ? file : "?", line,
            v0.legacyAccepted ? "true" : "false",
            v1.legacyAccepted ? "true" : "false",
            v2.legacyAccepted ? "true" : "false",
            pol_any ? "true" : "false",
            pol_all ? "true" : "false",
            pol_maj ? "true" : "false",
            currentSubmitted  ? "true" : "false",
            containsRejected  ? "true" : "false",
            area);
    }

    if (g_pzDoSummary || g_pzDoHeatmap) {
        s_triTotal++;
        if (containsRejected) s_triContainsRejected++;

        // Purple class: NaN/Inf in screen coords or area > half-viewport threshold.
        // Threshold: area > 0.5 * (screenResX * screenResY) would need screen dims;
        // use a large fixed threshold for now (quarter-million pixels ≈ 500×500).
        bool purple = !pz_isfinite(v0.wx) || !pz_isfinite(v1.wx) || !pz_isfinite(v2.wx) ||
                      !pz_isfinite(v0.wy) || !pz_isfinite(v1.wy) || !pz_isfinite(v2.wy) ||
                      (area > 250000.0f);
        if (purple) s_triAreaExceeded++;

        if (g_pzDoHeatmap) {
            int hi = hm_index(callsiteId);
            if (currentSubmitted) s_hm[hi].tri_submitted++;
            if (containsRejected) s_hm[hi].tri_contains_rejected++;
        }
    }
}

void projectz_frame_tick() {
    if (!g_pzTrace) return;
    s_frameCount++;
    if (g_pzDoSummary && (s_frameCount % 600 == 0)) {
        emit_summary(s_frameCount);
    }
    if (s_traceFile && (s_frameCount % 60 == 0)) {
        fflush(s_traceFile);
    }
}

void projectz_shutdown() {
    if (!g_pzTrace) return;
    if (g_pzDoSummary) {
        emit_summary(s_frameCount);
    }
    if (s_traceFile) {
        fflush(s_traceFile);
        fclose(s_traceFile);
        s_traceFile = nullptr;
    }
}
