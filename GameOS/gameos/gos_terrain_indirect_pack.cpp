// gos_terrain_indirect: thin-record packer, indirect-command builder, handle LUT and solid quad window SSBO.
// Declared in the class headers; shared names in gos_terrain_indirect_internal.h.
#include "gos_terrain_indirect_internal.h"


// ---------------------------------------------------------------------------
// Stage 3 — per-frame thin-record packer, indirect-command builder,
//            preflight arming, and DrawIndirect thin executor.
//
// Bridge function (gos_terrain_bridge_drawIndirect) lives in gameos_graphics.cpp
// where gosRenderer state is accessible.  This file owns the CPU-side logic;
// the bridge is called by DrawIndirect() after arming.
// ---------------------------------------------------------------------------

namespace gos_terrain_indirect_detail {

// ---------------------------------------------------------------------------
// Thin-record SSBO (triple-buffered, GPU_STREAM_DRAW).
// Mirrors the M2 thin-record ring in gos_terrain_patch_stream.cpp.
// ---------------------------------------------------------------------------

GLuint g_thinRecordSSBO              = 0;
int    g_thinRingSlot                = 0;
GLsync g_thinRingFences[kThinRingFrames] = { 0, 0, 0 };

GLuint g_indirectCmdBuffer = 0;

// Per-frame arming state (reset each frame by ComputePreflight).
bool  s_frameSolidArmed           = false;
int   s_frameSolidPackedThinCount = 0;
int   s_frameSolidCmdCount        = 0;

// Process-sticky hard-failure latch.
bool  s_processArmingDisabled     = false;

// Process-sticky "intro pan complete observed" latch. Set on the first
// frame where ComputePreflight() arms (path=gpu or path=cpu); never
// resets within a process. Consumed by VisualDiff::onFrameTick to gate
// frame counting on the engine's own intro-complete signal -- the
// IsFrameSolidArmed() per-frame flag is reset by gosRenderer::endFrame()
// before VisualDiff's post-PP hook fires, so a sticky cousin is needed.
bool  s_everFrameSolidArmed       = false;


// ResourcesReady lazy-alloc state.
static bool  s_resourcesAllocated = false;
static bool  s_resourcesReady     = false;

// ---------------------------------------------------------------------------
// Phase C Stage 2 — SOLID GPU compute resources.
// Lazy-allocated on first ComputeDispatch(); destroyed on ResetDenseRecipe.
// ---------------------------------------------------------------------------
GLuint  g_solidComputeProgram    = 0;
// Step 2b (VPL retirement): cmd-patch program retired.  Primary compute is
// the sole writer of cmds[0].count via atomicAdd.  Bucket-header SSBO is
// demoted behind MC2_BUCKET_HEADER_TRACE=1 (default-off) per plan
// amendment C1; when off the SSBO is not allocated, not bound, not cleared,
// and no diagnostic atomicAdds run in the shader.
GLuint  g_solidBucketHeaderSsbo  = 0;
GLuint  g_terrainHandleLutSSBO   = 0;
// Camera-windowed solid dispatch (Approach A) state — DECLARED EARLY (next to
// g_denseRecipes) so the per-mission reset in ResetDenseRecipe() can clear it
// (MAJOR-1).  Definitions live near g_denseRecipes; see the comment there.
GLuint  g_thinCanarySSBO         = 0;   // probe 6: separate buffer, never bound by bridge
// Probe 8: MVP fingerprint at compute dispatch time, read by bridge at draw time.
uint32_t g_dispatchMvpFp      = 0;
uint64_t g_dispatchMvpFrameIdx = 0;
// RENDER-VIEW-CURRENCY-1: the VIEW EPOCH (g_mvpDiagFrame, bumped on every
// gos_SetWorldToClipGL = every authoritative camera-matrix publish) observed at
// the moment this snapshot's MVP was sourced. Object/mech consumers compare this
// against the CURRENT g_mvpDiagFrame to reject a snapshot taken under a different
// camera — even within the same engine frame (e.g. an early publish followed by a
// mid-frame zoom-anchor camera change). -1 = never published. Engine-frame is NOT
// sufficient here: same frame, different camera must be detectable.
long     g_dispatchMvpViewEpoch = -1;
// Probe 8b: verification — also stash first 4 floats from compute-time MVP.
// Bridge logs both sets on mismatch so we can see byte-level difference.
float    g_dispatchMvpFloats[4] = { 0, 0, 0, 0 };
// Water-consistency fix (2026-05-17): full 16-float snapshot of the MVP
// terrain-solid baked its Fix-B clipPos with THIS frame. The water compute
// reads this (via gos_terrain_indirect_getDispatchMvp16) so the drawn water
// is bit-consistent with the drawn terrain. [WATER_DEPTHPROBE v1] proved an
// exact 1-frame divergence (terrain-solid stale vs water fresh) was the
// shoreline recede/flicker/vanish root cause. ring_slot_state_must_travel_
// with_slot.md. Unconditional 64 B/frame; written only when ComputeDispatch
// actually runs (terrain-solid armed), so a consumer that gates on
// IsFrameSolidArmed() always sees this-frame-fresh data.
float    g_dispatchMvp16[16] = { 0 };

// Fix A (2026-05-14): per-ring-slot MVP stash for the intentional 1-frame
// compute->bridge lag.  Compute writes thin records to slot S using MVP_X;
// the bridge consumes slot S one frame later when terrain_mvp_ has rotated
// to MVP_Y.  Without this snapshot the VS projects records with MVP_Y while
// their pzOk gates assume MVP_X — producing the giant grey-banded terrain
// triangle under fast camera rotation documented in
// docs/superpowers/progress/2026-05-14-raster-triangle-handoff.md.
// Both the GPU path (ComputeDispatch) and the CPU path (PackThinRecordsForFrame)
// stash here for invariant uniformity, even though the CPU path is not lagged.
//
// VPL retirement step 9 (2026-05-15): DEMOTED behind MC2_RING_TRACE.  The VPL
// body was deleted in step 8c-2 (commit 63c023f) and Fix B re-homed projection
// into tr.clipPos[] (the thin VS no longer declares a terrainMVP uniform —
// shaders/gos_terrain_thin.vert:56-61), so this per-slot MVP snapshot is no
// longer load-bearing defense-in-depth.  Per the Debug-instrumentation rule
// (demote-not-silently-delete; memory/debug_instrumentation_rule.md) the array
// + the Probe 8 [RING_MVP_DELTA v1] snapshot/compare path are KEPT but gated:
// when g_envRingTrace is false the writers below are skipped and the
// gos_terrain_indirect_getRingSlotMvp() accessor returns nullptr, so the
// snapshot does zero per-frame work.  Net cost when off: ~192 bytes of static
// state.  Re-armable for any future temporal-misalignment regression via
// MC2_RING_TRACE=1.  See docs/superpowers/plans/
// 2026-05-14-vertex-project-loop-retirement.md §"Step 9" and
// memory/ring_slot_state_must_travel_with_slot.md (Fix B is the surviving
// instance of that rule; Fix A's snapshot is the retired mechanism).
extern const bool g_envRingTrace = mc2::gate::get<"MC2_RING_TRACE">();
float    g_thinSlotMVP[kThinRingFrames][16] = { { 0 } };
bool     g_thinSlotMVPValid[kThinRingFrames] = { false, false, false };

// Cached uniform locations — populated once when programs are compiled.
// Per-frame varying uniforms (u_windowCount, u_terrainMVP, u_alphaOverride)
// are still uploaded every frame; only the locations themselves are cached.
GLint g_locSolidWC  = -1;   // u_windowCount   (per-frame)
GLint g_locSolidAO  = -1;   // u_alphaOverride (per-frame, may be absent)
GLint g_locSolidMVP = -1;   // u_terrainMVP    (per-frame)
// Step 2b (VPL retirement): cmd-patch uniform-location caches retired
// (g_locCmdVPE, g_locCmdCC removed) — cmd-patch program is no longer compiled.
// u_bucketHeaderTrace gates the demoted bucket-header SSBO writes in the
// primary compute shader (MC2_BUCKET_HEADER_TRACE).
GLint g_locSolidBHT = -1;   // u_bucketHeaderTrace (per-frame int gate)
GLint g_locSolidUW  = -1;   // u_useWindow (per-frame: 1=windowed, 0=full-range identity)
GLint g_locSolidMS  = -1;   // u_mapSide (PER-FRAME: changes when a different-size map loads)
// WATER-REFLECTION-CLIP-1: u_reflectionPass (per-frame int gate; see .comp for
// rationale). Set true only for the duration of RenderWaterReflectionPass()'s
// ComputeDispatch() call, false otherwise -- so the main SOLID dispatch is
// unaffected (byte-identical pzOk behavior).
GLint g_locSolidReflPass = -1;
bool  s_solidReflectionPassActive = false;


// Flag: whether ComputeDispatch() ran the GPU path this frame.
bool s_solidGpuDispatchRanThisFrame = false;

// ---------------------------------------------------------------------------
// ResourcesReady() — lazy-allocate thin-record SSBO + indirect buffer.
// Called from ComputePreflight; returns true once both are allocated.
// ---------------------------------------------------------------------------
bool ResourcesReady() {
    if (s_resourcesReady) {
        // Between-mission check: atlas is deleted by ResetDenseRecipe but
        // s_resourcesReady isn't visible from that public function. Re-arm
        // the lazy setup path so the atlas guard re-runs next mission.
        if (g_atlasGLTex == 0) {
            s_resourcesReady     = false;
            s_resourcesAllocated = false;
        } else {
            return true;
        }
    }
    if (s_resourcesAllocated) return false;  // already tried and failed

    // Thin-record SSBO: triple-buffered, GL_STREAM_DRAW.
    if (g_thinRecordSSBO == 0) {
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_thinRecordSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinRecordSSBO);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(kThinRingFrames * kThinRecordBytes),
                     nullptr, GL_STREAM_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        if (g_thinRecordSSBO == 0) {
            if (traceOn()) {
                MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=resources_alloc_fail reason=thin_ssbo\n");
                fflush(stderr);
            }
            s_resourcesAllocated = true;
            return false;
        }

        // REGISTRY-TERRAIN-SSBO-1: register the triple-buffered thin-record SSBO
        // (observe-only metadata; never read by the draw path).
        {
            RenderCore::RenderResourceDesc d;
            d.id        = RenderCore::RenderResourceId::TerrainThinBuffer;
            d.kind      = RenderCore::RenderResourceKind::Buffer;
            d.lifetime  = RenderCore::RenderResourceLifetime::Mission;  // rebuilt per mission load (triple-buffered thin records)
            d.format    = RenderCore::RenderResourceFormat::BufferRaw;
            d.debugName = "TerrainThinBuffer";
            d.glName    = static_cast<uint32_t>(g_thinRecordSSBO);
            d.sizeBytes = static_cast<uint64_t>(kThinRingFrames) * static_cast<uint64_t>(kThinRecordBytes);
            d.valid     = true;
            RenderCore::registerOrUpdateRenderResource(d);
        }
    }

