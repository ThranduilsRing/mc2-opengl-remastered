// gos_terrain_indirect: preflight arming, ComputeDispatch, DrawIndirect, the water reflection pass and the ring/MVP bridge accessors.
// Declared in the class headers; shared names in gos_terrain_indirect_internal.h.
#include "gos_terrain_indirect_internal.h"


namespace gos_terrain_indirect {

// ---------------------------------------------------------------------------
// Stage 3 public API
// ---------------------------------------------------------------------------

bool IsFrameSolidArmed() {
    return s_frameSolidArmed && !s_processArmingDisabled;
}

// Process-sticky variant: true once ComputePreflight() has armed at least
// one frame in this process. Used by VisualDiff to latch "intro pan
// complete" -- the per-frame arm is cleared by gosRenderer::endFrame()
// before the VisualDiff post-PP hook reads it, so a sticky-once cousin
// is required. Never resets within a process.
bool WasEverFrameSolidArmed() {
    return s_everFrameSolidArmed && !s_processArmingDisabled;
}

void ForceDisableArmingForProcess() {
    if (!s_processArmingDisabled) {
        // Loud-fail (T16/T19): a hard GL/init failure has PERMANENTLY disabled the GPU
        // terrain path for this process. The only backstop is the legacy setupTextures
        // fallback, which Phase 8z removes -- after 8z this condition renders NO terrain
        // (black) with no recovery. State it prominently; the preceding event=error line
        // carries the actual cause.
        MC2_DIAGFE(stderr,
            "[TERRAIN_INDIRECT v1] FATAL event=arming_disabled_process_sticky "
            "msg=GPU_terrain_path_PERMANENTLY_DISABLED_after_hard_GL_or_init_failure "
            "(post-8z: terrain will NOT render -- see preceding event=error line)\n");
        fflush(stderr);
    }
    s_processArmingDisabled = true;
}

void BeginFrame() {
    // Reset armed flag unconditionally every frame so mech-bay / menu frames
    // don't inherit the armed state from the last gameplay frame.
    // ComputePreflight() re-arms when terrain is actually present.
    static int s_beginFrameCount = 0;
    ++s_beginFrameCount;
    if (s_solidGpuDispatchRanThisFrame && !s_frameSolidArmed) {
        // Dispatch ran this frame but arming is already false — double BeginFrame call.
        MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=double_beginframe count=%d\n",
                s_beginFrameCount);
        fflush(stderr);
    }
    s_frameSolidArmed = false;
    // BeginFrame() is the true end-of-frame boundary; clear BOTH per-frame latches here
    // so they can't desync on idle (un-preflighted) frames. On logistics / mech-bay /
    // menu screens ComputePreflight() stops running, so without this the dispatch latch
    // stays stale-true and the guard above fires every frame (false-positive spam).
    // ComputePreflight() re-sets it next in-mission frame.
    s_solidGpuDispatchRanThisFrame = false;
}

void BeginFrameSolidWindow() {
    if (!SolidWindowEnabled()) return;
    // Reserve last-frame max + 10% slack + 64, mirroring water :149-158.
    // vector::clear on a trivially-destructible uint32_t is a size reset; no
    // allocation occurs in the hot loop once the high-water mark is reached.
    const size_t reserve =
        (size_t)(g_solidWindowMaxSeen + (g_solidWindowMaxSeen / 10) + 64);
    if (g_solidWindowStaging.capacity() < reserve)
        g_solidWindowStaging.reserve(reserve);
    g_solidWindowStaging.clear();
}

void AppendSolidWindowCandidate(int32_t vn0) {
    // Caller (terrain.cpp) asserts the quad already passed a predicate that is
    // STRICTLY LOOSER than the GPU keep-set: corners non-null, vertexNum >= 0,
    // and RecipeForVertexNum(vn0) != nullptr.  It applies NO pz, NO
    // terrainHandle, NO _wp2 filter — the GPU shader still does all of those
    // per-thread.  Looseness == guaranteed superset == no terrain-vanish.
    if (vn0 < 0) return;
    g_solidWindowStaging.push_back((uint32_t)vn0);
    if (g_solidWindowStaging.size() > g_solidWindowMaxSeen)
        g_solidWindowMaxSeen = (uint32_t)g_solidWindowStaging.size();
}

// Phase 8b A/B: read-only view of the legacy staging window. No mutation.
const uint32_t* SolidWindowStagingData(uint32_t* outCount) {
    if (outCount) *outCount = (uint32_t)g_solidWindowStaging.size();
    return g_solidWindowStaging.empty() ? nullptr : g_solidWindowStaging.data();
}

