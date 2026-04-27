//===========================================================================
// tex_resolve_table.h — Shape A (M0a) per-frame texture-handle memoization.
// Spec: docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md
//
// Lazy first-touch design: tex_resolve(nodeId) goes through the legacy
// MC_TextureManager::get_gosTextureHandle(nodeId) accessor on the first read
// of that node this frame, memoizes the result in handles[], and returns
// indexed loads for subsequent reads. Killswitch OFF reverts to legacy.
//===========================================================================
#pragma once

#include "txmmgr.h"   // for MC_MAXTEXTURES, mcTextureManager
#include "dstd.h"     // DWORD

struct TexResolveTable {
    static constexpr DWORD kSentinel = 0xFFFFFFFFu;

    DWORD       handles[MC_MAXTEXTURES];
    uint64_t    buildGeneration;
    uint32_t    resolvedThisFrame;
    bool        enabled;        // MC2_MODERN_TEX_RESOLVE on, OR validate on (validate implies enabled)
    bool        validate;       // MC2_MODERN_TEX_RESOLVE_VALIDATE — run both paths every call, compare
    bool        trace;          // MC2_MODERN_TEX_RESOLVE_TRACE — per-frame begin_frame line + oob_node prints
    bool        frameActive;    // set on first beginFrameTexResolve(); guards out-of-frame inline-accessor callers
};

extern TexResolveTable g_texResolveTable;
extern uint64_t        g_currentFrameId;

// Bounds-checked at compile time.
static_assert(sizeof(((TexResolveTable*)0)->handles) / sizeof(DWORD) >= MC_MAXTEXTURES,
              "g_texResolveTable.handles too small for MC_MAXTEXTURES");

// Reset table to all-sentinel + bump generation + set frameActive=true.
// Called once per frame as the FIRST statement of Terrain::geometry
// (mission-update phase — runs before any converted setup-time read).
void beginFrameTexResolve(uint64_t frameId);

// Clear frameActive and emit the 600-frame summary tick when due.
// Called immediately after mcTextureManager->renderLists() returns in
// GameCamera::render. Pairs with beginFrameTexResolve to make the
// "this is a terrain frame" window an explicit invariant.
void endFrameTexResolve(void);

// Initialize from env vars; print [TEX_RESOLVE v1] event=startup. Called
// once at engine init.
void initTexResolveTable(void);

// Print [TEX_RESOLVE v1] event=shutdown summary. Called from atexit / engine teardown.
void shutdownTexResolveTable(void);

// Out-of-line cold-path helpers (declared here, defined in .cpp).
void texResolveLogMismatch(DWORD nodeId, DWORD table_h, DWORD legacy_h);
void texResolveLogOOB(DWORD nodeId);

// Lazy first-touch resolve. The hot path.
inline DWORD tex_resolve(DWORD nodeId)
{
    // Killswitch OFF, or table not yet initialized this run, or out-of-frame
    // (mission-load / UI / non-terrain inline-accessor caller). Fall through
    // to legacy with bit-exact semantics.
    if (!g_texResolveTable.enabled || !g_texResolveTable.frameActive) {
        return mcTextureManager->get_gosTextureHandle(nodeId);
    }
    if (nodeId == 0xFFFFFFFFu) {
        return nodeId;   // matches MC_TextureManager::get_gosTextureHandle wrapper at txmmgr.h:528–531
    }
    // OOB guard. MC_MAXTEXTURES is the table cap; any nodeId beyond it would
    // index off the end of handles[]. Fall through to legacy and log once.
    if (nodeId >= MC_MAXTEXTURES) {
        texResolveLogOOB(nodeId);
        return mcTextureManager->get_gosTextureHandle(nodeId);
    }

    if (g_texResolveTable.validate) {
        // Validate mode: run both paths on EVERY call (not only first-touch),
        // compare, fall through to legacy result. This catches within-frame
        // stale-memoization caused by §7.2 out-of-scope legacy callsites
        // triggering CACHED_OUT_HANDLE eviction on a node we already memoized.
        // Performance does not matter in validate mode.
        DWORD legacy = mcTextureManager->get_gosTextureHandle(nodeId);
        DWORD memo   = g_texResolveTable.handles[nodeId];
        if (memo == TexResolveTable::kSentinel) {
            // First touch this frame — store and continue.
            g_texResolveTable.handles[nodeId] = legacy;
            g_texResolveTable.resolvedThisFrame++;
        } else if (memo != legacy) {
            texResolveLogMismatch(nodeId, memo, legacy);
            g_texResolveTable.handles[nodeId] = legacy;  // self-heal so subsequent reads converge
        }
        return legacy;
    }

    // Production hot path: lazy first-touch memoization.
    DWORD h = g_texResolveTable.handles[nodeId];
    if (h != TexResolveTable::kSentinel) {
        return h;   // memoized hit
    }
    h = mcTextureManager->get_gosTextureHandle(nodeId);
    g_texResolveTable.handles[nodeId] = h;
    g_texResolveTable.resolvedThisFrame++;
    return h;
}