    // Indirect command buffer.
    if (g_indirectCmdBuffer == 0) {
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_indirectCmdBuffer);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
        // Step 2b (VPL retirement, C2): pre-initialize {count=0,
        // instanceCount=1, first=0, baseInstance=0} once at allocation.
        // The primary compute touches only cmds[0].count (via atomicAdd) so
        // the other three fields must be correct at allocation.  Per-frame
        // count clear is done in ComputeDispatch via glClearBufferSubData of
        // the 4-byte count slot before the primary dispatch fires.
        DrawArraysIndirectCommand initCmd[16];
        for (size_t i = 0; i < 16; ++i) {
            initCmd[i].count         = 0;
            initCmd[i].instanceCount = 1;
            initCmd[i].first         = 0;
            initCmd[i].baseInstance  = 0;
        }
        MC2_GL_BufferData(GL_DRAW_INDIRECT_BUFFER,
                     (GLsizeiptr)kIndirectCmdBufferBytes,
                     initCmd, GL_STREAM_DRAW);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        if (g_indirectCmdBuffer == 0) {
            if (traceOn()) {
                MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=resources_alloc_fail reason=indirect_buf\n");
                fflush(stderr);
            }
            s_resourcesAllocated = true;
            return false;
        }
    }

    // Atlas guard: refuse to arm if BuildColormapAtlas didn't produce a texture.
    // This keeps ResourcesReady() idempotent-false until BuildDenseRecipe has run
    // (and thus BuildColormapAtlas has run). Without this, ComputePreflight could
    // arm and the bridge would draw with no texture bound — reproducing the grey bug.
    if (g_atlasGLTex == 0) {
        if (traceOn()) {
            MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=resources_not_ready reason=atlas_not_built\n");
            fflush(stderr);
        }
        // Don't mark s_resourcesAllocated — retry next frame once atlas is ready.
        return false;
    }

    s_resourcesAllocated = true;
    s_resourcesReady     = true;
    if (traceOn()) {
        MC2_DIAGFE(stderr,
            "[TERRAIN_INDIRECT v1] event=resources_ready "
            "thinSSBO=%u indirectBuf=%u ringFrames=%d maxThin=%zu atlasGLTex=%u\n",
            (unsigned)g_thinRecordSSBO, (unsigned)g_indirectCmdBuffer,
            kThinRingFrames, kMaxThinRecords, (unsigned)g_atlasGLTex);
        fflush(stderr);
    }
    return true;
}