bool ComputePreflight() {
    ZoneScopedN("Terrain::IndirectPreflight");
    {
        static bool s_loggedParityRetired = false;
        if (!s_loggedParityRetired) {
            s_loggedParityRetired = true;
            MC2_DIAGF("[TERRAIN_INDIRECT v1] event=parity_infra_retired "
                   "path=gos_terrain_indirect scope=full "
                   "removed=s_packParityMask,ComputeDispatchParity_Check,getPackParityMask,txmmgr_call\n");
            fflush(stdout);
        }
    }
    s_frameSolidArmed                = false;
    s_frameSolidPackedThinCount      = 0;
    s_frameSolidCmdCount             = 0;
    s_solidGpuDispatchRanThisFrame   = false;

    if (s_processArmingDisabled) {
        // Loud-fail (T16/T19): the one-shot disable line is easy to miss, and post-8z
        // this is a silent black-terrain state with no fallback. Re-warn periodically so
        // the condition is unmissable in logs for the whole session it persists.
        static unsigned long s_disabledTicks = 0;
        if ((s_disabledTicks++ % 300u) == 0u) {
            MC2_DIAGFE(stderr,
                "[TERRAIN_INDIRECT v1] FATAL arming still disabled (tick %lu) -- "
                "GPU terrain not rendering; no fallback post-8z.\n", s_disabledTicks);
            fflush(stderr);
        }
        return false;  // ForceDisableArmingForProcess already printed the initial cause
    }
    if (!IsEnabled()) {
        static bool s_warnedEnabled = false;
        if (!s_warnedEnabled) {
            s_warnedEnabled = true;
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=preflight_fail reason=not_enabled\n");
            fflush(stderr);
        }
        return false;
    }
    if (!IsDenseRecipeReady()) {
        static bool s_warnedRecipe = false;
        if (!s_warnedRecipe) {
            s_warnedRecipe = true;
            MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=preflight_fail reason=recipe_not_ready\n");
            fflush(stderr);
        }
        return false;
    }
    if (!ResourcesReady()) {
        static bool s_warnedResources = false;
        if (!s_warnedResources) {
            s_warnedResources = true;
            MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=preflight_fail reason=resources_not_ready\n");
            fflush(stderr);
        }
        return false;
    }
    if (InMissionTransition())    return false;

    // MC2_LOAD_RACE_DELAY_ATLAS negative control: pump the deferred atlas upload
    // once scenario time crosses the 30s threshold (BuildColormapAtlas self-gates).
    if (s_loadRaceDelayAtlas && g_atlasGLTex == 0) {
        BuildColormapAtlas();
    }

    FlushDirtyRecipeSlotsToGPU();

    // Arm the GPU terrain path when this is a colormap (terrainTextures2) map.
    // Two independent signals indicate that:
    //   (a) g_uniqueTerrainNodeIds non-empty — the map has cement/overlay quads
    //       whose recipe._wp2 is a real tile node index (needs the handle LUT); OR
    //   (b) g_atlasGLTex != 0 — BuildColormapAtlas produced the merged colormap
    //       atlas. The bulk of a colormap map's quads carry the 0xFFFFFFFF
    //       sentinel in _wp2 and sample this atlas directly by world position,
    //       NOT via the node LUT, so they render even with an empty LUT.
    //
    // The legacy code armed on (a) alone. That silently fails for a map made
    // ENTIRELY of colormap quads with no cement/overlay tiles — e.g. a freshly
    // generated flat editor map: every quad's _wp2 is 0xFFFFFFFF, which
    // CollectUniqueNodeIds() drops (>= MC_MAXTEXTURES), leaving the node list
    // empty. The GPU path never armed, the frame fell through to the CPU thin
    // path which packed zero records (zero_thin), and the terrain rendered as
    // the black clear color. Adding (b) fixes that without changing behaviour
    // for any existing map: stock maps always have both an atlas AND some tile
    // quads, and legacy non-colormap maps build no atlas (terrainTextures2 NULL)
    // and leave _wp2=0 — so neither signal fires and they still take the CPU
    // path. g_atlasGLTex is zeroed per-mission on unload, so it can't go stale.
    if (gos_terrain_arm::ShouldArmGpuTerrain(
            gpu_driven::IsTerrainSolidEnabled(),
            !g_uniqueTerrainNodeIds.empty(),
            g_atlasGLTex != 0)) {
        // GPU path: arm immediately. ComputeDispatch() (called after Phase 1
        // PackAndDispatch) will build the window SSBO and issue the primary
        // cull/pack compute dispatch.  Per VPL step 2 (commit 2b), the primary
        // dispatch is the sole writer of cmds[0].count (atomicAdd of 6u per
        // admitted record); the cmd-patch shader dispatch is retired.
        // DrawIndirect() fires glMultiDrawArraysIndirect with cmdCount=1.
        s_frameSolidPackedThinCount = -1;   // sentinel: GPU path (logged only)
        s_frameSolidCmdCount        = 1;    // PR1: always 1 indirect draw command
        s_frameSolidArmed           = true;
        s_everFrameSolidArmed       = true;
        // MC2_LOAD_RACE_TRACE: latch first GPU-arm frame; mark path taken.
        if (g_loadRaceTrace) {
            g_loadRaceLastTerrainPath = 1;
            if (g_loadRaceArmFirstFrame < 0) {
                g_loadRaceArmFirstFrame = (long)g_mc2FrameCounter;
                fprintf(stderr, "[LOADRACE] gpu terrain first-arm frame=%ld "
                        "scenarioTime=%.2f atlasTex=%u nodeIds=%zu\n",
                        g_loadRaceArmFirstFrame, scenarioTime,
                        (unsigned)g_atlasGLTex, g_uniqueTerrainNodeIds.size());
                fflush(stderr);
            }
        }
        {
            static bool s_firstArm = false;
            if (!s_firstArm) {
                s_firstArm = true;
                MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=first_arm path=gpu nodeIds=%zu atlasTex=%u\n",
                        g_uniqueTerrainNodeIds.size(), (unsigned)g_atlasGLTex);
                fflush(stderr);
            }
        }
        return true;
    }

    // CPU path (legacy — demote-don't-delete per CLAUDE.md debug instrumentation rule).
    const int thinCount = PackThinRecordsForFrame();
    if (thinCount == 0) {
        if (traceOn())
            MC2_DIAGF("[TERRAIN_INDIRECT v1] event=preflight_skip reason=zero_thin\n");
        return false;
    }

    const int cmdCount = BuildIndirectCommands(thinCount);
    if (cmdCount == 0) {
        if (traceOn())
            MC2_DIAGF("[TERRAIN_INDIRECT v1] event=preflight_skip reason=zero_cmd\n");
        return false;
    }

    s_frameSolidPackedThinCount = thinCount;
    s_frameSolidCmdCount        = cmdCount;
    s_frameSolidArmed           = true;
    s_everFrameSolidArmed       = true;
    if (g_loadRaceTrace) g_loadRaceLastTerrainPath = 2;   // cpu-thin path
    {
        static bool s_firstArm = false;
        if (!s_firstArm) {
            s_firstArm = true;
            MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=first_arm path=cpu thin=%d cmd=%d\n",
                    thinCount, cmdCount);
            fflush(stderr);
        }
    }
    return true;
}


