// GpuStaticPropBatcher: the flush setup phases -- candidate log, GL guards, coalesce instance write, alpha flags
// and the coalesce program uniforms. The orchestrator is in gos_static_prop_batcher_flush.cpp.
#include "gos_static_prop_batcher_internal.h"
#include "mdi_submit.h"                          // MDI-SUBMISSION-SCAFFOLD-1: flush issues the indirect draws
#include "gos_static_prop_flush_frame.h"
#include "mc2fmt.h"


// Emit the candidate log and read back / reset the parity output buffer.
void GpuStaticPropBatcher::candidateLogAndParityReadback(StaticPropFlushFrame& f) {
    auto& parityBuffer = f.parityBuffer;
    batcher_buildCandidateLog();

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
    parityBuffer = 0;
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
        s_parityBytesUsedPerSlot[s_frameSlot] = 0;
    }
    s_parityBytesUsedThisFrame = 0;
}

// Save the program, VAO, buffer, texture-unit and SSBO bindings for the restore at exit.
void GpuStaticPropBatcher::acquireGlGuards(StaticPropFlushFrame& f) {
    auto& guardProgram = f.guardProgram; auto& guardVao = f.guardVao; auto& guardArrayBuf = f.guardArrayBuf; auto& guardElemBuf = f.guardElemBuf;
    auto& guardTexUnit0 = f.guardTexUnit0; auto& guardSsbo0 = f.guardSsbo0; auto& guardSsbo1 = f.guardSsbo1; auto& guardSsbo2 = f.guardSsbo2;
    auto& guardSsbo3 = f.guardSsbo3; auto& guardSsbo5 = f.guardSsbo5;
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
}

// Decide the compute-cull indirect draw and wait for the coalesce fence.
void GpuStaticPropBatcher::prepareIndirectAndFence(StaticPropFlushFrame& f) {
    auto& useC1bIndirect = f.useC1bIndirect;
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
    useC1bIndirect = s_c1bDrawOverride &&
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
}

// Write each type bucket into the coalesce instance map.
void GpuStaticPropBatcher::writeCoalesceInstances() {
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

            if (typeID >= s_perTypePeak.size()) s_perTypePeak.resize(typeID + 1, 0u);
            const uint32_t cnt = static_cast<uint32_t>(bucket.instances.size());
            if (cnt > s_perTypePeak[typeID]) s_perTypePeak[typeID] = cnt;

            if (bucket.instances.size() > type.instanceCap) {
                MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=type_overflow "
                             "type=%u count=%zu cap=%u\n",
                             typeID, bucket.instances.size(), type.instanceCap);
                s_coalesceArmed = false;
                break;
            }

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
}

// One-shot path diagnostic for the instance draw.
void GpuStaticPropBatcher::logInstDiagPath() {
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
            mc2::fmt::print_err("[INSTDIAG] PATH coalesceEnabled={:d} globalPoolLegacy={:d} instanceCapacity={:d} frameSlot={:d} typeRanges={:d}\n", mc2::fmt::as_i32((int)IsCoalesceEnabled()), mc2::fmt::as_i32((int)s_globalPoolLegacy), mc2::fmt::as_u32((unsigned)s_instanceCapacity), mc2::fmt::as_u32((unsigned)s_frameSlot), mc2::fmt::as_size(s_typeRanges.size()));
            fflush(stderr);
        }
    }
}

// Re-resolve the per-packet alpha-test flags into the per-draw SSBO.
void GpuStaticPropBatcher::refreshAlphaFlags() {
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
}

// Compute the group byte ranges and save SSBO slot 4 and the 2D-array texture binding.
void GpuStaticPropBatcher::bindCoalesceGuards(StaticPropFlushFrame& f) {
    auto& fr_off_bytes_d = f.fr_off_bytes_d; auto& off_total_bytes = f.off_total_bytes; auto& on_total_bytes = f.on_total_bytes; auto& guardSsbo4 = f.guardSsbo4;
    auto& guardTex2DArray = f.guardTex2DArray;
        fr_off_bytes_d =
            static_cast<size_t>(s_coalesceFrameSlot) * batcher_getCoalescePerFrameInstanceBytes();
        // Legacy mode: separate off/on group byte boundaries.
        // Global-pool mode: single contiguous range covering both groups.
        off_total_bytes = s_globalPoolLegacy
            ? s_offGroupTotalBytes
            : s_offGroupBytesThisFrame;
        on_total_bytes  = s_globalPoolLegacy
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
        guardSsbo4.emplace(4);
        guardTex2DArray.emplace(0, GL_TEXTURE_2D_ARRAY);
}

// Upload the object MVP to the coalesce program.
void GpuStaticPropBatcher::uploadCoalesceMvp() {
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
}

// Upload the shared uniforms to the coalesce program.
void GpuStaticPropBatcher::uploadCoalesceUniforms() {
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
        // EDGE-LIGHT-EMISSION-1: stock hot-colour NIGHT tag state for get_base_light
        // (lit windows, building base lights, runway/beacon fixtures).
        if (s_locsCoalesce.isNight >= 0 || s_locsCoalesce.nightFactor >= 0) {
            bool  nIsNight = false;
            float nFactor  = 0.0f;
            extern void gos_GetNightState(bool*, float*);
            gos_GetNightState(&nIsNight, &nFactor);
            if (s_locsCoalesce.isNight >= 0)
                glUniform1i(s_locsCoalesce.isNight, nIsNight ? 1 : 0);
            if (s_locsCoalesce.nightFactor >= 0)
                glUniform1f(s_locsCoalesce.nightFactor, nFactor);
            if (s_locsCoalesce.nightEmissiveScale >= 0)
                glUniform1f(s_locsCoalesce.nightEmissiveScale, kNightEmissiveScale);
            if (s_locsCoalesce.fixtureEmissiveScale >= 0)
                glUniform1f(s_locsCoalesce.fixtureEmissiveScale, kFixtureEmissiveScale);
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

        // 11.7.e — slot 1 is NOT bound (per v2r18 §3.X.1: colors_.c[]
        // unread in any live shader path; coalesce branch does not bind
        // it, and the legacy bind site at the per-type loop is dead state).

}

// Bind the per-draw SSBO and the material tables.
void GpuStaticPropBatcher::bindPerDrawAndMaterial() {
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
                mc2::fmt::to_buf(buf, "[MATERIAL_GPU v4] GL ERROR after bind (real): 0x{:x}\n", mc2::fmt::as_u32(bindErr));
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
                mc2::fmt::to_buf(buf, "[MATERIAL_GPU v4] GL ERROR after sample uniform: 0x{:x}\n", mc2::fmt::as_u32(uniformErr));
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

            char buf[96];
            if (sampleOn) {
                // sampleOn=true guarantees loc >= 0, so loc is always valid here.
                mc2::fmt::to_buf(buf, "[MATERIAL_GPU v4] event=sample_mode enabled=1 loc={:d}\n", mc2::fmt::as_i32(s_locsCoalesce.materialGpuSample));
            } else {
                mc2::fmt::to_buf(buf, "[MATERIAL_GPU v4] event=sample_mode enabled=0 reason={}\n", mc2::fmt::safe(reason));
            }
            MC2_DIAG_FPUTS(buf, stderr);
        }

}
