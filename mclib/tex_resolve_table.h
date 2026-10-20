//===========================================================================
// tex_resolve_table.h — Shape A (M0a) per-frame texture-handle memoization.
// Spec: docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md
//
// Lazy first-touch design: tex_resolve(nodeId) goes through the legacy
// legacy texture manager::get_gosTextureHandle(nodeId) accessor on the first read
// of that node this frame, memoizes the result in handles[], and returns
// indexed loads for subsequent reads. Killswitch OFF reverts to legacy.
//===========================================================================
#pragma once

#include "render_submissions.h"   // frame/render integration
#include "texture_assets.h"
#include "dstd.h"     // DWORD
#include <vector>

struct TexResolveTable {
    static constexpr DWORD kSentinel = 0xFFFFFFFFu;

    std::vector<DWORD> handles;
    std::vector<uint64_t> stamps;
    uint64_t memoGeneration;
    uint64_t    buildGeneration;
    uint32_t    resolvedThisFrame;
    bool        enabled;        // MC2_MODERN_TEX_RESOLVE on, OR validate on (validate implies enabled)
    bool        validate;       // MC2_MODERN_TEX_RESOLVE_VALIDATE — run both paths every call, compare
    bool        trace;          // MC2_MODERN_TEX_RESOLVE_TRACE — per-frame begin_frame line + oob_node prints
    bool        frameActive;    // set on first beginFrameTexResolve(); guards out-of-frame inline-accessor callers
};

extern TexResolveTable g_texResolveTable;
extern uint64_t        g_currentFrameId;

// Reset table to all-sentinel + bump generation + set frameActive=true.
// Called once per frame as the FIRST statement of Terrain::geometry
// (mission-update phase — runs before any converted setup-time read).
void beginFrameTexResolve(uint64_t frameId);

// Clear frameActive and emit the 600-frame summary tick when due.
// Called immediately after mc2::VkSceneRenderer::renderLists(mc2::renderSubmissions()) returns in
// GameCamera::render. Pairs with beginFrameTexResolve to make the
// "this is a terrain frame" window an explicit invariant.
void endFrameTexResolve(void);

// Drop every memoized handle (keeps frameActive). Call after a mid-frame texture-node teardown/reload
// (Mission::load) so tex_resolve re-reads the live nodes instead of the pre-destroy handles.
void invalidateTexResolveTable(void);

// Initialize from env vars; print [TEX_RESOLVE v1] event=startup. Called
// once at engine init.
void initTexResolveTable(void);

// Print [TEX_RESOLVE v1] event=shutdown summary. Called from atexit / engine teardown.
void shutdownTexResolveTable(void);

// Out-of-line cold-path helpers (declared here, defined in .cpp).
void texResolveLogMismatch(DWORD nodeId, DWORD table_h, DWORD legacy_h);
void texResolveLogOOB(DWORD nodeId);

// Dense, generation-stamped memoization. Capacity grows on the main thread at
// frame begin; a worker never resizes a vector from a submitted asset number.
inline DWORD tex_resolve(DWORD asset)
{
    auto& table=g_texResolveTable;
    if (!table.enabled || !table.frameActive) return mc2::textures::sourceId(asset);
    if (asset==0xffffffffu) return asset;
    if (asset>=table.handles.size()) {
        texResolveLogOOB(asset);
        return mc2::textures::sourceId(asset);
    }
    const bool cached=table.stamps[asset]==table.memoGeneration;
    if (table.validate) {
        const DWORD live=mc2::textures::sourceId(asset);
        if (!cached) {
            table.handles[asset]=live; table.stamps[asset]=table.memoGeneration; ++table.resolvedThisFrame;
        } else if (table.handles[asset]!=live) texResolveLogMismatch(asset,table.handles[asset],live);
        return live;
    }
    if (cached) return table.handles[asset];
    const auto live=mc2::textures::sourceId(asset);
    table.handles[asset]=live; table.stamps[asset]=table.memoGeneration; ++table.resolvedThisFrame;
    return live;
}
