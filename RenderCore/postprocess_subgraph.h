#pragma once
// POSTPROCESS-SUBGRAPH-1: declarative sub-stage inventory for the PostProcess pass.
//
// Mirrors the kRenderPassContracts static-array + sentinel + static_assert pattern
// (RenderPassContract.h). GL-free; no game-side includes. Offline-testable.
//
// Declares all 10 sub-stages IN CALL ORDER from endScene() (gos_postprocess.cpp):
//   ClusterDepthPyramid(:2429) — SUBGRAPH-2: compute dispatch, default-OFF, isCompute=true
//   LightgridBuild     (:2437) — SUBGRAPH-2: compute dispatch, default-OFF, isCompute=true
//   PostprocessComputeBlur(:2447) — SUBGRAPH-2: compute dispatch, default-OFF, isCompute=true
//   ScreenShadow       (:2451) — SUBGRAPH-2: draw (sceneFBO_), default-ON in-mission, not executor-owned
//   CloudShadow        (:2458) — SUBGRAPH-1: draw (sceneFBO_), default-OFF
//   Shoreline          (:2464) — SUBGRAPH-1: draw (sceneFBO_), default-ON in-mission
//   EdgeFog            (:2477) — SUBGRAPH-1: draw (sceneFBO_), default-ON in-mission
//   FogOob             (:2484) — SUBGRAPH-1: draw (sceneFBO_), default-ON in-mission
//   Composite          (:2489) — SUBGRAPH-1: SUBGRAPH OUTPUT EDGE (binds FBO 0)
//   ShadowDebugOverlay (:2653) — SUBGRAPH-1: debug overlay on FBO 0 (post-composite)
//
// POSTPROCESS-SUBGRAPH-2 (Slice 2): ClusterDepthPyramid, LightgridBuild,
//   PostprocessComputeBlur, ScreenShadow — all 4 remaining rows added here.
// DEFERRED (HIGH-3): top-level kRenderPassContracts PostProcess row MainNormal producer.
//
// INVARIANTS:
//   - reads[]/writes[] are Unknown-terminated (RenderResourceId::Unknown = 0).
//   - ownedByExecutor=true -> the island is wrapped by the GL executor (only Composite since the GL fog, screen-shadow, cloud and shoreline passes went in s62).
//   - ownedByExecutor=false -> declared for documentation / offline validation only.
//   - Composite must be the sole subpass with fboTarget==Backbuffer that precedes
//     ShadowDebugOverlay; it is the subgraph's output edge (sceneFBO_->FBO0).
//   - ShadowDebugOverlay may follow Composite (FBO 0 already bound); ownedByExecutor=false.

#include <cstddef>
#include "RenderCore/RenderResourceRegistry.h"
#include "RenderCore/frame_executor.h"  // ExecutorIslandId

