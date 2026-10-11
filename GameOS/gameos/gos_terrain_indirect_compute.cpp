// gos_terrain_indirect: the ComputeDispatch phases -- program build, ring fence wait and tripwires, counter clears,
// compute bindings and uniforms, the depth-transition probe and the post-dispatch probes. The orchestrator is in
// gos_terrain_indirect_dispatch.cpp.
#include "gos_terrain_indirect_internal.h"
#include "mc2fmt.h"

namespace gos_terrain_indirect {


// Build the solid compute program and cache its uniform locations on first use.
bool computeBuildProgram() {
    // Lazy-build compute programs and cache uniform locations + constant uniforms.
    if (g_solidComputeProgram == 0) {
        g_solidComputeProgram = gpu_driven::BuildComputeProgramFromFile(
            "shaders/gpu_driven_terrain_solid.comp", nullptr, 0, "gpu_driven_terrain_solid");
        if (g_solidComputeProgram == 0) {
            MC2_DIAGFE(stderr, "[TERRAIN_INDIRECT v1] event=error msg=solid_compute_compile_failed\n");
            fflush(stderr);
            ForceDisableArmingForProcess();
            return true;
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
    return false;
}

// Allocate the bucket-header and canary SSBOs the first time they are needed.
void computeAllocDebugBuffers(bool s_bucketHeaderTrace) {
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
}

// Fingerprint the terrain MVP for the ring probe.
void computeFingerprintMvp(bool s_ringTrace, uint32_t& mvpFp) {
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
}

// Poll the ring slot fence without blocking and release it once signalled.
void computeRingFenceWait(int probedSlot, GLenum& waitResult, uint64_t& waitUs) {
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
}

// Report a missing fence past warm-up or a fence timeout.
void computeTripwireMiss(FILE* s_probeSink, uint64_t ringFrameIdx, int probedSlot, bool fencePresent, uint32_t& mvpFp, GLenum& waitResult, uint64_t& waitUs) {
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
}

// Read back the previous bucket header and report the visible-count overshoot probes.
void computeBucketHeaderReadback(FILE* s_probeSink, bool s_ringTrace, uint64_t ringFrameIdx, int probedSlot, bool fencePresent, uint32_t& mvpFp, GLenum& waitResult, uint64_t& waitUs) {
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
            mc2::fmt::print_out("[RING v1] frame={:d} slot={:d} fence={:d} wait=0x{:x} wait_us={:d} prev_visible={:d} near_w={:d} peak={:d} peakNearW={:d} mvpFp=0x{:08x}\n", mc2::fmt::as_u64((unsigned long long)ringFrameIdx), mc2::fmt::as_i32(probedSlot), mc2::fmt::as_i32((int)fencePresent), mc2::fmt::as_u32((unsigned)waitResult), mc2::fmt::as_u64((unsigned long long)waitUs), mc2::fmt::as_u32((unsigned)prevVisible), mc2::fmt::as_u32((unsigned)prevNearW), mc2::fmt::as_u32((unsigned)s_peakVisible), mc2::fmt::as_u32((unsigned)s_peakNearW), mc2::fmt::as_u32((unsigned)mvpFp));
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
        mc2::fmt::print_out("[RING v1] frame={:d} slot={:d} fence={:d} wait=0x{:x} wait_us={:d} mvpFp=0x{:08x}\n", mc2::fmt::as_u64((unsigned long long)ringFrameIdx), mc2::fmt::as_i32(probedSlot), mc2::fmt::as_i32((int)fencePresent), mc2::fmt::as_u32((unsigned)waitResult), mc2::fmt::as_u64((unsigned long long)waitUs), mc2::fmt::as_u32((unsigned)mvpFp));
        fflush(stdout);
    }
}

// Skip the dispatch while the ring slot is still in flight.
bool computeRingSkipGuard(FILE* s_probeSink, uint64_t ringFrameIdx, int probedSlot, GLenum& waitResult) {
    // C-02: if the fence for this slot was not signaled (timeout=0 above), the
    // prior GPU consumer is still reading it.  Skip the dispatch to preserve ring
    // safety; the slot fence is left intact so the next frame can retry.
    if (waitResult == GL_TIMEOUT_EXPIRED || waitResult == GL_WAIT_FAILED) {
        static uint32_t s_ringSkipCount = 0;
        ++s_ringSkipCount;
        PROBE_LOG("[RING_SKIP v1] frame=%llu slot=%d wait=0x%x skip_total=%u\n",
            (unsigned long long)ringFrameIdx, probedSlot,
            (unsigned)waitResult, (unsigned)s_ringSkipCount);
        return true;
    }
    return false;
}

// Zero the indirect command count and the bucket header and order the clear before the dispatch.
void computeClearCounters() {
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
}

// Bind the compute storage buffers and upload the per-frame uniforms.
bool computeBindAndUniforms(GLuint lightSsbo, bool s_bucketHeaderTrace, uint32_t windowCount, int s_solidUseWindow, GLintptr thinSlotOffset) {
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
        return true;
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
    return false;
}

// Print the zoom-step depth probe on the frame the MVP jumps.
void computeDepthTransitionProbe() {
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
                mc2::fmt::print_out("[DEPTH_TRANSITION v1] kind={} f={:d} maxMvpDelta={:.7f} z_terr={:.7f} z_cpuw={:.7f} z_gpuw={:.7f} z_decal={:.7f} dz_cpuw={:.7f} dz_gpuw={:.7f} dz_decal={:.7f} cpuw_fresh={:d} cpuw_any={:d} cpuw_stamp={:d} cpuw_stamp_delta={:d} decal_ok={:d} decal_z_approx=0\n", mc2::fmt::safe(kind), mc2::fmt::as_u64((unsigned long long)L.f), (double)L.md, (double)L.zt, (double)L.zc, (double)L.zg, (double)L.zd, (double)(L.zc - L.zt), (double)(L.zg - L.zt), (double)(L.zd - L.zt), mc2::fmt::as_i32(cpuwFresh), mc2::fmt::as_i32(L.cAny), mc2::fmt::as_u64((unsigned long long)L.cStamp), mc2::fmt::as_i64(cpuStampDelta), mc2::fmt::as_i32(decalOk));
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
}

// Force a GPU drain and read back the command count for the ring probes.
void computePostDispatchProbes(FILE* s_probeSink, bool s_ringTrace, uint64_t ringFrameIdx) {
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
}

}  // namespace gos_terrain_indirect