// ---------------------------------------------------------------------------
// PackThinRecordsForFrame() — walks live quadList, packs thin-record SSBO
// ring slot for this frame.  Returns count of packed quads (≥ 0).
//
// Skip set (per memory/water_ssbo_pattern.md):
//   1. Pointer guards: q.vertices[0..3] and ->pVertex null check
//   2. Map-edge blank-vertex: vertexNum < 0 sentinel
//   3. Recipe coverage: RecipeForVertexNum returns nullptr
//   4. Per-tri pz check (drives flags bits 1, 2)
// ---------------------------------------------------------------------------
int PackThinRecordsForFrame() {
    // VPL retirement step 1 (2026-05-14): CPU thin-record pack demoted behind
    // MC2_TERRAIN_INDIRECT_CPU_FALLBACK. Default-off — the GPU compute path in
    // shaders/gpu_driven_terrain_solid.comp is the sole projection authority
    // post Fix-B (commit 005ebc7). The CPU body is retained as the GPU-arm-
    // failure safety net per memory/stock_install_must_remain_playable.md;
    // future plan retires the declaration itself once telemetry confirms the
    // env-gate is never set in production. See
    // docs/superpowers/plans/2026-05-14-cpu-pack-retirement.md §3.
    static const bool s_cpuFallback = mc2::gate::get<"MC2_TERRAIN_INDIRECT_CPU_FALLBACK">();
    if (!s_cpuFallback) {
        // One-shot demotion log so smoke artifacts capture the gate state.
        static bool s_loggedDemote = false;
        if (!s_loggedDemote) {
            s_loggedDemote = true;
            printf("[TERRAIN_INDIRECT v1] event=cpu_pack_demoted "
                   "path=stock_solid gate=MC2_TERRAIN_INDIRECT_CPU_FALLBACK\n");
            fflush(stdout);
        }
        return 0;  // no records this frame — indirect pipeline disarms
    }

    ZoneScopedN("Terrain::ThinRecordPack");

    // Diagnostic Test 1 — reset per-frame cement classification counters.
    g_cementMappedThisFrame       = 0;
    g_concreteAllCornersThisFrame = 0;

    if (!land) return 0;
    const long total          = land->getNumQuads();
    const TerrainQuadPtr quads = land->getQuadList();
    if (!quads || total <= 0) return 0;

    // Advance ring slot — wait on the fence for this slot before overwriting.
    g_thinRingSlot = (g_thinRingSlot + 1) % kThinRingFrames;
    if (g_thinRingFences[g_thinRingSlot]) {
        glClientWaitSync(g_thinRingFences[g_thinRingSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT, 10000000u /* 10ms */);
        glDeleteSync(g_thinRingFences[g_thinRingSlot]);
        g_thinRingFences[g_thinRingSlot] = 0;
    }

    // Fix A: snapshot current MVP for this slot.  The CPU pack path is not
    // frame-lagged (records are produced and drawn in the same frame), so this
    // stash equals the bridge's terrain_mvp_ at draw time — the override is a
    // no-op for CPU-packed slots.  Stash anyway to keep the "every slot has
    // a valid MVP" invariant uniform across CPU and GPU paths.
    // Step 9 (2026-05-15): demoted behind MC2_RING_TRACE — skipped by default.
    if (g_envRingTrace) {
        if (const float* curMvp = gos_GetTerrainMVPMat4()) {
            memcpy(g_thinSlotMVP[g_thinRingSlot], curMvp, sizeof(float) * 16);
            g_thinSlotMVPValid[g_thinRingSlot] = true;
        }
    }

    // Stage area: up to kMaxThinRecords records into a stack-local shadow.
    // glBufferSubData one shot at the end.
    static TerrainQuadThinRecord s_shadow[kMaxThinRecords];
    int packed = 0;

    for (long qi = 0; qi < total && (size_t)packed < kMaxThinRecords; ++qi) {
        const TerrainQuad& q = quads[qi];

        // 1. Pointer guards (pVertex not required here — we only need
        //    vertexNum + pz for the skip set; lightRGB comes from
        //    vertices[c]->lightRGB which only requires ScreenVertex).
        if (!q.vertices[0] || !q.vertices[1] ||
            !q.vertices[2] || !q.vertices[3]) continue;

        // 2. Map-edge blank-vertex skip (vertexNum == -1 sentinel)
        const int32_t vn0 = q.vertices[0]->vertexNum;
        if (vn0 < 0 ||
            q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 ||
            q.vertices[3]->vertexNum < 0) continue;

        // 3. Recipe coverage gate
        const TerrainQuadRecipe* rec = gos_terrain_indirect::RecipeForVertexNum(vn0);
        if (!rec) continue;

        // terrainHandle: use tex_resolve for per-frame handle indirection
        // (memory/mc2_texture_handle_is_live.md — never cache raw handle).
        const uint32_t th = static_cast<uint32_t>(
            tex_resolve(static_cast<DWORD>(q.terrainHandle)));
        // Skip quads with no base terrain texture (detail-only quads).
        if (th == 0 || th == 0xffffffffu) continue;

        // 4. Per-tri pz check — mirrors quad.cpp:1836-1845 logic.
        //    vertices[c]->pz is pre-projected by vertexProjectLoop.
        bool pzc[4];
        for (int c = 0; c < 4; c++) {
            float pz_adj = q.vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
            pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
        }

        const int uvMode = q.uvMode;
        bool pzTri1, pzTri2;
        if (uvMode == 1 /*BOTTOMLEFT*/) {
            pzTri1 = pzc[0] && pzc[1] && pzc[3];
            pzTri2 = pzc[1] && pzc[2] && pzc[3];
        } else {
            // BOTTOMRIGHT / TOPRIGHT diagonal
            pzTri1 = pzc[0] && pzc[1] && pzc[2];
            pzTri2 = pzc[0] && pzc[2] && pzc[3];
        }
        if (!pzTri1 && !pzTri2) continue;  // both culled

        // lightRGB — mirrors quad.cpp:1860-1865 (lightRGBc lambda).
        // alphaOverride (whitens lighting when terrainTextures2 is active
        // and not pure-cement-with-no-overlay) is baked into the existing
        // M2 thin path.  We mirror the exact same logic here.
        // The recipe already carries terrainHandle; we just need per-corner light.
        // Judgment call: use the same simplified lightRGBc pattern the M2 thin
        // path uses — terrainTextures2 check for alphaOverride, selection check.
        const bool alphaOverride = (Terrain::terrainTextures2 != nullptr);
        auto lightRGBc = [&](int c) -> uint32_t {
            DWORD lc = q.vertices[c]->lightRGB;
            if (alphaOverride) lc = 0xffffffffu;
            if (q.vertices[c]->pVertex && q.vertices[c]->pVertex->selected)
                lc = static_cast<DWORD>(0xffff7fffu /*SELECTION_COLOR*/);
            return static_cast<uint32_t>(lc);
        };

        TerrainQuadThinRecord& tr = s_shadow[packed];
        tr.recipeIdx     = static_cast<uint32_t>(vn0);
        tr.terrainHandle = th;
        tr.flags         = static_cast<uint32_t>((uvMode == 1 ? 1u : 0u)
                         | (pzTri1 ? 2u : 0u)
                         | (pzTri2 ? 4u : 0u));
        // Cement layer-index lookup, keyed by textures[] SLOT (stable across
        // frames).  nodeIdx (q.terrainHandle) mutates per-frame per
        // memory/mc2_texture_handle_is_live.md, so a nodeIdx-keyed lookup
        // intermittently misses → cement validity bit flickers → visible
        // concrete flicker on indirect base + perceived flicker on alpha-cement
        // overlay composite.  Slot is allocated once by initTexture and stable
        // for the mission lifetime.  Re-derive the slot at packer time from
        // pVertex->textureData & 0xFFFFu (the same expression quad.cpp:546
        // passes to getTextureHandle).
        //
        // Encoding (V23, widened in V27):
        //   bit 31     = CEMENT_LAYER_VALID — disambiguates "layer 0" from "not cement"
        //   bits 30:16 = reserved for future layers (decals, scorch)
        //   bits 15:0  = cement atlas layer index (0..65535 encoding cap;
        //                practically capped at 1024 by atlas budget — see
        //                BuildCementCatalogAtlas)
        constexpr uint32_t kCementLayerValidBit = 0x80000000u;
        uint32_t cementWord = 0u;
        uint16_t idx = 0xFFFFu;
        if (g_cementLayerMapReady && q.vertices[0] && q.vertices[0]->pVertex) {
            const DWORD slot = q.vertices[0]->pVertex->textureData & 0xFFFFu;
            if (slot < (DWORD)MC_MAX_TERRAIN_TXMS) {
                idx = g_cementLayerIndexBySlot[slot];
                if (idx != 0xFFFFu) {
                    cementWord = kCementLayerValidBit | ((uint32_t)idx & 0xFFFFu);
                    ++g_cementMappedThisFrame;
                }
            }
            // Lifecycle counter: count quads that EXPECT a cement layer
            // (all 4 corner materials in recipe._wp0 == Concrete=3),
            // independent of whether the layer lookup succeeded.
            if (rec) {
                uint32_t tpacked = 0u;
                memcpy(&tpacked, &rec->_wp0, 4);
                const bool allConcrete =
                    ((tpacked        & 0xFFu) == 3u) &&
                    (((tpacked >> 8) & 0xFFu) == 3u) &&
                    (((tpacked >>16) & 0xFFu) == 3u) &&
                    (((tpacked >>24) & 0xFFu) == 3u);
                if (allConcrete) {
                    ++g_concreteAllCornersThisFrame;
                    if (idx == 0xFFFFu) ++g_cementPackUnmappedCount;
                }
            }
        }
        // Old nodeIdx-based lookup — kept as dead code for one commit; the
        // slot-keyed lookup above is what drives correctness now.  Slot is
        // stable across frames; nodeIdx is not.
        // const DWORD nodeIdx = (DWORD)q.terrainHandle;
        // if (nodeIdx < (DWORD)MC_MAXTEXTURES) {
        //     const uint16_t idx2 = g_cementLayerIndexByNodeIdx[nodeIdx];
        //     ...
        // }
        tr.cementWord    = cementWord;
        tr.lightRGB0     = lightRGBc(0);
        tr.lightRGB1     = lightRGBc(1);
        tr.lightRGB2     = lightRGBc(2);
        tr.lightRGB3     = lightRGBc(3);

        // Fix B 2026-05-14: per-corner clip-space positions from the recipe's
        // worldPos0..3 multiplied by the current terrainMVP.  Mirrors the
        // compute shader's projectClip() at gpu_driven_terrain_solid.comp:174.
        //
        // Matrix convention (memory/terrain_mvp_gl_false.md): terrain_mvp_ is
        // stored row-major on CPU and uploaded with GL_FALSE.  GLSL interprets
        // bytes as column-major, so GPU's `m * v` reads as a dot of v with the
        // i-th *column* of our row-major storage — i.e. out[i] = sum_j m[j*4+i]
        // * v[j].  CPU mirror uses the same formula so values match modulo
        // FMA-fusion / rounding-mode drift (parity comparator allows ~4 ULP).
        const float* mvp = gos_GetTerrainMVPMat4();
        if (mvp) {
            const float* wp[4] = {
                &rec->wx0, &rec->wx1, &rec->wx2, &rec->wx3
            };
            for (int c = 0; c < 4; ++c) {
                const float vx = wp[c][0], vy = wp[c][1], vz = wp[c][2];
                for (int i = 0; i < 4; ++i) {
                    tr.clipPos[c * 4 + i] =
                        mvp[0 * 4 + i] * vx +
                        mvp[1 * 4 + i] * vy +
                        mvp[2 * 4 + i] * vz +
                        mvp[3 * 4 + i];
                }
            }
        } else {
            // No MVP this frame (impossible during normal play, defensive).
            // Zero clipPos so VS produces a degenerate triangle behind near
            // clip — same fail-safe shape as the pzOk culled path in the VS.
            for (int k = 0; k < 16; ++k) tr.clipPos[k] = 0.0f;
        }

        gos_terrain_indirect::Counters_AddIndirectSolidPackedQuad();
        ++packed;
    }

    if (packed == 0) return 0;

    // Upload to the current ring slot.
    const GLintptr slotOffset = (GLintptr)(g_thinRingSlot * kThinRecordBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinRecordSSBO);
    MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER,
                    slotOffset,
                    (GLsizeiptr)(packed * sizeof(TerrainQuadThinRecord)),
                    s_shadow);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Diagnostic Test 1 — per-frame cement classification trace (1/sec at 60fps).
    {
        static uint32_t s_frameCount = 0;
        ++s_frameCount;
        if (traceOn() && (s_frameCount % 60u == 0u)) {
            printf("[CEMENT_FRAME v1] frame=%u packed_total=%u cement_mapped=%u concrete_all_corners=%u\n",
                   (unsigned)s_frameCount, (unsigned)packed,
                   (unsigned)g_cementMappedThisFrame,
                   (unsigned)g_concreteAllCornersThisFrame);
            fflush(stdout);
        }
    }

    return packed;
}

