// GpuStaticPropBatcher: GpuStaticPropBatcher::flush -- the orchestrator and the setup / coalesce-write helpers.
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"
#include "gos_static_prop_flush_frame.h"
#include "mdi_submit.h"                          // MDI-SUBMISSION-SCAFFOLD-1: flush issues the indirect draws
#include "mc2fmt.h"


// [RENDER_CONTRACT:Pass=StaticProp id=GpuStaticPropBatcher_flush]
//   Routes through static_prop.frag which writes
//   rc_gbuffer1_screenShadowEligible (production) or
//   rc_gbuffer1_legacyDebugSentinelScreenShadowEligible (debug).

// Apply the opaque pipeline state, bind the shared VAO / IBO and per-type SSBO, clear the parity slot.
void GpuStaticPropBatcher::applyOpaqueState(StaticPropFlushFrame& f) {
    auto& parityBuffer = f.parityBuffer; auto& locParityWrite = f.locParityWrite; auto& locParityVerts = f.locParityVerts; auto& locParityBaseVertex = f.locParityBaseVertex;
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
    locParityWrite = s_loc_u_parityWrite;
    locParityVerts = s_loc_u_parityVertsPerType;
    locParityBaseVertex = s_loc_u_parityBaseVertex;
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
}

// Draw the static-population split when its gate is armed.
void GpuStaticPropBatcher::drawStaticPopulation() {
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
}

// BC7 bucketed multidraw dispatch.
void GpuStaticPropBatcher::dispatchBc7Buckets(StaticPropFlushFrame& f) {
    auto& fr_off_bytes_d = f.fr_off_bytes_d; auto& off_total_bytes = f.off_total_bytes; auto& on_total_bytes = f.on_total_bytes;
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
}

// Existing indirect multidraw dispatch.
void GpuStaticPropBatcher::dispatchMultiDraw(StaticPropFlushFrame& f) {
    auto& fr_off_bytes_d = f.fr_off_bytes_d; auto& off_total_bytes = f.off_total_bytes; auto& on_total_bytes = f.on_total_bytes;
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

// Legacy path: draw each type.
void GpuStaticPropBatcher::legacyDrawLoop(StaticPropFlushFrame& f) {
    auto& parityBuffer = f.parityBuffer; auto& locParityWrite = f.locParityWrite; auto& locParityVerts = f.locParityVerts; auto& locParityBaseVertex = f.locParityBaseVertex;
    auto& useC1bIndirect = f.useC1bIndirect;
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
                mc2::fmt::print_err("[INSTDIAG] sample=first40 typeID={:d} slot={:d} instByteOff={:d} count={:d} byteSize={:d} drawData0_rawXYZ=({:.1f},{:.1f},{:.1f}) zero0={:d} midXYZ=({:.1f},{:.1f},{:.1f})\n", mc2::fmt::as_u32(typeID), mc2::fmt::as_u32((unsigned)s_frameSlot), mc2::fmt::as_size((size_t)r.instanceByteOffset), mc2::fmt::as_u32(r.instanceCount), mc2::fmt::as_size((size_t)r.instanceByteSize), -m0[3], m0[11], m0[7], mc2::fmt::as_i32((int)z0), -mm[3], mm[11], mm[7]);
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
}

// Draws the frame static props. The top-level pass guard, the timing clocks, the early bail-outs and the coalesce /
// legacy branch skeleton stay here; each phase body is a member helper declared in gos_static_prop_batcher.h.
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
                    mc2::fmt::print_err("[LODBUG v1] event=debug_mode_env_override mode={:d}\n", mc2::fmt::as_i32(m));
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

    StaticPropFlushFrame f;
    candidateLogAndParityReadback(f);
    acquireGlGuards(f);
    applyOpaqueState(f);
    prepareIndirectAndFence(f);

    const auto _btf_t_writeStart = std::chrono::steady_clock::now();
    writeCoalesceInstances();
    // Step 11.6 — legacy CPU write path is unchanged: it already ran via
    // uploadAllBucketsIfNeeded() above (which filled s_instanceSsbo). No
    // gating on IsCoalesceEnabled() — that lets a runtime-disarm transition
    // mid-frame still draw legally via the legacy loop below.
    const auto _btf_t_drawStart = std::chrono::steady_clock::now();
    s_btf_ns_coalesce_write += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            _btf_t_drawStart - _btf_t_writeStart).count());

    logInstDiagPath();

    // Plan v3.8 Step 11.7 / 11.8 — coalesce branch first; on disarm,
    // fall through to the legacy per-type loop with no other state change.
    if (IsCoalesceEnabled()) {
        refreshAlphaFlags();
        bindCoalesceGuards(f);

        // 11.7.c — program already bound by applyPipeline() above (v1: PipelineRegistry
        // holds s_staticPropProgramCoalesce after loadProgramsIfNeeded; no explicit
        // glUseProgram needed here).

        uploadCoalesceMvp();
        uploadCoalesceUniforms();
        bindPerDrawAndMaterial();
        armV5();
        armV6();
        drawStaticPopulation();

        // 2026-05-11 per-packet rework: each indirect cmd is per-PACKET, so
        // the multidraw counts and the alpha-ON byte offset use per-PACKET
        // counts (s_alphaOffCmdCount / s_alphaOnCmdCount). The PerDrawEntry
        // SSBO is also per-packet, indexed by gl_DrawID = sorted-packet slot.

        decideV6(f);

        static std::vector<RenderCore::DrawPacket>  v6Packets;
        static std::vector<StaticPropDispatchMeta>  v6Meta;
        initV6State(f);
        resetV6FrameCounters(f);
        decideV5(f);

        if (f.runV6 && f.v6LockstepViolations == 0u) {
            dispatchV6(snap, v6Packets, v6Meta, f);
        } else if (f.runV5) {
            dispatchV5(f);
        } else if (s_staticPropBc7Enabled && !s_slotBucketIndex.empty() &&
                   !s_bucketCmdCount.empty()) {
            dispatchBc7Buckets(f);
        } else {
            dispatchMultiDraw(f);
        }

        finishCoalesceDraw(f);
    } else {
        // ---- Step 11.8 legacy per-type/per-packet draw loop (unchanged) ----

        legacyPrepare();
    legacyCensus();
    legacyDrawLoop(f);
    } // end Step 11.8 else (!IsCoalesceEnabled() legacy draw loop)

    fenceAndParitySlot();
    releaseGlGuards(f);
    finishFlush();

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
            mc2::fmt::print_err("[BTF_TIMING v1] calls={:d} mean_total_us={:.2f} mean_coalesce_write_us={:.2f} mean_drawpath_us={:.2f}\n", mc2::fmt::as_u64((unsigned long long)s_btf_calls), mean_total_us, mean_write_us, mean_draw_us);
            std::fflush(stderr);
        }
    }

    // FOLIAGE-STATICPROP-DEPTH-PREPASS-1 (C): the color EQUAL/no-write override is
    // applied inline above directly off flushDepthPrepassV6()'s return value — there
    // is no per-frame "prepass ran" flag to consume (review MINOR-1 removed the dead
    // s_depthPrepassRanThisFrame state that was set+reset but never read).
}
