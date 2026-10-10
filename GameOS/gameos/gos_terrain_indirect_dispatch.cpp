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

    // Lazy-build compute programs and cache uniform locations + constant uniforms.
    if (g_solidComputeProgram == 0) {
        g_solidComputeProgram = gpu_driven::BuildComputeProgramFromFile(
            "shaders/gpu_driven_terrain_solid.comp", nullptr, 0, "gpu_driven_terrain_solid");
        if (g_solidComputeProgram == 0) {
            MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=error msg=solid_compute_compile_failed\n");
            fflush(stderr);
            ForceDisableArmingForProcess();
            return;
        }
        g_locSolidWC  = glGetUniformLocation(g_solidComputeProgram, "u_windowCount");
        g_locSolidAO  = glGetUniformLocation(g_solidComputeProgram, "u_alphaOverride");
        g_locSolidMVP = glGetUniformLocation(g_solidComputeProgram, "u_worldToClipGL");
        // Step 2b: u_bucketHeaderTrace — gate for hdr.{visibleCount,pad1_,pad2_}
        // writes in the primary compute shader.
        g_locSolidBHT = glGetUniformLocation(g_solidComputeProgram, "u_bucketHeaderTrace");
        // Approach A: window-vs-identity gate for the solid index buffer.
        g_locSolidUW  = glGetUniformLocation(g_solidComputeProgram, "u_useWindow");
        // u_mapSide is NOT a constant: it changes whenever a different-size map
        // loads. The program is compiled once per process, so uploading it here
        // baked in the FIRST map's side and a later, larger map (e.g. editor blank
        // 120 -> generated 260) had the shader skip ~79% of quads (edge + index
        // decompose use the stale side) -> all-black terrain. Cache the location;
        // upload the live value per frame in ComputeDispatch().
        g_locSolidMS  = glGetUniformLocation(g_solidComputeProgram, "u_mapSide");
        // WATER-REFLECTION-CLIP-1: may be optimized out if unused by the
        // driver in some future shader revision; -1 is handled below (skip
        // upload, shader-side uniform defaults to 0 = main-pass behavior).
        g_locSolidReflPass = glGetUniformLocation(g_solidComputeProgram, "u_reflectionPass");
        MC2_GL_UseProgram(g_solidComputeProgram);
        const GLint locMTR  = glGetUniformLocation(g_solidComputeProgram, "u_maxThinRecords");
        const GLint locWUPV = glGetUniformLocation(g_solidComputeProgram, "u_worldUnitsPerVertex");
        if (locMTR  >= 0) glUniform1i(locMTR, (int)kMaxThinRecords);
        if (locWUPV >= 0) glUniform1f(locWUPV, Terrain::worldUnitsPerVertex);
        MC2_GL_UseProgram(0);
    }

    // Step 2b: bucket-header SSBO demoted behind MC2_BUCKET_HEADER_TRACE.
    // Cached env lookup, used at alloc / bind / clear / readback / teardown.
    static const bool s_bucketHeaderTrace = mc2::gate::get<"MC2_BUCKET_HEADER_TRACE">();
    // Lazy-allocate bucket header SSBO (16 B GpuDrivenBucketHeader) ONLY when
    // diagnostic tracing is requested.  Default-off: zero allocation, zero
    // per-frame work, zero binding.
    if (s_bucketHeaderTrace && g_solidBucketHeaderSsbo == 0) {
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_solidBucketHeaderSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidBucketHeaderSsbo);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Probe 6 + 7a: lazy-allocate canary SSBO — 2*kMaxThinRecords uints,
    // never bound by the bridge.  Compute writes [recipeIdx, flags] per record.
    if (g_thinCanarySSBO == 0) {
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_thinCanarySSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinCanarySSBO);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(2u * kMaxThinRecords * sizeof(uint32_t)),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

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
    if (s_ringTrace) {
        const float* mvpProbe = gos_GetTerrainMVPMat4();
        if (mvpProbe) {
            // FNV-1a over rotation/translation rows (skip projection row mvp[12..15])
            // — gives a stable fingerprint that changes proportionally to camera
            // rotation rate but is insensitive to projection-only mutation.
            mvpFp = 2166136261u;
            for (int k = 0; k < 12; ++k) {
                uint32_t bits = 0;
                memcpy(&bits, &mvpProbe[k], sizeof(bits));
                mvpFp ^= bits;
                mvpFp *= 16777619u;
            }
        }
    }

    GLenum waitResult = GL_ALREADY_SIGNALED;
    uint64_t waitUs   = 0;
    if (g_thinRingFences[probedSlot]) {
        // C-02: use timeout=0 (non-blocking) instead of 10ms so the CPU never
        // stalls waiting for the GPU.  If the fence is not yet signaled the slot
        // is still in use by the GPU; skip this dispatch entirely (return below)
        // so we never overwrite an in-flight ring slot.  Ring-overwrite safety is
        // preserved: the fence is only deleted when it has signaled.
        const auto t0 = std::chrono::steady_clock::now();
        waitResult = glClientWaitSync(g_thinRingFences[probedSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT, 0u /*non-blocking*/);
        const auto t1 = std::chrono::steady_clock::now();
        waitUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (waitResult != GL_TIMEOUT_EXPIRED && waitResult != GL_WAIT_FAILED) {
            // Fence signaled (GL_ALREADY_SIGNALED or GL_CONDITION_SATISFIED).
            glDeleteSync(g_thinRingFences[probedSlot]);
            g_thinRingFences[probedSlot] = 0;
        }
        // If GL_TIMEOUT_EXPIRED: fence NOT deleted; slot still in-flight.
        // The tripwire block below will log it, then we return early.
    }

    // Tripwire #1: missing fence past warmup OR fence timeout.
    // Warmup window = 2*kThinRingFrames frames; first slot 1 wrap can miss
    // a fence if frame-1's DrawIndirect didn't issue (recipe not ready /
    // MVP not yet valid).  This is benign startup, not a bug.
    {
        // When MC2_TERRAIN_LOD_CHUNK=1 the chunk path owns terrain rendering and
        // DrawIndirect is suppressed — no fences are ever written, so every frame
        // past warmup triggers fenceMissedAfterWarmup. Gate it out to silence the
        // flood and let useful diagnostics from the LOD chunk path show up in logs.
        const bool s_lodChunkActive = mc2TerrainLodChunkEnabled();  // default-on aware
        const bool fenceMissedAfterWarmup =
            (!fencePresent) && (ringFrameIdx > (uint64_t)(2 * kThinRingFrames))
            && !s_lodChunkActive;
        const bool timeoutFired =
            (waitResult == GL_TIMEOUT_EXPIRED || waitResult == GL_WAIT_FAILED);
        if (fenceMissedAfterWarmup || timeoutFired) {
            static uint32_t s_tripwireMissCount    = 0;
            static uint32_t s_tripwireTimeoutCount = 0;
            uint32_t totalMiss    = (fenceMissedAfterWarmup ? ++s_tripwireMissCount    : s_tripwireMissCount);
            uint32_t totalTimeout = (timeoutFired           ? ++s_tripwireTimeoutCount : s_tripwireTimeoutCount);
            PROBE_LOG(
                "[RING_TRIPWIRE v1] frame=%llu slot=%d fence=%d wait=0x%x wait_us=%llu "
                "totalMiss=%u totalTimeout=%u mvpFp=0x%08x\n",
                (unsigned long long)ringFrameIdx, probedSlot, (int)fencePresent,
                (unsigned)waitResult, (unsigned long long)waitUs,
                (unsigned)totalMiss, (unsigned)totalTimeout, (unsigned)mvpFp);
        }
    }

    // Tripwire #2: prev-frame visibleCount overshoot.
    // The cull compute shader does atomicAdd(visibleCount, 1) unconditionally,
    // then early-returns if outSlot >= kMaxThinRecords.  So visibleCount can
    // climb past the cap; in that case the records that ran the atomicAdd
    // LAST get dropped — atomicAdd ordering is race-dependent → different
    // frames drop different records → thin-SSBO content varies between
    // compute write and VS read EVEN AT THE SAME RING SLOT.  This is the
    // candidate root cause for the "huge garbage triangle on fast rotation"
    // signature: cmd-patch clamps the count to 65536*6 but the records past
    // the kept records are race-dependent.
    //
    // Cost: one 4-byte readback per frame.  The fence wait above guarantees
    // all prior GPU commands are complete, so glGetBufferSubData is stall-
    // free here on AMD/NV (driver fast-path for already-signaled buffers).
    if (g_solidBucketHeaderSsbo != 0 && ringFrameIdx > 1) {
        // Read all 16 bytes of GpuDrivenBucketHeader:
        //   [0]=visibleCount, [1]=pad0_ (unused), [2]=pad1_ (near-zero clip.w count,
        //   probe 4), [3]=pad2_ (recipe-spread corruption count, probe 5).
        //
        // C-04 (non-blocking): only issue glGetBufferSubData when the fence for
        // probedSlot already signaled (GPU-done confirmed by the timeout=0 wait
        // above).  If the fence was absent / still in-flight, reuse the last
        // cached header values — a 1-frame-stale overshoot LOG is harmless.
        // This prevents a per-frame CPU stall on the default-ON GPU-driven path.
        static uint32_t s_hdr4Cached[4] = { 0, 0, 0, 0 };
        const bool gpuDone = (waitResult == GL_ALREADY_SIGNALED ||
                              waitResult == GL_CONDITION_SATISFIED);
        if (gpuDone) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidBucketHeaderSsbo);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                               (GLsizeiptr)sizeof(s_hdr4Cached), s_hdr4Cached);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
        const uint32_t prevVisible = s_hdr4Cached[0];
        const uint32_t prevNearW   = s_hdr4Cached[2];
        const uint32_t prevSpread  = s_hdr4Cached[3];

        static uint32_t s_peakVisible    = 0;
        static uint32_t s_overshootCount = 0;
        static uint32_t s_peakNearW      = 0;
        static uint32_t s_nearWFrames    = 0;
        static uint64_t s_lastSummary    = 0;
        if (prevVisible > s_peakVisible) s_peakVisible = prevVisible;
        if (prevNearW   > s_peakNearW  ) s_peakNearW   = prevNearW;
        if (prevNearW > 0) ++s_nearWFrames;
        const bool overshot = (prevVisible >= (uint32_t)kMaxThinRecords);
        if (overshot) {
            ++s_overshootCount;
            PROBE_LOG(
                "[RING_OVERSHOOT v1] frame=%llu prev_visible=%u cap=%zu "
                "overshootCount=%u peak=%u mvpFp=0x%08x\n",
                (unsigned long long)ringFrameIdx, (unsigned)prevVisible,
                kMaxThinRecords, (unsigned)s_overshootCount,
                (unsigned)s_peakVisible, (unsigned)mvpFp);
        }
        // Probe 6 (FULL COVERAGE): compare thin[i].recipeIdx vs canary[i]
        // for EVERY i in [0, visibleCount).  Compute writes both at the same
        // outSlot in the SAME dispatch; canary buffer is never bound by the
        // bridge.  If for any i the two disagree, the thin SSBO has been
        // clobbered between compute write and our readback.
        //
        // User-reported symptom: giant grey-banded triangle that appears in
        // varied screen locations (skybox / mid-screen / behind terrain at
        // water level) frame-to-frame.  Only mechanism that explains location
        // variance is ONE thin record per frame having a corrupt recipeIdx
        // pointing to different recipes each occurrence.  Sample-based probe
        // (5 indices) almost certainly missed it; full coverage will catch it.
        //
        // Cost: visibleCount * 4 bytes for canary + visibleCount * 4 bytes for
        // thin.recipeIdx = ~80KB/frame at peak visibleCount=10000. PCIe stall
        // possible but acceptable for diagnostic.
        if (s_thinCanaryEnabled && g_thinCanarySSBO != 0 && g_thinRecordSSBO != 0 && prevVisible > 0) {
            const uint32_t vis = prevVisible < (uint32_t)kMaxThinRecords
                                ? prevVisible : (uint32_t)kMaxThinRecords;
            const int priorSlot = (g_thinRingSlot + kThinRingFrames - 1) % kThinRingFrames;
            const GLintptr thinSlotByteOffset = (GLintptr)(priorSlot * kThinRecordBytes);

            static std::vector<uint32_t> s_canaryBuf;
            static std::vector<uint32_t> s_thinBlock;
            if (s_canaryBuf.size() < (size_t)vis * 2u) s_canaryBuf.resize((size_t)vis * 2u);
            const size_t thinDwords = (size_t)vis * 8u;
            if (s_thinBlock.size() < thinDwords) s_thinBlock.resize(thinDwords);

            // 1) Bulk-read canary[0..2*vis-1] (single readback, vis*8 bytes).
            //    Layout: [recipeIdx_0, flags_0, recipeIdx_1, flags_1, ...]
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinCanarySSBO);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                               (GLsizeiptr)(vis * 2u * sizeof(uint32_t)),
                               s_canaryBuf.data());

            // 2) Read full thin records as packed block (vis * 32 bytes).
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinRecordSSBO);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, thinSlotByteOffset,
                               (GLsizeiptr)(vis * 32u),
                               s_thinBlock.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

            // 3) Compare BOTH recipeIdx AND flags.
            //    Thin record layout: dword 0=recipeIdx, dword 1=terrainHandle,
            //    dword 2=flags, dword 3=cementWord, dwords 4..7=lightRGB0..3.
            //    Canary layout: word 0=recipeIdx, word 1=flags (paired per record).
            static uint32_t s_recipeIdxMismatchFrames = 0;
            static uint64_t s_recipeIdxMismatchTotal  = 0;
            static uint32_t s_flagsMismatchFrames     = 0;
            static uint64_t s_flagsMismatchTotal      = 0;
            static uint32_t s_nextRecipeReportAt      = 1;
            static uint32_t s_nextFlagsReportAt       = 1;
            uint32_t recipeMismatchInFrame = 0, flagsMismatchInFrame = 0;
            uint32_t firstBadIdxR = 0, firstBadCanaryR = 0, firstBadThinR = 0;
            uint32_t firstBadIdxF = 0, firstBadCanaryF = 0, firstBadThinF = 0;
            for (uint32_t i = 0; i < vis; ++i) {
                const uint32_t canaryR = s_canaryBuf[i * 2u];
                const uint32_t canaryF = s_canaryBuf[i * 2u + 1u];
                const uint32_t thinR   = s_thinBlock[i * 8u];        // recipeIdx
                const uint32_t thinF   = s_thinBlock[i * 8u + 2u];   // flags
                if (canaryR != thinR) {
                    if (recipeMismatchInFrame == 0) {
                        firstBadIdxR    = i;
                        firstBadCanaryR = canaryR;
                        firstBadThinR   = thinR;
                    }
                    ++recipeMismatchInFrame; ++s_recipeIdxMismatchTotal;
                }
                if (canaryF != thinF) {
                    if (flagsMismatchInFrame == 0) {
                        firstBadIdxF    = i;
                        firstBadCanaryF = canaryF;
                        firstBadThinF   = thinF;
                    }
                    ++flagsMismatchInFrame; ++s_flagsMismatchTotal;
                }
            }
            if (recipeMismatchInFrame > 0) {
                ++s_recipeIdxMismatchFrames;
                if (s_recipeIdxMismatchFrames == s_nextRecipeReportAt) {
                    PROBE_LOG(
                        "[RING_CANARY_RECIPE v1] frame=%llu visible=%u prior_slot=%d "
                        "mismatch_in_frame=%u first_bad_idx=%u "
                        "first_bad_canary=%u first_bad_thin=%u "
                        "frames=%u total=%llu mvpFp=0x%08x\n",
                        (unsigned long long)ringFrameIdx, (unsigned)vis, priorSlot,
                        (unsigned)recipeMismatchInFrame, (unsigned)firstBadIdxR,
                        (unsigned)firstBadCanaryR, (unsigned)firstBadThinR,
                        (unsigned)s_recipeIdxMismatchFrames,
                        (unsigned long long)s_recipeIdxMismatchTotal, (unsigned)mvpFp);
                    s_nextRecipeReportAt = s_nextRecipeReportAt < 100
                        ? (s_nextRecipeReportAt + 1) : (s_nextRecipeReportAt * 10);
                }
            }
            if (flagsMismatchInFrame > 0) {
                ++s_flagsMismatchFrames;
                if (s_flagsMismatchFrames == s_nextFlagsReportAt) {
                    PROBE_LOG(
                        "[RING_CANARY_FLAGS v1] frame=%llu visible=%u prior_slot=%d "
                        "mismatch_in_frame=%u first_bad_idx=%u "
                        "first_bad_canary=0x%x first_bad_thin=0x%x "
                        "frames=%u total=%llu mvpFp=0x%08x\n",
                        (unsigned long long)ringFrameIdx, (unsigned)vis, priorSlot,
                        (unsigned)flagsMismatchInFrame, (unsigned)firstBadIdxF,
                        (unsigned)firstBadCanaryF, (unsigned)firstBadThinF,
                        (unsigned)s_flagsMismatchFrames,
                        (unsigned long long)s_flagsMismatchTotal, (unsigned)mvpFp);
                    s_nextFlagsReportAt = s_nextFlagsReportAt < 100
                        ? (s_nextFlagsReportAt + 1) : (s_nextFlagsReportAt * 10);
                }
            }
        }

        // Probe 5 tripwire: any frame where pad2_ > 0 means at least one
        // RECIPE entry has worldPos corners spanning more than 2.5 cells —
        // ground-truth evidence that the recipe SSBO is corrupt.  If this
        // fires AND the bug is visible, recipe data is the root cause.
        // If silent AND bug visible, recipe is fine — corruption is in the
        // VS or downstream.
        if (prevSpread > 0) {
            static uint32_t s_spreadFrames = 0;
            static uint32_t s_peakSpread   = 0;
            static uint32_t s_nextSpreadReportAt = 1;
            ++s_spreadFrames;
            if (prevSpread > s_peakSpread) s_peakSpread = prevSpread;
            if (s_spreadFrames == s_nextSpreadReportAt) {
                PROBE_LOG(
                    "[RING_SPREAD v1] frame=%llu spread_count=%u spread_frames=%u "
                    "peak_spread=%u prev_visible=%u mvpFp=0x%08x\n",
                    (unsigned long long)ringFrameIdx, (unsigned)prevSpread,
                    (unsigned)s_spreadFrames, (unsigned)s_peakSpread,
                    (unsigned)prevVisible, (unsigned)mvpFp);
                s_nextSpreadReportAt = s_nextSpreadReportAt < 100 ? (s_nextSpreadReportAt + 1)
                                     : (s_nextSpreadReportAt * 10);
            }
        }
        // Probe 4 tripwire: any frame where pad1_ > 0 means at least one quad
        // passed the pzOk gate while having a corner with |clip.w| < 0.05.
        // Strong candidate for the "huge garbage triangle" bug per
        // memory/clip_w_sign_trap.md.  Rate-limit by printing only on first
        // event AND on every 100x increase in s_nearWFrames so we don't
        // flood the log when the bug is continuous.
        if (prevNearW > 0) {
            static uint32_t s_nextNearWReportAt = 1;
            if (s_nearWFrames == s_nextNearWReportAt) {
                PROBE_LOG(
                    "[RING_NEARW v1] frame=%llu near_w_count=%u total_near_w_frames=%u "
                    "peak_near_w=%u prev_visible=%u mvpFp=0x%08x\n",
                    (unsigned long long)ringFrameIdx, (unsigned)prevNearW,
                    (unsigned)s_nearWFrames, (unsigned)s_peakNearW,
                    (unsigned)prevVisible, (unsigned)mvpFp);
                s_nextNearWReportAt = s_nextNearWReportAt < 100 ? (s_nextNearWReportAt + 1)
                                    : (s_nextNearWReportAt * 10);
            }
        }
        if (s_ringTrace) {
            printf("[RING v1] frame=%llu slot=%d fence=%d wait=0x%x wait_us=%llu "
                   "prev_visible=%u near_w=%u peak=%u peakNearW=%u mvpFp=0x%08x\n",
                (unsigned long long)ringFrameIdx, probedSlot, (int)fencePresent,
                (unsigned)waitResult, (unsigned long long)waitUs,
                (unsigned)prevVisible, (unsigned)prevNearW,
                (unsigned)s_peakVisible, (unsigned)s_peakNearW, (unsigned)mvpFp);
            fflush(stdout);
        }
        // Periodic peak summary even when MC2_RING_TRACE is off so manual
        // runs without env-var still surface the peak visible count.
        if (ringFrameIdx - s_lastSummary >= 600) {
            s_lastSummary = ringFrameIdx;
            PROBE_LOG(
                "[RING_PEAK v1] frame=%llu peak_visible=%u cap=%zu "
                "overshootCount=%u peak_near_w=%u near_w_frames=%u\n",
                (unsigned long long)ringFrameIdx, (unsigned)s_peakVisible,
                kMaxThinRecords, (unsigned)s_overshootCount,
                (unsigned)s_peakNearW, (unsigned)s_nearWFrames);
        }
    } else if (s_ringTrace) {
        printf("[RING v1] frame=%llu slot=%d fence=%d wait=0x%x wait_us=%llu mvpFp=0x%08x\n",
            (unsigned long long)ringFrameIdx, probedSlot, (int)fencePresent,
            (unsigned)waitResult, (unsigned long long)waitUs, (unsigned)mvpFp);
        fflush(stdout);
    }

    // C-02: if the fence for this slot was not signaled (timeout=0 above), the
    // prior GPU consumer is still reading it.  Skip the dispatch to preserve ring
    // safety; the slot fence is left intact so the next frame can retry.
    if (waitResult == GL_TIMEOUT_EXPIRED || waitResult == GL_WAIT_FAILED) {
        static uint32_t s_ringSkipCount = 0;
        ++s_ringSkipCount;
        PROBE_LOG("[RING_SKIP v1] frame=%llu slot=%d wait=0x%x skip_total=%u\n",
            (unsigned long long)ringFrameIdx, probedSlot,
            (unsigned)waitResult, (unsigned)s_ringSkipCount);
        return;
    }

    const GLintptr thinSlotOffset = (GLintptr)(g_thinRingSlot * kThinRecordBytes);

    // Step 2b: zero the indirect-cmd buffer's count slot (offset 0, 4 bytes)
    // every frame before the primary dispatch.  instanceCount / first /
    // baseInstance were initialized to {1,0,0} at allocation and are never
    // re-written (cmd-patch dispatch retired).
    {
        const uint32_t zero = 0u;
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
        glClearBufferSubData(GL_DRAW_INDIRECT_BUFFER, GL_R32UI, 0, 4,
                             GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    }

    // Step 2b: bucket-header SSBO (when traced) gets the same per-frame clear
    // it always had.  No-op when MC2_BUCKET_HEADER_TRACE is unset because
    // g_solidBucketHeaderSsbo is 0.
    if (g_solidBucketHeaderSsbo != 0) {
        const uint32_t zero = 0u;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidBucketHeaderSsbo);
        glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, 16,
                             GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // GPU-SYNC-CONTRACT: order the cmd-count clear (and the recipe/window/LUT
    // glBufferSubData uploads earlier this frame) BEFORE the cull/pack dispatch
    // below, which atomicAdds into cmds[0].count. Without this, NVIDIA may reorder
    // the clear after the dispatch -> atomicAdd accumulates onto a stale count ->
    // garbage/giant terrain triangles on fast camera motion. AMD tolerated the
    // omission. (Routed through the typed helper, not a hand-placed barrier.)
    gpuSyncBarrier(GpuProducer::ClearBuffer, GpuConsumer::ComputeShader,
                   "terrain_indirect_count_clear");

    // ------------------------------------------------------------------
    // DISPATCH 1: cull/pack (gpu_driven_terrain_solid.comp)
    // Bindings: 0=recipe, 1=lighting, 2=terrainHandleLut, 3=thin, 6=header,
    //           7=canary, 8=cmdbuf, 9=solid window (Approach A).
    // NOTE: the plan named binding 7 for the window SSBO, but on this branch
    // bindings 7 (canary) and 8 (cmdbuf) are already occupied (post-VPL
    // retirement additions that did not exist when 08bd3b2 freed binding 2).
    // Binding 9 is the first free slot — chosen to avoid the collision the
    // plan explicitly warned against.
    // ------------------------------------------------------------------
    MC2_GL_UseProgram(g_solidComputeProgram);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_recipeSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, lightSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2,
                     g_terrainHandleLutSSBO ? g_terrainHandleLutSSBO : 0);
    gpuBindSsboRange(3, g_thinRecordSSBO,
                     (long long)thinSlotOffset, (long long)kThinRecordBytes,
                     "terrain.thinRecord");
    // Step 2b: bucket-header SSBO only bound when MC2_BUCKET_HEADER_TRACE
    // armed.  When unbound, the shader sees u_bucketHeaderTrace=0 and skips
    // all hdr.* writes (including the slot-counter atomicAdd on visibleCount).
    if (g_solidBucketHeaderSsbo != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, g_solidBucketHeaderSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, g_thinCanarySSBO);  // probe 6
    // Step 2b: bind the indirect-cmd buffer as SSBO at slot 8.  The primary
    // compute atomicAdds 6u into cmds[0].count per admitted record and uses
    // the returned-before-increment value as the thin-record slot index when
    // the bucket-header trace path is off.  Buffer is double-bound
    // (GL_DRAW_INDIRECT_BUFFER for the draw, GL_SHADER_STORAGE_BUFFER for
    // the atomicAdd).  Coherent qualifier on the shader-side declaration
    // covers cross-target visibility within the frame.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, g_indirectCmdBuffer);
    // Approach A: camera-windowed recipe-index buffer.  Only meaningful when
    // s_solidUseWindow==1; bound unconditionally (when allocated) so the
    // shader's solidWin[] declaration always resolves.  When the window is
    // inactive the shader uses identity (vn0 = id) and never reads solidWin[].
    if (g_solidQuadWindowSsbo != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, g_solidQuadWindowSsbo);

    if (g_locSolidWC < 0 || g_locSolidMVP < 0) {
        MC2_DIAGFE(stderr,
            "[TERRAIN_INDIRECT v1] event=error msg=solid_compute_missing_uniform "
            "u_windowCount=%d u_terrainMVP=%d\n",
            g_locSolidWC, g_locSolidMVP);
        fflush(stderr);
        MC2_GL_UseProgram(0);
        ForceDisableArmingForProcess();
        return;
    }

    glUniform1i(g_locSolidWC, (int)windowCount);
    // Per-frame: the current map's side. Stale-from-first-map here = all-black on
    // any later different-size map (the shader's edge cull + index decompose use it).
    if (g_locSolidMS >= 0)
        glUniform1i(g_locSolidMS, (int)Terrain::realVerticesMapSide);
    if (g_locSolidAO >= 0)
        glUniform1i(g_locSolidAO, Terrain::terrainTextures2 != nullptr ? 1 : 0);
    // Step 2b: upload u_bucketHeaderTrace gate every frame.  Uniform values
    // reset across glUseProgram cycles on some drivers, so per-frame upload
    // is safer than once-at-compile.
    if (g_locSolidBHT >= 0)
        glUniform1i(g_locSolidBHT, s_bucketHeaderTrace ? 1 : 0);
    // Approach A: 1 == read vn0 from solidWin[id]; 0 == identity vn0 = id
    // (full-range fallback == current HEAD behavior).  CPU-side value, no
    // GPU readback.
    if (g_locSolidUW >= 0)
        glUniform1i(g_locSolidUW, s_solidUseWindow);
    // WATER-REFLECTION-CLIP-1: armed ONLY for the duration of the reflection
    // pass's own ComputeDispatch() call (see RenderWaterReflectionPass). 0 on
    // every other call site (main SOLID dispatch) -> byte-identical pzOk.
    if (g_locSolidReflPass >= 0)
        glUniform1i(g_locSolidReflPass, s_solidReflectionPassActive ? 1 : 0);

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

        // ── [DEPTH_TRANSITION v1] zoom-step depth-pop diagnostic ───────────
        // Env-gated MC2_DEPTH_TRANSITION_PROBE (cached static const bool;
        // silent default; matches the [WATER_DEPTHPROBE v2] idiom at
        // gos_terrain_water_stream.cpp:1429 — raw printf+fflush). HIGH STAKES:
        // this zoom-step depth-pop root-cause has been WRONG 3x. The probe
        // dumps each flat consumer's ACTUAL resulting screen-z for a FIXED
        // world point ONLY on the MVP-transition (zoom-step) frame so the
        // ~0.1-unit 1-frame pop is numerically visible. [WATER_DEPTHPROBE v2]
        // is structurally blind (it FNV-hashes the MVP, not the downstream
        // per-consumer depth-fudge). RenderDoc cannot capture a 1-frame
        // transient. ZERO behavior change: pure reads of g_dispatchMvp16 (the
        // exact matrix terrain-solid's Fix-B clipPos bake + the armed GPU
        // water fast path both project with this frame), live terrain_mvp_
        // (the matrix the decal overlay binds), and the cross-TU sampled CPU
        // water value; O(1)/frame; printf only on transition frames (latched).
        {
            static const bool s_depthTransProbe = mc2::gate::get<"MC2_DEPTH_TRANSITION_PROBE">();
            if (s_depthTransProbe) {
                // --- Transition detector -----------------------------------
                // maxDelta = max_i |g_dispatchMvp16[i] - prev[i]|. kEps chosen
                // so a smooth pan does NOT trip but a DISCRETE zoom-level step
                // does. Justification: a per-frame camera pan changes the MVP
                // translation row by a small continuous increment (sub-1e-4 in
                // the normalized clip-matrix entries per frame at normal pan
                // speed); MC2's zoom is a DISCRETE camera-distance step (the
                // bug only reproduces on zoom/elevation-change, NOT pan — see
                // CLAUDE.md Known issues), which jumps several matrix entries
                // (projection scale + translation) by >>1e-4 in one frame. A
                // smooth same-magnitude pan would also trip a too-small kEps,
                // so kEps=1e-3f is used: large enough that normalized
                // per-frame pan deltas (empirically ~1e-5..1e-4 in these
                // entries) stay below it, small enough that a discrete zoom
                // step (whole-number camera-distance change -> matrix entry
                // jumps ~1e-2+) trips it decisively. This is a DIAGNOSTIC
                // gate, not a control path; over/under-trip only changes which
                // frames print, never rendering.
                static float g_prevDispatchMvp16[16] = { 0.0f };
                static bool  s_prevValid = false;
                static unsigned long long s_dtFrame = 0;
                const unsigned long long f = ++s_dtFrame;
                const float kEps = 1.0e-3f;
                float maxDelta = 0.0f;
                for (int i = 0; i < 16; ++i) {
                    const float d =
                        std::abs(g_dispatchMvp16[i] - g_prevDispatchMvp16[i]);
                    if (d > maxDelta) maxDelta = d;
                }
                const bool transition = s_prevValid && (maxDelta > kEps);

                // --- Fixed probe point P -----------------------------------
                // Deterministic mid-map XY at the water plane. FRAME-STABLE
                // (map geometry constants + waterElevation, NOT camera): the
                // dz across the transition is therefore attributable purely to
                // the per-consumer depth-fudge x MVP interaction, not point
                // motion. MC2 world layout (terrain.h:367-368): X increases
                // from mapTopLeft3d.x, Y decreases from mapTopLeft3d.y.
                const float Px = Terrain::mapTopLeft3d.x
                                 + Terrain::worldUnitsMapSide * 0.5f;
                const float Py = Terrain::mapTopLeft3d.y
                                 - Terrain::worldUnitsMapSide * 0.5f;
                const float Pz = Terrain::waterElevation;

                // Project P with a given GL_FALSE row-major MVP using the
                // file's authoritative convention (see :1881-1898 and
                // memory/terrain_mvp_gl_false.md): GLSL reads our row-major
                // bytes column-major, so clip[i] = sum_j m[j*4+i]*P[j]. This
                // is byte-identical to gpu_driven_terrain_solid.comp's
                // projectClip() (terrain-solid's clipPos source, read verbatim
                // by gos_terrain_thin.vert:210) and to the `terrainMVP*vec4`
                // in gos_terrain_water_fast_mdi.vert:283 and
                // terrain_overlay.vert:31 (same upload convention).
                auto projZ = [](const float* m,
                                 float x, float y, float z,
                                 float& outZ, float& outW) {
                    const float cz =
                        m[0 * 4 + 2] * x + m[1 * 4 + 2] * y +
                        m[2 * 4 + 2] * z + m[3 * 4 + 2];
                    const float cw =
                        m[0 * 4 + 3] * x + m[1 * 4 + 3] * y +
                        m[2 * 4 + 3] * z + m[3 * 4 + 3];
                    outZ = cz; outW = cw;
                };

                // terrain SOLID: mirror gos_terrain_thin.vert:219 exactly.
                // clip = (dispatch MVP) * P (the compute bakes tr.clipPos with
                // EXACTLY g_dispatchMvp16 -- see :1521 decl + :1894 bake);
                // screen.z = clip.z/clip.w + TERRAIN_DEPTH_FUDGE (0.002,
                // single-sourced mc2depth::TERRAIN_DEPTH_FUDGE).
                float czT, cwT;
                projZ(g_dispatchMvp16, Px, Py, Pz, czT, cwT);
                const float z_terr =
                    (czT / cwT) + mc2depth::TERRAIN_DEPTH_FUDGE;

                // GPU WATER (fast MDI): mirror
                // gos_terrain_water_fast_mdi.vert:291 exactly. The armed water
                // fast path binds gos_terrain_indirect_getDispatchMvp16()
                // (== g_dispatchMvp16; gos_terrain_water_stream.cpp:1409-1413)
                // and ComputeDispatch only runs armed, so this is the live
                // GPU-water producer's exact matrix this frame. screen.z =
                // clip.z/clip.w + WATER_DEPTH_FUDGE_FAST (0.0025).
                const float z_gpuw =
                    (czT / cwT) + mc2depth::WATER_DEPTH_FUDGE_FAST;

                // DECAL (post-Fix B, probe==producer): the armed overlay/
                // decal callers (drawTerrainOverlays / drawDecalStaticBatch /
                // drawDecals) bind the symmetric-mirror MVP -- armed ->
                // gos_terrain_indirect_getDispatchMvp16() == g_dispatchMvp16,
                // the SAME matrix z_terr/z_gpuw use here (uploadOverlayUniforms_
                // in gameos_graphics.cpp) -- and terrain_overlay.vert adds
                // OVERLAY_DEPTH_BIAS (single-sourced mc2depth). This code is
                // armed-only (ComputeDispatch), so the faithful producer model
                // is czT/cwT + OVERLAY_DEPTH_BIAS, co-planar with z_terr/z_gpuw
                // by construction (Fix B). Retired the pre-Fix-B model (stale
                // liveMvp gos_GetTerrainMVPMat4 + kPolyOffsetNdcApprox modelling
                // the REMOVED glPolygonOffset(-1,-1)); that produced a
                // counterfactual constant dz_decal blind to the fix
                // (parity_probe_100pct: probe must equal producer). z_decal is
                // now exact, not an approximation.
                const float z_decal =
                    (czT / cwT) + mc2depth::OVERLAY_DEPTH_BIAS;
                const int decalOk = 1;

                // CPU WATER: SAMPLED real produced value (NOT a CPU re-derive;
                // the Stuff eye->projectForTerrainAdmission pipeline is not
                // reachable here). TerrainQuad::drawWater (mclib/quad.cpp
                // ~:3345) latches vertices[k]->wz + WATER_DEPTH_FUDGE (raster
                // 0.0025) for the water vertex nearest the SAME fixed P.
                // CRITICAL: CPU water and the GPU fast path are mutually
                // exclusive per frame (terrain.cpp:1225-1229). ComputeDispatch
                // (this code) only runs ARMED, i.e. exactly when CPU water is
                // SKIPPED -- so g_cpuWaterProbeZ is the LAST un-armed steady
                // sample, NOT this-frame data. We emit cpuw_stamp + its delta
                // vs the previous dump so a stale CPU value is VISIBLE and
                // never silently trusted (we have been wrong 3x trusting
                // models over real data; a flagged-stale real value is honest
                // evidence, a fabricated this-frame CPU value would be false).
                // (declared at global file scope above - unqualified lookup
                // here binds the global ::g_cpuWaterProbe* defined in quad.cpp)
                const float z_cpuw = g_cpuWaterProbeZ;
                const int   cpuwAny = g_cpuWaterProbeAny ? 1 : 0;
                static unsigned long long s_prevCpuStamp = 0;
                const unsigned long long cpuStampNow = g_cpuWaterProbeStamp;

                // --- Emit (transition frames only, latched) ----------------
                // Also emit the 1 steady frame immediately BEFORE a transition
                // (one-frame-back latch) and the 1 steady frame AFTER, so the
                // transition-frame dz can be compared to adjacent steady dz.
                // s_emitTail counts down post-transition steady frames to
                // print; s_havePrevSteady stashes the immediately-preceding
                // steady line so we can print it retroactively when a
                // transition is detected (cheap: 4 floats).
                struct DtLine {
                    unsigned long long f; float md; float zt, zc, zg, zd;
                    int cAny; unsigned long long cStamp;
                };
                static DtLine s_prevSteady = {0,0,0,0,0,0,0,0};
                static bool   s_havePrevSteady = false;
                static int    s_emitTail = 0;

                DtLine cur;
                cur.f = f; cur.md = maxDelta;
                cur.zt = z_terr; cur.zc = z_cpuw;
                cur.zg = z_gpuw; cur.zd = z_decal;
                cur.cAny = cpuwAny; cur.cStamp = cpuStampNow;

                auto emitLine = [&](const DtLine& L, const char* kind) {
                    const long long cpuStampDelta =
                        (long long)L.cStamp - (long long)s_prevCpuStamp;
                    // cpuw_fresh=1 only if the CPU sampler advanced its stamp
                    // since the previous emitted line AND it found a vertex.
                    const int cpuwFresh =
                        (L.cAny && cpuStampDelta != 0) ? 1 : 0;
                    printf("[DEPTH_TRANSITION v1] kind=%s f=%llu "
                           "maxMvpDelta=%.7f "
                           "z_terr=%.7f z_cpuw=%.7f z_gpuw=%.7f z_decal=%.7f "
                           "dz_cpuw=%.7f dz_gpuw=%.7f dz_decal=%.7f "
                           "cpuw_fresh=%d cpuw_any=%d cpuw_stamp=%llu "
                           "cpuw_stamp_delta=%lld decal_ok=%d decal_z_approx=0\n",
                           kind,
                           (unsigned long long)L.f,
                           (double)L.md,
                           (double)L.zt, (double)L.zc,
                           (double)L.zg, (double)L.zd,
                           (double)(L.zc - L.zt),
                           (double)(L.zg - L.zt),
                           (double)(L.zd - L.zt),
                           cpuwFresh, L.cAny,
                           (unsigned long long)L.cStamp,
                           cpuStampDelta, decalOk);
                    fflush(stdout);
                    s_prevCpuStamp = L.cStamp;
                };

                if (transition) {
                    if (s_havePrevSteady)
                        emitLine(s_prevSteady, "pre");
                    emitLine(cur, "trans");
                    s_emitTail = 1;  // print 1 steady frame after
                } else if (s_emitTail > 0) {
                    emitLine(cur, "post");
                    --s_emitTail;
                }
                // Always stash the latest (steady or not) for the next
                // transition's retroactive "pre" line.
                s_prevSteady = cur;
                s_havePrevSteady = true;

                // Maintain prev MVP snapshot AFTER the diff (copy current ->
                // prev). s_prevValid gates the first frame (no prior to diff).
                memcpy(g_prevDispatchMvp16, g_dispatchMvp16,
                       sizeof(float) * 16);
                s_prevValid = true;
            }
        }
        // ── end [DEPTH_TRANSITION v1] ──────────────────────────────────────

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

    // ── Probe 7 (MC2_RING_FORCE_FINISH) — directed barrier-hypothesis test ──
    // Forces full GPU pipeline drain after the compute dispatches.
    // If the giant-triangle bug DISAPPEARS with this on, the barrier is
    // insufficient on AMD — VS reads stale thin records via L1$ even though
    // CPU readback sees correct data in VRAM (probe 6 full coverage silent).
    // Severe perf cost (every-frame stall); diagnostic-only, default OFF.
    static const bool s_ringForceFinish = mc2::gate::get<"MC2_RING_FORCE_FINISH">();
    if (s_ringForceFinish) {
        glFinish();
    }

    // ── Probe 3 (MC2_RING_TRACE) — read back the indirect cmd's count after patch ──
    // If a cmd-patch race is occurring under fast rotation, cmd.count may not equal
    // min(visibleCount, kMaxThinRecords) * 6.  Reading it here after the
    // GL_COMMAND_BARRIER_BIT means the GPU has finished cmd-patch; no stall.
    // Cost: one 16-byte readback per frame (one DrawArraysIndirectCommand).
    if (s_ringTrace && g_indirectCmdBuffer != 0) {
        uint32_t cmdQuad[4] = { 0, 0, 0, 0 };
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
        glGetBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, (GLsizeiptr)sizeof(cmdQuad), cmdQuad);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        // Step 2b: with cmd-patch retired the [RING_CMDPATCH] tripwire can no
        // longer recompute expected from hdr.visibleCount unless the bucket-
        // header SSBO is also armed (MC2_BUCKET_HEADER_TRACE).  When it is
        // off, cmdQuad[0] IS the authoritative count (primary's atomicAdd
        // accounting) so there is nothing to cross-check; we still surface
        // the raw cmd values via the existing RING_PEAK summary.
        uint32_t vis = 0;
        uint32_t expectedCount = cmdQuad[0];
        if (g_solidBucketHeaderSsbo != 0) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidBucketHeaderSsbo);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)sizeof(vis), &vis);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            expectedCount =
                (vis < (uint32_t)kMaxThinRecords ? vis : (uint32_t)kMaxThinRecords) * 6u;
        }
        if (g_solidBucketHeaderSsbo != 0 && cmdQuad[0] != expectedCount) {
            static uint32_t s_cmdMismatchCount = 0;
            ++s_cmdMismatchCount;
            PROBE_LOG(
                "[RING_CMDPATCH v1] frame=%llu cmd_count=%u expected=%u "
                "visible=%u inst=%u first=%u base=%u mismatchCount=%u\n",
                (unsigned long long)ringFrameIdx, (unsigned)cmdQuad[0],
                (unsigned)expectedCount, (unsigned)vis,
                (unsigned)cmdQuad[1], (unsigned)cmdQuad[2], (unsigned)cmdQuad[3],
                (unsigned)s_cmdMismatchCount);
        }
    }

    // Step 2b: parity probe retired with cmd-patch (commit 2a's safety net no
    // longer needed once cmd-patch is gone -- there is nothing to compare
    // against).  The surviving MC2_RING_TRACE probe below still reads
    // cmds[0].count for the [RING_CMDPATCH] tripwire; its expected-value
    // calculation now derives from the primary's atomicAdd accounting
    // (see comment in that block).

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
