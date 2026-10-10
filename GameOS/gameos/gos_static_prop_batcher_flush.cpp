// GpuStaticPropBatcher: GpuStaticPropBatcher::flush (one 2,362-line function).
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"
#include "mdi_submit.h"                          // MDI-SUBMISSION-SCAFFOLD-1: flush issues the indirect draws


// [RENDER_CONTRACT:Pass=StaticProp id=GpuStaticPropBatcher_flush]
//   Routes through static_prop.frag which writes
//   rc_gbuffer1_screenShadowEligible (production) or
//   rc_gbuffer1_legacyDebugSentinelScreenShadowEligible (debug).
void GpuStaticPropBatcher::flush(const RenderSnapshot* snap) {
    // SAME-ORDER-EXECUTOR-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical). PIN INVARIANT: additive only — no GL state change,
    // no reorder. Body sets its own state UNCHANGED between begin and end.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::StaticProp,
                                              "GpuStaticPropBatcher_flush");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::StaticProp,
                                                    "GpuStaticPropBatcher_flush");
        }
    } _tlGuard;
    ZoneScopedN("GpuStaticProps.Flush");
    mc2_hitch::HitchScope _hitchFlush(mc2_hitch::HitchSpanKind::GpuStaticPropsFlush);
    initTraceOnce();
    // LODBUG probe: env-var override for debugAddrMode_.  RAlt+9 cycling is
    // unreliable on some hosts; set MC2_GPU_PROPS_DEBUG_MODE=8 to force the
    // magenta "did this draw call land?" mode at startup.  Applied once
    // per process; the env value wins over any subsequent RAlt+9 cycling
    // for the FIRST flush only, after which the cycler is authoritative
    // again (so the user can still cycle if they want).
    {
        static bool s_dbgEnvInit = false;
        if (!s_dbgEnvInit) {
            s_dbgEnvInit = true;
            if (mc2::gate::isNonEmpty<"MC2_GPU_PROPS_DEBUG_MODE">()) {
                const int m = mc2::gate::get<"MC2_GPU_PROPS_DEBUG_MODE">();
                if (m >= 0 && m <= 8) {
                    debugAddrMode_ = m;
                    std::fprintf(stderr,
                        "[LODBUG v1] event=debug_mode_env_override mode=%d\n", m);
                    std::fflush(stderr);
                }
            }
        }
    }
    // 2026-05-11 perf diag: wall-clock timer for substrate-coalesce perf hunt.
    // Mean us reported every 600 calls when MC2_BATCHER_FLUSH_TIMING=1.
    static uint64_t s_btf_calls = 0;
    static uint64_t s_btf_ns_total = 0;
    static uint64_t s_btf_ns_coalesce_write = 0;
    static uint64_t s_btf_ns_coalesce_draw = 0;
    static const bool s_btf_enabled = mc2::gate::get<"MC2_BATCHER_FLUSH_TIMING">();
    const auto _btf_t0 = std::chrono::steady_clock::now();

    // v2.3 snap-cull: reset per-frame counters unconditionally so batcher_getSnapCullStats()
    // always returns valid (zero) data even when geometry is not yet finalized or runV6 is false.
    s_snapCullSkipped      = 0u;
    s_snapCullActive       = 0u;
    s_snapCullSlotMismatch = 0u;

    if (!s_geometryFinalized || s_fatalRegistrationFailure) {
        s_bucketsByType.clear();
        accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);
        return;
    }
    if (!uploadAllBucketsIfNeeded()) {
        s_bucketsByType.clear();
        accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);
        gos_object_parity::ParityFrameTick();
        return;
    }

    // [RENDER_PASS v1] advisory telemetry (env-gated, rate-limited).
    render_contract::noteRenderPass(render_contract::PassIdentity::StaticProp,
                                    "GpuStaticPropBatcher_flush");
    // Program compile/link latch. submitMultiShape already gates submissions
    // on this, so reaching here with an empty program is a logic bug — but
    // guard anyway so we never pump uniform calls against a null program.
    if (s_programLoadFailed || s_staticPropProgram == 0) {
        s_bucketsByType.clear();
        s_lastUploadedSlot = 0xFFFFFFFFu;
        accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);
        gos_object_parity::ParityFrameTick();
        return;
    }

    batcher_buildCandidateLog();

    // Slice 2 (object-offload) — Stage 2.D.1: parity readback handshake.
    //
    // uploadAllBucketsIfNeeded() above just glClientWaitSync'd on
    // s_fence[s_frameSlot] (the fence from RING_FRAMES frames ago). At
    // this point the parity SSBO writes from that prior cycle are
    // GPU-complete and safe to read on the CPU.
    //
    // 2.D.1 acceptance is "the readback runs without crash or stall" — we
    // glGetBufferSubData the live byte range from this slot into a discard
    // scratch buffer and DROP the bytes. 2.D.2/2.D.3 will swap the discard
    // for a CPU recompute compare.
    //
    // Skips entirely when MC2_OBJECT_PARITY_CHECK is unset
    // (IsParityCheckEnabled() returns false → EnsureParityOutputSSBO()
    // returns 0 → the bind/uniform/readback all short-circuit). This is
    // the load-bearing default-off contract per the advisor.
    GLuint parityBuffer = 0;
    if (gos_object_parity::IsParityCheckEnabled()) {
        parityBuffer = gos_object_parity::EnsureParityOutputSSBO(kParitySlotBytes);
        if (parityBuffer != 0) {
            const size_t bytesToRead = s_parityBytesUsedPerSlot[s_frameSlot];
            if (bytesToRead > 0) {
                if (s_parityReadbackScratch.size() < bytesToRead) {
                    s_parityReadbackScratch.resize(bytesToRead);
                }
                const GLintptr slotBase =
                    static_cast<GLintptr>(s_frameSlot) *
                    static_cast<GLintptr>(kParitySlotBytes);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, parityBuffer);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                                   slotBase,
                                   static_cast<GLsizeiptr>(bytesToRead),
                                   s_parityReadbackScratch.data());
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                gos_object_parity::Counters_AddReadbackBytesThisFrame(
                    static_cast<uint64_t>(bytesToRead));
                // Stage 2.D.2: if the latch is WaitingForReadback, compare
                // the GPU bytes against the CPU snapshot captured on frame N.
                // After compare, advance latch to Done and free the snapshot.
                // The fence wait above (glClientWaitSync in
                // uploadAllBucketsIfNeeded) guarantees GPU writes are complete.
                // Stage 2.D.2: compare only when the ring has returned to the
                // exact slot the Armed frame wrote to (IsDualEmitReadyForSlot
                // checks both state==WaitingForReadback AND slot match).
                if (gos_object_parity::IsDualEmitReadyForSlot(s_frameSlot)) {
                    gos_object_parity::CompareAndReport(
                        s_parityReadbackScratch.data(),
                        bytesToRead,
                        static_cast<unsigned>(s_counters.frame_count));
                    gos_object_parity::AdvanceDualEmitToDone();
                }
            }
        }
        // Reset this slot's usage; it will be re-populated by the per-type
        // bind loop below.
        s_parityBytesUsedPerSlot[s_frameSlot] = 0;
    }
    s_parityBytesUsedThisFrame = 0;

    // Save binding state (program/VAO/SSBO/texture) for restore at exit.
    // Pipeline state (depth/blend/cull) is set by applyPipeline() below and
    // NOT restored: gos_InvalidateRenderStateCache() at exit ensures the next
    // applyRenderStates() re-applies from scratch.
    // GLSTATE-GUARD-SITE5: capture-only guards replace the hand-rolled
    // prevProgram/prevVao/prevArrayBuf/prevElemBuf/prevActiveTex/prevTexUnit0/
    // prevSsbo{0,1,2,3,5} save + restore. gos_InvalidateRenderStateCache() is
    // called later in this function — ~30 lines of unrelated bookkeeping
    // (parity tick, sample pick, bucket-map clear) run between the original
    // manual-restore point and that call — so, mirroring the
    // GpuMechBatcher::flush() SSBO 0/1/2 precedent (gos_mech_batcher.cpp
    // ~2167/2212, GLSTATE-GUARD-ADOPTION-3), every guard here is wrapped in
    // std::optional and .reset() explicitly at the ORIGINAL manual-restore
    // point below, instead of at this scope's natural end. Slice 2
    // (object-offload) Stage 2.C.2/2.D.1: SSBO slots 2/3 covered too
    // (per-type hot-color / parity readback).
    std::optional<mc2gl::GlScopedProgram>       guardProgram;
    std::optional<mc2gl::GlScopedVertexArray>   guardVao;
    std::optional<mc2gl::GlScopedBufferBinding> guardArrayBuf;
    std::optional<mc2gl::GlScopedBufferBinding> guardElemBuf;
    std::optional<mc2gl::GlScopedTextureUnit>   guardTexUnit0;
    std::optional<mc2gl::GlScopedSsboBinding>   guardSsbo0;
    std::optional<mc2gl::GlScopedSsboBinding>   guardSsbo1;
    std::optional<mc2gl::GlScopedSsboBinding>   guardSsbo2;
    std::optional<mc2gl::GlScopedSsboBinding>   guardSsbo3;
    std::optional<mc2gl::GlScopedSsboBinding>   guardSsbo5;
    guardProgram.emplace();
    guardVao.emplace();
    guardArrayBuf.emplace(GL_ARRAY_BUFFER);
    guardElemBuf.emplace(GL_ELEMENT_ARRAY_BUFFER);
    guardTexUnit0.emplace(0);   // ctor also leaves GL_TEXTURE0 active (matches the old explicit glActiveTexture(GL_TEXTURE0) here)
    guardSsbo0.emplace(0);
    guardSsbo1.emplace(1);
    guardSsbo2.emplace(2);
    guardSsbo3.emplace(3);
    guardSsbo5.emplace(5);
    // Bind program + depth/blend/cull via PipelineDesc.
    // Uses StaticPropOpaque — both alpha and opaque packets share the same
    // program and fixed-function state; the alpha distinction is texture-array
    // selection and shader discard, not a different GL pipeline state.
    // applyPipeline: binds program, depth (test+write+func=GL_GEQUAL), blend, cull.
    // PipelineDesc v1: DepthFunc::GreaterEqual encoded in the table row — no
    // explicit glDepthFunc() call needed here any more.
    // APPLY-STATE-STATICPROP-1: third top-level scene-pass apply-state. Gated by the
    // SAME predicate executorOwnBeginTopLevel uses internally (MC2_FRAMEGRAPH_EXECUTOR).
    // Dispatched here (after the early-return guards, immediately before the body bind)
    // so the executor apply only fires when the COLOR pass actually reaches its bind —
    // exactly the body's applyPipeline call lifted up, no double-count. Idempotent ON.
    // ★ Unlike decal/overlay, StaticPropOpaque runs every tier1 frame, so this body-skip
    // path is genuinely exercised at runtime.
    if (render_contract::isTopLevelExecutorEnabled() &&
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::StaticPropOpaque) != nullptr) {
        executorApplyStaticPropOpaqueState();
    }
    // APPLY-STATE-STATICPROP-1: skip when the executor already applied the pipeline
    // (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw. Gate OFF -> flag false ->
    // body applies -> byte-identical. The separate StaticPropDepth prepass is untouched.
    if (!s_staticPropStateAppliedByExecutor) {
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropOpaque),
        "StaticPropOpaque");  // STATIC-PROP-COLOR-LABEL-HARDENING-1: label the live color bind
    }
    s_staticPropStateAppliedByExecutor = false;
    render_frame_plan::trace(render_frame_plan::Phase::StaticOpaque, "StaticPropOpaque",
        render_frame_plan::PathKind::Batcher, -1, "StaticPropOpaque");
    // MDI-SUBMISSION-SCAFFOLD-1: flush-level submission self-report. Per-variant command
    // counts (s_alphaOff/OnCmdCount, BC7 buckets) and the CPU fallback are computed later
    // in this function; this names the static-prop indirect submitter family (live default
    // = GPU-produced coalesce v6 reading gpu_cull's indirect buffer). commands=-1 here
    // (not yet known at bind time); the [STATIC_PROP]/[DRAW_PACKET_V6] traces carry counts.
    mdi_submit::trace("StaticPropOpaque", "MultiDrawElementsIndirect", -1,
                      "GPU", "gpu_cull_indirect", false);
    glBindVertexArray(s_sharedVao);
    // Explicit IBO rebind: GL_ELEMENT_ARRAY_BUFFER is VAO state and can be
    // left at 0 if flushShadow() or any other pass clobbers s_sharedVao's
    // element-buffer slot (e.g. wrong restore order while s_sharedVao was
    // active). Matching the explicit-bind pattern in flushShadow() makes
    // flush() self-healing regardless of prior VAO state. Belt-and-suspenders.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    // Slice 2 (object-offload) — Stage 2.C.2: bind per-type hot-color SSBO
    // once for the whole flush. The data is per-map immutable so a single
    // bind covers every typeID in the per-type loop below — static_prop.vert
    // indexes by inst.typeID. Restored at the bottom alongside slots 0+1.
    if (s_perTypeSsbo) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_perTypeSsbo);
    }

    // Slice 2 (object-offload) — Stage 2.D.1: parity write defaults OFF.
    // Per-type loop below flips it to 1 only for draws whose bound
    // parity-buffer range fits in this slot's remaining byte budget.
    // When MC2_OBJECT_PARITY_CHECK is unset, parityBuffer == 0 and the
    // uniform stays 0 for every draw (no shader writes happen).
    // Stage 2.D.1.1 (Item 2): use link-time cached locations instead of
    // per-flush glGetUniformLocation (one driver round-trip per flush saved).
    const GLint locParityWrite      = s_loc_u_parityWrite;
    const GLint locParityVerts      = s_loc_u_parityVertsPerType;
    const GLint locParityBaseVertex = s_loc_u_parityBaseVertex;
    if (locParityWrite >= 0) glUniform1i(locParityWrite, 0);

    // Stage 2.D.2: zero the Armed frame's parity SSBO slot before drawing so
    // that back-facing vertices (GPU-culled by GL_CULL_FACE GL_BACK, not
    // written by the shader) read as zero on the compare frame — matching the
    // zero-initialized CPU snapshot (vector initialized to 0u in submit()).
    // Without this clear, stale uint32 values from RING_FRAMES ago remain in
    // the slot for unwritten (back-facing) positions and cause false mismatches
    // against the CPU's 0x00000000 for those same positions.
    if (parityBuffer != 0 && gos_object_parity::IsDualEmitArmedAnyActor()) {
        const GLintptr slotBase =
            static_cast<GLintptr>(s_frameSlot) *
            static_cast<GLintptr>(kParitySlotBytes);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, parityBuffer);
        const GLuint zero = 0u;
        glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI,
                             slotBase,
                             static_cast<GLsizeiptr>(kParitySlotBytes),
                             GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // C1b: when compute cull is enabled and the indirect command buffer is
    // ready, write the per-type CPU instance counts into the indirect buffer
    // before the draw loop (shadow mode: CPU counts match what GPU will see
    // once C1b->C1c flips GPU-driven counts ON). Then the draw loop uses
    // glDrawElementsIndirect (one draw per type) instead of the instanced call.
    //
    // State setup for C1b: three inheritance traps (gpu_direct_renderer_bringup_checklist.md):
    //   1. Sampler state: bind REPEAT/LINEAR before first C1b draw (below).
    //   2. Depth state:   glEnable(GL_DEPTH_TEST) + GL_GEQUAL (reverse-Z) — set by applyPipeline() above.
    //   3. Blend state:   glDisable(GL_BLEND) — set by applyPipeline() above.
    // Verified: depth and blend are set unconditionally by applyPipeline() regardless of C1b path.
    // C1B-DRAW-FIX: glDrawElementsIndirect uses GPU bucket count (how many visible by
    // count) but draws the FIRST N instances in SSBO registration order — NOT the
    // actually-visible N instances identified by visibleIds[]. When the camera moves,
    // the GPU cull count changes and instances at high SSBO indices (e.g. center-screen
    // props registered late) get cut off even though they ARE visible. The visibleIds
    // compaction path (vertex shader reading visibleIds[gl_BaseInstance+gl_InstanceID])
    // is not wired to the per-type legacy draw loop, so the C1b indirect count is
    // architecturally incorrect here. Use CPU instance count (r.instanceCount) always.
    // The GPU cull still runs (bucket counts → readback/lifecycle), just not for draw.
    // Kill-switch: MC2_STATIC_C1B_DRAW=1 re-enables the (buggy) GPU indirect count.
    static const bool s_c1bDrawOverride = mc2::gate::get<"MC2_STATIC_C1B_DRAW">();
    const bool useC1bIndirect = s_c1bDrawOverride &&
                                gpu_cull::compute_isEnabled() &&
                                (gpu_cull::compute_getIndirectCmdBuf() != 0) &&
                                (gpu_cull::compute_getBucketCount() == s_types.size());

    // C1b GPU authority flip: compute_dispatch() now runs BEFORE flush() and writes
    // GPU-computed per-type instanceCounts via the patch shader. The CPU overwrite
    // below is REMOVED — glMultiDrawElementsIndirect reads GPU-authoritative counts.
    //
    // Shadow mode is no longer needed: static prop substrate records are emitted by
    // GpuStaticPropRegistry::flush() with category = Cat_StaticProp | (typeID<<4),
    // so the cull shader correctly scatters each instance into its typeID bucket.
    //
    // Invariant: compute_dispatch() → GL_COMMAND_BARRIER_BIT → flush() ordering is
    // guaranteed by txmmgr.cpp (registry::flush → compute_dispatch → batcher::flush).
    // The barrier in compute_dispatch() after the patch shader covers GL_COMMAND_BARRIER_BIT.
    //
    // The CPU-overwrite block (shadow mode) is intentionally REMOVED here.
    // If visual corruption occurs, re-enable by reverting this comment block to the
    // original glBufferSubData loop (see git history for the shadow mode code).

    // =======================================================================
    // Substrate Multi-Draw Coalesce — flush() extensions (plan v3.8 Step 11).
    //
    // The CPU write loop below (11.2) and the draw branch later (11.7) form
    // the runtime side of the coalesce path. Per-frame write happens
    // unconditionally if coalesce is armed (legacy s_instanceSsbo write
    // already happened above via uploadAllBucketsIfNeeded — both buffers
    // get filled so a runtime-disarm transition leaves legacy with valid
    // data). The legacy DRAW loop below is gated on !IsCoalesceEnabled()
    // by Step 11.7 (coalesce armed) vs Step 11.8 (coalesce off).
    // =======================================================================

    // Step 11.1 — unconditional coalesce fence cleanup. Runs whether or
    // not coalesce is currently armed: out-MAJ-2 ensures fences from
    // disarmed-mid-mission frames drain. Otherwise outstanding GLsync
    // objects leak until onMapUnload (or longer). Per Step 11.1
    // first-flush latch: do NOT set s_coalesceFirstFlushDone here — the
    // latch is for first ARMED flush (placed in Step 11.7.k).
    // Step 4.4 — legacy mode only: wait on coalesce fence here.
    // Non-legacy mode: prepare-table already waited and cleared this fence;
    // skip to avoid a double-wait on the same sync object.
    if (s_globalPoolLegacy && s_coalesceFence[s_coalesceFrameSlot]) {
        MC2_GL_ClientWaitSync(s_coalesceFence[s_coalesceFrameSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT,
                         GL_TIMEOUT_IGNORED);
        glDeleteSync(s_coalesceFence[s_coalesceFrameSlot]);
        s_coalesceFence[s_coalesceFrameSlot] = nullptr;
    }

    // Steps 11.2 / 11.3 / 11.4 / 11.5 — coalesce per-frame CPU write loop.
    // Step 4.7 — split into global-pool (sorted prefix-sum) and legacy (per-type-cap) branches.
    const auto _btf_t_writeStart = std::chrono::steady_clock::now();
    if (IsCoalesceEnabled() && !s_globalPoolLegacy) {
        // Global-pool path: iterate s_sortedTypeOrder (same order as
        // batcher_prepareBaseInstanceTable) and write instances tightly packed.
        // Uses s_coalesceFrameSlot — the coalesce ring, advanced in prepare-table.
        const size_t fr_off_bytes =
            static_cast<size_t>(s_coalesceFrameSlot) * batcher_getCoalescePerFrameInstanceBytes();
        uint8_t* coalesceMapBase =
            static_cast<uint8_t*>(s_coalesceInstanceMap) + fr_off_bytes;

        std::array<uint32_t, 2> groupCursor = {0u, 0u};
        for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
            const uint32_t typeID = s_sortedTypeOrder[i];
            if (typeID >= s_types.size()) continue;
            auto kvIt = s_bucketsByType.find(typeID);
            if (kvIt == s_bucketsByType.end()) continue;
            const PerTypeBucket& bucket = kvIt->second;
            GpuStaticPropType& type = s_types[typeID];

            // Peak tracker — runs unconditionally so slice-2 GPU-emit can read peak[t].
            if (typeID >= s_perTypePeak.size()) s_perTypePeak.resize(typeID + 1, 0u);
            const uint32_t cnt = static_cast<uint32_t>(bucket.instances.size());
            if (cnt > s_perTypePeak[typeID]) s_perTypePeak[typeID] = cnt;

            // Overflow gate is dead under global-pool mode: detected in prepare-table.

            // Eviction-detect (adapted from legacy loop — same logic, loop-local var names).
            const TG_TypeShape* src = type.source;
            if (src && src->listOfTextures && type.packetCount > 0u &&
                type.lastSeenGosHandle != 0xFFFFFFFFu) {
                const auto& firstPkt = s_packets[type.firstPacket];
                if (firstPkt.textureSlot < src->numTextures) {
                    const DWORD now = src->listOfTextures[firstPkt.textureSlot].gosTextureHandle;
                    if (now != type.lastSeenGosHandle) {
                        MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=tex_evicted "
                                     "type=%u old_handle=%lu new_handle=%lu\n",
                                     typeID,
                                     (unsigned long)type.lastSeenGosHandle,
                                     (unsigned long)now);
                        s_coalesceArmed = false;
                        break;
                    }
                }
            }

            // Prefix-sum write: all types of each group are packed contiguously.
            const uint32_t group = type.alphaClass;
            const uint32_t typeBaseInstance = groupCursor[group]
                                            + (group == 1u ? s_offGroupCountThisFrame : 0u);
            uint8_t* dst = coalesceMapBase + typeBaseInstance * sizeof(GpuStaticPropInstance);
            std::memcpy(dst, bucket.instances.data(),
                        bucket.instances.size() * sizeof(GpuStaticPropInstance));
            groupCursor[group] += static_cast<uint32_t>(bucket.instances.size());
        }
    }



    if (IsCoalesceEnabled() && s_globalPoolLegacy) {
        // Legacy per-type-cap path (unchanged from substrate-coalesce). Uses s_coalesceFrameSlot
        // (which mirrors s_frameSlot under legacy mode — set at the :2644 advance site).
        const size_t fr_off_bytes =
            static_cast<size_t>(s_coalesceFrameSlot) * batcher_getCoalescePerFrameInstanceBytes();
        uint8_t* coalesceMapBase =
            static_cast<uint8_t*>(s_coalesceInstanceMap) + fr_off_bytes;

        for (auto& kv : s_bucketsByType) {
            const uint32_t typeID = kv.first;
            const PerTypeBucket& bucket = kv.second;
            if (typeID >= s_types.size()) continue;
            GpuStaticPropType& type = s_types[typeID];

            // Peak tracker.
            if (typeID >= s_perTypePeak.size()) s_perTypePeak.resize(typeID + 1, 0u);
            const uint32_t cnt = static_cast<uint32_t>(bucket.instances.size());
            if (cnt > s_perTypePeak[typeID]) s_perTypePeak[typeID] = cnt;

            // Per-type overflow guard.
            if (bucket.instances.size() > type.instanceCap) {
                MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=type_overflow "
                             "type=%u count=%zu cap=%u\n",
                             typeID, bucket.instances.size(), type.instanceCap);
                s_coalesceArmed = false;
                break;
            }

            // Eviction-detect.
            const TG_TypeShape* src = type.source;
            if (src && src->listOfTextures && type.packetCount > 0u &&
                type.lastSeenGosHandle != 0xFFFFFFFFu) {
                const auto& firstPkt = s_packets[type.firstPacket];
                if (firstPkt.textureSlot < src->numTextures) {
                    const DWORD now = src->listOfTextures[firstPkt.textureSlot].gosTextureHandle;
                    if (now != type.lastSeenGosHandle) {
                        MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=tex_evicted "
                                     "type=%u old_handle=%lu new_handle=%lu\n",
                                     typeID,
                                     (unsigned long)type.lastSeenGosHandle,
                                     (unsigned long)now);
                        s_coalesceArmed = false;
                        break;
                    }
                }
            }

            // Per-type-cap memcpy into group-relative slot.
            const size_t groupBase_bytes =
                (type.alphaClass == 1u) ? s_offGroupTotalBytes : 0u;
            uint8_t* dst = coalesceMapBase
                         + groupBase_bytes
                         + type.coalesceByteOffsetWithinGroup;
            std::memcpy(dst,
                        bucket.instances.data(),
                        bucket.instances.size() * sizeof(GpuStaticPropInstance));
        }
    }
    // Step 11.6 — legacy CPU write path is unchanged: it already ran via
    // uploadAllBucketsIfNeeded() above (which filled s_instanceSsbo). No
    // gating on IsCoalesceEnabled() — that lets a runtime-disarm transition
    // mid-frame still draw legally via the legacy loop below.
    const auto _btf_t_drawStart = std::chrono::steady_clock::now();
    s_btf_ns_coalesce_write += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            _btf_t_drawStart - _btf_t_writeStart).count());

    // Per-type drawing: bind per-type instance & color SSBO ranges, then
    // issue one instanced draw per packet. gl_InstanceID in the shader
    // addresses 0..N-1 within the bound range (no gl_BaseInstance needed).
    //
    // [INSTDIAG] one-shot path-state log so we know which draw path is active on the
    // tester's NVIDIA (and therefore whether the per-draw [INSTDIAG] below fires from the
    // legacy loop or whether the coalesce/MDI path is in play instead).
    {
        static const bool s_instDiag = mc2::gate::get<"MC2_STATIC_INST_DIAG">();
        static bool s_pathLogged = false;
        if (s_instDiag && !s_pathLogged) {
            s_pathLogged = true;
            fprintf(stderr,
                "[INSTDIAG] PATH coalesceEnabled=%d globalPoolLegacy=%d instanceCapacity=%u "
                "frameSlot=%u typeRanges=%zu\n",
                (int)IsCoalesceEnabled(), (int)s_globalPoolLegacy,
                (unsigned)s_instanceCapacity, (unsigned)s_frameSlot,
                s_typeRanges.size());
            fflush(stderr);
        }
    }

    // Plan v3.8 Step 11.7 / 11.8 — coalesce branch first; on disarm,
    // fall through to the legacy per-type loop with no other state change.
    if (IsCoalesceEnabled()) {
        // 2026-05-11 per-packet alpha-test re-resolve. s_perDrawSsbo's
        // materialFlags is baked at finalizeGeometry time. Damage-shape
        // textures aren't loaded until the moment of destruction (bdactor
        // setObjStatus runs the texture-load + SetTextureAlpha(true) loop
        // on demand) — so at finalize, leaf-level `textureAlpha` is false
        // and the bake leaves materialFlags=0 → no alpha-test discard.
        // After destruction, the leaf-propagation fix in
        // TG_TypeMultiShape::SetTextureAlpha (msl.cpp) updates every leaf
        // TG_TypeShape's TG_TinyTexture; this loop refreshes the SSBO so
        // the shader sees the live bit. Cost: ~packet count entries × OR-
        // reduce + glBufferSubData when changed (~10 µs/frame in mc2_10).
        {
            static std::vector<int32_t> s_lastSeenMaterialFlags;
            const size_t pktCount = s_sortedPacketOrder.size();
            if (s_lastSeenMaterialFlags.size() != pktCount) {
                s_lastSeenMaterialFlags.assign(pktCount, -1);
            }
            // SP-BATCHER-ALPHASCAN-GATE-1: the per-packet OR-reduce scan below is
            // frame-constant except immediately after a destruction/damage event
            // (which flips a type's texture alpha via TG_TypeMultiShape::SetTextureAlpha
            // and bumps g_staticPropAlphaGeneration). When the gate is armed and the
            // generation is unchanged since the last scan AND the packet count is
            // unchanged (buffer layout stable), skip the whole scan — the SSBO already
            // holds the correct flags from the last event. Default OFF => scan always
            // runs (byte-identical). First scan always runs (cached gen 0 != live >=1).
            static const bool s_alphaScanGate = mc2::gate::get<"MC2_SP_ALPHASCAN_GATE">();
            static uint64_t s_lastScannedAlphaGen = 0;
            static size_t   s_lastScannedPktCount = (size_t)-1;
            const uint64_t liveAlphaGen = g_staticPropAlphaGeneration.load(std::memory_order_relaxed);
            const bool alphaScanSkip = s_alphaScanGate &&
                s_lastScannedAlphaGen == liveAlphaGen &&
                s_lastScannedPktCount == pktCount;
            if (!alphaScanSkip) {
                s_lastScannedAlphaGen = liveAlphaGen;
                s_lastScannedPktCount = pktCount;
                bool anyChanged = false;
                for (size_t i = 0; i < pktCount; ++i) {
                    const uint32_t globalPktIdx = s_sortedPacketOrder[i];
                    if (globalPktIdx >= s_packets.size()) continue;
                    const auto& pkt = s_packets[globalPktIdx];
                    int32_t flags = (pkt.materialFlags & STATIC_PROP_FLAG_ALPHA_TEST)
                                    ? STATIC_PROP_FLAG_ALPHA_TEST : 0;
                    if (flags == 0 && pkt.owningTypeID < s_types.size()) {
                        const TG_TypeShape* src = s_types[pkt.owningTypeID].source;
                        if (src && src->listOfTextures &&
                            pkt.textureSlot < src->numTextures &&
                            src->listOfTextures[pkt.textureSlot].textureAlpha) {
                            flags = STATIC_PROP_FLAG_ALPHA_TEST;
                        }
                    }
                    if (s_lastSeenMaterialFlags[i] != flags) {
                        s_lastSeenMaterialFlags[i] = flags;
                        anyChanged = true;
                    }
                }
                if (anyChanged && s_perDrawSsbo) {
                    const size_t entryStride = sizeof(PerDrawEntry);
                    const size_t flagOffset  = offsetof(PerDrawEntry, materialFlags);
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_perDrawSsbo);
                    for (size_t i = 0; i < pktCount; ++i) {
                        const int32_t f = s_lastSeenMaterialFlags[i];
                        MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER,
                                        static_cast<GLintptr>(i * entryStride + flagOffset),
                                        sizeof(int32_t), &f);
                    }
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                }
            }
        }
        // ---- Step 11.7 coalesce draw branch ----
        // Step 4.8 — use s_coalesceFrameSlot (coalesce ring slot).
        const size_t fr_off_bytes_d =
            static_cast<size_t>(s_coalesceFrameSlot) * batcher_getCoalescePerFrameInstanceBytes();
        // Legacy mode: separate off/on group byte boundaries.
        // Global-pool mode: single contiguous range covering both groups.
        const size_t off_total_bytes = s_globalPoolLegacy
            ? s_offGroupTotalBytes
            : s_offGroupBytesThisFrame;
        const size_t on_total_bytes  = s_globalPoolLegacy
            ? (batcher_getCoalescePerFrameInstanceBytes() - s_offGroupTotalBytes)
            : s_onGroupBytesThisFrame;

        // 11.7.a — save SSBO slot 4 + unit-0 GL_TEXTURE_BINDING_2D_ARRAY.
        // Active texture is already GL_TEXTURE0 (set by prologue at line
        // ~2192) and every mid-block excursion below (kColormapTexUnit,
        // kOrmTexUnit) returns to GL_TEXTURE0 before falling through here —
        // verified by inspection, so GlScopedTextureUnit's default unit=0
        // capture/restore is behavior-identical to the old manual pair.
        // Slot 15 is NOT saved here: the draw branch never binds it; Step
        // 10.3 envelope in compute_dispatch() honors the spec §3.X / §9
        // binding-hygiene contract for slot 15.
        // GLSTATE-GUARD-SITE5: both restore-previous, so both are
        // guard-able; wrapped in std::optional and .reset() explicitly at
        // the original 11.7.j restore point (this nested block's nearest
        // enclosing gos_InvalidateRenderStateCache() is the outer flush()
        // epilogue's, well past this block's own natural close).
        std::optional<mc2gl::GlScopedSsboBinding> guardSsbo4;
        std::optional<mc2gl::GlScopedTextureUnit> guardTex2DArray;
        guardSsbo4.emplace(4);
        guardTex2DArray.emplace(0, GL_TEXTURE_2D_ARRAY);

        // 11.7.b — legacy prologue at lines ~2192-2247 ran unconditionally
        // (slot 2 perTypeSsbo bind inherited). Verified by inspection.

        // 11.7.c — program already bound by applyPipeline() above (v1: PipelineRegistry
        // holds s_staticPropProgramCoalesce after loadProgramsIfNeeded; no explicit
        // glUseProgram needed here).

        // 11.7.d — upload shared uniforms to the coalesce program. Source
        // values match the existing legacy upload sites; -1 cached
        // locations are skipped (legacy-only uniforms removed under
        // MC2_COALESCE).
        // [MVP_DIAG v1] S2.7 — null-guard the coalesce MVP upload to match
        // the legacy path at line ~3284. RenderDoc capture (frame 488, EID
        // 7714) showed gl_Position = NaN/0 with $Globals.terrainMVP = all
        // zeros while the legacy path's u_mvp uploaded correctly; AMD
        // driver appears to zero the uniform when handed nullptr. Capture
        // pointer once, branch on it. Probe logs row0 (or "null") throttled
        // to frames {1,5,30,120} to keep silent at steady state.
        {
            extern long g_mvpDiagFrame;
            const float* mvpPtr = gos_GetObjectFixBMVPMat4();
            if (g_mvpDiagFrame == 1 || g_mvpDiagFrame == 5 ||
                g_mvpDiagFrame == 30 || g_mvpDiagFrame == 120) {
                if (mvpPtr) {
                    MC2_DIAGFE(stderr,
                            "[MVP_DIAG v1] event=coalesce_upload frame=%ld loc=%d ptr=%p row0=[%g %g %g %g]\n",
                            g_mvpDiagFrame, (int)s_locsCoalesce.terrainMVP,
                            (void*)mvpPtr, mvpPtr[0], mvpPtr[1], mvpPtr[2], mvpPtr[3]);
                } else {
                    MC2_DIAGFE(stderr,
                            "[MVP_DIAG v1] event=coalesce_upload frame=%ld loc=%d ptr=%p row0=null\n",
                            g_mvpDiagFrame, (int)s_locsCoalesce.terrainMVP, (void*)mvpPtr);
                }
                fflush(stderr);
            }
            if (s_locsCoalesce.terrainMVP >= 0 && mvpPtr)
                glUniformMatrix4fv(s_locsCoalesce.terrainMVP, 1, GL_FALSE, mvpPtr);
        }
        if (s_locsCoalesce.mvp             >= 0)
            glUniformMatrix4fv(s_locsCoalesce.mvp,           1, GL_TRUE,  gos_GetProj2ScreenMat4());
        if (s_locsCoalesce.fogValue        >= 0)
            glUniform1f       (s_locsCoalesce.fogValue,      1.0f);
        if (s_locsCoalesce.debugAddrMode   >= 0)
            glUniform1i       (s_locsCoalesce.debugAddrMode, debugAddrMode_);
        // V-AMBIENT-STATIC-1: hemisphere ambient fill strength. Default 0.0
        // (env unset or =0) -> shader hemisphere term contributes vec3(0) ->
        // byte-identical to pre-slice output. =1 -> 1.0 -> visible subtle
        // fill in shadowed faces. Skipped for window-flag nodes inside .vert.
        if (s_locsCoalesce.ambientV1Strength >= 0)
            glUniform1f       (s_locsCoalesce.ambientV1Strength,
                               staticPropAmbientV1Enabled() ? 1.0f : 0.0f);
        // PROP-BASE-LIGHT-DIR-1: directional modulation of the baked seed.
        // 0.0 (default) -> shader's `> 0.0` guard skips the block entirely,
        // so this is a no-op stronger than a multiply-by-one: the code path
        // never runs and the pixel is byte-identical.
        if (s_locsCoalesce.propBaseLightDirStrength >= 0)
            glUniform1f       (s_locsCoalesce.propBaseLightDirStrength,
                               s_propBaseLightDirStrength);
        // TERRAIN-DECAL-FILL-1: cliff-decal shadow-side ambient floor. Uploaded
        // globally but the frag only applies it to fragments whose instance
        // carries kFlagDecalFill (bit 3) — set solely on MarbleCliff decal
        // instances. Other props are byte-identical regardless of this value.
        if (s_locsCoalesce.terrainDecalFill >= 0)
            glUniform1f       (s_locsCoalesce.terrainDecalFill, g_terrainDecalFill);
        // TERRAIN-DECAL-COLORBLEND-1: bind the terrain colormap atlas + atlas-UV
        // params so the flagged cliff decal can pull its albedo toward the
        // surrounding terrain colour. Only touches GL texture state when the
        // feature is active (blend>0); with blend==0 (default) we still upload
        // the 0.0 scalar so the shader's flag-gated branch is a no-op and every
        // prop stays byte-identical.
        if (s_locsCoalesce.terrainDecalColorBlend >= 0)
            glUniform1f       (s_locsCoalesce.terrainDecalColorBlend, g_terrainDecalColorBlend);
        // BUILDING-GLASS-1: dither-glass translucency toggle (default ON,
        // MC2_SP_GLASS_DITHER=0 -> 0.0 -> legacy binary discard in frag).
        if (s_locsCoalesce.glassDither >= 0)
            glUniform1f       (s_locsCoalesce.glassDither,
                               s_glassDitherEnabled ? 1.0f : 0.0f);
        if (g_terrainDecalColorBlend > 0.0f) {
            extern GLuint gos_terrain_indirect_getAtlasGLTex();
            extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
            extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
            extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
            glActiveTexture(GL_TEXTURE0 + kColormapTexUnit);
            glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());
            glActiveTexture(GL_TEXTURE0);
            if (s_locsCoalesce.terrainColormap >= 0)
                glUniform1i(s_locsCoalesce.terrainColormap, static_cast<GLint>(kColormapTexUnit));
            if (s_locsCoalesce.decalAtlasTLX >= 0)
                glUniform1f(s_locsCoalesce.decalAtlasTLX, gos_terrain_indirect_getAtlasMapTopLeftX());
            if (s_locsCoalesce.decalAtlasTLY >= 0)
                glUniform1f(s_locsCoalesce.decalAtlasTLY, gos_terrain_indirect_getAtlasMapTopLeftY());
            if (s_locsCoalesce.decalAtlasOOW >= 0)
                glUniform1f(s_locsCoalesce.decalAtlasOOW, gos_terrain_indirect_getAtlasOneOverWorldUnits());
        }
        // V-MATERIAL-DEBUG-1: per-frag material debug view. Default 0 = OFF;
        // shader skips entire debug branch when uniform == 0 (byte-identical
        // pixel invariant — proof at static_prop.frag `if (u_debugMaterialMode != 0)`).
        if (s_locsCoalesce.debugMaterialMode >= 0)
            glUniform1i       (s_locsCoalesce.debugMaterialMode,
                               s_staticPropDebugMaterialMode);
        // LIGHTING-DEBUG-VIEWS-1B: unified lighting debug channel (40-series).
        // Resolver returns -1 when MC2_LIGHTING_DEBUG_VIEW is unset/unknown ->
        // upload 0 -> shader skips all channels (pixel-invariant default).
        {
            extern int mc2LightingDebugMode();
            int lvm = mc2LightingDebugMode();
            glUniform1i(glGetUniformLocation(s_staticPropProgramCoalesce, "u_lightingDebugView"),
                        lvm < 0 ? 0 : lvm);
        }
        // MC2_SHADER_PATH_TINT: solid-color path-id debug (default 0 = OFF).
        if (s_locsCoalesce.pathTint >= 0)
            glUniform1i       (s_locsCoalesce.pathTint, mc2ShaderPathTint());
        // GREYBEARD-DIFFUSE-TEST: upload raw MC2 terrain sun for forced diffuse.
        if (s_locsCoalesce.terrainSunMC2 >= 0) {
            extern void gos_GetTerrainLightDir(float*, float*, float*);
            float tsx = 0.0f, tsy = 0.0f, tsz = 1.0f;
            gos_GetTerrainLightDir(&tsx, &tsy, &tsz);
            glUniform3f(s_locsCoalesce.terrainSunMC2, tsx, tsy, tsz);
        }
        // EDGE-LIGHT-EMISSION-1: stock hot-colour NIGHT tag state. Gate OFF ->
        // 0/0/0 uploaded -> get_base_light takes its daytime arms -> render is
        // byte-identical to the pre-fix build.
        if (s_locsCoalesce.isNight >= 0 || s_locsCoalesce.nightFactor >= 0) {
            bool  nIsNight = false;
            float nFactor  = 0.0f;
            // BEACON-FIXTURE-ID-1: the fixture gate also needs real night state.
            // Safe for non-fixtures: their scale stays 0.0, so get_base_light
            // keeps its daytime result (delta * 0 == 0).
            if (s_nightTagMode != 0 || s_fixtureLightsScale > 0.0f ||
                    s_fixtureGlowStrength > 0.0f) {
                extern void gos_GetNightState(bool*, float*);
                gos_GetNightState(&nIsNight, &nFactor);
            }
            if (s_locsCoalesce.isNight >= 0)
                glUniform1i(s_locsCoalesce.isNight, nIsNight ? 1 : 0);
            if (s_locsCoalesce.nightFactor >= 0)
                glUniform1f(s_locsCoalesce.nightFactor, nFactor);
            if (s_locsCoalesce.nightEmissiveScale >= 0)
                glUniform1f(s_locsCoalesce.nightEmissiveScale,
                            (s_nightTagMode != 0) ? s_nightTagScale : 0.0f);
            if (s_locsCoalesce.fixtureEmissiveScale >= 0)
                glUniform1f(s_locsCoalesce.fixtureEmissiveScale, s_fixtureLightsScale);
            // FIXTURE-GLOW-1: 0.0 -> v_fixtureGlow is vec3(0.0) -> byte-identical.
            if (s_locsCoalesce.fixtureGlowStrength >= 0)
                glUniform1f(s_locsCoalesce.fixtureGlowStrength, s_fixtureGlowStrength);
        }
        // FAR-BACKDROP-LIT: upload gate + distance band + tint (coalesce). When
        // the env gate is OFF, u_farBackdropLit=0 -> shader branch skipped ->
        // byte-identical. Static-band/strength/tint values are harmless when the
        // gate is 0 (never read). Mirror in the legacy block below.
        if (s_locsCoalesce.farBackdropLit >= 0)
            glUniform1i(s_locsCoalesce.farBackdropLit, s_farBackdropLitEnabled ? 1 : 0);
        if (s_locsCoalesce.farBackdropNear >= 0)
            glUniform1f(s_locsCoalesce.farBackdropNear, s_farBackdropNear);
        if (s_locsCoalesce.farBackdropFar >= 0)
            glUniform1f(s_locsCoalesce.farBackdropFar, s_farBackdropFar);
        if (s_locsCoalesce.farBackdropStrength >= 0)
            glUniform1f(s_locsCoalesce.farBackdropStrength, s_farBackdropStrength);
        if (s_locsCoalesce.farBackdropTint >= 0)
            glUniform3f(s_locsCoalesce.farBackdropTint,
                        s_farBackdropTint[0], s_farBackdropTint[1], s_farBackdropTint[2]);
        // POOL-CONSUMERS-1 (batch-6 item 6): bind + upload the night light-pool
        // SSBO/uniforms (u_numPoolLights=0 when gated -> byte-identical).
        gos_terrain_lighting::UploadObjectLightPoolUniforms(s_staticPropProgramCoalesce);
        // V-IBL-STATIC-1: SH-L2 image-based ambient. Strength gate is the env
        // var (s_iblShEnabled); when OFF -> upload 0.0 -> shader short-circuits
        // before evalShL2 (byte-identical to pre-slice output). When ON, the
        // ImGui slider g_iblShStrength modulates magnitude (range 0..3).
        // V-IBL-STATIC-2: source coeffs from the per-mission set (defaults to
        // "default" == kIblShCoeffs, so byte-identical to V-IBL-STATIC-1 when
        // no registry entry matches).
        // IBL-SUN-ROTATE-1: when MC2_STATIC_PROP_IBL_SUN_ROTATE is off this
        // returns &s_currentShSet->coeffs[0][0] -- the same pointer this line
        // passed before the slice -- so the upload is byte-identical.
        if (s_locsCoalesce.iblSh >= 0)
            glUniform3fv      (s_locsCoalesce.iblSh, 9,
                               iblShCoeffsForUpload());
        if (s_locsCoalesce.iblShStrength >= 0)
            glUniform1f       (s_locsCoalesce.iblShStrength,
                               iblShStrengthNightScaled());
        // V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel specular. Default OFF;
        // strength=0.0 when env unset/=0 -> shader short-circuits (mathematical
        // proof: lit += specular * 0.0 = lit unchanged). Safety interlock:
        // if MC2_VIEW_UNIFORMS=0, force-zero regardless of gate/slider
        // (shader's #if defined(MC2_USE_VIEW_UNIFORMS) excludes the block
        // and u_cameraWorldPos read; this is belt-and-suspenders).
        if (s_locsCoalesce.pbrV1Strength >= 0) {
            const float pbrStrength =
                (pbrV1Enabled() && !s_viewUniformsDisabled)
                    ? g_pbrV1Strength : 0.0f;
            glUniform1f(s_locsCoalesce.pbrV1Strength, pbrStrength);
        }
        // V-MATERIAL-PBR-2-TUNE-UI: upload roughness override sentinel.
        // Disabled (default) or MC2_VIEW_UNIFORMS=0 -> -1.0 -> shader falls
        // through to its literal 0.6 -> byte-identical to PBR-2-TUNE.
        if (s_locsCoalesce.pbrV1RoughnessOverride >= 0) {
            const float pbrRough =
                (g_pbrV1RoughnessOverrideEnabled && !s_viewUniformsDisabled)
                    ? g_pbrV1RoughnessOverrideValue : -1.0f;
            glUniform1f(s_locsCoalesce.pbrV1RoughnessOverride, pbrRough);
        }
        // V-MATERIAL-PBR-2-DIAG: diagnostic visualizer upload. Default 0
        // (off) -> shader runs existing PBR math unchanged.
        if (s_locsCoalesce.pbrV1DiagSunFound >= 0) {
            glUniform1i(s_locsCoalesce.pbrV1DiagSunFound,
                        s_pbrV1DiagSunFound ? 1 : 0);
        }

        // 11.7.e — slot 1 is NOT bound (per v2r18 §3.X.1: colors_.c[]
        // unread in any live shader path; coalesce branch does not bind
        // it, and the legacy bind site at the per-type loop is dead state).

        // 11.7.f — bind slot 4 (PerDraw SSBO).
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, s_perDrawSsbo);
        // MaterialGpu-2/v3: v3+ static_prop.frag declares MaterialTable at binding 5.
        // The terrain pass uses slot 5 for WaterRecipeBuf in a different GL program —
        // that is not a conflict (terrain re-binds WaterRecipeBuf before its own draw).
        // guardSsbo5 captures at flush() entry and restores at flush() exit.
        if (s_materialGpuEnabled && s_materialGpuSsbo.backendHandle != 0) {
            while (glGetError() != GL_NO_ERROR) {}  // drain stale before bind check
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, static_cast<GLuint>(s_materialGpuSsbo.backendHandle));
            const GLenum bindErr = glGetError();
            if (bindErr != GL_NO_ERROR) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] GL ERROR after bind (real): 0x%x\n", bindErr);
                MC2_DIAG_FPUTS(buf, stderr);
            }
        }

        // MaterialGpu-3: compute sampleOn once per flush.
        // sampleOn: all five conditions required.
        // s_materialGpuEnabled is default-ON; s_materialGpuSampleEnabled is default-ON (set MC2_MATERIAL_GPU_SAMPLE=0 to disable).
        // When sampleOn=0, static_prop.frag falls back to texArrayLayer (no pixel change).
        const bool sampleOn = s_materialGpuEnabled
                           && s_materialGpuSampleEnabled
                           && s_materialGpuSsbo.backendHandle != 0
                           && s_materialGpuSidecarValid
                           && s_locsCoalesce.materialGpuSample >= 0;

        if (s_locsCoalesce.materialGpuSample >= 0) {
            glUniform1i(s_locsCoalesce.materialGpuSample, sampleOn ? 1 : 0);
            const GLenum uniformErr = glGetError();  // sample AFTER
            if (uniformErr != GL_NO_ERROR) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] GL ERROR after sample uniform: 0x%x\n",
                              uniformErr);
                MC2_DIAG_FPUTS(buf, stderr);
            }
        }
        // If loc == -1: M3 error already logged at loadProgramsIfNeeded(); no-op here.

        // STATICPROP-MATERIAL-ORM-1: per-frame baseline for u_ormSampleEnable.
        // S5-B hardening: the ORM texture arrays (s_ormBucketArrays) are ONLY built
        // and bound inside the BC7-bucketed paths (guarded by s_staticPropBc7Enabled).
        // The legacy group-array paths (s_texArrayOff/On) bind albedo only and leave
        // the ORM unit holding whatever was last bound. So sampling must be enabled
        // ONLY when BOTH gates are on — gate the baseline on s_staticPropBc7Enabled
        // so every draw branch (incl. the v5 snap-cull multidraw path, which falls
        // back to legacy arrays when bc7Buckets is false) is covered at the source,
        // not just the non-bucketed else branch below. s_staticPropBc7Enabled is set
        // once at startup and never mutated, so build-time array existence and this
        // draw-time decision agree. loc-guarded → no-op until the shader declares it.
        if (s_locsCoalesce.ormSampleEnable >= 0)
            glUniform1i(s_locsCoalesce.ormSampleEnable,
                        (s_ormSlotsEnabled && s_staticPropBc7Enabled) ? 1 : 0);

        // M2: diagnostic log — first frame + every 600 frames (mirrors accumulateMonotonicAndMaybeEmit cadence).
        // C1: guard fires on every run now that s_materialGpuEnabled is default-ON (v5).
        // Set MC2_MATERIAL_GPU=0 to silence. reason cascade mirrors sampleOn condition order.
        if ((s_materialGpuEnabled || s_materialGpuSampleEnabled)
                && (s_counters.frame_count == 1
                    || (s_counters.frame_count % 600 == 0 && s_counters.frame_count > 0))) {
            const char* reason = "ok";
            if (!s_materialGpuEnabled)                    reason = "upload_env_off";
            else if (!s_materialGpuSampleEnabled)          reason = "sample_env_off";
            else if (s_materialGpuSsbo.backendHandle == 0)        reason = "no_ssbo";
            else if (!s_materialGpuSidecarValid)           reason = "sidecar_invalid";
            else if (s_locsCoalesce.materialGpuSample < 0) reason = "uniform_missing";
            // else: reason == "ok" -> sampleOn is true (all 5 conditions met)

            char buf[96];
            if (sampleOn) {
                // sampleOn=true guarantees loc >= 0, so loc is always valid here.
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] event=sample_mode enabled=1 loc=%d\n",
                              s_locsCoalesce.materialGpuSample);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] event=sample_mode enabled=0 reason=%s\n",
                              reason);
            }
            MC2_DIAG_FPUTS(buf, stderr);
        }

        // DrawPacket v5: gate-arm checks (run once per process on first gate-ON flush).
        if (s_v5Enabled && !s_v5Armed && !s_v5Disarmed) {
            s_v5Armed = true;
            s_baseInstanceSupported = (GLEW_ARB_base_instance == GL_TRUE);
            bool disarm = false;

            // Precondition 1: base-instance map must be non-null.
            // Under MC2_STATIC_PROP_GLOBAL_POOL_LEGACY=1, s_baseInstanceByCmdSsbo is
            // never allocated and s_baseInstanceByCmdMap stays nullptr.
            // Check here (not per-slot) to avoid 753x base_instance_missing masking root cause.
            if (s_baseInstanceByCmdMap == nullptr) {
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] event=disarmed reason=legacy_pool_no_baseinst_map\n");
                disarm = true;
            }

            // Precondition 2: u_drawIDBase uniform location must be valid.
            // If -1, glUniform1i is a GL no-op and the shader reads the wrong PerDrawEntry.
            if (!disarm && s_locsCoalesce.drawIDBase < 0) {
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] WARNING event=disarmed reason=drawid_loc_missing"
                    " loc=%d\n",
                    static_cast<int>(s_locsCoalesce.drawIDBase));
                disarm = true;
            }

            if (disarm) {
                s_v5Disarmed = true;
            } else {
                const uint32_t totalSlots = s_alphaOffCmdCount + s_alphaOnCmdCount;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] event=armed slots=%u ext_supported=%d"
                    " drawid_loc=%d\n",
                    totalSlots,
                    static_cast<int>(s_baseInstanceSupported),
                    static_cast<int>(s_locsCoalesce.drawIDBase));
            }
        }

        // DrawPacket v6: gate-arm checks (run once per process on first gate-ON flush).
        if (s_v6Enabled && !s_v6Armed && !s_v6Disarmed) {
            s_v6Armed = true;
            bool disarm = false;
            const char* disarmReason = nullptr;

            // Check ARB_base_instance independently — do NOT read s_baseInstanceSupported
            // (written only inside v5 arm block; may be false if v5 gate is OFF).
            if (GLEW_ARB_base_instance != GL_TRUE) {
                disarmReason = "base_instance_ext_missing";
                disarm = true;
            }

            if (!disarm && s_baseInstanceByCmdMap == nullptr) {
                disarmReason = "legacy_pool_no_baseinst_map";
                disarm = true;
            }

            if (!disarm && s_locsCoalesce.drawIDBase < 0) {
                disarmReason = "drawid_loc_missing";
                disarm = true;
            }

            if (!disarm &&
                static_cast<uint32_t>(RenderCore::PipelineId::StaticPropOpaque) >=
                static_cast<uint32_t>(RenderCore::PipelineId::Count_)) {
                disarmReason = "pipeline_enum_stale_opaque";
                disarm = true;
            }

            if (!disarm &&
                static_cast<uint32_t>(RenderCore::PipelineId::StaticPropAlphaTest) >=
                static_cast<uint32_t>(RenderCore::PipelineId::Count_)) {
                disarmReason = "pipeline_enum_stale_alphatest";
                disarm = true;
            }

            if (disarm) {
                s_v6Disarmed = true;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V6] event=disarmed reason=%s\n",
                    disarmReason);
            } else {
                const uint32_t totalSlots = s_alphaOffCmdCount + s_alphaOnCmdCount;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V6] event=armed slots=%u alpha_off=%u alpha_on=%u"
                    " ext_supported=1 drawid_loc=%d default=1\n",
                    totalSlots, s_alphaOffCmdCount, s_alphaOnCmdCount,
                    static_cast<int>(s_locsCoalesce.drawIDBase));
            }
        }

        // === M2a Task 4: StaticPopulation draw (gate MC2_STATIC_POP_SPLIT) ===
        // Emitted ONCE here, before the dynamic draw chain (runV6/runV5/BC7/
        // existing), so it is path-independent: it reuses the program + s_sharedVao
        // + depth(GEQUAL)/blend(disabled) state set by applyPipeline() at the top of
        // the draw section, and s_perDrawSsbo (slot 4, prologue), then binds its OWN
        // instance SSBO at binding 0 + its own indirect cmds and issues OFF then ON
        // multidraws. Static opaque draws before all alpha; alpha is alpha-test with
        // blend disabled, so cross-population alpha order is not a correctness
        // concern (depth-writing cutout). De-merge (reinjectPersistentStatic
        // early-out under the gate) keeps statics out of the dynamic bucket so they
        // are not double-drawn. instanceCount = full static population (first-N, NOT
        // GPU-culled — visibleIds authority is M2b).
        if (staticPopSplitArmed() && s_staticIndirectCmdBuf && !s_staticDrawCmds.empty()) {
            size_t staticTotal = 0;
            for (auto& kv : s_persistentStaticStore) staticTotal += kv.second.size();
            gpuBindSsboRange(0, s_staticInstanceSsbo, 0LL,
                (long long)((staticTotal ? staticTotal : 1) * sizeof(GpuStaticPropInstance)),
                "sp.staticInstance");
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_staticIndirectCmdBuf);
            const uintptr_t cmdSize =
                static_cast<uintptr_t>(gpu_cull::kDrawElementsIndirectCommandSize);
            // GPU-SYNC-CONTRACT: s_staticInstanceSsbo (bound above) was filled via
            // a persistent GL_MAP_COHERENT_BIT mapping (fillStaticInstanceBufferIfDirty).
            // Order those CPU writes before the indirect draws read them. Without
            // this, NVIDIA may draw from stale instance data; AMD tolerated it.
            // (M2a path -- default-off MC2_STATIC_POP_SPLIT, latent until enabled;
            // routed through the typed helper.)
            gpuSyncBarrier(GpuProducer::CpuCoherentWrite, GpuConsumer::MultiDrawIndirect,
                           "staticpop_m2a_instance");
            if (s_alphaOffCmdCount > 0u) {
                if (s_locsCoalesce.drawIDBase >= 0) glUniform1i(s_locsCoalesce.drawIDBase, 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(0)),
                    static_cast<GLsizei>(s_alphaOffCmdCount), 0);
            }
            if (s_alphaOnCmdCount > 0u) {
                if (s_locsCoalesce.drawIDBase >= 0)
                    glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(s_alphaOffCmdCount));
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(s_alphaOffCmdCount) * cmdSize),
                    static_cast<GLsizei>(s_alphaOnCmdCount), 0);
            }
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
            // Criterion 2: fence this draw so the next dirty in-place fill waits on it.
            if (s_staticDrawFence) glDeleteSync(s_staticDrawFence);
            s_staticDrawFence = MC2_GL_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }

        // 2026-05-11 per-packet rework: each indirect cmd is per-PACKET, so
        // the multidraw counts and the alpha-ON byte offset use per-PACKET
        // counts (s_alphaOffCmdCount / s_alphaOnCmdCount). The PerDrawEntry
        // SSBO is also per-packet, indexed by gl_DrawID = sorted-packet slot.

        // DrawPacket v6: build packet+meta arrays then validate before dispatch.
        const bool runV6 = s_v6Enabled && !s_v6Disarmed;

        static std::vector<RenderCore::DrawPacket>  v6Packets;
        static std::vector<StaticPropDispatchMeta>  v6Meta;
        uint32_t v6LockstepViolations = 0u;
        // v8: snapshot-sole-owner decision. Set in the runV6 preamble, read again in
        // the dispatch block (a separate scope) to select dispatch + structural fallback.
        bool retireLiveBuilder = false;

        if (runV6) {
            s_v6FrameDrawsIssued        = 0u;
            s_v6FrameZeroInstSkips      = 0u;
            s_v6FrameSortedOob          = 0u;
            s_v6FramePacketOob          = 0u;
            s_v6FrameTypeOob            = 0u;
            s_v6FrameLockstepViolations = 0u;
            s_v6FrameGlErrors           = 0u;
            ++s_v6TotalFrameCount;

            // v8: retirement predicate. Consumed just below to gate the live builder,
            // set s_spBuildRetired, and emit the one-shot arm log.
            //
            // Why !s_snapCullEnabled is a condition: the snap-cull path (MC2_SNAP_CULL,
            // opt-in, default OFF) still relies on the live<->snapshot compare for slot
            // ownership in v8; it has not been ported to sole-owner packet build. Keep
            // the live builder whenever snap-cull is active until that port lands.
            const bool retireLiveBuilderPlanned =
                !s_keepLiveBuilder && s_snapshotBuildEnabled && !s_snapCullEnabled;

            // v8: snapshot is now the SOLE owner when retired. Consume the planned
            // predicate, set telemetry, emit the one-shot arm log.
            retireLiveBuilder = retireLiveBuilderPlanned;
            if (!s_v8ArmLogged) {
                s_v8ArmLogged = true;
                const char* reason =
                    retireLiveBuilder       ? "snapshot_sole_owner" :
                    s_keepLiveBuilder       ? "live_builder_forced" :
                    !s_snapshotBuildEnabled ? "snapshot_packet_build_disabled_keep_live" :
                                              "snap_cull_collision_keep_live";
                MC2_DIAGFE(stderr,
                    "[STATIC_PROP_PACKET_DISPATCH v8] event=arm live_builder_retired=%d"
                    " snapshot_packet_build=%d live_builder_forced=%d reason=%s\n",
                    retireLiveBuilder ? 1 : 0, s_snapshotBuildEnabled ? 1 : 0,
                    s_keepLiveBuilder ? 1 : 0, reason);
                std::fflush(stderr);
            }

            // v3: reset per-flush build counters so stale stats never persist.
            s_spBuildAttempted = s_spBuildCountMismatch = s_spBuildPacketMismatch =
            s_spBuildMetaMismatch = s_spBuildFallback = s_spBuildRetired = 0u;

            // v8: set retirement telemetry AFTER the unconditional reset above so it
            // is not clobbered. 1 = live builder skipped this frame (snapshot sole owner).
            s_spBuildRetired = retireLiveBuilder ? 1u : 0u;

            const uint32_t totalCmds = s_alphaOffCmdCount + s_alphaOnCmdCount;

            if (g_mc2HitchEnabled) {
                g_mc2HitchAccum.staticRecords  = static_cast<uint32_t>(s_bucketsByType.size());
                g_mc2HitchAccum.staticDrawCmds = totalCmds;
            }

            // Ring-slot base-instance map pointer (same arithmetic as v5).
            const size_t fr_off_bi_v6 =
                static_cast<size_t>(s_coalesceFrameSlot) * s_baseInstanceByCmdBytesPerFrame;
            const uint32_t* baseInstanceMap = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off_bi_v6);

            // When retired and the snapshot builds clean, s_v6FrameLockstepViolations
            // stays 0 from the unconditional per-flush reset above (no live build runs).
            if (!retireLiveBuilder) {
                ZoneScopedN("StaticProp.LiveBuild");
                v6LockstepViolations = buildLiveV6Arrays(totalCmds, baseInstanceMap, v6Packets, v6Meta);
                s_v6FrameLockstepViolations = v6LockstepViolations;
            }
        }

        // DrawPacket v5: per-draw-call dispatch (MC2_DRAW_PACKET_COALESCE_V5=1).
        // When active, replaces both glMultiDrawElementsIndirect calls with a
        // per-slot glDrawElementsInstancedBaseVertexBaseInstance loop.
        const bool runV5 = s_v5Enabled && s_baseInstanceSupported && !s_v5Disarmed;

        if (runV6 && v6LockstepViolations == 0u) {
            const uint32_t totalCmds = s_alphaOffCmdCount + s_alphaOnCmdCount;

            // v8: re-home the ring-slot base-instance map for this dispatch. The snapshot
            // build borrows baseInstance from here (NOT from the live build, which is
            // skipped when retired). Same arithmetic as the live path's baseInstanceMap.
            const size_t fr_off_bi_disp =
                static_cast<size_t>(s_coalesceFrameSlot) * s_baseInstanceByCmdBytesPerFrame;
            const uint32_t* baseInstanceMapDisp = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off_bi_disp);

            // Unconditionally bind SSBO slot 0 (same as v5).
            {
                const size_t totalUsed = s_totalUsedBytesThisFrame;
                gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                 (long long)fr_off_bytes_d,
                                 (long long)(totalUsed > 0 ? totalUsed : sizeof(GpuStaticPropInstance)),
                                 "sp.coalesce.v6a");
            }

            glActiveTexture(GL_TEXTURE0);
            // COMPRESSION-BC7-STATICPROP-2: gate ON binds per-bucket inside the
            // loop (init to an invalid bucket so the first drawn slot binds).
            // Gate OFF: original OFF→ON flip, byte-identical.
            const bool bc7Buckets = s_staticPropBc7Enabled && !s_slotBucketIndex.empty();
            int32_t curBucket = -1;  // gate-ON tracker
            if (!bc7Buckets) glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
            bool enteredOnGroup = false;

            // v2.3 snap-cull: determine whether to use prev-frame snapshot for slot skipping.
            // Activation requires:
            //   - MC2_SNAP_CULL=1 and snap non-null
            //   - snap->ok == 1u (snapshot was structurally valid — v2.2 gates passed)
            //   - count matches totalCmds (snapshot covers exactly the live draw slots)
            // Warmup guard: even when all conditions above hold, if ALL instanceCounts are zero
            // (frame 1 or all-invisible scene) don't cull — avoids frame-1 blank screen.
            bool snapCullActive = false;
            uint32_t snapN      = 0u;
            if (s_snapCullEnabled &&
                snap != nullptr &&
                snap->ok == 1u &&
                snap->staticPropPackets.data != nullptr &&
                snap->staticPropPackets.count == totalCmds &&
                snap->staticPropPackets.count > 0u) {
                bool allZero = true;
                for (size_t scj = 0; scj < snap->staticPropPackets.count; ++scj) {
                    if (snap->staticPropPackets.data[scj].instanceCount != 0u) {
                        allZero = false;
                        break;
                    }
                }
                snapCullActive = !allZero;
                snapN          = static_cast<uint32_t>(snap->staticPropPackets.count);
            }

            // ---------------------------------------------------------------
            // v3 snapshot builder: activation guard → build → compare
            // ---------------------------------------------------------------
            bool snapBuilt = false;
            s_spBuildAttempted = s_snapshotBuildEnabled ? 1u : 0u;

            if (s_snapshotBuildEnabled) {
                // Stage 1: snap-cull collision — both gates active → disable v3.
                if (s_snapCullEnabled) {
                    if (!s_spBuildCollisionLoggedOnce) {
                        s_spBuildCollisionLoggedOnce = true;
                        std::fprintf(stderr,
                            "[RENDER_SNAPSHOT v3] frame=%u disabled — MC2_SNAP_CULL collision\n",
                            s_v6TotalFrameCount);
                    }
                    ++s_spBuildFallback;
                }
                // Stage 2: structural guards — snap unusable (no counter).
                else if (snap == nullptr || snap->ok != 1u ||
                         snap->staticPropPackets.data == nullptr) {
                    /* no counter — snap is unusable, not a v3 failure */
                }
                // Stage 3: v2.2 structural must be clean.
                else if (snap->spCountMismatch       != 0u ||
                         snap->spSortedSlotMismatch  != 0u ||
                         snap->spGlobalPacketMismatch != 0u ||
                         snap->spPipelineMismatch     != 0u ||
                         snap->spMaterialIdxMismatch  != 0u ||
                         snap->spTexLayerMismatch     != 0u) {
                    /* no counter — v2.2 gate failed; ordering unreliable */
                }
                // Stage 4: count mismatch → record and fall back.
                else if (snap->staticPropPackets.count != totalCmds) {
                    ++s_spBuildCountMismatch;
                    ++s_spBuildFallback;
                }
                else {
                    // All guards passed — build snapshot arrays.
                    {
                    ZoneScopedN("StaticProp.SnapshotBuild");
                    mc2_hitch::HitchScope _hitchSnap(mc2_hitch::HitchSpanKind::GpuStaticPropsSnapshot);
                    s_snapV6Packets.resize(totalCmds);
                    s_snapV6Meta.resize(totalCmds);

                    for (uint32_t si = 0u; si < totalCmds; ++si) {
                        const ExtractedStaticPropPacket& row =
                            snap->staticPropPackets.data[si];

                        // Snapshot owns slot identity — row must claim this slot.
                        if (row.sortedSlot != si) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // Guard: globalPacketIdx in bounds.
                        if (row.globalPacketIdx >=
                                static_cast<uint32_t>(s_packets.size())) {
                            ++s_spBuildPacketMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        const GpuStaticPropPacket& spkt = s_packets[row.globalPacketIdx];

                        // Guard: typeId in type table.
                        if (row.typeId >= static_cast<uint32_t>(s_types.size())) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // Fail closed on unknown pipelineId.
                        const uint32_t grp = pipelineId_to_group(row.pipelineId);
                        if (grp == 0xFFFFFFFFu) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // instanceCount from current-frame live (NOT snapshot prev-frame).
                        const auto typeIt = s_typeRanges.find(row.typeId);
                        const uint32_t instCount =
                            (typeIt != s_typeRanges.end())
                                ? typeIt->second.instanceCount : 0u;

                        s_snapV6Meta[si].sortedSlot      = row.sortedSlot;
                        s_snapV6Meta[si].globalPacketIdx = row.globalPacketIdx;
                        s_snapV6Meta[si].typeId          = row.typeId;
                        s_snapV6Meta[si].group           = grp;
                        s_snapV6Meta[si].instanceCount   = instCount;
                        s_snapV6Meta[si].baseInstance    = baseInstanceMapDisp[si];
                        s_snapV6Meta[si].drawIDBase      = si;
                        s_snapV6Meta[si].baseVertex      = spkt.baseVertex;

                        s_snapV6Packets[si]            = RenderCore::DrawPacket{};
                        s_snapV6Packets[si].pipelineId = static_cast<uint32_t>(
                            grp == 0u ? RenderCore::PipelineId::StaticPropOpaque
                                      : RenderCore::PipelineId::StaticPropAlphaTest);
                        s_snapV6Packets[si].firstIndex = spkt.firstIndex;
                        s_snapV6Packets[si].indexCount = spkt.indexCount;
                    }

                    }  // end StaticProp.SnapshotBuild

                    // Compare snapshot-built vs live-built, field-by-field per slot.
                    // v8: skip when the live builder is retired — v6Meta/v6Packets are
                    // STALE (not rebuilt this frame), so the compare is meaningless and
                    // would set spurious mismatches. Snapshot is sole owner in that mode.
                    if (!retireLiveBuilder) {
                    ZoneScopedN("StaticProp.BuildCompare");
                    for (uint32_t ci = 0u; ci < totalCmds; ++ci) {
                        const StaticPropDispatchMeta& sm = s_snapV6Meta[ci];
                        const StaticPropDispatchMeta& lm = v6Meta[ci];
                        const RenderCore::DrawPacket&  sp = s_snapV6Packets[ci];
                        const RenderCore::DrawPacket&  lp = v6Packets[ci];

                        if (sm.globalPacketIdx != lm.globalPacketIdx) ++s_spBuildMetaMismatch;
                        if (sm.typeId          != lm.typeId)          ++s_spBuildMetaMismatch;
                        if (sm.group           != lm.group)           ++s_spBuildMetaMismatch;
                        if (sm.sortedSlot      != lm.sortedSlot)      ++s_spBuildMetaMismatch;
                        if (sm.baseVertex      != lm.baseVertex)      ++s_spBuildMetaMismatch;
                        if (sm.drawIDBase      != lm.drawIDBase)      ++s_spBuildMetaMismatch;

                        if (sp.pipelineId != lp.pipelineId) ++s_spBuildPacketMismatch;
                        if (sp.firstIndex  != lp.firstIndex)  ++s_spBuildPacketMismatch;
                        if (sp.indexCount  != lp.indexCount)  ++s_spBuildPacketMismatch;
                    }
                    }  // end StaticProp.BuildCompare

                    snapBuilt = true;
                }
            }

            // Dispatch ref-swap.
            bool useSnapshot;
            bool skipDispatch = false;
            if (retireLiveBuilder) {
                // Snapshot is sole owner. Dispatch unless STRUCTURALLY invalid.
                // snapshotInvalid == NOT built (guards failed: snap nullptr / ok!=1 /
                // count mismatch / malformed metadata). An empty-but-valid snapshot
                // (snapBuilt with totalCmds==0) is VALID → dispatch zero, NEVER fallback.
                //
                // A per-row guard failure during the snapshot build increments these
                // (and zeroes the bad slot) but still leaves snapBuilt=true. Treat any
                // such divergence as structurally invalid so we fall back to the live
                // build for the whole frame — matching the dual path's mismatch->fallback
                // safety. This also makes the spBuildFallback==0 merge gate cover it.
                const bool snapshotInvalid = !snapBuilt
                    || s_spBuildMetaMismatch   != 0u
                    || s_spBuildPacketMismatch != 0u;
                if (snapshotInvalid) {
                    // Stage 4 (count mismatch) already counted a fallback for this frame;
                    // avoid double-counting so spBuildFallback reflects frames, not events.
                    if (s_spBuildCountMismatch == 0u) ++s_spBuildFallback;
                    ZoneScopedN("StaticProp.LiveBuild.Fallback");  // rare fallback build, counted
                    v6LockstepViolations =
                        buildLiveV6Arrays(totalCmds, baseInstanceMapDisp, v6Packets, v6Meta);
                    s_v6FrameLockstepViolations = v6LockstepViolations;
                    useSnapshot = false;
                    if (v6LockstepViolations != 0u) {
                        // Fallback build itself is inconsistent — do not issue draws from a
                        // broken array this frame (matches dual-mode lockstep gating).
                        skipDispatch = true;
                    }
                } else {
                    useSnapshot = true;
                }
            } else {
                // Legacy dual path: use snapshot arrays only when compare clean.
                useSnapshot = snapBuilt
                    && s_spBuildPacketMismatch == 0
                    && s_spBuildMetaMismatch   == 0;

                if (snapBuilt && !useSnapshot) {
                    ++s_spBuildFallback;
                    if (!s_spBuildFirstFallbackLogged) {
                        s_spBuildFirstFallbackLogged = true;
                        std::fprintf(stderr,
                            "[RENDER_SNAPSHOT v3] frame=%u first-fallback"
                            " attempted=1 count_mismatch=%u pkt_mismatch=%u"
                            " meta_mismatch=%u fallback=1\n",
                            s_v6TotalFrameCount,
                            s_spBuildCountMismatch,
                            s_spBuildPacketMismatch,
                            s_spBuildMetaMismatch);
                    }
                }
            }

            const std::vector<RenderCore::DrawPacket>* pDispatchPackets =
                useSnapshot ? &s_snapV6Packets : &v6Packets;
            const std::vector<StaticPropDispatchMeta>* pDispatchMeta =
                useSnapshot ? &s_snapV6Meta    : &v6Meta;

            // FOLIAGE-STATICPROP-DEPTH-PREPASS-1 (B + C): run the depth prepass
            // over the SAME dispatch lists the color loop below draws, then flip
            // the color pass to GL_EQUAL + depthWrite-off. Gated/no-op by default
            // (flushDepthPrepassV6 returns false → color path byte-identical to
            // pre-change behavior, no re-applyPipeline issued).
            //
            // The prepass clobbers the bound program + depth state + color mask;
            // it restores the color mask itself. We re-apply the color pipeline
            // here (with the EQUAL/no-write override) so the color loop below runs
            // against the right program + depth state. SSBO slot 0 (instance
            // range) and the VAO/IBO survive (prepass does not rebind slot 0; the
            // color loop re-binds the OFF texture array via enteredOnGroup logic).
            if (!skipDispatch &&
                flushDepthPrepassV6(*pDispatchPackets, *pDispatchMeta,
                                    totalCmds, bc7Buckets, debugAddrMode_)) {
                // Color pass: keep ONLY the front-most fragment the prepass laid
                // (reverse-Z, so GEQUAL prepass stored the nearest depth). EQUAL +
                // no depth re-write. Object-ID (frag loc=2) is unaffected — it is a
                // color attachment write, not a depth-state field.
                RenderCore::PipelineDesc colorDesc =
                    RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropOpaque);
                colorDesc.depthFunc        = RenderCore::DepthFunc::Equal;
                colorDesc.depthWriteEnable = false;
                pipeline_binder::applyPipeline(colorDesc, "StaticPropOpaque");  // re-test variant (Equal depth)
                // applyPipeline rebinds the program but NOT the VAO/IBO/SSBO/
                // texture; re-assert the geometry bindings the color loop expects
                // (the prepass left the OFF texture array bound, slot-2 bound, and
                // slot 0 untouched, so this is belt-and-suspenders).
                glBindVertexArray(s_sharedVao);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
            }

            // OBJECT-TERRAIN-COPLANAR-OFFSET-1: slope-scaled depth push so object
            // bodies win the coplanar tie over true-depth terrain (gate default OFF).
            const bool s_objPO = objPolyOffsetEnabled();
            if (s_objPO) {
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(objPolyOffsetFactor(), objPolyOffsetUnits());
            }
            if (!skipDispatch)
            for (uint32_t i = 0u; i < totalCmds; ++i) {
                const StaticPropDispatchMeta& m  = (*pDispatchMeta)[i];
                const RenderCore::DrawPacket&  dp = (*pDispatchPackets)[i];

                // v2.3 snap-cull: skip slots whose prev-frame instanceCount was zero.
                // Per-row slot identity check guards against any snapshot/live ordering drift.
                // On identity mismatch: don't skip — count it and fall through to current-frame logic.
                if (snapCullActive && i < snapN) {
                    const ExtractedStaticPropPacket& scRow = snap->staticPropPackets.data[i];
                    if (scRow.sortedSlot != i) {
                        // Slot identity mismatch — snapshot and live order diverged.
                        ++s_snapCullSlotMismatch;
                        // Fall through to current-frame logic.
                    } else if (scRow.instanceCount == 0u) {
                        // Prev-frame zero — safe to skip.
                        ++s_snapCullSkipped;
                        continue;
                    } else {
                        // Prev-frame non-zero — draw; count as snap-cull active.
                        ++s_snapCullActive;
                    }
                }

                if (m.instanceCount == 0u) {
                    ++s_v6FrameZeroInstSkips;
                    continue;
                }

                if (bc7Buckets) {
                    // Bind this slot's bucket array (contiguous runs → at most
                    // one bind per bucket). s_slotBucketIndex is in sorted-slot
                    // order, same as the dispatch loop index i.
                    if (i < s_slotBucketIndex.size()) {
                        const int32_t b = static_cast<int32_t>(s_slotBucketIndex[i]);
                        if (b != curBucket &&
                            b >= 0 && b < static_cast<int32_t>(s_bucketArrays.size())) {
                            glBindTexture(GL_TEXTURE_2D_ARRAY, s_bucketArrays[b]);
                            // STATICPROP-MATERIAL-ORM-1: bind the layer-aligned ORM
                            // sibling to kOrmTexUnit, then restore active unit 0.
                            if (s_ormSlotsEnabled &&
                                static_cast<size_t>(b) < s_ormBucketArrays.size()) {
                                glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
                                glBindTexture(GL_TEXTURE_2D_ARRAY, s_ormBucketArrays[b]);
                                glActiveTexture(GL_TEXTURE0);
                            }
                            curBucket = b;
                        }
                    }
                } else if (m.group == 1u && !enteredOnGroup) {
                    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                    enteredOnGroup = true;
                }

                glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(m.drawIDBase));

                glDrawElementsInstancedBaseVertexBaseInstance(
                    GL_TRIANGLES,
                    static_cast<GLsizei>(dp.indexCount),
                    GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(dp.firstIndex) * sizeof(uint32_t)),
                    static_cast<GLsizei>(m.instanceCount),
                    m.baseVertex,
                    m.baseInstance);

                if (s_v6TraceEnabled)
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V6] slot=%u pkt=%u type=%u group=%u"
                        " inst=%u base=%u drawID=%u first=%u count=%u baseV=%d\n",
                        m.sortedSlot, m.globalPacketIdx, m.typeId, m.group,
                        m.instanceCount, m.baseInstance, m.drawIDBase,
                        dp.firstIndex, dp.indexCount, m.baseVertex);

                ++s_v6FrameDrawsIssued;
            }

            if (s_objPO) glDisable(GL_POLYGON_OFFSET_FILL);  // OBJECT-TERRAIN-COPLANAR-OFFSET-1

            if (enteredOnGroup)
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);

            {
                const GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    ++s_v6FrameGlErrors;
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V6] event=gl_error frame=%u err=0x%x\n",
                        s_v6TotalFrameCount, static_cast<unsigned>(err));
                }
            }

            if ((s_v6TotalFrameCount % 600u) == 0u) {
                const uint32_t slotsConsidered = totalCmds;
                const int ok = (s_v6FrameSortedOob          == 0u &&
                                s_v6FramePacketOob           == 0u &&
                                s_v6FrameTypeOob             == 0u &&
                                s_v6FrameLockstepViolations  == 0u &&
                                s_v6FrameGlErrors            == 0u &&
                                (s_v6FrameDrawsIssued + s_v6FrameZeroInstSkips) == slotsConsidered)
                               ? 1 : 0;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V6] frame=%u event=dispatch_summary"
                    " slots_considered=%u draws_issued=%u zero_instance_skips=%u"
                    " sorted_oob=%u packet_oob=%u type_oob=%u lockstep_violations=%u"
                    " gl_errors=%u ok=%d\n",
                    s_v6TotalFrameCount, slotsConsidered,
                    s_v6FrameDrawsIssued, s_v6FrameZeroInstSkips,
                    s_v6FrameSortedOob, s_v6FramePacketOob, s_v6FrameTypeOob,
                    s_v6FrameLockstepViolations, s_v6FrameGlErrors, ok);
                if (s_snapshotBuildEnabled) {
                    std::fprintf(stderr,
                        "[RENDER_SNAPSHOT v3] frame=%u"
                        " attempted=%u count_mismatch=%u pkt_mismatch=%u"
                        " meta_mismatch=%u fallback=%u using_snapshot=%d\n",
                        s_v6TotalFrameCount,
                        s_spBuildAttempted,
                        s_spBuildCountMismatch,
                        s_spBuildPacketMismatch,
                        s_spBuildMetaMismatch,
                        s_spBuildFallback,
                        useSnapshot ? 1 : 0);
                }
            }

        } else if (runV5) {
            s_v5FrameDrawsIssued     = 0u;
            s_v5FrameZeroInstSkips   = 0u;
            s_v5FrameSortedOob       = 0u;
            s_v5FramePacketOob       = 0u;
            s_v5FrameTypeOob         = 0u;
            s_v5FrameBaseInstMissing = 0u;
            s_v5FrameGlErrors        = 0u;
            ++s_v5TotalFrameCount;

            const uint32_t totalCmds = s_alphaOffCmdCount + s_alphaOnCmdCount;

            // Compute base-instance map pointer for this ring slot.
            // Arithmetic is in BYTES: multiply ring-slot index by bytes-per-frame,
            // byte-cast the void*, then reinterpret as uint32_t*.
            // Do NOT use index arithmetic on the void* base directly.
            const size_t fr_off_bi =
                static_cast<size_t>(s_coalesceFrameSlot) * s_baseInstanceByCmdBytesPerFrame;
            const uint32_t* baseInstanceMap = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off_bi);

            // Unconditionally bind SSBO slot 0 before the loop.
            // The prologue only binds inside if(s_alphaOffCmdCount > 0u).
            // If all props are alpha-ON (OFF count == 0), slot 0 is stale.
            // v5 only runs in global-pool mode (legacy mode disarms at gate-arm).
            {
                const size_t totalUsed = s_totalUsedBytesThisFrame;
                gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                 (long long)fr_off_bytes_d,
                                 (long long)(totalUsed > 0 ? totalUsed : sizeof(GpuStaticPropInstance)),
                                 "sp.coalesce.v5");
            }

            // Bind alpha-OFF texture array before loop.
            glActiveTexture(GL_TEXTURE0);
            // COMPRESSION-BC7-STATICPROP-2: gate ON binds per-bucket in-loop.
            const bool bc7Buckets = s_staticPropBc7Enabled && !s_slotBucketIndex.empty();
            int32_t curBucket = -1;
            if (!bc7Buckets) glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);

            bool enteredOnGroup = false;

            for (uint32_t i = 0u; i < totalCmds; ++i) {
                if (i >= static_cast<uint32_t>(s_sortedPacketOrder.size())) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=sorted_oob\n", i);
                    ++s_v5FrameSortedOob;
                    continue;
                }
                const uint32_t globalPktIdx = s_sortedPacketOrder[i];

                if (globalPktIdx >= static_cast<uint32_t>(s_packets.size())) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=packet_oob\n", i);
                    ++s_v5FramePacketOob;
                    continue;
                }
                const GpuStaticPropPacket& pkt = s_packets[globalPktIdx];

                // Genuine OOB: owningTypeID outside the static type table.
                if (pkt.owningTypeID >= static_cast<uint32_t>(s_types.size())) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=type_oob\n", i);
                    ++s_v5FrameTypeOob;
                    continue;
                }
                // s_typeRanges only has snapshot (instance-bearing) types.
                // Missing = zero instances this frame; not an error.
                const auto typeIt = s_typeRanges.find(pkt.owningTypeID);
                const uint32_t instanceCount =
                    (typeIt != s_typeRanges.end()) ? typeIt->second.instanceCount : 0u;

                if (instanceCount == 0u) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=zero_inst\n", i);
                    ++s_v5FrameZeroInstSkips;
                    continue;
                }
                const GLuint baseInstance = baseInstanceMap[i];

                // Group boundary: switch texture array when entering alpha-ON group.
                if (bc7Buckets) {
                    if (i < s_slotBucketIndex.size()) {
                        const int32_t b = static_cast<int32_t>(s_slotBucketIndex[i]);
                        if (b != curBucket &&
                            b >= 0 && b < static_cast<int32_t>(s_bucketArrays.size())) {
                            glBindTexture(GL_TEXTURE_2D_ARRAY, s_bucketArrays[b]);
                            // STATICPROP-MATERIAL-ORM-1: bind layer-aligned ORM sibling.
                            if (s_ormSlotsEnabled &&
                                static_cast<size_t>(b) < s_ormBucketArrays.size()) {
                                glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
                                glBindTexture(GL_TEXTURE_2D_ARRAY, s_ormBucketArrays[b]);
                                glActiveTexture(GL_TEXTURE0);
                            }
                            curBucket = b;
                        }
                    }
                } else if (i == s_alphaOffCmdCount) {
                    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                    enteredOnGroup = true;
                }

                // Per-draw uniform: u_drawIDBase = absolute sorted slot.
                // In multidraw: u_drawIDBase = group-start, gl_DrawID = within-group index.
                // In per-draw:  u_drawIDBase = i,           gl_DrawID = always 0.
                // Net shader index (gl_DrawID + u_drawIDBase) is identical in both paths.
                glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(i));

                // firstIndex is uint32_t; multiply in uintptr_t space to avoid 32-bit overflow.
                glDrawElementsInstancedBaseVertexBaseInstance(
                    GL_TRIANGLES,
                    static_cast<GLsizei>(pkt.indexCount),
                    GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                    static_cast<GLsizei>(instanceCount),
                    static_cast<GLint>(pkt.baseVertex),
                    baseInstance);

                if (s_v5TraceEnabled)
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] slot=%u type=%u inst=%u base_inst=%u draw_id_base=%u\n",
                        i, pkt.owningTypeID, instanceCount, static_cast<unsigned>(baseInstance), i);

                ++s_v5FrameDrawsIssued;
            }

            // Restore texture array if loop entered the alpha-ON group.
            if (enteredOnGroup) {
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
            }

            // GL error check (one glGetError per v5-active flush).
            {
                const GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    ++s_v5FrameGlErrors;
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] event=gl_error frame=%u err=0x%x\n",
                        s_v5TotalFrameCount, static_cast<unsigned>(err));
                }
            }

            // Per-frame summary log: every 600 frames.
            if ((s_v5TotalFrameCount % 600u) == 0u) {
                const int ok = (s_v5FrameSortedOob       == 0u &&
                                s_v5FramePacketOob        == 0u &&
                                s_v5FrameTypeOob          == 0u &&
                                s_v5FrameBaseInstMissing  == 0u &&
                                s_v5FrameGlErrors         == 0u) ? 1 : 0;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] frame=%u event=dispatch_summary"
                    " slots_considered=%u draws_issued=%u zero_instance_skips=%u"
                    " sorted_oob=%u packet_oob=%u type_oob=%u base_instance_missing=%u"
                    " gl_errors=%u ok=%d\n",
                    s_v5TotalFrameCount,
                    s_alphaOffCmdCount + s_alphaOnCmdCount,
                    s_v5FrameDrawsIssued,
                    s_v5FrameZeroInstSkips,
                    s_v5FrameSortedOob,
                    s_v5FramePacketOob,
                    s_v5FrameTypeOob,
                    s_v5FrameBaseInstMissing,
                    s_v5FrameGlErrors,
                    ok);
            }

        } else if (s_staticPropBc7Enabled && !s_slotBucketIndex.empty() &&
                   !s_bucketCmdCount.empty()) {
            // ---- COMPRESSION-BC7-STATICPROP-2 bucketed multidraw path ----
            // One glMultiDrawElementsIndirect per contiguous bucket run. Bucket
            // runs are ordered alpha-OFF buckets first, then alpha-ON (build
            // order), so alpha render ordering is preserved. drawIDBase = the
            // run's first global cmd index; the indirect-cmd buffer is indexed
            // by global sorted slot so per-run offset = cmdBase * cmdSize.
            // SSBO bind: legacy mode needs the group's sub-range; non-legacy
            // binds the full used range once. A bucket is wholly within one
            // group, so we select by the bucket's alpha group.
            glActiveTexture(GL_TEXTURE0);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());
            if (!s_globalPoolLegacy) {
                const size_t totalUsed = s_totalUsedBytesThisFrame;
                gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                 (long long)fr_off_bytes_d,
                                 (long long)(totalUsed > 0 ? totalUsed : sizeof(GpuStaticPropInstance)),
                                 "sp.coalesce.bucket");
            }
            uint32_t cmdBase = 0u;
            for (uint32_t b = 0u; b < s_bucketCmdCount.size(); ++b) {
                const uint32_t count = s_bucketCmdCount[b];
                if (count == 0u) continue;
                if (b < s_bucketArrays.size() && s_bucketArrays[b] != 0u)
                    glBindTexture(GL_TEXTURE_2D_ARRAY, s_bucketArrays[b]);
                // STATICPROP-MATERIAL-ORM-1: bind the layer-aligned ORM sibling to
                // kOrmTexUnit, then restore active unit 0 for the next albedo bind.
                if (s_ormSlotsEnabled && b < s_ormBucketArrays.size() &&
                    s_ormBucketArrays[b] != 0u) {
                    glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
                    glBindTexture(GL_TEXTURE_2D_ARRAY, s_ormBucketArrays[b]);
                    glActiveTexture(GL_TEXTURE0);
                }
                if (s_globalPoolLegacy) {
                    const bool isOnGroup = (b < s_bucketInfo.size() && s_bucketInfo[b].group == 1u);
                    gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                     (long long)(fr_off_bytes_d + (isOnGroup ? off_total_bytes : 0u)),
                                     (long long)(isOnGroup ? on_total_bytes : off_total_bytes),
                                     "sp.coalesce.bucket.legacy");
                }
                if (s_locsCoalesce.drawIDBase >= 0)
                    glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(cmdBase));
                const uintptr_t runOffset =
                    static_cast<uintptr_t>(cmdBase) *
                    static_cast<uintptr_t>(gpu_cull::kDrawElementsIndirectCommandSize);
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(runOffset),
                    static_cast<GLsizei>(count),
                    0);
                cmdBase += count;
            }
        } else {
            // ---- Existing multidraw path (unchanged) ----
            // STATICPROP-MATERIAL-ORM-1: this path binds NO ORM array (it uses the
            // legacy s_texArrayOff/On group arrays, not the per-bucket siblings), so
            // force u_ormSampleEnable=0 to keep the shader from sampling a stale /
            // unbound ORM unit. S5-B: the per-frame baseline above already drives this
            // to 0 whenever !s_staticPropBc7Enabled (this branch only runs in that
            // case), so this is now defensive belt-and-suspenders. loc-guarded → no-op
            // until the shader declares it.
            if (s_locsCoalesce.ormSampleEnable >= 0)
                glUniform1i(s_locsCoalesce.ormSampleEnable, 0);
            // 11.7.g — alpha-OFF group draw.
            // Step 4.8: under global-pool mode bind once covering both groups; under legacy bind per-group.
            if (s_alphaOffCmdCount > 0u) {
                if (s_locsCoalesce.drawIDBase >= 0)
                    glUniform1i(s_locsCoalesce.drawIDBase, 0);
                if (s_globalPoolLegacy) {
                    gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                     (long long)(fr_off_bytes_d + 0u),
                                     (long long)off_total_bytes,
                                     "sp.coalesce.alphaOff.legacy");
                } else {
                    const size_t totalUsed = s_totalUsedBytesThisFrame;
                    gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                     (long long)fr_off_bytes_d,
                                     (long long)(totalUsed > 0 ? totalUsed : sizeof(GpuStaticPropInstance)),
                                     "sp.coalesce.alphaOff");
                }
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(0)),
                    static_cast<GLsizei>(s_alphaOffCmdCount),
                    0);
            }

            // 11.7.h — alpha-ON group draw.
            if (s_alphaOnCmdCount > 0u) {
                if (s_locsCoalesce.drawIDBase >= 0)
                    glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(s_alphaOffCmdCount));
                if (s_globalPoolLegacy) {
                    gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                     (long long)(fr_off_bytes_d + off_total_bytes),
                                     (long long)on_total_bytes,
                                     "sp.coalesce.alphaOn.legacy");
                }
                // else: single bind from alpha-OFF draw still covers alpha-ON (same range).
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());
                const uintptr_t alphaOnOffset =
                    static_cast<uintptr_t>(s_alphaOffCmdCount) *
                    static_cast<uintptr_t>(gpu_cull::kDrawElementsIndirectCommandSize);
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(alphaOnOffset),
                    static_cast<GLsizei>(s_alphaOnCmdCount),
                    0);
            }
        }

        // 11.7.i — restore indirect-buffer binding.
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

        // STATICPROP-MATERIAL-ORM-1: unbind the ORM unit and restore active unit 0
        // so we leave GL state exactly as the gate-OFF path does. Only touch the
        // ORM unit when the gate is ON (no extra state churn when OFF).
        if (s_ormSlotsEnabled) {
            glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            glActiveTexture(GL_TEXTURE0);
        }

        // 11.7.j — restore slot 4 + unit-0 GL_TEXTURE_BINDING_2D_ARRAY via the
        // guards' explicit .reset() (SSBO first, then texture, matching the
        // old bind-order). Active texture stays GL_TEXTURE0 (guard restores
        // it). Slot 15 restore lives at the Step 10.3 envelope in
        // compute_dispatch().
        guardSsbo4.reset();
        guardTex2DArray.reset();

        // 11.7.k — first-flush latch (per-mission). event=ready is logged
        // here, not at finalize, because finalize emits event=armed and
        // we want a separate ready signal that confirms the GPU actually
        // accepted the coalesce draw without disarm.
        if (!s_coalesceFirstFlushDone) {
            MC2_DIAGFE(stderr,
                "[COALESCE v1] event=ready buckets_off=%u buckets_on=%u "
                "cmds_off=%u cmds_on=%u\n",
                s_alphaOffCount, s_alphaOnCount,
                s_alphaOffCmdCount, s_alphaOnCmdCount);
            s_coalesceFirstFlushDone = true;
        }
    } else {
        // ---- Step 11.8 legacy per-type/per-packet draw loop (unchanged) ----

        // GPU-SYNC-CONTRACT: the legacy instance/color SSBOs were CPU-written this
        // frame through a persistent GL_MAP_COHERENT_BIT mapping (instMapBase /
        // colMapBase). The ring's glClientWaitSync handles cross-frame WAR reuse, but
        // ordering THIS frame's CPU writes BEFORE the draws reads them still needs
        // GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT. Without it, NVIDIA reads stale instance
        // data each running frame -> props invisible during gameplay, reappearing only
        // when paused (the per-frame rewrite stops). The legacy path is the ACTIVE one
        // on NVIDIA (coalesce disarms there), so this is the live fix. AMD tolerated it.
        gpuSyncBarrier(GpuProducer::CpuCoherentWrite, GpuConsumer::InstancedDraw,
                       "staticprop_legacy_instance");

        // v1: applyPipeline() above bound the coalesce program (if available).
        // Restore the legacy program before issuing legacy uniform uploads and draws.
        glUseProgram(s_staticPropProgram);

        // Direct uniforms for the legacy path. Static props use the same
        // CPU-composed terrainMVP as terrain/terrain_overlay.vert: axisSwap *
        // worldToClip, row-major rewritten in gamecam.cpp, uploaded GL_FALSE.
        {
            const GLint locTerrainMVP = glGetUniformLocation(s_staticPropProgram, "u_worldToClipGL");
            const float* terrainMVP = gos_GetObjectFixBMVPMat4();
            if (locTerrainMVP >= 0 && terrainMVP)
                glUniformMatrix4fv(locTerrainMVP, 1, GL_FALSE, terrainMVP);
            const GLint locMVP = glGetUniformLocation(s_staticPropProgram, "u_mvp");
            const float* mm = gos_GetProj2ScreenMat4();
            if (locMVP >= 0 && mm) glUniformMatrix4fv(locMVP, 1, GL_TRUE, mm);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_tex"),           0);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_debugAddrMode"), debugAddrMode_);
            // FIXME(task-10): no clean per-scene global fog scalar source
            // available for static props; per-instance fog color is on v_fog.
            // 1.0 == "clear" per shader convention. Revisit for distance fog.
            glUniform1f(glGetUniformLocation(s_staticPropProgram, "u_fogValue"),      1.0f);
            // V-AMBIENT-STATIC-1: hemisphere ambient strength (legacy program).
            // Same semantics as coalesce site: 0.0 default -> byte-identical OFF.
            if (s_locsLegacy.ambientV1Strength >= 0)
                glUniform1f(s_locsLegacy.ambientV1Strength,
                            staticPropAmbientV1Enabled() ? 1.0f : 0.0f);
            // PROP-BASE-LIGHT-DIR-1: same semantics as the coalesce site.
            // 0.0 default -> byte-identical OFF.
            if (s_locsLegacy.propBaseLightDirStrength >= 0)
                glUniform1f(s_locsLegacy.propBaseLightDirStrength,
                            s_propBaseLightDirStrength);
            // TERRAIN-DECAL-FILL-1: cliff-decal shadow-side ambient floor (legacy
            // program). Frag applies it only to kFlagDecalFill (bit 3) fragments.
            if (s_locsLegacy.terrainDecalFill >= 0)
                glUniform1f(s_locsLegacy.terrainDecalFill, g_terrainDecalFill);
            // TERRAIN-DECAL-COLORBLEND-1: colormap atlas + UV params + blend
            // (legacy program). Same flag-gated no-op contract as coalesce.
            if (s_locsLegacy.terrainDecalColorBlend >= 0)
                glUniform1f(s_locsLegacy.terrainDecalColorBlend, g_terrainDecalColorBlend);
            // BUILDING-GLASS-1: dither-glass translucency toggle (legacy program).
            if (s_locsLegacy.glassDither >= 0)
                glUniform1f(s_locsLegacy.glassDither,
                            s_glassDitherEnabled ? 1.0f : 0.0f);
            if (g_terrainDecalColorBlend > 0.0f) {
                extern GLuint gos_terrain_indirect_getAtlasGLTex();
                extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
                extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
                extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
                glActiveTexture(GL_TEXTURE0 + kColormapTexUnit);
                glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());
                glActiveTexture(GL_TEXTURE0);
                if (s_locsLegacy.terrainColormap >= 0)
                    glUniform1i(s_locsLegacy.terrainColormap, static_cast<GLint>(kColormapTexUnit));
                if (s_locsLegacy.decalAtlasTLX >= 0)
                    glUniform1f(s_locsLegacy.decalAtlasTLX, gos_terrain_indirect_getAtlasMapTopLeftX());
                if (s_locsLegacy.decalAtlasTLY >= 0)
                    glUniform1f(s_locsLegacy.decalAtlasTLY, gos_terrain_indirect_getAtlasMapTopLeftY());
                if (s_locsLegacy.decalAtlasOOW >= 0)
                    glUniform1f(s_locsLegacy.decalAtlasOOW, gos_terrain_indirect_getAtlasOneOverWorldUnits());
            }
            // V-MATERIAL-DEBUG-1: per-frag material debug view (legacy program).
            // Default 0 = OFF; shader skips entire debug branch when 0.
            if (s_locsLegacy.debugMaterialMode >= 0)
                glUniform1i(s_locsLegacy.debugMaterialMode,
                            s_staticPropDebugMaterialMode);
            // LIGHTING-DEBUG-VIEWS-1B: unified lighting debug channel (legacy
            // program). Resolver -1 -> upload 0 -> pixel-invariant default.
            {
                extern int mc2LightingDebugMode();
                int lvm = mc2LightingDebugMode();
                glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_lightingDebugView"),
                            lvm < 0 ? 0 : lvm);
            }
            // MC2_SHADER_PATH_TINT: solid-color path-id debug (default 0 = OFF).
            if (s_locsLegacy.pathTint >= 0)
                glUniform1i(s_locsLegacy.pathTint, mc2ShaderPathTint());
            // GREYBEARD-DIFFUSE-TEST: upload raw MC2 terrain sun for forced diffuse.
            if (s_locsLegacy.terrainSunMC2 >= 0) {
                extern void gos_GetTerrainLightDir(float*, float*, float*);
                float tsx = 0.0f, tsy = 0.0f, tsz = 1.0f;
                gos_GetTerrainLightDir(&tsx, &tsy, &tsz);
                glUniform3f(s_locsLegacy.terrainSunMC2, tsx, tsy, tsz);
            }
            // EDGE-LIGHT-EMISSION-1: stock hot-colour NIGHT tag state. Gate OFF ->
            // 0/0/0 uploaded -> get_base_light takes its daytime arms -> render is
            // byte-identical to the pre-fix build.
            if (s_locsLegacy.isNight >= 0 || s_locsLegacy.nightFactor >= 0) {
                bool  nIsNight = false;
                float nFactor  = 0.0f;
                // BEACON-FIXTURE-ID-1: the fixture gate also needs real night state.
                // Safe for non-fixtures: their scale stays 0.0, so get_base_light
                // keeps its daytime result (delta * 0 == 0).
                if (s_nightTagMode != 0 || s_fixtureLightsScale > 0.0f ||
                    s_fixtureGlowStrength > 0.0f) {
                    extern void gos_GetNightState(bool*, float*);
                    gos_GetNightState(&nIsNight, &nFactor);
                }
                if (s_locsLegacy.isNight >= 0)
                    glUniform1i(s_locsLegacy.isNight, nIsNight ? 1 : 0);
                if (s_locsLegacy.nightFactor >= 0)
                    glUniform1f(s_locsLegacy.nightFactor, nFactor);
                if (s_locsLegacy.nightEmissiveScale >= 0)
                    glUniform1f(s_locsLegacy.nightEmissiveScale,
                                (s_nightTagMode != 0) ? s_nightTagScale : 0.0f);
                if (s_locsLegacy.fixtureEmissiveScale >= 0)
                    glUniform1f(s_locsLegacy.fixtureEmissiveScale, s_fixtureLightsScale);
                // FIXTURE-GLOW-1: 0.0 -> v_fixtureGlow is vec3(0.0) -> byte-identical.
                if (s_locsLegacy.fixtureGlowStrength >= 0)
                    glUniform1f(s_locsLegacy.fixtureGlowStrength, s_fixtureGlowStrength);
            }
            // FAR-BACKDROP-LIT: upload gate + distance band + tint (legacy).
            // Mirror of the coalesce block above; OFF -> u_farBackdropLit=0 ->
            // byte-identical.
            if (s_locsLegacy.farBackdropLit >= 0)
                glUniform1i(s_locsLegacy.farBackdropLit, s_farBackdropLitEnabled ? 1 : 0);
            if (s_locsLegacy.farBackdropNear >= 0)
                glUniform1f(s_locsLegacy.farBackdropNear, s_farBackdropNear);
            if (s_locsLegacy.farBackdropFar >= 0)
                glUniform1f(s_locsLegacy.farBackdropFar, s_farBackdropFar);
            if (s_locsLegacy.farBackdropStrength >= 0)
                glUniform1f(s_locsLegacy.farBackdropStrength, s_farBackdropStrength);
            if (s_locsLegacy.farBackdropTint >= 0)
                glUniform3f(s_locsLegacy.farBackdropTint,
                            s_farBackdropTint[0], s_farBackdropTint[1], s_farBackdropTint[2]);
            // POOL-CONSUMERS-1 (batch-6 item 6): night light-pool consumption
            // (legacy program path; same gating as the coalesce site).
            gos_terrain_lighting::UploadObjectLightPoolUniforms(s_staticPropProgram);
            // V-IBL-STATIC-1: SH-L2 image-based ambient (legacy program). Same
            // semantics as coalesce site: strength 0.0 default -> byte-identical OFF.
            // V-IBL-STATIC-2: same per-mission source as coalesce site.
            // IBL-SUN-ROTATE-1: same accessor as the coalesce site; gate OFF
            // returns the unchanged shipped-table pointer.
            if (s_locsLegacy.iblSh >= 0)
                glUniform3fv(s_locsLegacy.iblSh, 9,
                             iblShCoeffsForUpload());
            if (s_locsLegacy.iblShStrength >= 0)
                glUniform1f(s_locsLegacy.iblShStrength,
                            iblShStrengthNightScaled());
            // V-MATERIAL-PBR-2: legacy program upload. Same gating as coalesce
            // site (env + ViewUniforms-disabled interlock). Default-OFF is
            // upload 0.0f -> shader short-circuit.
            if (s_locsLegacy.pbrV1Strength >= 0) {
                const float pbrStrength =
                    (pbrV1Enabled() && !s_viewUniformsDisabled)
                        ? g_pbrV1Strength : 0.0f;
                glUniform1f(s_locsLegacy.pbrV1Strength, pbrStrength);
            }
            // V-MATERIAL-PBR-2-TUNE-UI: legacy program override upload (mirror).
            if (s_locsLegacy.pbrV1RoughnessOverride >= 0) {
                const float pbrRough =
                    (g_pbrV1RoughnessOverrideEnabled && !s_viewUniformsDisabled)
                        ? g_pbrV1RoughnessOverrideValue : -1.0f;
                glUniform1f(s_locsLegacy.pbrV1RoughnessOverride, pbrRough);
            }
            // V-MATERIAL-PBR-2-DIAG: legacy program diag visualizer upload.
            if (s_locsLegacy.pbrV1DiagSunFound >= 0) {
                glUniform1i(s_locsLegacy.pbrV1DiagSunFound,
                            s_pbrV1DiagSunFound ? 1 : 0);
            }
        }

    // [SEAMPROBE] stage 8: one-shot per-type draw census (which types have a
    // range / instances / packets at first legacy draw). Gated on MC2_MODOVERRIDE_TRACE.
    {
        static bool s_seamDrawCensus = false;
        if (!s_seamDrawCensus && mc2::gate::get<"MC2_MODOVERRIDE_TRACE">()) {
            s_seamDrawCensus = true;
            for (uint32_t tid = 0; tid < s_types.size(); ++tid) {
                auto rit2 = s_typeRanges.find(tid);
                bool hasRange = (rit2 != s_typeRanges.end());
                uint32_t ic = hasRange ? rit2->second.instanceCount : 0;
                const GpuStaticPropType& ty = s_types[tid];
                if (ty.packetCount > 0 && (!hasRange || ic == 0)) {
                    fprintf(stderr, "[SEAMPROBE] draw-census typeID=%u src=%p packetCount=%u vertexCount=%u hasRange=%d instanceCount=%u (NOT DRAWN)\n",
                        tid, (void*)ty.source, ty.packetCount, ty.vertexCount, (int)hasRange, ic);
                }
            }
            fflush(stderr);
        }
    }
    for (uint32_t typeID = 0; typeID < s_types.size(); ++typeID) {
        auto rit = s_typeRanges.find(typeID);
        if (rit == s_typeRanges.end()) continue;
        const TypeRangeSsbo& r = rit->second;
        const GpuStaticPropType& type = s_types[typeID];
        if (r.instanceCount == 0 || type.packetCount == 0) continue;

        // Gate F: count actors that produced ≥1 packet draw this frame.
        // Per-type increment, not per-packet — we want "actors drawn,"
        // not "draw-call count."
        // STALE-COUNTER-RETIRE-1: this is the ONLY increment of gpu_drawn_instances and it
        // reads the CPU-side TypeRangeSsbo.instanceCount, which is meaningful only on this
        // legacy/per-type path. The live GPU-authority coalesce-v6 / C1b path authors
        // instance counts on the GPU (indirect buffer) and never reaches here, so the
        // counter stays 0 there. The [OBJBATCHER v1] summary carries gpu_drawn_note to say
        // so; read submitted_instances for the live signal. (Do NOT add a per-frame GPU
        // readback to "fix" this — that would stall the frame.)
        s_counters.gpu_drawn_instances += r.instanceCount;

        // [INSTDIAG] MC2_STATIC_INST_DIAG=1 — peek the EXACT instance data the GPU will
        // read at the bound offset (s_instanceMap is the coherent CPU mapping). Decisive
        // for the no-trees-on-registered-path bug: if data here is ZERO, the bucket/upload
        // dropped it (LEAFDIAG already proved recipe leaves are valid -> reinject/submit
        // bug); if VALID, the data is fine and the bug is downstream (shader/cull/ring).
        // Focus on high-instance types (trees). Default-off: zero behavior change.
        {
            static const bool s_instDiag = mc2::gate::get<"MC2_STATIC_INST_DIAG">();
            static int s_instDiagLogged = 0;
            if (s_instDiag && r.instanceCount >= 20 && s_instDiagLogged < 40 && s_instanceMap) {
                ++s_instDiagLogged;
                const uint8_t* base = static_cast<const uint8_t*>(s_instanceMap)
                                    + static_cast<size_t>(r.instanceByteOffset);
                const float* m0 = reinterpret_cast<const float*>(base);
                const size_t midOff = static_cast<size_t>(r.instanceCount / 2u)
                                    * sizeof(GpuStaticPropInstance);
                const float* mm = reinterpret_cast<const float*>(base + midOff);
                const bool z0 = (m0[0]==0.0f && m0[3]==0.0f && m0[7]==0.0f && m0[11]==0.0f);
                fprintf(stderr,
                    "[INSTDIAG] sample=first40 typeID=%u slot=%u instByteOff=%zu count=%u byteSize=%zu "
                    "drawData0_rawXYZ=(%.1f,%.1f,%.1f) zero0=%d midXYZ=(%.1f,%.1f,%.1f)\n",
                    typeID, (unsigned)s_frameSlot, (size_t)r.instanceByteOffset,
                    r.instanceCount, (size_t)r.instanceByteSize,
                    -m0[3], m0[11], m0[7], (int)z0, -mm[3], mm[11], mm[7]);
                fflush(stderr);
            }
        }

        gpuBindSsboRange(0, s_instanceSsbo,
                         (long long)r.instanceByteOffset, (long long)r.instanceByteSize,
                         "sp.instance");
        // STATICPROP-COLORS-FILL-DEBUGONLY-1: colorByteSize is 0 for static types
        // when the colors fill is skipped (the default). A 0-size bind is GL_INVALID_VALUE
        // — and no shader reads binding 1 — so skip it (gpuBindSsboRange would catch it too).
        if (r.colorByteSize)
            gpuBindSsboRange(1, s_colorSsbo,
                             (long long)r.colorByteOffset, (long long)r.colorByteSize,
                             "sp.color");

        // SEMANTIC: max VALID local vertex index, not count. Lets the
        // gradient debug mode hit t=1.0 at the last vertex.
        // NOTE: shader declares u_* ints (uniform uint crashes this engine's
        // shader compile); values are always positive and < 2^31, so
        // signed int is lossless here. Upload via glUniform1i.
        const int maxID = (type.vertexCount > 0u) ? (int)(type.vertexCount - 1u) : 0;
        glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_maxLocalVertexID"), maxID);

        // Slice 2 (object-offload) — Stage 2.D.1: per-type parity bind.
        //
        // The shader writes parityOut[gl_InstanceID * vertsPerType + gl_VertexID].
        // We bind a tight glBindBufferRange so that index always lands inside
        // the bound region. Each per-type draw consumes its bytes from a
        // monotonic cursor within slot s_frameSlot of the parity SSBO; if
        // the slot's budget would be exceeded by this draw, we fall back to
        // u_parityWrite=0 and the shader skips the write (the readback at
        // top of next visit then doesn't see those vertices — fine for
        // 2.D.1 acceptance, will be tightened in 2.D.2/2.D.3 if real-world
        // peak exceeds kParitySlotBytes).
        // Stage 2.D.2: the parity SSBO uses the expanded (triangle-soup) vertex
        // count so gl_VertexID in static_prop.vert stays in-bounds. gl_VertexID
        // runs 0..numTris*3-1 (per the VBO layout in registerType), so the
        // correct u_parityVertsPerType and SSBO range size is numTris*3, not
        // type.vertexCount (which is numTypeVertices, the shared-vertex count).
        const uint32_t parityVerts = (type.source && type.source->numTypeTriangles > 0u)
            ? type.source->numTypeTriangles * 3u
            : type.vertexCount;

        bool wroteParityThisDraw = false;
        if (parityBuffer != 0 && parityVerts > 0u) {
            const size_t needBytes = static_cast<size_t>(r.instanceCount) *
                                     static_cast<size_t>(parityVerts) *
                                     sizeof(uint32_t);
            // SSBO offset alignment requirement. Mirrors the per-type
            // instance/color bind alignment used by uploadAllBucketsIfNeeded
            // above (its s_ssboAlignment is function-scope, so query once
            // here too — single static, set on first hit).
            static GLint s_parityAlignment = 0;
            if (s_parityAlignment == 0) {
                glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT,
                              &s_parityAlignment);
                if (s_parityAlignment < 16) s_parityAlignment = 256;
            }
            const size_t alignMask = static_cast<size_t>(s_parityAlignment) - 1u;
            const size_t cursor = (s_parityBytesUsedThisFrame + alignMask) & ~alignMask;
            if (needBytes > 0 && cursor + needBytes <= kParitySlotBytes) {
                const GLintptr slotBase =
                    static_cast<GLintptr>(s_frameSlot) *
                    static_cast<GLintptr>(kParitySlotBytes);
                gpuBindSsboRange(3, (unsigned int)parityBuffer,
                                 (long long)(slotBase + (GLintptr)cursor),
                                 (long long)needBytes,
                                 "sp.parity");
                // u_parityVertsPerType = expanded (triangle-soup) vertex count.
                if (locParityVerts >= 0)
                    glUniform1i(locParityVerts, (int)parityVerts);
                // u_parityBaseVertex = type's VBO baseVertex; shader subtracts
                // this from gl_VertexID to get the type-local index.
                if (locParityBaseVertex >= 0) {
                    const int typeBase = (type.packetCount > 0)
                        ? s_packets[type.firstPacket].baseVertex
                        : 0;
                    glUniform1i(locParityBaseVertex, typeBase);
                }
                if (locParityWrite >= 0)
                    glUniform1i(locParityWrite, 1);
                s_parityBytesUsedThisFrame = cursor + needBytes;
                gos_object_parity::Counters_AddVerticesWrittenThisFrame(
                    static_cast<uint64_t>(r.instanceCount) *
                    static_cast<uint64_t>(parityVerts));
                wroteParityThisDraw = true;
                // Stage 2.D.2: record slot-relative cursor + expanded vertex
                // count so CompareAndReport can decode this type's bytes on
                // the readback frame. parityVerts (not type.vertexCount) here.
                if (gos_object_parity::IsDualEmitArmedAnyActor()) {
                    gos_object_parity::RecordParityTypeRange(
                        typeID, cursor, r.instanceCount, parityVerts);
                }
            } else if (needBytes > 0) {
                // Stage 2.D.1.1 (Item 1): slot budget exhausted — record it.
                // u_parityWrite stays 0 so the shader skips the write;
                // the draw still happens but no parity bytes are produced.
                gos_object_parity::Counters_AddSlotOverflowThisFrame(
                    typeID, needBytes, kParitySlotBytes);
            }
        }

        for (uint32_t p = 0; p < type.packetCount; ++p) {
            const GpuStaticPropPacket& pkt = s_packets[type.firstPacket + p];
            // Resolve the texture handle at draw time. MC2 mutates
            // TG_TypeShape::listOfTextures[slot].gosTextureHandle each frame
            // in TransformMultiShape,
            // so capturing the handle at registration time gives stale
            // (usually zero) reads.
            uint32_t gosHandle = 0;
            const TG_TypeShape* src = type.source;
            if (src && src->listOfTextures && pkt.textureSlot < src->numTextures) {
                gosHandle = src->listOfTextures[pkt.textureSlot].gosTextureHandle;
            }
            const uint32_t glTexId = gos_GetGLTextureId(gosHandle);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, glTexId);
            // Re-resolve materialFlags at draw time so the textureAlpha flag set during
            // bdactor.cpp init (after registerType) is captured. Same pattern as gosTextureHandle.
            // textureAlpha==true means the CPU path routes via MC2_DRAWALPHA; the GPU path
            // approximates this with shader discard (valid for binary-alpha fence/gate textures).
            uint32_t effectiveMaterialFlags = pkt.materialFlags;
            if (src && src->listOfTextures && pkt.textureSlot < src->numTextures &&
                src->listOfTextures[pkt.textureSlot].textureAlpha) {
                effectiveMaterialFlags |= STATIC_PROP_FLAG_ALPHA_TEST;
            }
            ALPHA_TRACE("draw type=%u pkt=%u slot=%u pktFlags=0x%x effective=0x%x texAlpha=%d",
                        typeID, p, pkt.textureSlot, pkt.materialFlags, effectiveMaterialFlags,
                        (src && src->listOfTextures && pkt.textureSlot < src->numTextures)
                            ? (int)src->listOfTextures[pkt.textureSlot].textureAlpha : -1);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_materialFlags"),
                        (int)effectiveMaterialFlags);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_packetID"),
                        (int)(type.firstPacket + p));
            // Drain any stale GL error first so our check is clean.
            while (glGetError() != GL_NO_ERROR) {}
            if (useC1bIndirect) {
                // C1b GPU authority flip: glDrawElementsIndirect reads the command
                // at offset typeID*20 from the GPU indirect buffer. instanceCount
                // was written by the patch shader (compute_dispatch ran before flush).
                // baseVertex in the struct handles the VBO offset — no separate pass.
                // Sampler state trap: ensure REPEAT/LINEAR is bound before first
                // indirect draw so world-scale UVs don't collapse to texture edge.
                // (The per-packet texture bind above uses GL_TEXTURE_2D default
                //  sampler which inherits from prior state — explicitly set here.)
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());
                const GLintptr cmdOffset = static_cast<GLintptr>(typeID * 20);
                glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                                       reinterpret_cast<const void*>(cmdOffset));
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
            } else {
                glDrawElementsInstancedBaseVertex(
                    GL_TRIANGLES,
                    pkt.indexCount,
                    GL_UNSIGNED_INT,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                    r.instanceCount,
                    pkt.baseVertex);
            }
        }
        // Stage 2.D.1: clear u_parityWrite back to 0 so a subsequent type
        // that gets rejected by the slot-budget check (or has zero
        // vertices) can't inherit a stale write authorization against this
        // type's binding range.
        if (wroteParityThisDraw && locParityWrite >= 0) {
            glUniform1i(locParityWrite, 0);
        }
    }
    } // end Step 11.8 else (!IsCoalesceEnabled() legacy draw loop)

    s_fence[s_frameSlot] = MC2_GL_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    // Plan v3.8 Step 11.9 — coalesce fence insert after all draws issued.
    // Independent of legacy s_fence[]: tracks s_coalesceInstanceSsbo's
    // ring-slot lifecycle so the next visit (RING_FRAMES frames from now)
    // can wait on this fence before overwriting the slot's CPU writes.
    if (IsCoalesceEnabled()) {
        // Step 4.4 — use s_coalesceFrameSlot (coalesce ring), not s_frameSlot (legacy ring).
        s_coalesceFence[s_coalesceFrameSlot] = MC2_GL_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    // Stage 2.D.1: record this slot's parity-byte usage so the next visit
    // (RING_FRAMES frames from now) knows exactly how much to glGetBufferSubData.
    s_parityBytesUsedPerSlot[s_frameSlot] = s_parityBytesUsedThisFrame;
    // Stage 2.D.2: after the fence is inserted (GPU draw committed), advance
    // the dual-emit latch from Armed → WaitingForReadback. Pass s_frameSlot
    // so the compare only fires when the ring revisits this exact slot (after
    // glClientWaitSync ensures GPU writes are complete — RING_FRAMES later).
    if (gos_object_parity::IsDualEmitArmedAnyActor()) {
        gos_object_parity::AdvanceDualEmitToWaiting(s_frameSlot);
    }

    // Restore GL state to EXACTLY what it was at flush start.
    // GLSTATE-GUARD-SITE5: explicit .reset() in the same order the hand-
    // rolled epilogue used to restore (SSBOs, then texture unit 0, then
    // program/VAO/buffer bindings) — must close here, BEFORE
    // gos_InvalidateRenderStateCache() below, not at this optional's natural
    // (function-end) scope exit. See the capture-site comment above.
    guardSsbo0.reset();
    guardSsbo1.reset();
    guardSsbo2.reset();
    guardSsbo3.reset();
    guardSsbo5.reset();
    guardTexUnit0.reset();
    guardVao.reset();
    guardArrayBuf.reset();
    guardElemBuf.reset();
    guardProgram.reset();
    // Pipeline state: no explicit restore needed — applyPipeline() set state
    // above, and gos_InvalidateRenderStateCache() below ensures the next
    // applyRenderStates() call re-applies from scratch regardless of what
    // applyPipeline left.
    accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);

    // Slice 2 (object-offload) — Stage 2.D.1: tick parity counters and emit
    // the 600-frame summary line if the cadence is hit. Internally a no-op
    // when MC2_OBJECT_PARITY_CHECK is unset.
    gos_object_parity::ParityFrameTick();

    // Stage 2.D.3 — round-robin sampler pick for next frame.
    //
    // Called AFTER ParityFrameTick so the summary's parity_compared_actors
    // sees the just-finalized count, and AFTER AdvanceDualEmitToWaiting so
    // the state machine has progressed for this frame. PickNextSampleIfReady:
    //   - rotates this frame's submitMultiShape observation list into the
    //     prior-frame pool (always),
    //   - if state == Done AND the pool is non-empty, picks one shape via
    //     round-robin and re-arms (state=Armed, armMode=SampledOnly,
    //     s_currentSampledShape=picked).
    // Default-off inside the sidecar; safe to call unconditionally.
    gos_object_parity::PickNextSampleIfReady();

    s_bucketsByType.clear();
    s_reinjectedThisFrame = false;
    s_lastUploadedSlot = 0xFFFFFFFFu;  // reset for next frame

    // RENDER_STATES v1: this path mutates GL_TEXTURE_WRAP_S/T (lines 1700-1701
    // above) on the bound texture object — persistent state that survives
    // function exit and is not tracked by applyRenderStates' cache. Without
    // invalidation, a subsequent applyRenderStates call would early-out on
    // matching tracked-slot values while wrap stays at REPEAT even when
    // gos_State_TextureAddress=Clamp was requested. CRITICAL-1 from the
    // 2026-05-08 adversarial review of the state-equality early-out.
    gos_InvalidateRenderStateCache();

    // v2.1: snapshot per-type instance counts for next-frame extraction.
    // s_typeRanges is module-static and holds this frame's counts.
    // Reading here (before s_bucketsByType.clear) gives prior-frame
    // semantics to batcher_getDrawSlotEntry() called from ExtractRenderSnapshot().
    s_typeInstanceCountPrevFrame.clear();
    for (const auto& kv : s_typeRanges)
        s_typeInstanceCountPrevFrame[kv.first] = kv.second.instanceCount;

    // 2026-05-11 perf diag: flush() total + coalesce sub-stage timings.
    {
        const auto _btf_tEnd = std::chrono::steady_clock::now();
        const uint64_t total_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                _btf_tEnd - _btf_t0).count());
        s_btf_ns_total += total_ns;
        s_btf_ns_coalesce_draw += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                _btf_tEnd - _btf_t_drawStart).count());
        ++s_btf_calls;
        if (s_btf_enabled && (s_btf_calls % 600) == 0) {
            const double mean_total_us =
                static_cast<double>(s_btf_ns_total) / s_btf_calls / 1000.0;
            const double mean_write_us =
                static_cast<double>(s_btf_ns_coalesce_write) / s_btf_calls / 1000.0;
            const double mean_draw_us =
                static_cast<double>(s_btf_ns_coalesce_draw) / s_btf_calls / 1000.0;
            std::fprintf(stderr,
                "[BTF_TIMING v1] calls=%llu mean_total_us=%.2f "
                "mean_coalesce_write_us=%.2f mean_drawpath_us=%.2f\n",
                (unsigned long long)s_btf_calls,
                mean_total_us, mean_write_us, mean_draw_us);
            std::fflush(stderr);
        }
    }

    // FOLIAGE-STATICPROP-DEPTH-PREPASS-1 (C): the color EQUAL/no-write override is
    // applied inline above directly off flushDepthPrepassV6()'s return value — there
    // is no per-frame "prepass ran" flag to consume (review MINOR-1 removed the dead
    // s_depthPrepassRanThisFrame state that was set+reset but never read).
}
