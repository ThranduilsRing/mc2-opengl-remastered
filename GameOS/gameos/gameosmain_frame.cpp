// main: the per-frame helpers -- draw-packet emit, the screenshot oracle, the load-race probes, the camera park and the swap.
// The orchestrator is in gameosmain.cpp.
#include "gameosmain_internal.h"
#include "mc2fmt.h"
#include "sim_substeps.h"
#include "gos_host_vk.h"


// Throttle while the window is invisible, then run the game logic and the event pumps.
void frameThrottleLogicEvents() {
        // Throttle when the window is actually invisible (minimized / hidden),
        // NOT merely when it lacks keyboard focus. Plain focus loss happens
        // constantly during normal play (clicking a chat window, opening
        // Tracy, Windows shifting focus to a notification) and the loop must
        // keep running at full speed in those cases. g_focus_lost is still
        // used as the *input filter* below — different concern, same SDL event
        // source, but they want different predicates.
        bool windowInvisible = false;
        if (g_sdl_window) {
            Uint32 flags = SDL_GetWindowFlags(g_sdl_window);
            windowInvisible = (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
        }
        if (windowInvisible) {
            ZoneScopedN("Frame.BackgroundThrottle");
            timing::sleep(10 * 1000000);
        }

#ifdef MC2_IMGUI
        GuiRuntime::NewFrame();
#endif

        {
            ZoneScopedN("GameLogic");
            mc2_hitch::HitchScope _hitchLogic(mc2_hitch::HitchSpanKind::PhaseGameLogic);
            if(gos_RenderGetEnableDebugDrawCalls()) {
                gos_RenderUpdateDebugInput();
            } else {
                // MC2_SIM_SPEED under the harness clock: N fixed steps per drawn frame. The frame counter advances
                // between steps so every per-frame-keyed sim path (the brain tick accumulator among them) sees one
                // logic pass per counter value, exactly as at 1x.
                bool focused = false;
                if (g_sdl_window)
                    focused = (SDL_GetWindowFlags(g_sdl_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
                const int steps = mc2_sim_substeps::stepsPerFrame(mc2::gate::get<"MC2_SIM_SPEED">(),
                    SmokeMode::fixedTimestepEnabled(), SmokeMode::missionHasStarted(), focused);
                static int s_loggedSteps = 1;
                if (steps != s_loggedSteps) {
                    s_loggedSteps = steps;
                    mc2::fmt::print_out("[SIM_SPEED v1] steps_per_frame={:d} frame={:d}\n",
                        mc2::fmt::as_i32(steps), mc2::fmt::as_u32(g_mc2FrameCounter));
                }
                for (int i = 0; i < steps; ++i) {
                    if (i > 0)
                        ++g_mc2FrameCounter;
                    Environment.DoGameLogic();
                }
            }
        }

        {
            ZoneScopedN("Frame.ProcessEvents");
            process_events();
        }

        {
            ZoneScopedN("Frame.RendererHandleEvents");
		    gos_RendererHandleEvents();
        }
}

// Emit the draw packets and the debug / parity records for this frame snapshot.
void frameEmitDrawPackets(RenderSnapshot& snap) {
        {
            ZoneScopedN("EmitDrawPackets");

            // Size the candidate buffer to the exact total packet count.
            // batcher_getSortedPacketCount() returns s_sortedPacketOrder.size() — a
            // permutation over global s_packets[], same total count. Falls back to
            // 4096 before finalizeGeometry runs (returns 0 at that point).
            const uint32_t totalPackets = batcher_getSortedPacketCount();
            const uint32_t kMaxPackets  = totalPackets > 0u ? totalPackets : 4096u;

            // Module-static candidate buffer, resized on demand.
            // v0: population only — candidates are emitted but not dispatched.
            static std::vector<StaticPropDrawPacketCandidate> s_candidates;
            if (s_candidates.size() < static_cast<size_t>(kMaxPackets))
                s_candidates.resize(kMaxPackets);

            // Static storage reused every frame; contents are frame-local and invalid
            // after the current frame path. Sized to kMaxPackets on first resize, same
            // as s_candidates — overflow is structurally impossible.
            // sorted_packet_cap observed in tier1 smoke: mc2_01=134, mc2_03=359,
            // mc2_10=417, mc2_17=312, mc2_24=753 (all well under 4096 fallback).
            static std::vector<RenderCore::DrawPacket> s_drawPackets;
            if (s_drawPackets.size() < static_cast<size_t>(kMaxPackets))
                s_drawPackets.resize(kMaxPackets);

            const DrawPacketEmitStats stats =
                emitStaticPropDrawPackets(snap, s_candidates.data(), kMaxPackets);

            // DrawPacket v2: per-frame field compare. Set MC2_DRAW_PACKET_COMPARE=1 to enable.
            static const bool s_dpCompareEnabled = mc2::gate::get<"MC2_DRAW_PACKET_COMPARE">();
            if (s_dpCompareEnabled) {
                comparePacketsToLegacy(s_candidates.data(), stats.emitted,
                    static_cast<uint32_t>(snap.frameIndex));
            }

            // DrawPacket v3: candidate → RenderCore::DrawPacket[] ABI promotion.
            // No dispatch; legacy flush() unchanged. Set MC2_DRAW_PACKET_V3=1 to enable.
            static const bool s_v3Enabled = mc2::gate::get<"MC2_DRAW_PACKET_V3">();
            static bool s_finalizeWarnedOnce = false;

            if (s_v3Enabled) {
                // Warn once if finalizeGeometry hasn't run yet. Once-only to avoid per-frame log spam.
                if (totalPackets == 0 && !s_finalizeWarnedOnce) {
                    s_finalizeWarnedOnce = true;
                    mc2::fmt::print_err("[DRAW_PACKET v3] WARNING (once): totalPackets=0 (finalizeGeometry not yet run, fallback kMaxPackets=4096)\n");
                }

                // candidateCount = stats.emitted, not kMaxPackets: only the v2-emitted slots are
                // initialized; trailing s_candidates entries are stale from prior frames.
                const DrawPacketBuildStats bstats = buildStaticPropDrawPackets(
                    s_candidates.data(), stats.emitted,
                    s_drawPackets.data(), static_cast<uint32_t>(s_drawPackets.size()));
                // maxPackets == s_drawPackets.size() == kMaxPackets >= stats.emitted: overflow impossible

                mc2::fmt::print_err("[DRAW_PACKET v3] frame={:d} input={:d} emitted={:d} invalid_pipeline={:d} pipeline_oor={:d} invalid_index={:d} invalid_instance={:d} overflow={:d} object_sentinel={:d} light_sentinel={:d} sorted_packet_cap={:d}\n",
                    mc2::fmt::as_u32(static_cast<uint32_t>(snap.frameIndex)),
                    mc2::fmt::as_u32(bstats.inputCandidates), mc2::fmt::as_u32(bstats.emitted),
                    mc2::fmt::as_u32(bstats.invalidPipeline), mc2::fmt::as_u32(bstats.pipelineOutOfRange),
                    mc2::fmt::as_u32(bstats.invalidIndexCount), mc2::fmt::as_u32(bstats.invalidInstanceCount),
                    mc2::fmt::as_u32(bstats.overflow),
                    mc2::fmt::as_u32(bstats.objectIndexSentinelCount), mc2::fmt::as_u32(bstats.lightIndexSentinelCount),
                    mc2::fmt::as_u32(totalPackets));
            }

            // Hard gate — REAL anomalies only, AND default-SILENT (set MC2_DRAW_PACKET_WARN=1
            // to re-enable). The old gate also tripped on `oldExpected != expectedPackets`,
            // but oldExpected is a legacy comparison field that is ALWAYS 0 while
            // expectedPackets grows (68..71+), so it fired the warning EVERY frame on a
            // totally benign mismatch -> console spam. Dropped that condition; the remaining
            // overflow/invalidRanges/materialMismatches are genuine but are now also gated
            // off by default so a shipping/player build is silent.
            static const bool s_drawPacketWarn = mc2::gate::get<"MC2_DRAW_PACKET_WARN">();
            if (s_drawPacketWarn
                    && (stats.overflow
                        || stats.invalidRanges > 0
                        || stats.materialMismatches > 0)) {
                mc2::fmt::print_err("[DRAW_PACKET v1] WARNING: overflow={:d} invalidRanges={:d} skippedRanges={:d} old_expected={:d} new_expected={:d} materialMismatches={:d}\n", mc2::fmt::as_i32(stats.overflow ? 1 : 0), mc2::fmt::as_u32(stats.invalidRanges), mc2::fmt::as_u32(stats.skippedRanges), mc2::fmt::as_u32(stats.oldExpected), mc2::fmt::as_u32(stats.expectedPackets), mc2::fmt::as_u32(stats.materialMismatches));
            }

            // Full log line under MC2_RENDER_SNAPSHOT_LOG=1.
            static const bool s_logEnabled = mc2::gate::get<"MC2_RENDER_SNAPSHOT_LOG">();
            if (s_logEnabled) {
                mc2::fmt::print_err("[DRAW_PACKET v1] frame={:d} emitted={:d} expected={:d} old_expected={:d} distinct_types={:d} static_props={:d} invalid={:d} skipped={:d} mat_mismatch={:d} overflow={:d}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(snap.frameIndex)), mc2::fmt::as_u32(stats.emitted), mc2::fmt::as_u32(stats.expectedPackets), mc2::fmt::as_u32(stats.oldExpected), mc2::fmt::as_u32(stats.distinctTypes), mc2::fmt::as_u32(static_cast<uint32_t>(snap.staticProps.size())), mc2::fmt::as_u32(stats.invalidRanges), mc2::fmt::as_u32(stats.skippedRanges), mc2::fmt::as_u32(stats.materialMismatches), mc2::fmt::as_i32(stats.overflow ? 1 : 0));
            }

            // Feed ImGui DrawPackets panel (read in GraphicsOptionsWindow::draw).
            g_dpSnapshot.frame              = snap.frameIndex;
            g_dpSnapshot.emitted            = stats.emitted;
            g_dpSnapshot.expected           = stats.expectedPackets;
            g_dpSnapshot.distinctTypes      = stats.distinctTypes;
            g_dpSnapshot.invalidRanges      = stats.invalidRanges;
            g_dpSnapshot.skippedRanges      = stats.skippedRanges;
            g_dpSnapshot.materialMismatches = stats.materialMismatches;
            g_dpSnapshot.overflow           = stats.overflow;
            g_dpSnapshot.typeDescCount      = batcher_getStaticPropTypeDescCount();

            // Selected-prop packet inspector (DrawPackets panel "Selected Prop" section).
            // g_dpSelectedRecipeIndex is written by EditorInspector on Ctrl+Shift+Click;
            // -1 = no selection. Render thread only — no mutex needed.
            {
                DrawPacketSelectedPropSnapshot sel{};
                const int32_t selRecipe = g_dpSelectedRecipeIndex;
                if (selRecipe >= 0) {
                    // Phase 1: find the first snapshot entry with matching recipeIndex.
                    const ExtractedStaticProp* found = nullptr;
                    for (const auto& sp : snap.staticProps) {
                        if (sp.recipeIndex == selRecipe) { found = &sp; break; }
                    }
                    if (found) {
                        const uint32_t selTypeId = found->typeId;

                        // Phase 2: count visible instances of the same typeId.
                        uint32_t instCount = 0u;
                        for (const auto& sp : snap.staticProps)
                            if (sp.typeId == selTypeId) ++instCount;

                        sel.valid         = true;
                        sel.recipeIndex   = selRecipe;
                        sel.typeId        = selTypeId;
                        sel.firstPacket   = found->firstPacket;
                        sel.packetCount   = found->packetCount;
                        sel.instanceCount = instCount;
                        sel.materialIdx   = found->materialIdx;
                        sel.alphaClass    = found->alphaClass;
                        static_assert(sizeof(sel.shapeName) == sizeof(found->shapeName),
                            "shapeName size mismatch");
                        std::memcpy(sel.shapeName, found->shapeName, sizeof(sel.shapeName));
                        sel.shapeName[sizeof(sel.shapeName) - 1] = '\0';

                        // MaterialGpu lookup — per-type (all packets share same materialIdx).
                        RenderCore::MaterialGpu mg{};
                        const bool mgOk = (found->materialIdx != 0xFFFFFFFFu)
                                          && batcher_getMaterialGpuEntry(found->materialIdx, &mg);
                        const uint32_t mgAlbedoTex = mgOk ? mg.albedoTex : 0xFFFFFFFFu;

                        // Per-packet rows.
                        const uint32_t rowCap = DrawPacketSelectedPropSnapshot::kMaxRows;
                        const uint32_t pktEnd = found->firstPacket + found->packetCount;
                        for (uint32_t pi = found->firstPacket;
                                      pi < pktEnd && sel.rowCount < rowCap; ++pi) {
                            DrawPacketPropRow& row = sel.rows[sel.rowCount];
                            row.globalPacketIdx = pi;
                            uint32_t idxCount = 0u, firstIdx = 0u, owningType = 0u;
                            int32_t  baseVtx = 0;
                            if (batcher_getPacketDrawInfo(pi, &idxCount, &firstIdx,
                                                          &baseVtx, &owningType)) {
                                row.indexCount = idxCount;
                                row.firstIndex = firstIdx;
                                row.baseVertex = baseVtx;
                            }
                            uint32_t matFlags = 0u;
                            if (batcher_getPacketMaterialFlags(pi, &matFlags)) {
                                row.materialFlags = matFlags;
                                row.pipelineId    = (matFlags & STATIC_PROP_FLAG_ALPHA_TEST)
                                                    ? 2u : 1u;
                            }
                            // Render-spine v1.1: legacy layer + MaterialGpu albedo + match flag.
                            row.texArrayLayer         = found->texArrayLayer;
                            row.albedoTex             = mgAlbedoTex;
                            row.materialMatchesLegacy = mgOk
                                && (found->texArrayLayer >= 0)
                                && (mgAlbedoTex == static_cast<uint32_t>(found->texArrayLayer));
                            ++sel.rowCount;
                        }
                    }
                }
                g_dpSelProp = sel;
            }

            // TERRAIN-PASS-PACKET-0: promote pass-level terrain facts from
            // live accessors into the RenderSnapshot. Pass-level only — no
            // per-tile identity, no new counters introduced. The inspector
            // (below) becomes a downstream consumer of snap.terrainPass.
            // Read-only — no GL state touched, no mutation of any render path.
            //
            // Free-function accessors defined in gameos_graphics.cpp; full
            // gosRenderer type is private to that TU.
            extern uint32_t gos_getTerrainSurfaceProgramId();
            extern uint32_t gos_getThinTerrainProgramId();
            extern uint32_t gos_getWaterFastProgramId();
            extern uint32_t gos_getTerrainOverlayProgramId();
            {
                const RenderCore::EngineView& view = RenderCore::getCurrentView();
                snap.terrainPass.viewId          = view.id;
                snap.terrainPass.legacyProgramId = gos_getTerrainSurfaceProgramId();
                snap.terrainPass.drawCallCount   = TerrainPatchStream::getLastFlushBucketCount();
                uint32_t flags = 0u;
                // tessellationOn: hard-coded true (matches inspector field semantics).
                flags |= TerrainPassFacts::kFlagTessellationOn;
                // viewUniformsBound: terrain shaders don't consume ViewUniforms (binding=3) yet.
                // (Leave kFlagViewUniformsBound clear.)
                if (TerrainPatchStream::wasLastFlushOverflowed())
                    flags |= TerrainPassFacts::kFlagOverflow;
                snap.terrainPass.flags = flags;
            }

            #ifdef MC2_IMGUI
            // TERRAIN-SPINE-0: inspector filler. Now a downstream consumer of
            // snap.terrainPass for the pass-level fields promoted in
            // TERRAIN-PASS-PACKET-0. Per-program ids + per-flush detail fields
            // that were not promoted to the snapshot continue to read directly
            // from live accessors (no behavior change for those).
            {
                EditorInspector::TerrainPassSnapshot ts;
                ts.surfaceProgramId   = snap.terrainPass.legacyProgramId;
                ts.thinProgramId      = gos_getThinTerrainProgramId();
                ts.waterFastProgramId = gos_getWaterFastProgramId();
                ts.overlayProgramId   = gos_getTerrainOverlayProgramId();
                ts.bucketCount  = snap.terrainPass.drawCallCount;
                ts.vertCount    = TerrainPatchStream::getLastFlushVertCount();
                ts.thinRecCount = TerrainPatchStream::getLastFlushThinRecCount();
                ts.recipeCount  = TerrainPatchStream::getLastFlushRecipeCount();
                ts.overflow     = (snap.terrainPass.flags & TerrainPassFacts::kFlagOverflow) != 0u;
                ts.viewUniformsBoundForTerrain =
                    (snap.terrainPass.flags & TerrainPassFacts::kFlagViewUniformsBound) != 0u;
                const RenderCore::EngineView& view = RenderCore::getCurrentView();
                ts.currentViewId   = snap.terrainPass.viewId;
                ts.currentViewName = view.debugName ? view.debugName : "";
                ts.tessellationOn  =
                    (snap.terrainPass.flags & TerrainPassFacts::kFlagTessellationOn) != 0u;
                EditorInspector::setTerrainPassSnapshot(ts);
            }

            // SHADOW-SPINE-0: pass-level snapshot of the shadow render spine.
            // Mirrors the terrain block above. Read-only — no GL state touched.
            {
                // C++-linkage helpers defined in gameos_graphics.cpp. C-linkage
                // accessors declared at file scope (top of TU).
                extern uint32_t gos_getTerrainShadowProgramId();
                extern uint32_t gos_getStaticPropShadowProgramId();
                extern bool     gos_getShadowsEnabled();
                extern bool     gos_StaticLightMatrixBuilt();
                extern int      gos_getShadowMapSize();
                extern int      gos_getDynShadowMapSize();
                EditorInspector::ShadowPassSnapshot sp;
                sp.terrainShadowProgramId      = gos_getTerrainShadowProgramId();
                sp.staticPropShadowProgramId   = gos_getStaticPropShadowProgramId();
                sp.shadowsEnabled              = gos_getShadowsEnabled();
                sp.staticLightMatrixBuilt      = gos_StaticLightMatrixBuilt();
                sp.shadowMapSize               = gos_getShadowMapSize();
                sp.dynShadowMapSize            = gos_getDynShadowMapSize();
                sp.mechShadowTypesDrawn        = gos_getMechShadowTypesDrawn();
                sp.mechShadowInstDrawn         = gos_getMechShadowInstDrawn();
                // v1: shadow shaders do not consume ViewUniforms (binding=3).
                sp.viewUniformsBoundForShadow  = false;
                EditorInspector::setShadowPassSnapshot(sp);
            }

            // VFX-SPINE-0: pass-level snapshot of the GPU particle / VFX
            // render spine. Mirrors the shadow block above. Read-only — no
            // GL state touched, no VFX mutation, no object-IDs.
            {
                EditorInspector::VfxPassSnapshot vs;
                vs.particleProgramId          = (uint32_t)gos_vfx_getParticleProgramId();
                vs.gpuParticlesEnabled        = (mc2_vfx_isEnabled() != 0);
                vs.gpuParticlesLogEnabled     = (mc2_vfx_isLogEnabled() != 0);
                vs.initFailed                 = (gos_vfx_getInitFailed() != 0);
                vs.cameraSetThisFrame         = (gos_vfx_getCameraSetThisFrame() != 0);
                vs.perFrameBudget             = (uint32_t)mc2_vfx_getBudget();
                vs.ssboCapacityRecords        = (uint32_t)gos_vfx_getSsboCapacity();
                vs.overflowReported           = (mc2_vfx_getOverflowReported() != 0);
                vs.emitTotal                  = mc2_vfx_getEmitTotal();
                vs.flushTotal                 = mc2_vfx_getFlushTotal();
                vs.nonemptyFlushTotal         = mc2_vfx_getNonemptyFlushTotal();
                vs.recordsFlushedTotal        = mc2_vfx_getRecordsFlushedTotal();
                vs.recordsPerFlushMax         = (uint32_t)mc2_vfx_getRecordsPerFlushMax();
                vs.trailSpawnTotal            = mc2_vfx_getTrailSpawnTotal();
                vs.trailHeadTotal             = mc2_vfx_getTrailHeadTotal();
                // v1: VFX shaders do not consume ViewUniforms (binding=3).
                vs.viewUniformsBoundForVfx    = false;
                vs.debugMode                  = gos_vfx_getDebugMode();
                EditorInspector::setVfxPassSnapshot(vs);
            }
            #endif // MC2_IMGUI

            // [FRAME_PASS_STATS v1] frame-level aggregates + per-pass draw/
            // instance counts, pulled from already-computed counters (no
            // hot-loop counting). OFF=zero cost (every Set* early-returns).
            // Counters reflect the prior frame's flush (this seam runs before
            // draw_screen); advisory, not gated.
            if (gos_frame_pass_stats::Enabled()) {
                extern uint64_t batcher_getLastFlushSubmitCount(); // gos_mech_batcher
                const uint32_t mechInst = (uint32_t)batcher_getLastFlushSubmitCount();
                const uint32_t spSlots  = batcher_getDrawSlotCount();

                gos_frame_pass_stats::FrameAggregates agg;
                agg.visibleTerrainChunks = 0u; // set by terrain flush producer
                agg.staticPropBatches    = spSlots;
                agg.mechBatchInstances   = mechInst;
                agg.vfxCount             = 0u; // per-frame VFX count deferred (VFX-SPINE-0)
                gos_frame_pass_stats::SetFrameAggregates(agg);

                gos_frame_pass_stats::SetPassCounts(
                    gos_render_pass_timer::Pass_Mechs, mechInst, mechInst);
                gos_frame_pass_stats::SetPassCounts(
                    gos_render_pass_timer::Pass_SpColor, spSlots, spSlots);
                gos_frame_pass_stats::SetPassCounts(
                    gos_render_pass_timer::Pass_ShadowDyn,
                    gos_getMechShadowInstDrawn(),
                    gos_getMechShadowInstDrawn());
            }

        }
}

// Capture the deterministic backbuffer screenshot at the requested frame.
void frameScreenshotOracle() {
        {
            // Oracle: deterministic backbuffer screenshot at a target frame.
            // MC2_SCREENSHOT_AT_FRAME=N + MC2_SCREENSHOT_PATH=out.tga. Captures
            // the finished backbuffer just before present (after draw_screen +
            // EndFrame), once, when the frame counter first reaches N. Default
            // OFF, zero cost when env unset. Lets the dynamic-pipeline oracle
            // produce a real pixel image headlessly (TGA) for visual regression.
            static const std::string s_ssPathText = mc2::gate::str<"MC2_SCREENSHOT_PATH">();   // "" when unset or empty
            static const char* s_ssPath  = s_ssPathText.empty() ? nullptr : s_ssPathText.c_str();
            static const long s_ssFrame = static_cast<long>(mc2::gate::get<"MC2_SCREENSHOT_AT_FRAME">());
            static bool s_ssDone = false;
            if (s_ssPath && s_ssFrame >= 0 && !s_ssDone
                && (long)g_mc2FrameCounter >= s_ssFrame) {
                // The finished frame is read back by the Vulkan present path; the request completes on that frame.
                gos::screenshot::request(s_ssPath);
                s_ssDone = true;   // one attempt: a rejected request logs why and is not retried every frame
            }
            // DEV-SHELL-1: pending shell screenshot.
            gos_dev_shell::capturePendingScreenshot(g_mc2FrameCounter);
        }
}

// Emit the one-shot load-race verdict at its frame.
void frameLoadRaceVerdict() {
        {
            // MC2_LOAD_RACE_TRACE: one machine-parseable per-run verdict, emitted
            // ONCE when the frame counter first reaches MC2_LOAD_RACE_VERDICT_FRAME
            // (falls back to MC2_SCREENSHOT_AT_FRAME so the verdict + backbuffer
            // screenshot are co-located at the same frame). The wave classifier
            // greps this single line. Default-OFF, zero cost when the gate unset.
            extern bool g_loadRaceTrace;
            if (g_loadRaceTrace) {
                extern long  g_loadRaceApprNull;
                extern long  g_loadRaceMechDegrade;
                extern long  g_loadRaceMechDegradeReal;
                extern long  g_loadRaceMechOkReal;
                extern long  g_loadRaceMechOkPreview;
                extern long  g_loadRaceTerrainFirstResidentFrame;
                extern bool  g_loadRaceTerrainResidentNow;
                extern float scenarioTime;
                static const long s_vFrame = static_cast<long>(
                    mc2::gate::isSet<"MC2_LOAD_RACE_VERDICT_FRAME">() ? mc2::gate::get<"MC2_LOAD_RACE_VERDICT_FRAME">()
                                                                      : mc2::gate::get<"MC2_SCREENSHOT_AT_FRAME">());
                static bool s_vDone = false;
                if (s_vFrame >= 0 && !s_vDone && (long)g_mc2FrameCounter >= s_vFrame) {
                    s_vDone = true;
                    // A run is "degraded" if a REAL world actor lost its shape
                    // (mech_degrade_real>0) or terrain never became resident.
                    // Preview-only degrades (obj==NULL) are benign and reported
                    // separately.
                    const bool degraded = (g_loadRaceMechDegradeReal > 0) ||
                                          !g_loadRaceTerrainResidentNow;
                    const char* fmt =
                        "LOADRACE verdict: frame=%u scenarioTime=%.2f "
                        "mech_degrade=%ld mech_degrade_real=%ld "
                        "mech_ok_real=%ld mech_ok_preview=%ld appr_null=%ld "
                        "terrain_resident=%s terrain_first_resident_frame=%ld "
                        "degraded=%s\n";
                    fprintf(stderr, fmt,
                        g_mc2FrameCounter, scenarioTime,
                        g_loadRaceMechDegrade, g_loadRaceMechDegradeReal,
                        g_loadRaceMechOkReal, g_loadRaceMechOkPreview,
                        g_loadRaceApprNull,
                        g_loadRaceTerrainResidentNow ? "Y" : "N",
                        g_loadRaceTerrainFirstResidentFrame,
                        degraded ? "YES" : "no");
                    fflush(stderr);
                    printf(fmt,
                        g_mc2FrameCounter, scenarioTime,
                        g_loadRaceMechDegrade, g_loadRaceMechDegradeReal,
                        g_loadRaceMechOkReal, g_loadRaceMechOkPreview,
                        g_loadRaceApprNull,
                        g_loadRaceTerrainResidentNow ? "Y" : "N",
                        g_loadRaceTerrainFirstResidentFrame,
                        degraded ? "YES" : "no");
                    fflush(stdout);
                }
            }
        }
}

// Dump the load-race transient probes at their frames.
void frameLoadRaceProbes() {
        {
            // MC2_LOAD_RACE_TRACE (S2b corrected transient probes): at a fixed
            // set of verdict frames, dump the terrain-path / atlas / arm state
            // plus fixed-texel scene readbacks and a direct atlas-texel sample.
            // This separates the three rival mechanisms for the "cool wash ->
            // warms over ~20-40s" transient: (1) late indirect arming, (2) atlas
            // content absent/wrong, (3) lighting/shading warm-up (atlas+arm early
            // but shaded pixels start cool). One-shot per listed frame, GL context
            // live (post-render, pre-swap). Default-OFF; zero cost when gate unset.
            extern bool g_loadRaceTrace;
            if (g_loadRaceTrace) {
                extern long  g_loadRaceAtlasFirstFrame;
                extern long  g_loadRaceArmFirstFrame;
                extern long  g_loadRaceLastTerrainPath;
                extern long  g_loadRaceTerrainFirstResidentFrame;
                extern bool  g_loadRaceTerrainResidentNow;
                extern long  g_loadRaceMechDegradeReal;
                extern float scenarioTime;
                extern GLuint gos_terrain_indirect_getAtlasGLTex();
                // Frames to probe. Overridable via MC2_LOAD_RACE_PROBE_FRAMES
                // (comma list) but defaults to the confirmed transient window.
                static const std::vector<long> s_probeFrames = []{
                    std::vector<long> v;
                    if (const std::string probeList = mc2::gate::str<"MC2_LOAD_RACE_PROBE_FRAMES">(); !probeList.empty()) {
                        const char* p = probeList.c_str();
                        while (*p) { v.push_back(atol(p)); const char* c = strchr(p, ','); if (!c) break; p = c + 1; }
                    }
                    if (v.empty()) v = {600, 1300, 2600, 4000, 6000};
                    return v;
                }();
                static size_t s_nextProbe = 0;
                if (s_nextProbe < s_probeFrames.size() &&
                    (long)g_mc2FrameCounter >= s_probeFrames[s_nextProbe]) {
                    const long pf = s_probeFrames[s_nextProbe];
                    ++s_nextProbe;

                    const GLuint atlasTex = gos_terrain_indirect_getAtlasGLTex();
                    const char* pathStr = (g_loadRaceLastTerrainPath == 1) ? "gpu-indirect"
                                        : (g_loadRaceLastTerrainPath == 2) ? "cpu-thin" : "none";

                    // (b) Fixed-texel scene oracle: read 3 deterministic terrain
                    // points from the offscreen scene FBO (robust to a minimized
                    // window). Points are normalized so they track resolution.
                    gosPostProcess* ppLr = getGosPostProcess();
                    GLuint lrFbo = ppLr ? ppLr->getSceneFBO() : 0;
                    int lrW = ppLr ? ppLr->getWidth()  : 0;
                    int lrH = ppLr ? ppLr->getHeight() : 0;
                    unsigned char px[3][4] = {{0,0,0,0},{0,0,0,0},{0,0,0,0}};
                    const float ptsN[3][2] = {{0.5f,0.62f},{0.30f,0.72f},{0.70f,0.55f}};
                    if (lrFbo && lrW > 0 && lrH > 0) {
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, lrFbo);
                        glReadBuffer(GL_COLOR_ATTACHMENT0);
                        for (int i = 0; i < 3; ++i) {
                            int sx = (int)(ptsN[i][0] * lrW);
                            int sy = (int)(ptsN[i][1] * lrH);
                            glReadPixels(sx, sy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[i]);
                        }
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    }

                    // (b) Direct atlas-texel sample: attach the atlas to a scratch
                    // FBO and read one texel. Skips (reports unreadable) when the
                    // atlas is a compressed (BC7) format -- not color-renderable.
                    unsigned char atlasTexel[4] = {0,0,0,0};
                    const char* atlasTexelStatus = "absent";
                    if (atlasTex != 0) {
                        GLint prevFbo = 0;
                        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevFbo);
                        GLuint scratch = 0;
                        glGenFramebuffers(1, &scratch);
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, scratch);
                        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_TEXTURE_2D, atlasTex, 0);
                        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                            glReadBuffer(GL_COLOR_ATTACHMENT0);
                            // Read a mid-atlas texel (content-bearing, not an edge).
                            GLint aw = 0, ah = 0;
                            glBindTexture(GL_TEXTURE_2D, atlasTex);
                            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &aw);
                            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &ah);
                            int ax = aw > 2 ? aw / 2 : 0;
                            int ay = ah > 2 ? ah / 2 : 0;
                            glReadPixels(ax, ay, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, atlasTexel);
                            atlasTexelStatus = "ok";
                        } else {
                            atlasTexelStatus = "unreadable_compressed";
                        }
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevFbo);
                        glDeleteFramebuffers(1, &scratch);
                    }

                    const char* fmt =
                        "LOADRACE probe: frame=%u probeFrame=%ld scenarioTime=%.2f "
                        "terrain_path=%s atlasTex=%u atlas_first_frame=%ld "
                        "arm_first_frame=%ld terrain_first_resident_frame=%ld "
                        "terrain_resident=%s mech_degrade_real=%ld "
                        "scene_px0=%u,%u,%u scene_px1=%u,%u,%u scene_px2=%u,%u,%u "
                        "atlas_texel[%s]=%u,%u,%u,%u\n";
                    fprintf(stderr, fmt,
                        g_mc2FrameCounter, pf, scenarioTime,
                        pathStr, (unsigned)atlasTex, g_loadRaceAtlasFirstFrame,
                        g_loadRaceArmFirstFrame, g_loadRaceTerrainFirstResidentFrame,
                        g_loadRaceTerrainResidentNow ? "Y" : "N", g_loadRaceMechDegradeReal,
                        px[0][0],px[0][1],px[0][2], px[1][0],px[1][1],px[1][2], px[2][0],px[2][1],px[2][2],
                        atlasTexelStatus, atlasTexel[0],atlasTexel[1],atlasTexel[2],atlasTexel[3]);
                    fflush(stderr);
                    printf(fmt,
                        g_mc2FrameCounter, pf, scenarioTime,
                        pathStr, (unsigned)atlasTex, g_loadRaceAtlasFirstFrame,
                        g_loadRaceArmFirstFrame, g_loadRaceTerrainFirstResidentFrame,
                        g_loadRaceTerrainResidentNow ? "Y" : "N", g_loadRaceMechDegradeReal,
                        px[0][0],px[0][1],px[0][2], px[1][0],px[1][1],px[1][2], px[2][0],px[2][1],px[2][2],
                        atlasTexelStatus, atlasTexel[0],atlasTexel[1],atlasTexel[2],atlasTexel[3]);
                    fflush(stdout);
                }
            }
        }
}