// Runs the solid-terrain cull/pack compute. The arming guards, the lighting guard, the per-frame ring locals, the
// window build, the MVP upload, the dispatch with its barrier and the unbinds stay here; each other phase body is a
// helper declared in gos_terrain_indirect_internal.h, run in the original order. A phase that abandons the
// dispatch returns true and the orchestrator returns.
void ComputeDispatch() {
    // Phase C Stage 2. Called AFTER gos_terrain_lighting::PackAndDispatch() so
    // Phase 1's lighting SSBO is populated with same-frame data.
    if (!s_frameSolidArmed)                    return;
    if (!gpu_driven::IsTerrainSolidEnabled())  return;

    ZoneScopedN("Terrain::SolidComputeDispatch");
    mc2_hitch::HitchScope _hitchTerrain(mc2_hitch::HitchSpanKind::TerrainSolidDispatch);

    // Guard: Phase 1 lighting SSBO must be ready.
    const GLuint lightSsbo = gos_terrain_lighting::GetOutputSsbo();
    if (lightSsbo == 0) {
        static bool s_warnedLight = false;
        if (!s_warnedLight) {
            MC2_DIAGF("[TERRAIN_INDIRECT v1] event=warn msg=solid_lighting_ssbo_not_ready\n");
            fflush(stdout);
            s_warnedLight = true;
        }
        return;
    }

    if (computeBuildProgram()) return;

    // Step 2b: bucket-header SSBO demoted behind MC2_BUCKET_HEADER_TRACE.
    // Cached env lookup, used at alloc / bind / clear / readback / teardown.
    static const bool s_bucketHeaderTrace = mc2::gate::get<"MC2_BUCKET_HEADER_TRACE">();
    computeAllocDebugBuffers(s_bucketHeaderTrace);

    // Upload per-frame terrain handle LUT (nodeId → gosHandle, ~20-50 entries).
    // KEPT from 08bd3b2: the shader still resolves thSlot via LUT[nodeId] from
    // recipe._wp2 and cementWord from _wp3 regardless of windowing.  The LUT
    // upload is O(uniqueTextures); its return value (full recipe count) is
    // discarded — the dispatch count now comes from the camera window.
    (void)UploadTerrainHandleLUT();

    // Approach A: build the per-frame camera-windowed recipe-index SSBO from
    // the list collected by AppendSolidWindowCandidate() during terrain.cpp's
    // setupTextures loop (NO new walk).  s_solidUseWindow=1 when the window is
    // active; 0 == fall back to full-range identity (current HEAD behavior /
    // safety escape hatch / first armed frame before the loop has run).
    int s_solidUseWindow = 0;
    const uint32_t windowCount = BuildSolidQuadWindowSSBO(&s_solidUseWindow);
    if (windowCount == 0) {
        // Zero visible quads this frame; leave armed — DrawIndirect fires with
        // cmd.count=0 (after bucket-clear + cmd-patch). No visual artifact.
        // Still need to run both dispatches to keep cmd buffer in defined state.
    }

    g_thinRingSlot = (g_thinRingSlot + 1) % kThinRingFrames;

    // ── Probe sink — writes tripwires to a file next to mc2.exe regardless of
    // how the process was launched.  Solves the "stderr disappears under
    // double-click on Windows" problem so raw user repros are captured.
    // Append-mode; line-buffered via fflush after each write.  Opened once,
    // never closed (process-lifetime).  Stderr writes are kept too — the
    // file is additive, not a redirect.
    static FILE* s_probeSink = []{
        FILE* f = fopen("ring_trace.log", "a");
        if (f) {
            fprintf(f, "\n[RING_SINK v1] event=open time=%lld\n",
                    (long long)time(nullptr));
            fflush(f);
        }
        return f;
    }();

    // ── Ring-hazard probe (MC2_RING_TRACE) ────────────────────────────────
    // Default-off per-frame trace; ALWAYS-on tripwire when the wait actually
    // times out or the fence is missing on a non-warmup frame. Either signal
    // proves the ring discipline is broken — the CPU is about to write a
    // thin-record slot whose prior GPU consumer may still be reading it.
    // Symptom we are chasing: huge raster triangles under fast camera
    // rotation; the bug is masked when the camera is still because the
    // thin-record content is frame-to-frame identical (atomicAdd reorder
    // notwithstanding) and the race produces the same bytes either way.
    // Fingerprint of the camera MVP rotation rows lets offline analysis
    // correlate fence misses with rotation magnitude.
    static const bool s_ringTrace        = mc2::gate::get<"MC2_RING_TRACE">();
    static uint64_t   s_ringTraceFrameIdx = 0;
    const uint64_t  ringFrameIdx = ++s_ringTraceFrameIdx;
    const int       probedSlot   = g_thinRingSlot;
    const bool      fencePresent = (g_thinRingFences[probedSlot] != 0);

    uint32_t mvpFp = 0u;
    computeFingerprintMvp(s_ringTrace, mvpFp);

    GLenum waitResult = GL_ALREADY_SIGNALED;
    uint64_t waitUs   = 0;
    computeRingFenceWait(probedSlot, waitResult, waitUs);
    computeTripwireMiss(s_probeSink, ringFrameIdx, probedSlot, fencePresent, mvpFp, waitResult, waitUs);
    computeBucketHeaderReadback(s_probeSink, s_ringTrace, ringFrameIdx, probedSlot, fencePresent, mvpFp, waitResult, waitUs);

    if (computeRingSkipGuard(s_probeSink, ringFrameIdx, probedSlot, waitResult)) return;

    const GLintptr thinSlotOffset = (GLintptr)(g_thinRingSlot * kThinRecordBytes);

    computeClearCounters();

    if (computeBindAndUniforms(lightSsbo, s_bucketHeaderTrace, windowCount, s_solidUseWindow, thinSlotOffset)) return;

    {
        const float* mvp = gos_GetTerrainMVPMat4();
        if (!mvp) { MC2_GL_UseProgram(0); return; }
        glUniformMatrix4fv(g_locSolidMVP, 1, GL_FALSE, mvp);

        // Probe 8: fingerprint the MVP the COMPUTE uploaded (FNV-1a over rotation
        // + translation rows = first 12 floats).  Bridge compares against MVP
        // at draw time via gos_terrain_indirect_getDispatchMvpFp().
        uint32_t fp = 2166136261u;
        for (int k = 0; k < 12; ++k) {
            uint32_t bits = 0;
            memcpy(&bits, &mvp[k], sizeof(bits));
            fp ^= bits; fp *= 16777619u;
        }
        g_dispatchMvpFp       = fp;
        g_dispatchMvpFrameIdx = ringFrameIdx;
        // Water-consistency fix: full MVP snapshot (see decl). This is the
        // exact matrix terrain-solid's Fix-B clipPos bake uses this frame.
        memcpy(g_dispatchMvp16, mvp, sizeof(float) * 16);
        // RENDER-VIEW-CURRENCY-1 / VIEW-EPOCH-DEDUPE-1: stamp the snapshot with the
        // semantic VIEW-CONTENT epoch it was sourced under (bumps only on real camera
        // change, NOT on the redundant early+gamecam republish), so object/mech
        // consumers prove same-view currency without false-staling every frame.
        g_dispatchMvpViewEpoch = ::g_viewContentEpoch;

        computeDepthTransitionProbe();

        // Probe 8b: stash first 4 floats so bridge can log byte-level delta.
        g_dispatchMvpFloats[0] = mvp[0];
        g_dispatchMvpFloats[1] = mvp[1];
        g_dispatchMvpFloats[2] = mvp[2];
        g_dispatchMvpFloats[3] = mvp[3];
        // Fix A: full 16-float snapshot for the bridge to re-upload at draw
        // time.  Indexed by the current ring slot — compute is about to write
        // thin records to g_thinRingSlot using this MVP, so the bridge must
        // project them with the same MVP one frame later.
        // Step 9 (2026-05-15): demoted behind MC2_RING_TRACE.  Default-off ⇒
        // the snapshot is not written; gos_terrain_indirect_getRingSlotMvp()
        // returns nullptr and the (already inert post-Fix-B) bridge override
        // is skipped.  One-shot lifecycle line per the Debug-instrumentation
        // rule, matching the step 8c-2 event=retired one-shots.
        if (g_envRingTrace) {
            memcpy(g_thinSlotMVP[g_thinRingSlot], mvp, sizeof(float) * 16);
            g_thinSlotMVPValid[g_thinRingSlot] = true;
        } else {
            static bool s_loggedFixADemote = false;
            if (!s_loggedFixADemote) {
                s_loggedFixADemote = true;
                MC2_DIAGF("[RING_MVP v1] event=fixA_demoted gate=MC2_RING_TRACE\n");
                fflush(stdout);
            }
        }
    }

    const uint32_t groups = (windowCount + 63u) / 64u;
    if (groups > 0) glDispatchCompute(groups, 1, 1);
    // Step 2b (VPL retirement): single barrier replaces the prior pair.  Both
    // bits are required:
    //   - GL_SHADER_STORAGE_BARRIER_BIT: fences thin[] + (when traced) hdr[]
    //     writes against the VS that reads thin[] at draw time.
    //   - GL_COMMAND_BARRIER_BIT: fences cmds[0].count atomicAdd writes
    //     against the subsequent glMultiDrawElementsIndirect's read of the
    //     indirect-cmd buffer.  Without this bit, AMD drivers can re-order
    //     the indirect-cmd fetch ahead of the compute write (NVIDIA tolerant,
    //     AMD failing per plan §"Load-bearing constraints").
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    computePostDispatchProbes(s_probeSink, s_ringTrace, ringFrameIdx);
    MC2_GL_UseProgram(0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
    // Step 2b: unbind the indirect-cmd SSBO at slot 8 to keep the SSBO
    // bind-points clean for the next compute pipeline (water-stream cmd-patch
    // uses different bindings but symmetry helps debug).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, 0);
    // Step 2b: unbind slot 6 only if we bound it (bucket-header SSBO trace path).
    if (g_solidBucketHeaderSsbo != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, 0);
    // Leave slot 3 as the thin-record range (DrawIndirect's bridge reads it via
    // gos_terrain_bridge_drawIndirect which re-binds by recipeSSBO/thinRecordSSBO
    // arguments — no leak risk).

    s_solidGpuDispatchRanThisFrame = true;
}

bool DrawIndirect() {
    if (!IsFrameSolidArmed()) {
        static bool s_warnedOnce = false;
        if (!s_warnedOnce) {
            s_warnedOnce = true;
            MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=draw_indirect_skip "
                   "armed=%d process_disabled=%d\n",
                   (int)s_frameSolidArmed, (int)s_processArmingDisabled);
            fflush(stderr);
        }
        return false;
    }
    ZoneScopedN("Terrain::SolidDrawIndirect");
    TracyGpuZone("Terrain::SolidDrawIndirect");

    const bool ok = gos_terrain_bridge_drawIndirect(
        s_frameSolidCmdCount,
        static_cast<unsigned int>(g_recipeSSBO),
        static_cast<unsigned int>(g_thinRecordSSBO),
        static_cast<unsigned int>(g_indirectCmdBuffer));

    if (!ok) {
        // Hard failure post-arming: gate-off already fired, cannot recover.
        static bool s_hardFailureLogged = false;
        if (!s_hardFailureLogged) {
            s_hardFailureLogged = true;
            MC2_DIAGFE(stderr,
                "[TERRAIN_INDIRECT v1] event=hard_failure reason=bridge_returned_false "
                "thin_count=%d cmd_count=%d "
                "advice=set MC2_TERRAIN_INDIRECT=0 to fall back to M2 legacy SOLID\n",
                s_frameSolidPackedThinCount, s_frameSolidCmdCount);
            fflush(stderr);
        }
        ForceDisableArmingForProcess();
        return false;
    }

    // first_draw lifecycle print — once per mission via the mission-latch.
    // s_firstDrawPrintedThisMission is declared in the Stage 2 anonymous ns;
    // it's reset by ResetDenseRecipe() at mission teardown.
    if (!s_firstDrawPrintedThisMission) {
        s_firstDrawPrintedThisMission = true;
        MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=first_draw "
               "thin_count=%d cmd_count=%d ring_slot=%d\n",
               s_frameSolidPackedThinCount, s_frameSolidCmdCount, g_thinRingSlot);
        fflush(stderr);
    }

    if (g_thinRingFences[g_thinRingSlot]) {
        glDeleteSync(g_thinRingFences[g_thinRingSlot]);
    }
    g_thinRingFences[g_thinRingSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // TERRAIN-INDIRECT-LATCH-FIX-1: like the chunk/patch-stream/legacy branches, the
    // Indirect terrain draw MUST set the terrain-drawn latch. sceneHasTerrain_ gates 5
    // post passes (screenShadow / cloudShadow / shoreline / edgeFog-godrays / clear-color);
    // without this, MC2_TERRAIN_LOD_CHUNK=0 + armed-camera + indirect draws terrain but
    // silently kills those passes (the dead-cloud-shadow bug the other branches were fixed
    // for; TERRAIN-SUBPASS-RECON-1 §4). Gated on commands actually issued so a terrainless
    // frame doesn't enable post (mirrors the legacy extras>0 caution). Suppressed by the
    // chunk default today -> byte-identical on the default path.
    if (s_frameSolidCmdCount > 0) {
        if (gosPostProcess* pp = getGosPostProcess()) pp->markTerrainDrawn();
        RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::IndirectBridge);  // TERRAIN-PATH-TELEMETRY-1
    }

    return true;
}