// ---------------------------------------------------------------------------
// BuildIndirectCommands() — builds 1 DrawArraysIndirectCommand (PR1 SOLID-only).
// Returns 1 on success, 0 on error.
// ---------------------------------------------------------------------------
int BuildIndirectCommands(int thinCount) {
    if (thinCount <= 0) return 0;

    DrawArraysIndirectCommand cmd{};
    cmd.count         = static_cast<GLuint>(thinCount * 6);
    cmd.instanceCount = 1u;
    cmd.first         = 0u;
    cmd.baseInstance  = 0u;

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
    MC2_GL_BufferSubData(GL_DRAW_INDIRECT_BUFFER, 0,
                    (GLsizeiptr)sizeof(cmd), &cmd);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    return 1;
}

// ---------------------------------------------------------------------------
// UploadTerrainHandleLUT — Phase C Stage 2 (GPU-direct replacement for
// BuildSolidQuadWindowSSBO). Builds a uint[MC_MAXTEXTURES] LUT mapping
// nodeId → resolved gosHandle (tex_resolve result), then uploads it as a
// 16KB SSBO on binding 2. The GPU shader reads nodeId from recipe._wp2 and
// indexes into this LUT — eliminating the per-frame quadList CPU walk.
// Returns g_denseRecipes.size() as the GPU dispatch count (full recipe range).
// ---------------------------------------------------------------------------
uint32_t UploadTerrainHandleLUT() {
    constexpr GLsizeiptr kLutBytes = MC_MAXTEXTURES * sizeof(uint32_t);

    if (g_terrainHandleLutSSBO == 0) {
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_terrainHandleLutSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_terrainHandleLutSSBO);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER, kLutBytes, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Resolve unique nodeIds from the mission-static set — typically 20-50
    // entries regardless of camera position. O(uniqueTextures), not O(quads).
    static uint32_t lut[MC_MAXTEXTURES];
    memset(lut, 0, sizeof(lut));
    for (uint32_t nodeId : g_uniqueTerrainNodeIds) {
        if (nodeId > 0u && nodeId < (uint32_t)MC_MAXTEXTURES) {
            const uint32_t th = static_cast<uint32_t>(tex_resolve((DWORD)nodeId));
            lut[nodeId] = (th == 0u || th == 0xffffffffu) ? 0u : th;
        }
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_terrainHandleLutSSBO);
    MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, kLutBytes, lut);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    {
        static bool s_lutFirstPrinted = false;
        if (!s_lutFirstPrinted) {
            s_lutFirstPrinted = true;
            int nonZero = 0;
            for (uint32_t nodeId : g_uniqueTerrainNodeIds) {
                if (nodeId > 0u && nodeId < (uint32_t)MC_MAXTEXTURES && lut[nodeId] != 0u)
                    ++nonZero;
            }
            MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=lut_upload unique=%d nonzero=%d\n",
                    (int)g_uniqueTerrainNodeIds.size(), nonZero);
            fflush(stderr);
        }
    }

    return (uint32_t)g_denseRecipes.size();
}

