// GameOS/gameos/gos_terrain_indirect.cpp
//
// Indirect terrain SOLID-only PR1 — Stage 0 scaffolding.
//
// See gos_terrain_indirect.h for the slice overview, the Tracy zone name
// reservations, and the public API contract (counter units = per-quad,
// parity-printer schema, env-gate semantics).
//
// Stage 0 lands env-gate readers, the three N1 counters with public function
// API, and the parity-printer + 600-frame summary skeleton. Stage 2 wires the
// recipe build/reset; Stage 3 wires the per-frame thin-record packer,
// indirect-command builder, preflight-arming, and bridge entry.

#include "gos_terrain_indirect.h"

#include <cstdio>
#include <cstdlib>

namespace gos_terrain_indirect {

// ---------------------------------------------------------------------------
// Env-gate readers
// ---------------------------------------------------------------------------

bool IsEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

bool IsParityCheckEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_PARITY_CHECK");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

bool IsTraceEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_TRACE");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

}  // namespace gos_terrain_indirect

// ---------------------------------------------------------------------------
// N1 counter storage (private to this TU; cross-TU access via public Add/Get)
// ---------------------------------------------------------------------------
namespace {
long long s_legacy_solid_setup_quads     = 0;
long long s_indirect_solid_packed_quads  = 0;
long long s_legacy_detail_overlay_quads  = 0;
}  // namespace

namespace gos_terrain_indirect {

void Counters_AddLegacySolidSetupQuad()      { ++s_legacy_solid_setup_quads; }
void Counters_AddIndirectSolidPackedQuad()   { ++s_indirect_solid_packed_quads; }
void Counters_AddLegacyDetailOverlayQuad()   { ++s_legacy_detail_overlay_quads; }

long long Counters_GetLegacySolidSetupQuads()    { return s_legacy_solid_setup_quads; }
long long Counters_GetIndirectSolidPackedQuads() { return s_indirect_solid_packed_quads; }
long long Counters_GetLegacyDetailOverlayQuads() { return s_legacy_detail_overlay_quads; }

}  // namespace gos_terrain_indirect

// ---------------------------------------------------------------------------
// Parity-printer + 600-frame summary
// ---------------------------------------------------------------------------
namespace {
int       s_parityMismatchesThisFrame = 0;
long long s_paritySummaryFrames       = 0;
long long s_paritySummaryQuads        = 0;
long long s_paritySummaryMismatches   = 0;
}  // namespace

namespace gos_terrain_indirect {

void ParityPrintMismatch(int frame, int quad, const char* layer, int tri,
                         int vert, const char* field,
                         uint32_t legacy, uint32_t fast) {
    if (s_parityMismatchesThisFrame >= 16) return;  // throttle 16/frame
    ++s_parityMismatchesThisFrame;
    fprintf(stderr,
            "[TERRAIN_INDIRECT_PARITY v1] event=mismatch frame=%d quad=%d "
            "layer=%s tri=%d vert=%d field=%s legacy=0x%08X fast=0x%08X\n",
            frame, quad, layer ? layer : "?", tri, vert, field ? field : "?",
            legacy, fast);
    fflush(stderr);
}

void ParityFrameTick(int quadsCheckedThisFrame) {
    ++s_paritySummaryFrames;
    s_paritySummaryQuads      += quadsCheckedThisFrame;
    s_paritySummaryMismatches += s_parityMismatchesThisFrame;
    s_parityMismatchesThisFrame = 0;
    if (s_paritySummaryFrames % 600 == 0) {
        fprintf(stderr,
                "[TERRAIN_INDIRECT_PARITY v1] event=summary frames=%lld "
                "quads_checked=%lld total_mismatches=%lld "
                "legacy_solid_setup_quads=%lld "
                "indirect_solid_packed_quads=%lld "
                "legacy_detail_overlay_quads=%lld\n",
                s_paritySummaryFrames,
                s_paritySummaryQuads,
                s_paritySummaryMismatches,
                Counters_GetLegacySolidSetupQuads(),
                Counters_GetIndirectSolidPackedQuads(),
                Counters_GetLegacyDetailOverlayQuads());
        fflush(stderr);
    }
}

}  // namespace gos_terrain_indirect