// Pin the camera to the MC2_CAMERA_PARK pose once its frame arrives.
void frameCameraPark(graphics::RenderWindowHandle win) {
        {
            // CAMERA-PARK-1 (perf-capture fixture, 2026-07-18): MC2_CAMERA_PARK=
            // "x,y,rot,proj,alt[@frame]" hard-sets the live camera pose once at
            // frame >= @frame (default 300) and pins it against scripted writers
            // (same freezeScriptedCameraForCapture pin the bookmark replay uses)
            // WITHOUT freezing the sim — for Tracy captures that need a
            // worst-case view (map edge, max altitude, looking across) while
            // gameplay keeps running. Default OFF = one static pointer check.
            // (Implementation lives in gos_visual_capture.cpp on this branch —
            // the mclib Camera type is visible there, not in this TU.)
            gos::visual_capture::cameraParkTick(g_mc2FrameCounter);

            // [VISUAL_CAPTURE v1] S9: deterministic PNG + capture-tuple sidecar,
            // and bookmark teleport/settle/capture replay. Reuses the same
            // offscreen scene FBO source as [SCREENSHOT v1] above (robust to a
            // minimized window). Default-OFF: onPostRenderFrame early-returns on
            // a single int/pointer check with no allocation when neither
            // MC2_VISUAL_CAPTURE_FRAME nor MC2_VISUAL_BOOKMARK_CAPTURE is set,
            // so we resolve the FBO lazily only when one of them is active.
            if (gos::visual_capture::active()) {
                // The shown image is the VK present: capture_to() queues a present readback (the GL scene FBO is not read any more).
                int vkW = 0, vkH = 0;
                graphics::get_drawable_size(win, &vkW, &vkH);
                if (vkW > 0 && vkH > 0)
                    gos::visual_capture::onPostRenderFrame(g_mc2RenderFrame, vkW, vkH);
            }
        }
}