// WATER-TERRAIN-REFLECTION-1 (Phase C1): render mirrored terrain-only into the
// quarter-res water reflection RT. Gate MC2_WATER_REFLECTION_RT default OFF ->
// no-op -> byte-identical. Called from gamecam AFTER renderLists (main SOLID
// DrawIndirect already consumed its ring slot; atlases warm) and BEFORE water.
// Installs a mirror MVP, re-dispatches the SOLID compute (advances to a fresh
// ring slot + refills the shared cmd buffer), draws into the reflection FBO,
// then RESTORES the production MVP (LOAD-BEARING: ~15 same-frame consumers read
// gos_GetTerrainMVPMat4 after this — water fast path, decals, overlays, cull).
// Terrain only: no water/props/mechs. No clip plane (-> WATER-REFLECTION-CLIP-1).
void RenderWaterReflectionPass() {
    static const bool s_rtEnabled = mc2::gate::get<"MC2_WATER_REFLECTION_RT">();
    if (!s_rtEnabled)          return;   // gate OFF -> byte-identical
    if (!IsFrameSolidArmed())  return;   // no terrain this frame
    const float* M = gos_GetTerrainMVPMat4();
    if (!M)                    return;
    gosPostProcess* pp = getGosPostProcess();
    if (!pp)                   return;
    GLuint fbo = (GLuint)pp->getWaterReflectionFBO();
    int rw = pp->getWaterReflectionWidth();
    int rh = pp->getWaterReflectionHeight();
    if (fbo == 0 || rw < 1 || rh < 1) return;

    // Mirror MVP: clip = G * R * world, where R reflects MC2 world Z across the
    // water plane (z' = 2*we - z). With G[r][c] = M[c*4+r] (M uploaded GL_FALSE),
    // R only touches columns 2,3: negate column 2; column3 += 2*we * orig col2.
    const float we2 = 2.0f * (float)Terrain::waterElevation;
    float saved[16]; for (int i = 0; i < 16; ++i) saved[i] = M[i];
    float mir[16];   for (int i = 0; i < 16; ++i) mir[i]   = M[i];
    for (int r = 0; r < 4; ++r) {
        mir[12 + r] = saved[12 + r] + we2 * saved[8 + r];  // col3 += 2we*col2(orig)
        mir[8  + r] = -saved[8 + r];                        // col2 negated
    }

    // Save all GL state we touch BEFORE any state change (FBO/viewport/clear
    // color). glClearDepth(0.0) == the engine's reverse-Z default, so no restore.
    GLint prevFBO = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevVP[4];   glGetIntegerv(GL_VIEWPORT, prevVP);
    GLfloat prevCC[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, prevCC);

    // LOAD-BEARING: ComputeDispatch() also bakes the MVP into the shared
    // g_dispatchMvp16 snapshot, which the GPU WATER fast path reads as its
    // u_worldToClipGL (gameos_graphics.cpp getDispatchMvp16). Restoring only
    // terrain_mvp_ (gos_SetTerrainMVP) is NOT enough -> water would be drawn with
    // the MIRROR matrix and vanish/flicker. Save + restore the snapshot too.
    float    savedDispatchMvp[16]; memcpy(savedDispatchMvp, g_dispatchMvp16, sizeof(savedDispatchMvp));
    uint32_t savedDispatchFp       = g_dispatchMvpFp;
    uint64_t savedDispatchFrameIdx = g_dispatchMvpFrameIdx;

    gos_SetTerrainMVP(mir);   // install mirror -> compute bakes mirrored clip
    // WATER-REFLECTION-CLIP-1: arm the reflection-pass pzOk relaxation ONLY for
    // this ComputeDispatch() call, then disarm immediately after -- the flag is
    // read once per dispatch (uploaded inside ComputeDispatch itself) so there is
    // no window where the main SOLID pass could see it armed.
    s_solidReflectionPassActive = true;
    ComputeDispatch();        // fresh ring slot + refills shared cmd buffer
    s_solidReflectionPassActive = false;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, rw, rh);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);   // alpha=0 = SH-sky fallback marker (C2)
    glClearDepth(0.0);                        // reverse-Z: far plane = 0 (GEQUAL)
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // WATER-REFLECTION-CLIP-1: cheap mirrored-draw-count proof. Read back just
    // the indirect cmd's DrawArraysIndirectCommand.count (16 bytes) rather than
    // the whole RT -- this is the count of mirrored VERTICES the CLIP-1-relaxed
    // pzOk gate admitted this dispatch (0 == the pre-fix bug: everything culled).
    // Still a GL sync point (glGetBufferSubData waits for the write), but is
    // ~4-5 orders of magnitude less data than the recon's landmine-6
    // whole-RT glReadPixels(GL_RGBA, GL_FLOAT) stall, which is now opt-in only
    // (MC2_WATER_REFL_RT_PIXELPROOF=1) for deep debugging.
    uint32_t mirroredCmdCount = 0;
    {
        GLint prevDIB = 0; glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &prevDIB);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
        glGetBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, (GLsizeiptr)sizeof(mirroredCmdCount), &mirroredCmdCount);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, (GLuint)prevDIB);
    }

    // WATER-REFLECTION-CLIP-1: pzOk (compute-side) now ADMITS far-mirrored quads
    // (see u_reflectionPass), but hardware near/far clipping happens again at
    // the rasterizer regardless of what the compute gate decided -- a quad with
    // clip.z > clip.w still gets clipped/discarded by the GPU, not just culled
    // by our own gate. GL_DEPTH_CLAMP disables that hardware far-plane clip
    // (clamps depth to [0,1] instead of discarding), matching the recon's
    // "relax pzOk + GL_DEPTH_CLAMP" interim-fix pairing. Scoped to this pass
    // only; restored immediately after (was disabled engine-wide before this).
    const GLboolean depthClampWasEnabled = glIsEnabled(GL_DEPTH_CLAMP);
    if (!depthClampWasEnabled) glEnable(GL_DEPTH_CLAMP);

    const bool drew = DrawIndirect();          // bridge inherits this FBO+viewport

    if (!depthClampWasEnabled) glDisable(GL_DEPTH_CLAMP);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
    glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
    glClearColor(prevCC[0], prevCC[1], prevCC[2], prevCC[3]);  // restore clear color

    gos_SetTerrainMVP(saved);   // RESTORE production MVP (load-bearing)
    // RESTORE the dispatch snapshot the water fast path consumes (see above).
    memcpy(g_dispatchMvp16, savedDispatchMvp, sizeof(g_dispatchMvp16));
    g_dispatchMvpFp       = savedDispatchFp;
    g_dispatchMvpFrameIdx = savedDispatchFrameIdx;

    // Throttled diagnostics. Default: cheap count-only proof (mirroredCmdCount,
    // read back above -- BEFORE the restore, since g_indirectCmdBuffer is
    // per-dispatch-shared and DrawIndirect()/the next frame's main dispatch may
    // reuse the slot). Opt-in MC2_WATER_REFL_RT_PIXELPROOF=1 additionally does
    // the old whole-RT glReadPixels coverage/alpha_cov breakdown for deep debug.
    static long s_frame = 0; ++s_frame;
    if (s_frame == 1 || s_frame == 5 || s_frame == 30 || s_frame == 120 ||
        (s_frame % 600) == 0) {
        static const bool s_pixelProof = mc2::gate::get<"MC2_WATER_REFL_RT_PIXELPROOF">();
        if (!s_pixelProof) {
            fprintf(stderr, "[WATER_REFL_RT v1] frame=%ld gate=1 fbo=%u dims=%dx%d drew=%d "
                    "mirrored_cmd_count=%u\n",
                    s_frame, (unsigned)fbo, rw, rh, (int)drew, (unsigned)mirroredCmdCount);
            fflush(stderr);
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            std::vector<float> px((size_t)rw * rh * 4, 0.0f);
            glReadPixels(0, 0, rw, rh, GL_RGBA, GL_FLOAT, px.data());
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
            int n = rw * rh, colorHits = 0, alphaHits = 0; float maxL = 0.0f; double acc = 0.0;
            for (int i = 0; i < n; ++i) {
                float L = 0.2126f*px[i*4] + 0.7152f*px[i*4+1] + 0.0722f*px[i*4+2];
                if (L > 0.001f)        ++colorHits;
                if (px[i*4+3] > 0.001f) ++alphaHits;
                if (L > maxL) maxL = L;
                acc += L;
            }
            GLenum err = glGetError();
            fprintf(stderr, "[WATER_REFL_RT v1] frame=%ld gate=1 fbo=%u dims=%dx%d drew=%d "
                    "mirrored_cmd_count=%u coverage=%.1f%% alpha_cov=%.1f%% max_luma=%.4f "
                    "avg_luma=%.5f gl_err=0x%x\n",
                    s_frame, (unsigned)fbo, rw, rh, (int)drew, (unsigned)mirroredCmdCount,
                    100.0*colorHits/(n?n:1), 100.0*alphaHits/(n?n:1), maxL,
                    (n?acc/n:0.0), (unsigned)err);
            fflush(stderr);
        }
    }
}