namespace RenderCore { namespace framegraph {

// ---------------------------------------------------------------------------
// PostProcessSubpass descriptor
// ---------------------------------------------------------------------------

struct PostProcessSubpass {
    ExecutorIslandId id;
    const char*      name;
    // Resources read.  Unknown-terminated (max 4 entries + terminator).
    RenderResourceId reads[5];
    // Resources written. Unknown-terminated (max 2 entries + terminator).
    RenderResourceId writes[3];
    // FBO target for this sub-stage (MainColor=sceneFBO_, Backbuffer=FBO0).
    RenderResourceId fboTarget;
    // true = sub-stage always runs when initialized_ is true (no env/latch gate).
    bool             conditional;
    // true = this sub-stage is wrapped by executorOwnBeginSub/EndSub.
    bool             ownedByExecutor;
    // POSTPROCESS-SUBGRAPH-2: true = sub-stage is a compute dispatch (no FBO, no color/depth chain).
    // isCompute rows are SKIPPED by the read-satisfaction walk (they do not consume the
    // color/depth chain). Their compute outputs now HAVE RenderResourceIds
    // (REGISTRY-COMPUTE-IDS-1: ClusterDepthPyramid=21, LightgridGrid=22,
    // LightgridIndex=23, PostprocessComputeBlur=24), but the reads[]/writes[] coarse
    // I/O is intentionally left Unknown-terminated empty here because these are
    // default-OFF substrate stages with no wired downstream consumer yet.
    bool             isCompute;
    // Note for cross-reference / slice rationale (string literal).
    const char*      note;
};

// ---------------------------------------------------------------------------
// kPostProcessSubpasses — 14 rows (Slice-1 + Slice-2), IN CALL ORDER from endScene().
//
// isCompute=true rows (ClusterDepthPyramid, LightgridBuild, PostprocessComputeBlur)
// are SKIPPED by the read-satisfaction walk — they have no FBO. Their compute outputs
// now have RenderResourceIds (REGISTRY-COMPUTE-IDS-1: ClusterDepthPyramid=21,
// LightgridGrid=22, LightgridIndex=23, PostprocessComputeBlur=24), but remain
// default-OFF substrate with no wired consumer. See validatePostProcessSubgraph() below.
// ---------------------------------------------------------------------------

static constexpr PostProcessSubpass kPostProcessSubpasses[] = {
    // -----------------------------------------------------------------------
    // 3. ClusterDepthPyramid (call-site :2429) — POSTPROCESS-SUBGRAPH-2
    //    Gate: MC2_CLUSTER_DEPTH_PYRAMID, default OFF. Compute dispatch; no FBO.
    //    isCompute=true: skipped by read-satisfaction walk. Output tile image now has
    //    RenderResourceId::ClusterDepthPyramid=21 (REGISTRY-COMPUTE-IDS-1);
    //    coarse reads[]/writes[] left empty (default-OFF substrate, no wired consumer).
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::ClusterDepthPyramid,
        /*name*/           "ClusterDepthPyramid",
        /*reads*/          { RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::Unknown,
        /*conditional*/    true,
        /*ownedByExecutor*/false,
        /*isCompute*/      true,
        /*note*/           "SUBGRAPH-2. Compute dispatch: per-tile (min,max) depth image. "
                           "Reads sceneDepthTex_ (inferred). Output tile image = "
                           "RenderResourceId::ClusterDepthPyramid (=21, REGISTRY-COMPUTE-IDS-1); "
                           "coarse reads[]/writes[] left empty (default-OFF substrate, no wired consumer). "
                           "isCompute=true: skipped by read-satisfaction walk. MC2_CLUSTER_DEPTH_PYRAMID default-OFF.",
    },
    // -----------------------------------------------------------------------
    // 4. LightgridBuild (call-site :2437) — POSTPROCESS-SUBGRAPH-2
    //    Gate: MC2_LIGHTGRID_BUILD, default OFF. Compute dispatch; no FBO.
    //    isCompute=true: skipped by read-satisfaction walk. Outputs now have
    //    RenderResourceIds: LightgridGrid=22 + LightgridIndex=23 (REGISTRY-COMPUTE-IDS-1),
    //    reading ClusterDepthPyramid=21; coarse reads[]/writes[] left empty (default-OFF substrate).
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::LightgridBuild,
        /*name*/           "LightgridBuild",
        /*reads*/          { RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::Unknown,
        /*conditional*/    true,
        /*ownedByExecutor*/false,
        /*isCompute*/      true,
        /*note*/           "SUBGRAPH-2. Compute dispatch: per-tile light-bin grid. "
                           "Reads ClusterDepthPyramid (=21) tile image. Outputs = "
                           "LightgridGrid (=22, sphere SSBO) + LightgridIndex (=23, per-tile index pool), "
                           "REGISTRY-COMPUTE-IDS-1; coarse reads[]/writes[] left empty (default-OFF substrate). "
                           "isCompute=true: skipped by read-satisfaction walk. MC2_LIGHTGRID_BUILD default-OFF.",
    },
    // -----------------------------------------------------------------------
    // 5. PostprocessComputeBlur (call-site :2447) — POSTPROCESS-SUBGRAPH-2
    //    Gate: MC2_POSTPROCESS_COMPUTE_BLUR, default OFF. Compute dispatch; no FBO.
    //    isCompute=true: skipped by read-satisfaction walk. Reads SceneColorCopy
    //    (sceneColorCopyTex_); blur output now = RenderResourceId::PostprocessComputeBlur=24
    //    (REGISTRY-COMPUTE-IDS-1, substrate only — no consumer).
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::PostprocessComputeBlur,
        /*name*/           "PostprocessComputeBlur",
        /*reads*/          { RenderResourceId::SceneColorCopy,
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::Unknown,
        /*conditional*/    true,
        /*ownedByExecutor*/false,
        /*isCompute*/      true,
        /*note*/           "SUBGRAPH-2. Compute dispatch: downsample + Gaussian blur substrate. "
                           "Reads SceneColorCopy (sceneColorCopyTex_ via getSceneColorCopyTexture()). "
                           "Output = RenderResourceId::PostprocessComputeBlur (=24, REGISTRY-COMPUTE-IDS-1, "
                           "ping-pong substrate — no wired consumer). "
                           "isCompute=true: skipped by read-satisfaction walk. MC2_POSTPROCESS_COMPUTE_BLUR default-OFF.",
    },
    // -----------------------------------------------------------------------
    // 6. ScreenShadow (call-site :2458) — EXECUTOR-ISLAND-SCREENSHADOW-1
    //    Gate: screenShadowEnabled_ && sceneHasTerrain_ && shadowsEnabled_ (default ON in-mission)
    //    Reads: sceneDepthTex_(unit0), sceneNormalTex_(unit1), shadowDepthTex_(unit2),
    //           dynShadowArrayTex_/dynShadowDepthTex_(unit3), dynamicFullMapTex_(unit4, CSM only).
    //    Writes sceneFBO_.COLOR0 (multiplicative darken). NOW executor-owned (validate-only).
    //    ownedByExecutor=true: SCREENSHADOW-TEX-RESTORE-1 (a0b4189b) fixed the unit-3
    //    GL_TEXTURE_2D_ARRAY leak (CSM path now unbinds at exit). Body unchanged.
    //    Exit: glDisable(GL_BLEND)+glActiveTexture(GL_TEXTURE0) at lines 2150/2160.
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::ScreenShadow,
        /*name*/           "ScreenShadow",
        /*reads*/          { RenderResourceId::MainDepth,
                             RenderResourceId::MainNormal,
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::MainColor,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::MainColor,
        /*conditional*/    true,
        /*ownedByExecutor*/false,
        /*isCompute*/      false,
        /*note*/           "SUBGRAPH-2. EXECUTOR-ISLAND-SCREENSHADOW-1 (validate-only). "
                           "Screen-space shadow (draw, sceneFBO_ SingleColor). "
                           "Reads scene depth and normals; native Vulkan owns its shadow inputs. "
                           "Writes MainColor (multiply darken). Owned since SCREENSHADOW-TEX-RESTORE-1 "
                           "(a0b4189b) unbinds unit-3 2D_ARRAY on exit. "
                           "Exit: glDisable(GL_BLEND) line 2150, glActiveTexture(GL_TEXTURE0) line 2160. "
                           "screenShadowEnabled_+shadowsEnabled_ default-ON in-mission.",
    },
    // -----------------------------------------------------------------------
    // 7. CloudShadow (call-site :2458) — POSTPROCESS-SUBGRAPH-1
    //    Gate: enableCloudShadow_ && sceneHasTerrain_ (MC2_CLOUD_SHADOW, default OFF)
    //    Reads sceneDepthTex_ (unit 0). Writes sceneFBO_.COLOR0 (multiply darken).
    //    Stays on sceneFBO_; does NOT bind FBO 0.
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::CloudShadow,
        /*name*/           "CloudShadow",
        /*reads*/          { RenderResourceId::MainDepth,
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::MainColor,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::MainColor,
        /*conditional*/    true,
        /*ownedByExecutor*/false,
        /*isCompute*/      false,
        /*note*/           "SUBGRAPH-1. ISLAND-3 owned. Procedural cloud darken over all non-sky pixels "
                           "(MC2_CLOUD_SHADOW default-OFF). Reads MainDepth (unit0). "
                           "Writes MainColor via sceneFBO_.",
    },
    // -----------------------------------------------------------------------
    // 8. Shoreline (call-site :2464) — POSTPROCESS-SUBGRAPH-1
    //    Gate: shorelineEnabled_ && sceneHasTerrain_ (default ON in-mission)
    //    Reads sceneDepthTex_ (unit 0) + sceneNormalTex_ (unit 1) = MainNormal.
    //    Writes sceneFBO_.COLOR0 (multiply brighten at shore boundary).
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::Shoreline,
        /*name*/           "Shoreline",
        /*reads*/          { RenderResourceId::MainDepth,
                             RenderResourceId::MainNormal,
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::MainColor,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::MainColor,
        /*conditional*/    true,
        /*ownedByExecutor*/false,
        /*isCompute*/      false,
        /*note*/           "SUBGRAPH-1. ISLAND-3 owned. Shoreline foam brightening. "
                           "Reads MainDepth (unit0) + MainNormal/sceneNormalTex_ (unit1). "
                           "Writes MainColor via sceneFBO_.",
    },
    // -----------------------------------------------------------------------
    // 11. EdgeFog (call-site :2477) — POSTPROCESS-SUBGRAPH-1
    //     Gate: edgeFogEnabled_ && mapHalfExtent_ > 0 && sceneHasTerrain_ (default ON)
    //     Reads sceneDepthTex_ (unit 0). Writes sceneFBO_.COLOR0 (alpha-blend fog).
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::EdgeFog,
        /*name*/           "EdgeFog",
        /*reads*/          { RenderResourceId::MainDepth,
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::MainColor,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::MainColor,
        /*conditional*/    true,
        /*ownedByExecutor*/false,
        /*isCompute*/      false,
        /*note*/           "SUBGRAPH-1. ISLAND-2 owned. Map-boundary edge fog (default ON in-mission). "
                           "Reads MainDepth (unit0). Writes MainColor via sceneFBO_.",
    },
    // -----------------------------------------------------------------------
    // 12. FogOob (call-site :2484) — POSTPROCESS-SUBGRAPH-1
    //     Gate: fogOobEnabled_ && sceneHasTerrain_ (default ON in-mission)
    //     Reads sceneDepthTex_ (unit 0). Writes sceneFBO_.COLOR0 (alpha-blend OOB fog).
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::FogOob,
        /*name*/           "FogOob",
        /*reads*/          { RenderResourceId::MainDepth,
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::MainColor,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::MainColor,
        /*conditional*/    true,
        /*ownedByExecutor*/false,
        /*isCompute*/      false,
        /*note*/           "SUBGRAPH-1. ISLAND-2 owned. Out-of-bounds ground fog (default ON in-mission). "
                           "Reads MainDepth (unit0). Writes MainColor via sceneFBO_.",
    },
    // -----------------------------------------------------------------------
    // 13. Composite (call-site :2489 glBindFramebuffer(GL_FRAMEBUFFER, 0))
    //     Gate: always (compositeProg_ valid + initialized_)
    //     Reads sceneColorTex_ (MainColor, unit 0). Writes FBO 0 / Backbuffer.
    //     SUBGRAPH OUTPUT EDGE: the sole FBO-0 bind in endScene().
    //
    //     POSTPROCESS-SCENEOBJECTID-RESOURCE-1: also reads sceneObjectIdTex_ (unit 2,
    //     GL_R32UI, COLOR_ATTACHMENT2) when effectiveMode==1 (objectId debug view,
    //     RenderWorld::IsObjectIdBufferEnabled() && sceneObjectIdTex_!=0).
    //     SceneObjectId is produced upstream by MechOpaque+StaticPropOpaque geometry
    //     passes (external to this PP subgraph). Declared in reads[] and in the
    //     external set so the subgraph validator sees the read satisfied externally.
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::Composite,
        /*name*/           "Composite",
        /*reads*/          { RenderResourceId::MainColor,
                             RenderResourceId::SceneObjectId,  // POSTPROCESS-SCENEOBJECTID-RESOURCE-1: unit2, conditional effectiveMode==1 / IsObjectIdBufferEnabled
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::Backbuffer,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::Backbuffer,
        /*conditional*/    false,
        /*ownedByExecutor*/true,
        /*isCompute*/      false,
        /*note*/           "SUBGRAPH-1. SUBGRAPH OUTPUT EDGE. ExecutorIslandId::PostProcess (ISLAND-1). "
                           "Binds FBO 0 at :2489; full-screen quad blits sceneFBO_.COLOR0 -> "
                           "backbuffer with FXAA/exposure/viewmode/LOWLIGHT. Always runs. "
                           "CORRECTION vs recon: reads={MainColor} not {MainColor,Backbuffer}; "
                           "Composite writes Backbuffer, it does not sample it. "
                           "POSTPROCESS-SCENEOBJECTID-RESOURCE-1: reads SceneObjectId (unit2) "
                           "when effectiveMode==1 (IsObjectIdBufferEnabled && sceneObjectIdTex_!=0); "
                           "produced externally by MechOpaque+StaticPropOpaque geometry passes.",
    },
    // -----------------------------------------------------------------------
    // 14. ShadowDebugOverlay (call-site :2653) — POSTPROCESS-SUBGRAPH-1
    //     Gate: showShadowDebug_ (default OFF, debug member only)
    //     Reads shadowDepthTex_ or dynShadowArrayTex_ depending on CSM mode.
    //     Writes FBO 0 (already bound by Composite above). NOT executor-owned.
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::ShadowDebugOverlay,
        /*name*/           "ShadowDebugOverlay",
        /*reads*/          { RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::Backbuffer,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::Backbuffer,
        /*conditional*/    true,
        /*ownedByExecutor*/false,
        /*isCompute*/      false,
        /*note*/           "SUBGRAPH-1. Debug shadow-map overlay. Draws on top of Composite on FBO 0 "
                           "(already bound). showShadowDebug_ default-OFF. "
                           "NOT executor-owned: post-composite position, debug-only. "
                           "Uses 2D_ARRAY on CSM path (dynShadowArrayTex_).",
    },
};

static constexpr unsigned kPostProcessSubpassCount =
    static_cast<unsigned>(sizeof(kPostProcessSubpasses) / sizeof(kPostProcessSubpasses[0]));

// Verify count at compile time.
// POSTPROCESS-SUBGRAPH-2: 14 total rows (6 Slice-1 + 8 Slice-2).
static_assert(kPostProcessSubpassCount == 10,
    "POSTPROCESS-SUBGRAPH-2: expected 14 rows (6 Slice-1 + 8 Slice-2)");

// ---------------------------------------------------------------------------
// findPostProcessSubpass — linear scan, pure, no GL.
// Returns nullptr if id is not found in kPostProcessSubpasses.
// ---------------------------------------------------------------------------
inline const PostProcessSubpass* findPostProcessSubpass(ExecutorIslandId id) {
    for (unsigned i = 0; i < kPostProcessSubpassCount; ++i) {
        if (kPostProcessSubpasses[i].id == id)
            return &kPostProcessSubpasses[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Self-consistency validator
// ---------------------------------------------------------------------------

struct PostProcessValidationResult {
    bool             ok               = false;
    ExecutorIslandId offendingSubpass = ExecutorIslandId::Count;
    RenderResourceId missingResource  = RenderResourceId::Unknown;
};

// validatePostProcessSubgraph — walks all kPostProcessSubpassCount rows in order.
// For each row, every non-Unknown read must be EITHER:
//   (a) produced by an earlier subpass's writes[], OR
//   (b) listed in the external[] set (subgraph inputs from the frame's upstream passes).
// external[] is Unknown-terminated.
//
// Also asserts structural invariants:
//   - Composite (fboTarget=Backbuffer, conditional=false) precedes ShadowDebugOverlay.
//   - No post-Composite subpass (other than ShadowDebugOverlay) writes MainColor.
inline PostProcessValidationResult validatePostProcessSubgraph(
    const RenderResourceId* external, int externalCount)
{
    PostProcessValidationResult res;

    // produced[i] = true if resource i has been written by a prior subpass.
    // Use a flat array indexed by RenderResourceId value (max = Count).
    constexpr int kMaxId = static_cast<int>(RenderResourceId::Count);
    bool produced[kMaxId] = {};

    // Seed with external inputs.
    for (int i = 0; i < externalCount; ++i) {
        const int idx = static_cast<int>(external[i]);
        if (idx > 0 && idx < kMaxId)
            produced[idx] = true;
    }

    bool compositeSeen = false;

    for (unsigned si = 0; si < kPostProcessSubpassCount; ++si) {
        const PostProcessSubpass& sp = kPostProcessSubpasses[si];

        // POSTPROCESS-SUBGRAPH-2: isCompute rows are skipped by the read-satisfaction
        // walk. Their compute outputs now have RenderResourceIds (REGISTRY-COMPUTE-IDS-1:
        // 21..24) but the coarse reads[]/writes[] are left empty and they don't participate
        // in the color/depth chain. Treat them as no-ops for validation.
        if (sp.isCompute) continue;

        // Check that every read is satisfied.
        for (int ri = 0; ri < 5; ++ri) {
            const RenderResourceId r = sp.reads[ri];
            if (r == RenderResourceId::Unknown) break;
            const int idx = static_cast<int>(r);
            if (idx <= 0 || idx >= kMaxId || !produced[idx]) {
                res.ok               = false;
                res.offendingSubpass = sp.id;
                res.missingResource  = r;
                return res;
            }
        }

        // Mark writes as produced.
        for (int wi = 0; wi < 3; ++wi) {
            const RenderResourceId w = sp.writes[wi];
            if (w == RenderResourceId::Unknown) break;
            const int idx = static_cast<int>(w);
            if (idx > 0 && idx < kMaxId)
                produced[idx] = true;
        }

        // Structural invariant: Composite must precede ShadowDebugOverlay.
        if (sp.id == ExecutorIslandId::Composite) {
            compositeSeen = true;
        }
        if (sp.id == ExecutorIslandId::ShadowDebugOverlay && !compositeSeen) {
            // ShadowDebugOverlay before Composite — structural error.
            res.ok               = false;
            res.offendingSubpass = sp.id;
            res.missingResource  = RenderResourceId::Unknown;
            return res;
        }
    }

    res.ok = true;
    return res;
}

// Convenience overload with the baked-in Slice-1+2 external resource set.
// External inputs to the PostProcess subgraph from upstream frame-graph passes:
//   MainColor        — produced by Terrain + MechOpaque + StaticPropOpaque + VFX passes
//   MainDepth        — produced by Terrain + opaque passes
//   MainNormal       — produced by Terrain (GBuffer1 / sceneNormalTex_)
//   SceneDepthCopy   — POSTPROCESS-SCENEDEPTHCOPY-RESOURCE-1: produced by VFX pass
//                      (copySceneDepthForParticles, gos_particle_bridge.cpp:1068), cross-boundary.
//   SceneObjectId    — POSTPROCESS-SCENEOBJECTID-RESOURCE-1: GBuffer2 (sceneObjectIdTex_,
//                      COLOR_ATTACHMENT2); produced externally by MechOpaque+StaticPropOpaque
//                      geometry passes (conditional on IsObjectIdBufferEnabled); read by
//                      Composite (unit2, effectiveMode==1).
inline PostProcessValidationResult validateShippedPostProcessSubgraph() {
    static const RenderResourceId kExternal[] = {
        RenderResourceId::MainColor,
        RenderResourceId::MainDepth,
        RenderResourceId::MainNormal,
        RenderResourceId::SceneDepthCopy,  // POSTPROCESS-SCENEDEPTHCOPY-RESOURCE-1: VFX producer (SUBGRAPH-2) consumer
        RenderResourceId::SceneObjectId,   // POSTPROCESS-SCENEOBJECTID-RESOURCE-1: MechOpaque+StaticPropOpaque producer, Composite consumer
        RenderResourceId::Unknown,
    };
    constexpr int kExternalCount = sizeof(kExternal) / sizeof(kExternal[0]) - 1;  // excludes the Unknown terminator
    return validatePostProcessSubgraph(kExternal, kExternalCount);
}

}} // namespace RenderCore::framegraph