// ---------------------------------------------------------------------------
// BuildSolidQuadWindowSSBO — Approach A.  Structural twin of WaterStream's
// BuildQuadWindowSSBO (gos_terrain_water_stream.cpp:1109).  Uploads the
// per-frame camera-windowed list of recipe indices collected by
// AppendSolidWindowCandidate() into a std430 plain-uint[] SSBO (4 B stride,
// NOT the old 16 B SolidQuadWindowEntry — no struct, no static_assert
// lockstep; the GPU re-derives thSlot/cementWord/uvMode from the recipe).
//
// Lazy-grow capped at kMaxThinRecords using the CPU-side capacity mirror
// (g_solidQuadWindowCapacity) — NO glGetBufferSubData / glMapBuffer.
//
// Return value == GPU dispatch count:
//   * narrow ON  && staging non-empty: window count; *outUseWindow = 1.
//   * narrow OFF || staging empty:     full g_denseRecipes.size() fallback;
//     *outUseWindow = 0 (shader uses identity vn0 = id == current HEAD path).
// The empty-on-armed-frame case (e.g. the 1-frame lag's very first armed
// frame, before the loop has run once) falls back to full-range so terrain
// is never blank.
uint32_t BuildSolidQuadWindowSSBO(int* outUseWindow) {
    const bool narrowOn = gos_terrain_indirect::SolidWindowEnabled();

    if (!narrowOn || g_solidWindowStaging.empty()) {
        if (outUseWindow) *outUseWindow = 0;
        return (uint32_t)g_denseRecipes.size();
    }

    // Cap the window at the thin-record capacity — the GPU output array is
    // kMaxThinRecords entries; dispatching more invocations than that cannot
    // admit more records, and over-large windows waste GPU threads.
    uint32_t windowCount = (uint32_t)g_solidWindowStaging.size();
    if (windowCount > (uint32_t)kMaxThinRecords)
        windowCount = (uint32_t)kMaxThinRecords;
    if (windowCount == 0) {
        if (outUseWindow) *outUseWindow = 0;
        return (uint32_t)g_denseRecipes.size();
    }

    const uint32_t bytesNeeded = windowCount * (uint32_t)sizeof(uint32_t);

    // Lazy-grow via the CPU-side capacity mirror (no GPU round-trip).  Cap the
    // allocation at kMaxThinRecords entries (the hard upper bound).
    if (g_solidQuadWindowSsbo != 0 && g_solidQuadWindowCapacity < bytesNeeded) {
        glDeleteBuffers(1, &g_solidQuadWindowSsbo);
        g_solidQuadWindowSsbo     = 0;
        g_solidQuadWindowCapacity = 0;
    }
    if (g_solidQuadWindowSsbo == 0) {
        const uint32_t cap =
            (uint32_t)(kMaxThinRecords * sizeof(uint32_t));
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_solidQuadWindowSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidQuadWindowSsbo);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)cap, nullptr,
                     GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        g_solidQuadWindowCapacity = cap;
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidQuadWindowSsbo);
    MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)bytesNeeded,
                    g_solidWindowStaging.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // -----------------------------------------------------------------------
    // PARITY PROBE (catastrophic-axis).  Active ONLY under
    // MC2_TERRAIN_SOLID_WINDOW_PARITY=1 — zero hot cost when off.
    //
    // Non-tautological by construction: `win` is built from
    // g_solidWindowStaging (produced by terrain.cpp's slim-loop predicate:
    // clipInfo cull-active + RecipeForVertexNum).  `ref` is built by an
    // INDEPENDENT producer here — iterating g_denseRecipes indices and
    // applying: Terrain::getObjVertexActive(vn) (the slim loop's dilated
    // visible-cull superset, the SAME source-of-truth the corrected collector
    // rides) AND the GPU shader keep predicate MINUS pz (edge-skip +
    // recipe-resolvable + _wp2!=0 + LUT-resolvable-or-cement).  Two different
    // producers (g_denseRecipes/objVertexActive vs g_solidWindowStaging), two
    // different data sources — ref does NOT read g_solidWindowStaging.
    //
    // Catastrophic direction only: a recipe that is cull-active AND the GPU
    // WOULD keep (ref-member) AND that is a live drawable quad
    // (RecipeForVertexNum non-null) but is ABSENT from `win` == a dropped
    // visible quad == terrain-vanish.  With identical source-of-truth and
    // same-frame consumption, dropped MUST be 0 steady-state AND under camera
    // motion; any nonzero is logged (gated stderr, NOT assert — assert is a
    // no-op under RelWithDebInfo's /DNDEBUG).
    if (gos_terrain_indirect::IsSolidWindowParityEnabled()) {
        const long mapSide = Terrain::realVerticesMapSide;
        const size_t N = g_denseRecipes.size();

        // win membership set (recipe idx -> present this frame's window).
        static std::vector<uint8_t> winSet;
        winSet.assign(N, 0u);
        for (uint32_t i = 0; i < windowCount; ++i) {
            const uint32_t v = g_solidWindowStaging[i];
            if (v < (uint32_t)N) winSet[v] = 1u;
        }

        uint64_t refVisible = 0;
        uint64_t dropped    = 0;
        if (mapSide > 0) {
            for (size_t vn = 0; vn < N; ++vn) {
                const long mx = (long)vn % mapSide;
                const long my = (long)vn / mapSide;
                // GPU edge-skip (gpu_driven_terrain_solid.comp:244).
                if (mx >= mapSide - 1 || my >= mapSide - 1) continue;

                // MAJOR-2: ref must add the cull-active term so it matches the
                // corrected (slim-loop / dilated-cull) collector's source-of-
                // truth.  Without this the probe would false-alarm on every
                // off-window-but-GPU-keepable recipe.  This is the SAME
                // objVertexActive[] the slim loop writes via
                // setObjVertexActive(rv->vertexNum,true) — read independently
                // here, NOT via g_solidWindowStaging.
                if (!Terrain::getObjVertexActive((long)vn)) continue;

                const TerrainQuadRecipe& rec = g_denseRecipes[vn];
                uint32_t nodeId = 0u, cementWord = 0u;
                memcpy(&nodeId,     &rec._wp2, 4);
                memcpy(&cementWord, &rec._wp3, 4);
                const bool cementQuad = (cementWord & 0x80000000u) != 0u;

                // GPU thSlot gate (comp:285-294): non-cement quads need a
                // resolvable nodeId; cement quads pass regardless.
                bool gpuKeep;
                if (nodeId == 0u || nodeId >= (uint32_t)MC_MAXTEXTURES) {
                    gpuKeep = cementQuad;
                } else {
                    const uint32_t th =
                        static_cast<uint32_t>(tex_resolve((DWORD)nodeId));
                    const bool resolvable =
                        (th != 0u && th != 0xffffffffu);
                    gpuKeep = resolvable || cementQuad;
                }
                if (!gpuKeep) continue;

                // Restrict to quads that are actually live drawables this
                // frame (a recipe slot with no current quad cannot vanish).
                if (!gos_terrain_indirect::RecipeForVertexNum((int32_t)vn)) continue;

                ++refVisible;
                if (vn < N && winSet[vn] == 0u) ++dropped;
            }
        }

        static int      s_parityFrame    = 0;
        static uint32_t s_parityMaxWin   = 0;
        static uint64_t s_parityTotDrop  = 0;
        const int frame = s_parityFrame++;
        if (windowCount > s_parityMaxWin) s_parityMaxWin = windowCount;
        s_parityTotDrop += dropped;

        if (frame < 64) {
            fprintf(stderr,
                "[TERRAIN_SOLID_WINDOW v1] event=frame frame=%d window=%u "
                "ref_visible=%u dropped=%u\n",
                frame, windowCount, (unsigned)refVisible,
                (unsigned)dropped);
            fflush(stderr);
        }
        if (frame > 0 && (frame % 600) == 0) {
            fprintf(stderr,
                "[TERRAIN_SOLID_WINDOW v1] event=summary frames=%u "
                "max_window=%u total_dropped=%llu\n",
                (unsigned)frame, s_parityMaxWin,
                (unsigned long long)s_parityTotDrop);
            fflush(stderr);
        }
        // CRITICAL-1: catastrophic-axis tripwire — gated unconditional stderr,
        // NOT assert.  The project mandates --config RelWithDebInfo, under
        // which MSVC injects /DNDEBUG (CMakeLists does not strip it), so
        // assert() compiles to a no-op and a dropped visible quad would pass
        // silently.  fprintf always fires regardless of NDEBUG.
        if (dropped) {
            fprintf(stderr,
                "[TERRAIN_SOLID_WINDOW v1] event=catastrophic dropped=%llu "
                "frame=%d\n",
                (unsigned long long)dropped, frame);
            fflush(stderr);
        }
        (void)dropped;
    }

    if (outUseWindow) *outUseWindow = 1;
    return windowCount;
}

}  // namespace gos_terrain_indirect_detail