// VPL parity-infra retirement (cpu-pack-retirement plan §7 OQ-2, full
// delete): gos_terrain_indirect::ComputeDispatchParity_Check() removed.
// Both retirement gates clear (Step 4 2e11617 retired the last accessor
// consumer; soak-waiver) and the txmmgr coupling is resolved by removing
// that call in this same commit. WaterStream::ComputeDispatchParity_Check
// (gos_terrain_water_stream.cpp) is a SEPARATE symbol and is unaffected.
// See memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md.

}  // namespace gos_terrain_indirect  // Stage 3 block

// Bridge accessor for the current thin-record ring slot (used by
// gos_terrain_bridge_drawIndirect to compute glBindBufferRange offset).
int gos_terrain_indirect_getRingSlot() {
    return g_thinRingSlot;
}

// Fix A: returns the per-slot MVP snapshot for the current ring slot, or
// nullptr if no MVP has been stashed yet (early frames before any pack /
// dispatch).  Bridge re-uploads this over terrain_mvp_ so the thin VS
// projects records with the MVP they were culled by.
// Step 9 (2026-05-15): demoted behind MC2_RING_TRACE.  When off (default) the
// writers above never populate g_thinSlotMVPValid[], so this would return
// nullptr regardless; the explicit early-out makes the demote contract
// self-documenting and guarantees zero snapshot work on the default render
// path (the gameos_graphics.cpp:2532 caller handles nullptr by skipping the
// terrainOverrideThinMVP override — itself a no-op post-Fix-B since the thin
// VS declares no terrainMVP uniform; see gos_terrain_thin.vert:56-61).
const float* gos_terrain_indirect_getRingSlotMvp() {
    if (!g_envRingTrace) return nullptr;
    const int slot = g_thinRingSlot;
    if (slot < 0 || slot >= kThinRingFrames) return nullptr;
    if (!g_thinSlotMVPValid[slot]) return nullptr;
    return g_thinSlotMVP[slot];
}