// Present the frame: pre-swap probes, the swap and the heartbeat.
void frameSwapWindow(graphics::RenderWindowHandle win) {
        {
            ZoneScopedN("SwapWindow");
            mc2_hitch::HitchScope _hitchPresent(mc2_hitch::HitchSpanKind::PhasePresent);
            // SwapWindow split: env-gated glFinish probe attributes the present
            // stall. If MC2_PRESWAP_FINISH=1 moves the ~45ms into PreFinish, the
            // cost is GPU-completion backpressure (GPU draining terrain/shadows),
            // not vsync/CPU. SwapWindow.SDL isolates the bare SDL_GL_SwapWindow.
            {
                static const bool s_preSwapFinish = mc2::gate::get<"MC2_PRESWAP_FINISH">();
                if (s_preSwapFinish) { ZoneScopedN("SwapWindow.PreFinish"); glFinish(); }
            }
            {
                ZoneScopedN("SwapWindow.SDL");
                // The shared host present (Vulkan composite + present, or the GL swap): also the editor's.
                gos::host::presentOrSwap(win, g_mc2RenderFrame);
            }
            render_contract::renderPassTelemetryFrameTick();  // [RENDER_PASS v1] frame boundary
            render_contract::renderPassScopeFrameBoundary();   // ENFORCEMENT-1 missing-end flush
            static bool s_first_frame_logged = false;
            if (!s_first_frame_logged) {
                s_first_frame_logged = true;
                startup_phase("first_frame_presented");
            }
            // Smoke mode: per-frame perf sample.
            {
                static uint64_t s_lastFrameT = SDL_GetPerformanceCounter();
                uint64_t now = SDL_GetPerformanceCounter();
                double ms = 1000.0 * (double)(now - s_lastFrameT) /
                            (double)SDL_GetPerformanceFrequency();
                s_lastFrameT = now;
                SmokeMode::samplePerf(ms);
            }
            // Heartbeat: every ~1s emit a frame-count marker so we can tell
            // whether the render loop is alive or frozen. Gated behind
            // MC2_HEARTBEAT so the default log is quiet — invaluable for
            // diagnosing freezes on content-faulting mod loads when enabled.
            static const bool s_hbTrace = mc2::gate::get<"MC2_HEARTBEAT">();
            if (s_hbTrace) {
                static int s_hb_frame = 0;
                static uint64_t s_hb_last_ms = 0;
                s_hb_frame++;
                uint64_t now_ms = (uint64_t)(SDL_GetTicks64());
                if (s_hb_last_ms == 0) s_hb_last_ms = now_ms;
                if (now_ms - s_hb_last_ms >= 1000) {
                    char _cbbuf[192];
                    mc2::fmt::to_buf(_cbbuf, "[HEARTBEAT] frames={:d} elapsed_ms={:d} fps={:.1f}", mc2::fmt::as_i32(s_hb_frame), mc2::fmt::as_u64((unsigned long long)(now_ms - s_hb_last_ms)), (double)s_hb_frame * 1000.0 / (double)(now_ms - s_hb_last_ms));
                    mc2::fmt::print_err("{}\n", mc2::fmt::safe(_cbbuf));
                    crashbundle_append(_cbbuf);
                    fflush(stderr);
                    s_hb_frame = 0;
                    s_hb_last_ms = now_ms;
                }
            }
        }
}