// Slice B4 Stage 1b — C-linkage accessors used by gos_terrain_mask_dispatch::DrawMaskSolid.
// Probe 8: MVP fingerprint accessors also live here (C linkage for cross-TU call from bridge).
extern "C" {

unsigned int gos_terrain_indirect_getRecipeSSBO() {
    return (unsigned int)g_recipeSSBO;
}

int gos_terrain_indirect_getRecipeMapSide() {
    return (int)g_recipeMapSide;
}

int gos_terrain_indirect_getRecipeQuadCount() {
    // The dense recipe is indexed by vertexNum = mx + my*mapSide, sized mapSide^2.
    return (int)g_recipeMapSide * (int)g_recipeMapSide;
}

uint32_t gos_terrain_indirect_getDispatchMvpFp() {
    return g_dispatchMvpFp;
}

// Water-consistency fix (2026-05-17): the full MVP terrain-solid baked its
// Fix-B clipPos with this frame. Callers MUST gate on IsFrameSolidArmed()
// (only then did ComputeDispatch run + refresh this); otherwise it is stale.
const float* gos_terrain_indirect_getDispatchMvp16() {
    return g_dispatchMvp16;
}

uint64_t gos_terrain_indirect_getDispatchMvpFrameIdx() {
    return g_dispatchMvpFrameIdx;
}

// RENDER-VIEW-CURRENCY-1: view epoch (g_mvpDiagFrame) the live dispatch snapshot
// was sourced under. Consumers compare against the current g_mvpDiagFrame.
long gos_terrain_indirect_getDispatchMvpViewEpoch() {
    return g_dispatchMvpViewEpoch;
}

void gos_terrain_indirect_getDispatchMvpFloats4(float out[4]) {
    out[0] = g_dispatchMvpFloats[0];
    out[1] = g_dispatchMvpFloats[1];
    out[2] = g_dispatchMvpFloats[2];
    out[3] = g_dispatchMvpFloats[3];
}

}  // extern "C"