// Sleep to the mission / menu frame cap unless vsync paces the loop.
void frameFrameCap(bool s_vsync_active, int s_fps_cap_mission, int s_fps_cap_menu, uint64_t start_tick) {
        // Frame cap: sleep to hold target frame time and reduce GPU thrashing.
        // Skipped when vsync is active (vsync already owns pacing).
        // Uses mission-state-aware cap: 90 FPS in menus, 165 FPS in missions.
        if (!s_vsync_active) {
            bool inMission = SmokeMode::missionHasStarted();
            int cap = inMission ? s_fps_cap_mission : s_fps_cap_menu;
            static bool s_capTraceLast = !inMission;
            static const bool s_capTrace = mc2::gate::get<"MC2_FRAMECAP_TRACE">();
            if (s_capTrace && s_capTraceLast != inMission) {
                mc2::fmt::print_err("[FRAMECAP] cap switched: inMission={:d} cap={:d}\n", mc2::fmt::as_i32((int)inMission), mc2::fmt::as_i32(cap));
                fflush(stderr);
                s_capTraceLast = inMission;
            }
            if (cap > 0) {
                ZoneScopedN("Frame.FrameCap");
                uint64_t target_ms = 1000u / (uint64_t)cap;
                uint64_t now = timing::gettickcount();
                uint64_t elapsed_ms = timing::ticks2ms(now - start_tick);
                if (elapsed_ms < target_ms) {
                    mc2_hitch::HitchScope _hitchCap(mc2_hitch::HitchSpanKind::PhaseCap);
                    timing::sleep((unsigned int)((target_ms - elapsed_ms) * 1000000u));
                }
            }
        }
}
