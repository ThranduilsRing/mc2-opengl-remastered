// GpuMechBatcher: submitActor, the executor opaque-state apply and GpuMechBatcher::flush (one 970-line function).
// Declared in the class headers; shared names in gos_mech_batcher_internal.h.
#include "gos_mech_batcher_internal.h"
#include "mc2fmt.h"
#include "Gates/render_options.h"


// APPLY-STATE-MECHOPAQUE-1: file-static flag mirroring s_staticPropStateAppliedByExecutor.
// When MC2_FRAMEGRAPH_EXECUTOR is ON, executorApplyMechOpaqueState() pre-applies the
// MechOpaque pipeline (at the flush call site in txmmgr renderLists) and sets this;
// flush()'s body then skips its own applyPipeline (one-shot reset). Gate OFF -> flag
// stays false -> body applies as before -> byte-identical.
static bool s_opaqueStateAppliedByExecutor = false;

// ---------------------------------------------------------------------------
// submitActor (Task 6) — bone staging + texture capture
// ---------------------------------------------------------------------------
bool GpuMechBatcher::submitActor(const GpuMechSubmitDesc& desc) {
    s_lastFailWasLateReg = false;

    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed) return false;
    if (!desc.mechShape || !desc.mechType) return false;

    const TypeLodKey key{desc.mechType, desc.currentLOD};
    auto it = s_typeLodIndex.find(key);
    if (it == s_typeLodIndex.end()) {
        s_lastFailWasLateReg = true;
        ++s_disallowedLateRegEvents;
        return false;
    }
    const uint32_t typeLodIdx = it->second;
    const GpuMechTypeLodRecord& rec = s_typeLodRecords[typeLodIdx];

    PendingSubmit ps;
    ps.desc       = desc;
    ps.typeLodIdx = typeLodIdx;
    ps.bones.reserve(rec.numBones);

    // Stage bone matrices from live shapeToWorld (set by TransformMultiShape).
    // listOfShapes[i].shapeToWorld is a Stuff::LinearMatrix4D with entries[12]
    // stored column-major: entries[(col<<2)+row], 3 explicit cols + implicit col3=[0,0,0,1].
    // Row k extraction: [entries[k], entries[4+k], entries[8+k], w] where w=1 for row3 only.
    // BT2018-SKEL-GPU-PALETTE-PLACEMENT-1: imported animated mech sources its bone
    // palette from boneT_i = shapeToWorld_root · model_delta_i. shapeToWorld_root is
    // the single merged node's live transform (placement, set by _HierarchyOnly);
    // model_delta_i (mc2mechanim, refreshed each frame) maps the Y-up rest VBO vertex
    // to the Z-up animated MODEL pose. M_sw (row-major) = entries[0..11] + (0,0,0,1).
    if (rec.importedGpuType) {
        const float* md = nullptr;
        // Per-actor palette + lift, keyed by this actor's instance shape (mechShape).
        const int mdCount = mc2mechanim::ImportedGpuModelDelta((const void*)desc.mechShape, &md);
        const float lift = mc2mechanim::ImportedGpuLift((const void*)desc.mechShape);
        const int   liftAxis = mc2mechanim::ImportedGpuLiftAxis();   // 0/1/2 = Stuff.x/y/z
        const int   liftIdx = 3 + 4 * (liftAxis & 3);                // F[3]/F[7]/F[11]
        float Msw[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        if (desc.mechShape->GetNumShapes() > 0) {
            const float* e = (const float*)desc.mechShape->listOfShapes[0].shapeToWorld.entries;
            for (int k = 0; k < 12; ++k) Msw[k] = e[k];
        }
        for (int b = 0; b < mdCount && b < (int)rec.numBones && md; ++b) {
            const float* D = md + (size_t)b * 16;
            float F[16];                                   // F = Msw * D (row-major)
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    F[r*4+c] = Msw[r*4+0]*D[0*4+c] + Msw[r*4+1]*D[1*4+c]
                             + Msw[r*4+2]*D[2*4+c] + Msw[r*4+3]*D[3*4+c];
            F[liftIdx] += lift;                            // foot-ground lift (world-up axis tunable)
            GpuMechBone bone;                              // pack columns of F
            bone.row0[0]=F[0]; bone.row0[1]=F[4]; bone.row0[2]=F[ 8]; bone.row0[3]=0.0f;
            bone.row1[0]=F[1]; bone.row1[1]=F[5]; bone.row1[2]=F[ 9]; bone.row1[3]=0.0f;
            bone.row2[0]=F[2]; bone.row2[1]=F[6]; bone.row2[2]=F[10]; bone.row2[3]=0.0f;
            bone.row3[0]=F[3]; bone.row3[1]=F[7]; bone.row3[2]=F[11]; bone.row3[3]=1.0f;
            ps.bones.push_back(bone);
        }
    } else {
        const int numShapes = desc.mechShape->GetNumShapes();
        for (int i = 0; i < numShapes && i < (int)rec.numBones; ++i) {
            const TG_ShapeRec& sr = desc.mechShape->listOfShapes[i];
            const float* e = (const float*)sr.shapeToWorld.entries;
            GpuMechBone bone;
            bone.row0[0]=e[0]; bone.row0[1]=e[4]; bone.row0[2]=e[ 8]; bone.row0[3]=0.0f;
            bone.row1[0]=e[1]; bone.row1[1]=e[5]; bone.row1[2]=e[ 9]; bone.row1[3]=0.0f;
            bone.row2[0]=e[2]; bone.row2[1]=e[6]; bone.row2[2]=e[10]; bone.row2[3]=0.0f;
            bone.row3[0]=e[3]; bone.row3[1]=e[7]; bone.row3[2]=e[11]; bone.row3[3]=1.0f;
            ps.bones.push_back(bone);
        }
    }
    while ((int)ps.bones.size() < (int)rec.numBones) {
        GpuMechBone id{};
        id.row0[0]=1.f; id.row1[1]=1.f; id.row2[2]=1.f; id.row3[3]=1.f;
        ps.bones.push_back(id);
    }

    // Capture live per-actor texture handle for each packet.
    //
    // SLOT 0 is per-actor (paint scheme / team color). TG_TypeShape::listOfTextures
    // is a shared type-level cache mutated by TransformMultiShape — by render time
    // it reflects the LAST actor through, not the current one. Use desc.slot0TexHandle
    // (the raw gos handle passed by the caller) directly for slot 0.
    //
    // SLOTS 1+ are type-stable; reading from owningTypeShape is correct.
    ps.packetTexHandles.resize(rec.packetCount, 0);
    for (uint32_t p = 0; p < rec.packetCount; ++p) {
        const GpuMechPacket& pkt = s_packets[rec.firstPacket + p];
        if (pkt.textureSlot == 0) {
            ps.packetTexHandles[p] = desc.slot0TexHandle;
        } else if (pkt.owningTypeShape && pkt.owningTypeShape->listOfTextures &&
                   pkt.textureSlot < (uint32_t)pkt.owningTypeShape->numTextures) {
            ps.packetTexHandles[p] =
                pkt.owningTypeShape->listOfTextures[pkt.textureSlot].gosTextureHandle;
        }
    }

    s_pendingSubmits.push_back(std::move(ps));

    // [SPOTLIGHT_REAL_TRACE v1] T0.2 (b) — per-instance counter. Fires once
    // per submitActor when the actor's mechType has at least one SpotLight_
    // TypeNode (recorded at registerTypeLod time). Per-frame increment is
    // unconditional; 600-frame summary is env-gated in flush() below.
    if (s_spotlightRealTypeNodeCount.find(desc.mechType)
            != s_spotlightRealTypeNodeCount.end()) {
        ++s_spotlightReal_drawWindow;
        ++s_spotlightReal_drawMono;
        if (!s_spotlightReal_firstHitDraw) {
            s_spotlightReal_firstHitDraw = true;
            MC2_DIAGFE(stderr,
                "[SPOTLIGHT_REAL_TRACE v1] event=first_hit site=mech_submit "
                "type=%p lod=%d\n",
                (const void*)desc.mechType, desc.currentLOD);
            std::fflush(stderr);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// flush (Task 7) — bucket-sorted compaction + draw loop
// ---------------------------------------------------------------------------
// APPLY-STATE-MECHOPAQUE-1: executor-driven pre-apply of the MechOpaque pipeline.
// Called from the flush call site in txmmgr renderLists() when MC2_FRAMEGRAPH_EXECUTOR
// is ON and a kTopLevelStateDesc row exists. Applies ONLY the pipeline (program +
// fixed-function depth/blend/cull); FBO/MRT/objectId(loc2)/viewport are inherited from
// the preceding passes and are NOT touched here (kTopLevelStateDesc records them as
// Unknown/Inherit). MVP is a view-UBO uniform (binding=3), not pipeline state -> pin-safe.
// Idempotent: flush()'s body makes the identical call when the flag is false.
void GpuMechBatcher::executorApplyMechOpaqueState() {
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::MechOpaque),
        "MechOpaque");
    mc2_framegraph_executor_bump_apply_state((unsigned)RenderCore::framegraph::ApplyPassId::MechOpaque);
    s_opaqueStateAppliedByExecutor = true;
}

namespace {

struct BucketKey {
    uint32_t typeLodIdx;
    uint32_t globalPacketIdx;
    uint32_t texHandle;
    uint32_t materialFlags;
    bool operator<(const BucketKey& o) const {
        if (typeLodIdx      != o.typeLodIdx)      return typeLodIdx      < o.typeLodIdx;
        if (globalPacketIdx != o.globalPacketIdx) return globalPacketIdx < o.globalPacketIdx;
        if (texHandle       != o.texHandle)       return texHandle       < o.texHandle;
        return materialFlags < o.materialFlags;
    }
};

struct DrawCall {
    uint32_t globalPacketIdx;
    uint32_t texHandle;
    uint32_t materialFlags;
    uint32_t instanceBase;
    uint32_t instanceCount;
    uint32_t aoTexHandle;   // AO-1: AO mcTextureManager slot (0=none; per-type, so
                            // consistent within a bucket). Bound to unit 6 at draw.
    uint32_t normalTexHandle; // NORMALS-1: normal-map slot (0=none; per-type).
                              // Bound to unit 7 at draw.
};

// Locals the flush phases hand to each other: the bucketed submits, the ring slice pointers,
// the draw-call list and the GL state saved before the draw loop and restored after it.
struct MechFlushFrame {
    size_t totalBones;
    std::map<BucketKey, std::vector<uint32_t>> buckets;  // key -> [submitIdx list]
    size_t totalInstances;
    GpuMechInstance* instDst;
    GpuMechBone* boneDst;
    std::vector<uint32_t> actorBoneBase;
    std::vector<DrawCall> drawCalls;
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo2;
    GLint prevDepthFunc;
    GLboolean prevDepthTest;
    GLboolean prevDepthMask;
    GLboolean prevBlend;
    GLboolean prevCull;
    GLint prevCullMode;
    GLint prevProgram;
    GLint prevVao;
    GLint prevArrayBuf;
    GLint prevElemBuf;
    GLint prevActiveTex;
    GLint prevSampler;
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo0;
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo1;
    GLint prevTexUnit0;
    GLint prevTexUnit1;
    GLint prevSampler1;
    GLint prevTexUnit2;
    GLint prevSampler2;
    GLint prevTexUnit3;
    GLint prevSampler3;
    GLint prevTexUnit4;
    GLint prevSampler4;
    GLint prevTexUnit6;
    GLint prevSampler6;
    GLint prevTexUnit7;
    GLint prevSampler7;
    uint32_t drawnCalls;

    void buildBuckets();
    void advanceRing();
    void writeBones();
    void buildMaterialTable();
    void writeInstances();
    void validateMaterialTable();
    void bindSsbos();
    void saveGlState();
    void drawBuckets();
    void restoreGlState();
    void persistDrawCalls();
#ifdef MC2_VULKAN_ISLAND
    void snapshotForVkIsland();
#endif
};

}  // namespace

// Latch the submit count and run the periodic and boot-time trace setup.
static void traceLatch() {
    // V1A: latch BEFORE any early-return guard clears s_pendingSubmits.
    // Counts GpuMechBatcher submits only (not MLR fallback, not alive count).
    s_lastFlushSubmitCount = static_cast<uint64_t>(s_pendingSubmits.size());

    // [SPOTLIGHT_REAL_TRACE v1] T0.2 (b) — periodic summary at 600-frame
    // cadence. Frame counter advances every flush() regardless of env or
    // pending submits, so the cadence stays stable across missions. Window
    // counter resets after emit; mono is monotonic.
    ++s_spotlightReal_drawFrameCount;
    const bool spotlightRealPeriodic =
        (s_spotlightReal_drawFrameCount % 600 == 0 && s_spotlightReal_drawFrameCount > 0);
    if (s_mechSpotlightRealTrace && spotlightRealPeriodic) {
        MC2_DIAGFE(stderr,
            "[SPOTLIGHT_REAL_TRACE v1] event=summary site=mech_submit "
            "frames=%llu window_mech_spotlight_draws=%llu mono_mech_spotlight_draws=%llu "
            "type_node_events_total=%llu distinct_types_with_spotlights=%zu\n",
            (unsigned long long)s_spotlightReal_drawFrameCount,
            (unsigned long long)s_spotlightReal_drawWindow,
            (unsigned long long)s_spotlightReal_drawMono,
            (unsigned long long)s_spotlightReal_typeNodeEvents,
            s_spotlightRealTypeNodeCount.size());
        std::fflush(stderr);
    }
    if (spotlightRealPeriodic) {
        s_spotlightReal_drawWindow = 0;
    }

    if (!s_mechBatcherTraceInit) {
        s_mechBatcherTrace     = mc2::gate::get<"MC2_MECH_BATCHER_STATS">();
        s_mechBatcherTraceInit = true;
    }
    if (!s_mechLightTraceInit) {
        s_mechLightTrace     = mc2::gate::get<"MC2_MECH_LIGHT_TRACE">();
        s_mechLightTraceInit = true;
    }
    if (s_mechLightTrace) {
        // [LIGHTSSBO v1] RF4: LightsData is an unbounded SSBO now — there
        // is NO 64-slot cap, so the old event=cache_full overflow alarm
        // would false-fire on exactly the dense missions this conversion
        // enables. Repurposed to a pure maxIdx observation (no cap, no
        // disable). s_lightCacheFullFrames retired.
        uint32_t maxIdx = 0;
        for (const auto& ps : s_pendingSubmits) {
            if (ps.desc.lightDataIndex > maxIdx) maxIdx = ps.desc.lightDataIndex;
        }
        mc2::fmt::print_err("[LIGHTSSBO v1] event=mech_lightidx_max submitted={:d} maxIdx={:d} (unbounded SSBO)\n", mc2::fmt::as_size(s_pendingSubmits.size()), mc2::fmt::as_u32(maxIdx));
    }
}

// Resolve the named surface-material profile indices once, after the material tables exist.
static void resolveSurfaceMaterial() {
    // Slice C1: resolve s_mechSurfaceMaterialIdx once after gos_materials::init() has run.
    // init() is called from finalizeGeometry() which always precedes the first flush().
    if (!s_mechSurfaceMaterialIdxResolved) {
        s_mechSurfaceMaterialIdxResolved = true;
        // Profiles exist only for these two names; any other text (none, 0, a typo) keeps the passthrough index 0.
        if (mc2::gate::is<"MC2_MECH_SURFACE_MATERIAL", "metal061b">())
            s_mechSurfaceMaterialIdx = gos_materials::getProfileIndex("metal061b");
        else if (mc2::gate::is<"MC2_MECH_SURFACE_MATERIAL", "painted_subtle">())
            s_mechSurfaceMaterialIdx = gos_materials::getProfileIndex("painted_subtle");
        // else stays 0 (default passthrough)
        if (s_mechSurfaceMaterialIdx != 0u)
            s_mechPaintSurfaceMaterialIdx = gos_materials::getProfileIndex("paintedmetal003");
    }
}

// Count bones and group the pending submits into draw buckets keyed by packet, texture and flags.
void MechFlushFrame::buildBuckets() {
    // Step 1: Count total bones (one block per actor).
    totalBones = 0;
    for (const auto& ps : s_pendingSubmits) totalBones += ps.bones.size();

    // Step 2: Build draw buckets.
    // Key: (typeLodIdx, globalPacketIdx, texHandle, materialFlags).
    // Each actor × packet produces one entry in the matching bucket.
    // Different per-actor paint schemes for the same packet -> different buckets.


    for (uint32_t si = 0; si < (uint32_t)s_pendingSubmits.size(); ++si) {
        const PendingSubmit& ps = s_pendingSubmits[si];
        const GpuMechTypeLodRecord& rec = s_typeLodRecords[ps.typeLodIdx];
        for (uint32_t p = 0; p < rec.packetCount; ++p) {
            const GpuMechPacket& pkt = s_packets[rec.firstPacket + p];
            BucketKey key;
            key.typeLodIdx       = ps.typeLodIdx;
            key.globalPacketIdx  = rec.firstPacket + p;
            key.texHandle        = ps.packetTexHandles[p];
            key.materialFlags    = pkt.materialFlags;
            buckets[key].push_back(si);
        }
    }

    totalInstances = 0;
    for (const auto& kv : buckets) totalInstances += kv.second.size();
}

// Advance the ring slot, wait for its oldest fence and point at the slot slices.
void MechFlushFrame::advanceRing() {
    // Step 3: Advance ring slot and wait for oldest fence.
    s_frameSlot = (s_frameSlot + 1) % MECH_RING_FRAMES;
    if (s_fence[s_frameSlot]) {
        glClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_fence[s_frameSlot]);
        s_fence[s_frameSlot] = 0;
    }

    instDst = (GpuMechInstance*)s_instanceMap + s_frameSlot * s_instanceCapacity;
    boneDst = (GpuMechBone*)    s_boneMap     + s_frameSlot * s_boneCapacity;
}

// Write each actor bone palette into the bone ring and record its base offset.
void MechFlushFrame::writeBones() {
    // Step 4: Write bone SSBO once per actor; record each actor's boneBase offset.
    actorBoneBase.assign(s_pendingSubmits.size(), 0u);
    uint32_t boneHead = 0;
    for (uint32_t si = 0; si < (uint32_t)s_pendingSubmits.size(); ++si) {
        actorBoneBase[si] = boneHead;
        for (const auto& b : s_pendingSubmits[si].bones)
            boneDst[boneHead++] = b;
    }
}

// Assign each bucket a material-table row and upload the table when it grew.
void MechFlushFrame::buildMaterialTable() {
    // Step 2.5: Build mech MaterialGpu table (same bucket iteration order as Step 5).
    // s_mechDrawMaterialIdx is per-draw (bucket-iteration order) — rebuilt every flush.
    // s_mechMaterialTable / s_mechHandleToMaterialIdx PERSIST within a mission
    // (texHandle->idx is stable; reset by onMapUnload). Upload only when the table grows.
    s_mechDrawMaterialIdx.clear();
    if (s_mechMaterialGpuEnabled) {
        if (s_mechMaterialTable.empty())
            s_mechMaterialTable.push_back(RenderCore::MaterialGpu{}); // index 0 = sentinel/not-assigned
        bool tableDirty = false;
        for (const auto& kv : buckets) {
            const uint32_t texHandle = kv.first.texHandle;
            uint32_t mIdx;
            auto it = s_mechHandleToMaterialIdx.find(texHandle);
            if (it != s_mechHandleToMaterialIdx.end()) {
                mIdx = it->second;
            } else {
                mIdx = static_cast<uint32_t>(s_mechMaterialTable.size());
                RenderCore::MaterialGpu entry{};
                entry.albedoTex = texHandle;
                s_mechMaterialTable.push_back(entry);
                s_mechHandleToMaterialIdx[texHandle] = mIdx;
                tableDirty = true;
            }
            s_mechDrawMaterialIdx.push_back(mIdx);
        }
        // Upload only when the table grew (or buffer not yet created).
        if (s_mechMaterialSsbo.backendHandle == 0 || tableDirty) {
            const size_t byteSize = s_mechMaterialTable.size() * sizeof(RenderCore::MaterialGpu);
            if (s_mechMaterialSsbo.backendHandle == 0) {
                GLuint local = 0;
                glGenBuffers(1, &local);
                s_mechMaterialSsbo.backendHandle = static_cast<uint32_t>(local);
            }
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_mechMaterialSsbo.backendHandle));
            glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(byteSize),
                         s_mechMaterialTable.data(), GL_DYNAMIC_DRAW);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

            // REGISTRY-MATERIAL-SSBO-1: register the mech material SSBO (observe-only
            // metadata; never read by the draw path). Registered here (not at init)
            // because both the live GL handle and the byte size are only known once
            // the material table has been sized and uploaded.
            {
                RenderCore::RenderResourceDesc d;
                d.id        = RenderCore::RenderResourceId::MaterialGpuBuffer;
                d.kind      = RenderCore::RenderResourceKind::Buffer;
                d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // persistent material table (not a pass output)
                d.format    = RenderCore::RenderResourceFormat::BufferRaw;
                d.debugName = "MaterialGpuBuffer";
                d.glName    = static_cast<GLuint>(s_mechMaterialSsbo.backendHandle);
                d.sizeBytes = static_cast<uint64_t>(byteSize);
                d.valid     = true;
                RenderCore::registerOrUpdateRenderResource(d);
            }
        }
    }
    // Throttled table_link log (first flush + every 600th)
    ++s_mechMaterialGpuFrameCount;
    if (s_mechMaterialGpuFrameCount % 600 == 1) {
        char buf[96];
        mc2::fmt::to_buf(buf, "[MECH_MATERIAL_GPU v1] event=table_link materials={:d}\n", mc2::fmt::as_u32(static_cast<uint32_t>(s_mechMaterialTable.size())));
        MC2_DIAG_FPUTS(buf, stderr);
    }
}

// Write the instance ring in bucket order and collect the draw calls.
void MechFlushFrame::writeInstances() {
    // Step 5: Write instance SSBO in bucket order; collect draw calls.
    drawCalls.reserve(buckets.size());

    auto unpack = [](uint32_t argb, float out[4]) {
        out[0] = ((argb >> 16) & 0xFF) / 255.f;  // r
        out[1] = ((argb >>  8) & 0xFF) / 255.f;  // g
        out[2] = ((argb >>  0) & 0xFF) / 255.f;  // b
        out[3] = ((argb >> 24) & 0xFF) / 255.f;  // a
    };

    uint32_t instHead = 0;
    uint32_t dcIdx    = 0;
    for (const auto& kv : buckets) {
        const BucketKey& key              = kv.first;
        const std::vector<uint32_t>& subs = kv.second;

        DrawCall dc;
        dc.globalPacketIdx = key.globalPacketIdx;
        dc.texHandle       = key.texHandle;
        dc.materialFlags   = key.materialFlags;
        dc.instanceBase    = instHead;
        dc.instanceCount   = (uint32_t)subs.size();
        // AO-1: AO is per-type, so all submits in this bucket share it; read from the first.
        dc.aoTexHandle     = subs.empty() ? 0u : s_pendingSubmits[subs[0]].desc.slot6TexHandle;
        // NORMALS-1: normal map is per-type too; same bucket-share rule.
        dc.normalTexHandle = subs.empty() ? 0u : s_pendingSubmits[subs[0]].desc.slot7TexHandle;

        for (uint32_t si : subs) {
            const PendingSubmit& ps   = s_pendingSubmits[si];
            const GpuMechSubmitDesc& d = ps.desc;
            GpuMechInstance inst{};
            inst.typeLodRecordIndex = ps.typeLodIdx;
            inst.baseBoneOffset     = actorBoneBase[si];
            inst.lightDataIndex     = d.lightDataIndex;
            inst.renderFlags        = d.renderFlags;
            // BT2018-MECH-MATERIAL-GAMMA-1: tag imported BT mechs (renderFlags bit 3)
            // so the frag linearizes their sRGB-authored albedo. Stock types have
            // importedGpuType == nullptr, so the bit stays clear (no-op for them).
            if (ps.typeLodIdx < s_typeLodRecords.size() &&
                s_typeLodRecords[ps.typeLodIdx].importedGpuType)
                inst.renderFlags |= 0x8u;
            unpack(d.highlightARGB, inst.aRGBHighlight);
            unpack(d.fogARGB,       inst.fogRGB);
            // M2.5 (Q3 unconditional): carry the RenderWorld handle through
            // to the SSBO. Env-OFF: GLSL macro gates out the FS write, so
            // the value is never read by the GPU.
            inst.objectIdRaw        = d.objectIdRaw;
            // M2.5 (Q4): count non-zero writes for per-mission observability.
            // Direct per-mission accumulation (no per-frame intermediate;
            // per adversarial M1 the per-frame counter had no consumer).
            if (d.objectIdRaw != 0u) {
                ++s_gpuMechIdWritesThisMission;
            }
            // Mech-1: per-instance materialIdx from parallel s_mechDrawMaterialIdx table
            inst.materialIdx = (s_mechMaterialGpuEnabled && dcIdx < s_mechDrawMaterialIdx.size())
                ? s_mechDrawMaterialIdx[dcIdx]
                : 0u;
            // Slice C1: when a named surface material profile is selected via
            // MC2_MECH_SURFACE_MATERIAL, override materialIdx with the binding-7
            // profile index. 0 = env unset/none → preserve existing albedo-table value.
            if (s_mechSurfaceMaterialIdx != 0u)
                inst.materialIdx = s_mechSurfaceMaterialIdx;
            // GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: per-mech visual state into the
            // SSBO record. No shader reads these in Slice 1.
            inst.visualDamage01     = d.damage01;
            inst.visualFlags        = d.visualFlags;
            instDst[instHead++]     = inst;
        }
        drawCalls.push_back(dc);
        ++dcIdx;
    }
}

// Check every draw call material row still names the bucket texture.
void MechFlushFrame::validateMaterialTable() {
    // Task 5: Compare validation — verify albedoTex == texHandle per draw call
    if (s_mechMaterialGpuEnabled) {
        int mismatches = 0;
        for (uint32_t i = 0; i < (uint32_t)drawCalls.size(); ++i) {
            const uint32_t mIdx = (i < (uint32_t)s_mechDrawMaterialIdx.size()) ? s_mechDrawMaterialIdx[i] : 0u;
            if (mIdx == 0u || mIdx >= (uint32_t)s_mechMaterialTable.size()) continue;
            const uint32_t albedo   = s_mechMaterialTable[mIdx].albedoTex;
            const uint32_t expected = drawCalls[i].texHandle;
            if (albedo != expected) {
                if (mismatches < 10) {
                    char buf[128];
                    mc2::fmt::to_buf(buf, "[MECH_MATERIAL_GPU v1] MISMATCH dc={:d} materialIdx={:d} albedo={:d} expected={:d}\n", mc2::fmt::as_u32(i), mc2::fmt::as_u32(mIdx), mc2::fmt::as_u32(albedo), mc2::fmt::as_u32(expected));
                    MC2_DIAG_FPUTS(buf, stderr);
                }
                ++mismatches;
            }
        }
        if (s_mechMaterialGpuFrameCount % 600 == 1) {
            char buf[128];
            mc2::fmt::to_buf(buf, "[MECH_MATERIAL_GPU v1] event=compare frame={:d} mechs={:d} mismatches={:d}\n", mc2::fmt::as_u32(s_mechMaterialGpuFrameCount), mc2::fmt::as_u32(static_cast<uint32_t>(drawCalls.size())), mc2::fmt::as_i32(mismatches));
            MC2_DIAG_FPUTS(buf, stderr);
        }
    }
}

// Bind the instance, bone and material SSBOs for the draw loop.
void MechFlushFrame::bindSsbos() {
    // Mech-1: save binding 2 before Step 6 mutates it.
    // GLSTATE-GUARD-ADOPTION-3 (glstate-guard-epilogue-recon-1.md Site 1 — "the
    // one genuinely clean, behavior-preserving win"): restore-previous SSBO
    // binding via GlScopedSsboBinding, replacing the hand-rolled prevSsbo2
    // save (was here) + restore (was after the draw loop). The recon requires
    // this guard to close BEFORE the function's trailing
    // gos_InvalidateRenderStateCache() call, which is earlier than this
    // function's natural end — so it is wrapped in std::optional and
    // .reset() explicitly at the original manual-restore point instead of a
    // ~430-line lexical block (avoids re-scoping every local declared in
    // between). Render-state (program/VAO/buffers/samplers/depth/blend/cull)
    // stays fully manual per the recon's recommendation — only SSBO 0/1/2
    // convert in this slice.
    guardSsbo2.emplace(2);

    // Step 6: Bind SSBOs (whole per-frame slices; shader indexes via u_instanceBase).
    gpuBindSsboRange(0, s_instanceSsbo,
        (long long)(s_frameSlot * s_instanceCapacity * sizeof(GpuMechInstance)),
        (long long)(totalInstances * sizeof(GpuMechInstance)),
        "mech.instance.shadow");
    gpuBindSsboRange(1, s_boneSsbo,
        (long long)(s_frameSlot * s_boneCapacity * sizeof(GpuMechBone)),
        (long long)(totalBones * sizeof(GpuMechBone)),
        "mech.bone.shadow");
    // Mech-1: bind MaterialGpu table SSBO at binding 2
    if (s_mechMaterialGpuEnabled && s_mechMaterialSsbo.backendHandle != 0) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, static_cast<GLuint>(s_mechMaterialSsbo.backendHandle));
    }
    // Slice C1: bind named material profile SSBO at binding 7.
    // No-op when MC2_MATERIAL_GPU=0, init() not called, or no textured profiles.
    gos_materials::bindMaterialTable();
}

// Save the GL state the draw loop changes so it can be restored afterwards.
void MechFlushFrame::saveGlState() {
    // Save prior GL state. The mech flush bypasses applyRenderStates'
    // tracked slot set, so the engine's render-state cache becomes
    // out-of-sync after we mutate state directly. We save EVERYTHING the
    // static_prop batcher saves (mirror its pattern), restore at end, and
    // call gos_InvalidateRenderStateCache() to force the next
    // applyRenderStates to re-issue. Skipping invalidate has been observed
    // to make subsequent draws (water, mechs themselves on later frames)
    // disappear after a few frames as the cache thinks GL is in state X
    // while reality is state Y.
    glGetIntegerv(GL_DEPTH_FUNC,      &prevDepthFunc);
    glGetBooleanv(GL_DEPTH_TEST,      &prevDepthTest);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    glGetBooleanv(GL_BLEND,            &prevBlend);
    glGetBooleanv(GL_CULL_FACE,        &prevCull);
    glGetIntegerv(GL_CULL_FACE_MODE,  &prevCullMode);
    glGetIntegerv(GL_CURRENT_PROGRAM,  &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElemBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE,   &prevActiveTex);
    prevSampler = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &prevSampler);
    // GLSTATE-GUARD-ADOPTION-3: SSBO 0/1 restore-previous, same pattern as
    // guardSsbo2 above (captured here, AFTER Step 6 already rebound slots 0/1
    // to s_instanceSsbo/s_boneSsbo — matching the exact pre-conversion capture
    // point/semantics unchanged, not "fixed").
    guardSsbo0.emplace(0);
    guardSsbo1.emplace(1);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit0);
    // Slice C2 + PBR-LAYERED-1: save PBR detail sampler units 1-4.
    glActiveTexture(GL_TEXTURE1);
    prevTexUnit1 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit1);
    prevSampler1 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 1, &prevSampler1);
    glActiveTexture(GL_TEXTURE2);
    prevTexUnit2 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit2);
    prevSampler2 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 2, &prevSampler2);
    glActiveTexture(GL_TEXTURE3);
    prevTexUnit3 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit3);
    prevSampler3 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &prevSampler3);
    glActiveTexture(GL_TEXTURE4);
    prevTexUnit4 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit4);
    prevSampler4 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 4, &prevSampler4);
    // AO-1: save unit 6 (imported-mech AO). Paranoid leak guard — restored below.
    glActiveTexture(GL_TEXTURE6);
    prevTexUnit6 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit6);
    prevSampler6 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 6, &prevSampler6);
    // NORMALS-1: save unit 7 (imported-mech normal map). Same leak guard.
    glActiveTexture(GL_TEXTURE7);
    prevTexUnit7 = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexUnit7);
    prevSampler7 = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 7, &prevSampler7);
    glActiveTexture(GL_TEXTURE0);
    glActiveTexture(GL_TEXTURE0);
}

// Apply the mech pipeline and bind the shared VAO, IBO, sampler and PBR detail textures.
static void applyMechPipeline() {
    // MECH-PIPELINEDESC-1: program + fixed-function state (depth test+write+func
    // GEQUAL, blend Opaque, cull Back) from the registered MechOpaque PipelineDesc,
    // replacing the prior hand-set glUseProgram/glEnable/glDepthFunc/... block.
    // applyPipeline also does glUseProgram(desc.glProgramName) (= s_mechProgram via
    // bindProgram at link). Sampler + VAO/IBO stay manual (not PipelineDesc fields).
    // APPLY-STATE-MECHOPAQUE-1: skip when the executor already applied the pipeline
    // (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw. Gate OFF -> flag false ->
    // body applies -> byte-identical. flushShadow() (separate Shadow pass) is untouched.
    if (!s_opaqueStateAppliedByExecutor) {
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::MechOpaque));
    }
    s_opaqueStateAppliedByExecutor = false;

    glBindVertexArray(s_sharedVao);
    // Explicit IBO rebind: mirrors flushShadow() pattern. GL_ELEMENT_ARRAY_BUFFER
    // is VAO state; s_sharedVao's slot can be left at 0 by a prior flushShadow()
    // restore-order bug. Belt-and-suspenders self-heal.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    // Bind our REPEAT/LINEAR sampler. Per
    // memory/sampler_state_inheritance_in_fast_paths.md: when a prior pass
    // (e.g. patch_stream) leaves a CLAMP_TO_EDGE sampler bound on unit 0,
    // mech UVs that fall outside [0,1] (legitimate for tiled mech body
    // textures) all clamp to the texture edge — which for many mech
    // textures is a black border, producing the all-black mech symptom.
    // Sampler-object state OVERRIDES the texture object's glTexParameter
    // values, so setting them on the texture object alone is insufficient.
    // Identified via debug=7 (hardcoded UV (0.5, 0.5) showed yellow paint
    // while debug=2 with v_uv showed black).
    glBindSampler(0, s_sampler);

    // Slice C2: bind PBR surface-detail textures to units 1 and 2.
    // s_mechSurfaceMaterialIdx 0 → both getters return 0 → unbind (safe; shader
    // only samples these when u_standardLitEnabled != 0, which requires a non-zero profile).
    {
        static const float s_pbrTileScale = mc2::gate::isSet<"MC2_PBR_TILE_SCALE">()
            ? mc2::gate::get<"MC2_PBR_TILE_SCALE">()
            : (mc2::gate::is<"MC2_MECH_SURFACE_MATERIAL", "painted_subtle">() ? 2.0f : 4.0f);
        const GLuint pbrNormal = gos_materials::getProfileNormalTex(s_mechSurfaceMaterialIdx);
        const GLuint pbrOrm    = gos_materials::getProfileOrmTex(s_mechSurfaceMaterialIdx);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, pbrNormal);
        glBindSampler(1, s_sampler);   // REPEAT/LINEAR same as unit 0
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, pbrOrm);
        glBindSampler(2, s_sampler);
        glActiveTexture(GL_TEXTURE0);
        if (s_loc_u_pbrNormalTex >= 0) glUniform1i(s_loc_u_pbrNormalTex, 1);
        if (s_loc_u_pbrOrmTex    >= 0) glUniform1i(s_loc_u_pbrOrmTex,    2);
        if (s_loc_u_pbrTileScale >= 0) glUniform1f(s_loc_u_pbrTileScale, s_pbrTileScale);
    }
    // PBR-LAYERED-1: bind PaintedMetal003 paint layer to units 3 (normal) and 4 (ORM).
    // Falls back to Metal061B textures when paint profile not registered (idx=0, id=0).
    {
        GLuint pbrPaintNormal = gos_materials::getProfileNormalTex(s_mechPaintSurfaceMaterialIdx);
        if (pbrPaintNormal == 0u) pbrPaintNormal = gos_materials::getProfileNormalTex(s_mechSurfaceMaterialIdx);
        GLuint pbrPaintOrm = gos_materials::getProfileOrmTex(s_mechPaintSurfaceMaterialIdx);
        if (pbrPaintOrm == 0u) pbrPaintOrm = gos_materials::getProfileOrmTex(s_mechSurfaceMaterialIdx);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, pbrPaintNormal);
        glBindSampler(3, s_sampler);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, pbrPaintOrm);
        glBindSampler(4, s_sampler);
        glActiveTexture(GL_TEXTURE0);
        if (s_loc_u_pbrPaintNormalTex >= 0) glUniform1i(s_loc_u_pbrPaintNormalTex, 3);
        if (s_loc_u_pbrPaintOrmTex    >= 0) glUniform1i(s_loc_u_pbrPaintOrmTex,    4);
    }
}

// Upload the per-flush shading, PBR and lighting uniforms.
static void uploadShadingUniforms() {
    // VK-PARITY-MECH-1: default shading knobs from the shared producer (the VK
    // mech island's frame feed reads the same function). Values unchanged.
    GpuMechShadingParams shp{};
    gpuMechShadingParams(&shp);

    // Static uniforms.
    glUniform1i(s_loc_u_tex,      0);
    glUniform1f(s_loc_u_fogValue, 1.0f);
    // BT2018-MECH-MATERIAL-GAMMA-1/TUNING-1: imported-mech albedo knobs (live ImGui).
    if (s_loc_u_importedGamma       >= 0) glUniform1f(s_loc_u_importedGamma,       shp.importedGamma);
    if (s_loc_u_importedAlbedoScale >= 0) glUniform1f(s_loc_u_importedAlbedoScale, shp.importedAlbedoScale);
    if (s_loc_u_mechAoTex           >= 0) glUniform1i(s_loc_u_mechAoTex,           6);  // AO-1: unit 6
    if (s_loc_u_aoStrength          >= 0) glUniform1f(s_loc_u_aoStrength,          shp.aoStrength);
    if (s_loc_u_mechNormalTex       >= 0) glUniform1i(s_loc_u_mechNormalTex,       7);  // NORMALS-1: unit 7
    if (s_loc_u_normalStrength      >= 0) glUniform1f(s_loc_u_normalStrength,      shp.normalStrength);
    // Slice B1: lighting mode 0 = Slice A flat-white passthrough,
    // 1 = calc_light() per-vertex. Set per-flush from killswitch.
    if (s_loc_u_lightingMode >= 0)
        glUniform1i(s_loc_u_lightingMode, 1);   // LIGHTING-RETIRE-1: calc_light always on
    // Slice C2: skinning mode 0 = rigid per-bone (Slice A), 1 = weighted
    // multi-bone blend. Stock data is byte-identical across modes.
    if (s_loc_u_skinningMode >= 0)
        glUniform1i(s_loc_u_skinningMode, 1);   // SKIN-RETIRE-1: weighted skinning always on
    // SUN-AXIS-FIX-1: hand calc_light's DIFFUSE directional branches the same
    // Stuff->MC2 swap mech.vert:279 already applies for SPECULAR, so mech
    // diffuse stops dotting an MC2-frame normal against a Stuff-frame sun.
    // Boot-cached (Debug Instrumentation Rule). Default OFF -> uniform 0 ->
    // shipped expression bit-for-bit. See RendererFeatureRegistry.h.
    if (s_loc_u_sunAxisMc2Fix >= 0) {
        // Canonical DEFAULT-OFF OPT-IN idiom `(v && v[0] && v[0] != '0')`.
        // Written in exactly this form on purpose: scripts/check-gate-default-
        // agreement.py classifies a Feature gate's default FROM ITS READER, and
        // an idiom it cannot parse is a FAILURE, not a pass. The first draft
        // here used an early-return plus `? 0 : 1` and came back UNCLASSIFIABLE.
        static const bool s_sunAxisMc2Fix = mc2::options::current().sunAxisFix;
        glUniform1i(s_loc_u_sunAxisMc2Fix, s_sunAxisMc2Fix ? 1 : 0);
    }
    if (s_mechBatcherTrace) {
        static int s_uniDiagPrinted = 0;
        if (s_uniDiagPrinted < 2) {
            ++s_uniDiagPrinted;
            GLint utexVal = -99;
            if (s_loc_u_tex >= 0)
                glGetUniformiv(s_mechProgram, s_loc_u_tex, &utexVal);
            MC2_DIAGFE(stderr,
                "[MECHBATCHER v1] event=uni_probe loc_u_tex=%d u_tex_val=%d s_loc_terrainMVP=%d s_loc_u_mvp=%d\n",
                s_loc_u_tex, utexVal, s_loc_terrainMVP, s_loc_u_mvp);
        }
    }
    {
        // MC2_MECH_FRAG_DEBUG=N: legacy env path; wins over ImGui s_mechDebugMode.
        // 0=normal, 1=magenta, 2=texOnly, 3=lightOnly, 4=normal-as-color.
        // When env is unset and s_mechDebugMode=0: dbgMode=0 → byte-identical.
        const int dbgMode = mc2::gate::isSet<"MC2_MECH_FRAG_DEBUG">() ? mc2::gate::get<"MC2_MECH_FRAG_DEBUG">() : s_mechDebugMode;
        if (s_loc_u_debugMode >= 0)
            glUniform1i(s_loc_u_debugMode, dbgMode);
    }
    // MECH-AMBIENT-1: hemisphere ambient fill strength. Uploads 0.0 when the
    // gate is OFF -> exact no-op (byte-identical default path).
    if (s_loc_u_mechAmbientV1Strength >= 0)
        glUniform1f(s_loc_u_mechAmbientV1Strength, shp.ambientStrength);
    // MECH-SPECULAR-V1: upload all 6 specular uniforms. Strength is 0.0 when
    // the gate is off -> exact no-op. The roughness/threshold uniforms are
    // uploaded unconditionally; they are inert when strength is 0. All 6 locs
    // are -1 on non-viewuniforms variants (glUniform* with loc -1 is a no-op
    // per GL spec), so the guards are redundant but kept for clarity.
    if (s_loc_u_mechSpecularV1Strength >= 0)
        glUniform1f(s_loc_u_mechSpecularV1Strength, shp.specularStrength);
    if (s_loc_u_mechMetalRoughness >= 0)
        glUniform1f(s_loc_u_mechMetalRoughness, shp.metalRoughness);
    if (s_loc_u_mechGlassRoughness >= 0)
        glUniform1f(s_loc_u_mechGlassRoughness, shp.glassRoughness);
    if (s_loc_u_mechGlassLumaThresh >= 0)
        glUniform1f(s_loc_u_mechGlassLumaThresh, shp.glassLumaThresh);
    if (s_loc_u_mechGlassMaxChanThresh >= 0)
        glUniform1f(s_loc_u_mechGlassMaxChanThresh, shp.glassMaxChanThresh);
    if (s_loc_u_mechSpecDebugMask >= 0)
        glUniform1i(s_loc_u_mechSpecDebugMask, shp.specDebugMask);
    if (s_loc_u_mechBackFillStrength >= 0)
        glUniform1f(s_loc_u_mechBackFillStrength, s_mechBackFillStrength);
    // Slice C1: StandardLit GGX gate (mutable via ImGui / batcher_setStandardLitEnabled).
    if (s_loc_u_standardLitEnabled >= 0)
        glUniform1i(s_loc_u_standardLitEnabled, s_standardLitEnabled);

    // PBR-TUNE-1: metallic influence, roughness clamp, ambient specular fill.
    // Mutable via ImGui / batcher_setPbr* accessors; env-seeded defaults.
    if (s_loc_u_pbrMetallicInfluence      >= 0) glUniform1f(s_loc_u_pbrMetallicInfluence,       s_pbrMetallicInfluence);
    if (s_loc_u_pbrRoughnessMin           >= 0) glUniform1f(s_loc_u_pbrRoughnessMin,            s_pbrRoughnessMin);
    if (s_loc_u_pbrRoughnessMax           >= 0) glUniform1f(s_loc_u_pbrRoughnessMax,            s_pbrRoughnessMax);
    if (s_loc_u_pbrAmbientSpecularStrength >= 0) glUniform1f(s_loc_u_pbrAmbientSpecularStrength, s_pbrAmbientSpecularStrength);
    // PBR-LAYERED-1: wear blend + triplanar.
    if (s_loc_u_pbrWearStrength   >= 0) glUniform1f(s_loc_u_pbrWearStrength,   s_pbrWearStrength);
    if (s_loc_u_pbrTriplanar      >= 0) glUniform1i(s_loc_u_pbrTriplanar,      s_pbrTriplanar ? 1 : 0);
    if (s_loc_u_pbrTriplanarScale >= 0) glUniform1f(s_loc_u_pbrTriplanarScale, s_pbrTriplanarScale);
    // PBR-IBL-MECH-1: SH-L2 ambient — same coefficients as static props.
    if (s_loc_u_iblSh         >= 0)
        glUniform3fv(s_loc_u_iblSh, 9, &RenderCore::kIblShCoeffs[0][0]);
    if (s_loc_u_iblShStrength >= 0)
        glUniform1f(s_loc_u_iblShStrength, s_mechIblShEnabled ? g_iblShStrength : 0.0f);
}

// Upload the light-pool and projection uniforms and run the view-uniform probe.
static void uploadProjectionUniforms() {
    // POOL-CONSUMERS-1 (batch-6 item 6): bind + upload the night light-pool
    // SSBO/uniforms so movers receive pool light (their own headlights,
    // building pools). u_numPoolLights=0 when gated -> byte-identical.
    gos_terrain_lighting::UploadObjectLightPoolUniforms(s_mechProgram);

    // Projection uniforms — match static_prop batcher and the
    // terrain_overlay.vert convention: terrainMVP = CPU-composed
    // axisSwap * worldToClip, row-major, uploaded with GL_FALSE.
    // Plan template said "TG_Shape::s_worldToClip with GL_TRUE" — that's
    // wrong; it skips the axis swap, mech ends up off-screen. Caught
    // 2026-05-08 by operator visual smoke (mechs invisible after PREC fix
    // re-enabled the GPU path).
    // terrainMVP fetched unconditionally so the DIAG probe below can compare
    // it against the binding-3 UBO even on the gated ViewUniforms path.
    const float* terrainMVP = gos_GetTerrainMVPMat4();
    // MECH-VIEWUNIFORMS-BLOCKBINDING-1: on the gated path the ViewUniformsBlock
    // UBO (binding=3) supplies u_worldToClipGL, so s_loc_terrainMVP is -1 (the
    // legacy uniform is #ifndef'd out) and this upload would be a no-op anyway;
    // skip it explicitly. Default path (gate OFF) uploads the legacy uniform
    // exactly as before.
    // PROP-FIXB-MVP: the default (non-UBO) path uploads the Fix-B-mirrored MVP
    // so the mech foot/terrain contact shares the terrain dispatch snapshot when
    // armed (gate ON). Gate OFF -> gos_GetMechFixBMVPMat4() returns the live MVP,
    // byte-identical to before. `terrainMVP` (live) is left untouched above so
    // the MECH_VU_DIAG probe keeps comparing the LIVE MVP against the binding-3
    // UBO. nullptr-safety: helper never returns null unless live is null too, so
    // the original `&& terrainMVP` guard still covers the null case.
    if (!s_mechViewUniforms && s_loc_terrainMVP >= 0 && terrainMVP)
        glUniformMatrix4fv(s_loc_terrainMVP, 1, GL_FALSE, gos_GetMechFixBMVPMat4());
    const float* mm = gos_GetProj2ScreenMat4();
    if (s_loc_u_mvp >= 0 && mm)
        glUniformMatrix4fv(s_loc_u_mvp, 1, GL_TRUE, mm);

    // MECH-VIEWUNIFORMS-BINDING-DIAG-1: gated read-only probe
    // (MC2_MECH_VIEWUNIFORMS_DIAG=1, default OFF). Diagnoses the reverted
    // MECH-VIEWUNIFORMS-1-PRE failure (mechs vanished on the UBO path). Recon
    // proved binding=3 is uploaded once per frame at gamecam top and is never
    // clobbered, so this confirms empirically AT MECH FLUSH TIME: (a) is the
    // ViewUniforms UBO still bound at binding point 3, and (b) does its
    // worldToClipGL match what the mech path actually uploads
    // (gos_GetTerrainMVPMat4). If max_diff != 0 the once-per-frame UBO capture
    // is stale/wrong for the mech pass; if binding3==0 the bind is dead here.
    // Pure read-back: no render effect. Rate-limited. Saves+restores the
    // generic GL_UNIFORM_BUFFER target binding.
    {
        if (s_mechViewUniformsDiag) {
            static int s_vuDiagFrame = 0;
            ++s_vuDiagFrame;
            if (s_vuDiagFrame <= 5 || (s_vuDiagFrame % 300) == 0) {
                GLint b3 = 0;
                glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 3, &b3);
                float ubo[16] = {0.0f};
                float maxDiff = -1.0f;
                if (b3 != 0) {
                    GLint prevBound = 0;
                    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &prevBound);
                    glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)b3);
                    glGetBufferSubData(GL_UNIFORM_BUFFER, 0,
                                       (GLsizeiptr)sizeof(ubo), ubo);
                    glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)prevBound);
                    if (terrainMVP) {
                        maxDiff = 0.0f;
                        for (int i = 0; i < 16; ++i) {
                            float d = ubo[i] - terrainMVP[i];
                            if (d < 0.0f) d = -d;
                            if (d > maxDiff) maxDiff = d;
                        }
                    }
                }
                mc2::fmt::print_err("[MECH_VU_DIAG v1] frame={:d} submits={:d} binding3_ubo={:d} max_diff_vs_terrainMVP={:.6f} ubo_row0=[{:.3f} {:.3f} {:.3f} {:.3f}] mvp_row0=[{:.3f} {:.3f} {:.3f} {:.3f}]\n", mc2::fmt::as_i32(s_vuDiagFrame), mc2::fmt::as_u64((unsigned long long)s_lastFlushSubmitCount), mc2::fmt::as_i32(b3), maxDiff, ubo[0], ubo[1], ubo[2], ubo[3], terrainMVP ? terrainMVP[0] : 0.0f, terrainMVP ? terrainMVP[1] : 0.0f, terrainMVP ? terrainMVP[2] : 0.0f, terrainMVP ? terrainMVP[3] : 0.0f);
            }
        }
    }
}

// Order the SSBO writes before the draws, then issue one instanced draw per bucket.
void MechFlushFrame::drawBuckets() {
    // GPU-SYNC-CONTRACT: the instance + bone SSBOs were just written through a
    // persistent GL_MAP_COHERENT_BIT mapping (Steps 4-5). Order those CPU writes
    // BEFORE the instanced draws below read them as SSBO. Without this, NVIDIA may
    // draw from stale/zero instance+bone data -> mechs garbled/invisible; AMD
    // tolerated it. (Default-on GPU mech path -- a sibling of the static-prop tree
    // bug; routed through the typed helper.)
    gpuSyncBarrier(GpuProducer::CpuCoherentWrite, GpuConsumer::InstancedDraw,
                   "mech_instance_bone");

    // Step 7: Issue one draw call per bucket.
    drawnCalls = 0;
    static int s_texDiagPrinted = 0;
#ifdef MC2_VULKAN_ISLAND
    // VK-P14 MECH TIER-1: when armed, capture each run (with its LIVE slot-0 paint
    // texture resolved this frame) for the island and SKIP the GL draw below.
    if (vkMechGateOn()) s_vkMechRunSnapshot.clear();
#endif
    for (const DrawCall& dc : drawCalls) {
        const GpuMechPacket& pkt = s_packets[dc.globalPacketIdx];

        if (s_loc_u_instanceBase >= 0)
            glUniform1i(s_loc_u_instanceBase, (int)dc.instanceBase);
        if (s_loc_u_materialFlags >= 0)
            glUniform1i(s_loc_u_materialFlags, (int)dc.materialFlags);

        // dc.texHandle is the mcTextureManager slot index, NOT a gos
        // handle (TG_TinyTexture::gosTextureHandle is a misnamed slot
        // index per memory/mc2_texture_handle_is_live.md). Resolve to the
        // live gos handle THIS FRAME, then to GL texture id.
        const DWORD liveGosHandle = (dc.texHandle != 0xFFFFFFFFu && mcTextureManager)
            ? mcTextureManager->get_gosTextureHandle(dc.texHandle)
            : 0u;
        const uint32_t glTexId = gos_GetGLTextureId((uint32_t)liveGosHandle);
#ifdef MC2_VULKAN_ISLAND
        if (vkMechGateOn()) {
            Mc2MechDrawRun run{};
            run.firstIndex    = pkt.firstIndex;
            run.indexCount    = pkt.indexCount;
            run.baseVertex    = pkt.baseVertex;
            run.instanceBase  = dc.instanceBase;
            run.instanceCount = dc.instanceCount;
            run.materialFlags = dc.materialFlags;
            run.slot0GlTex    = glTexId;   // LIVE per-actor paint scheme this frame
            // VK-PARITY-MECH-1: per-type AO / normal maps (same resolve as units 6/7 below).
            {
                const DWORD aoGos = (dc.aoTexHandle != 0u && dc.aoTexHandle != 0xFFFFFFFFu && mcTextureManager)
                    ? mcTextureManager->get_gosTextureHandle(dc.aoTexHandle) : 0u;
                run.aoGlTex = aoGos ? gos_GetGLTextureId((uint32_t)aoGos) : 0u;
                const DWORD nrmGos = (dc.normalTexHandle != 0u && dc.normalTexHandle != 0xFFFFFFFFu && mcTextureManager)
                    ? mcTextureManager->get_gosTextureHandle(dc.normalTexHandle) : 0u;
                run.normalGlTex = nrmGos ? gos_GetGLTextureId((uint32_t)nrmGos) : 0u;
            }
            s_vkMechRunSnapshot.push_back(run);
        }
#endif
        if (s_mechBatcherTrace && s_texDiagPrinted < 16) {
            ++s_texDiagPrinted;
            // Probe what the bound texture object actually contains.
            GLint tw = 0, th = 0, tfmt = 0, tMin = 0, tMag = 0;
            GLint sR = 0, sG = 0, sB = 0, sA = 0;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, glTexId);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,           &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,          &th);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &tfmt);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &tMin);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &tMag);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &sR);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, &sG);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &sB);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, &sA);
            // Probe the actually-effective sampler state (sampler-object
            // overrides texture-object params silently per brainstorm #1).
            GLint boundSampler = -1;
            glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &boundSampler);
            GLint sampMin = -1, sampMag = -1, sampWrapS = -1;
            if (boundSampler != 0) {
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_MIN_FILTER, &sampMin);
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_MAG_FILTER, &sampMag);
                glGetSamplerParameteriv((GLuint)boundSampler, GL_TEXTURE_WRAP_S,     &sampWrapS);
            }
            MC2_DIAGFE(stderr,
                "[MECHBATCHER v1] event=tex_probe glTex=%u w=%d h=%d fmt=0x%x texMin=0x%x texMag=0x%x boundSampler=%d sampMin=0x%x sampMag=0x%x sampWrapS=0x%x\n",
                glTexId, tw, th, tfmt, tMin, tMag, boundSampler, sampMin, sampMag, sampWrapS);
            // Pixel-readback diagnostic — gated on its own env var
            // (NOT MC2_MECH_BATCHER_STATS) because glGetTexImage is a
            // synchronous GPU stall that pollutes any perf measurement.
            // Only enable when actively diagnosing texture-content bugs.
            static const bool s_pixelReadback = mc2::gate::get<"MC2_MECH_TEX_READBACK">();
            if (s_pixelReadback && tw > 0 && th > 0 && tw <= 4096 && th <= 4096) {
                std::vector<uint8_t> pixels((size_t)tw * th * 3, 0);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
                int nonZero = 0;
                for (uint8_t b : pixels) if (b != 0) { nonZero = 1; break; }
                MC2_DIAGFE(stderr,
                    "[MECHBATCHER v1] event=tex_pixels glTex=%u bytes=%zu nonZero=%d first15="
                    "%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x\n",
                    glTexId, pixels.size(), nonZero,
                    pixels[ 0], pixels[ 1], pixels[ 2],
                    pixels[ 3], pixels[ 4], pixels[ 5],
                    pixels[ 6], pixels[ 7], pixels[ 8],
                    pixels[ 9], pixels[10], pixels[11],
                    pixels[12], pixels[13], pixels[14]);
            }
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, glTexId);
        // AO-1: bind this type's AO to unit 6. dc.aoTexHandle resolves slot -> live gos
        // -> GL id; 0 -> bind 0 (the shader's HAS_AO bit is OFF for AO-less types, so an
        // unbound unit 6 is never sampled -> no black). glBindSampler(6,0) clears any
        // stale sampler-object; mech.frag uses textureLod(...,0) so no mip strict-fail.
        {
            const DWORD aoGos = (dc.aoTexHandle != 0u && dc.aoTexHandle != 0xFFFFFFFFu && mcTextureManager)
                ? mcTextureManager->get_gosTextureHandle(dc.aoTexHandle) : 0u;
            const uint32_t aoGlId = aoGos ? gos_GetGLTextureId((uint32_t)aoGos) : 0u;
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, aoGlId);
            glBindSampler(6, 0);
            glActiveTexture(GL_TEXTURE0);
        }
        // NORMALS-1: bind this type's normal map to unit 7 (same contract as AO unit 6).
        // HAS_NORMAL bit is OFF for normal-less types, so an unbound unit 7 is never
        // sampled. textureLod(...,0) in the frag avoids AMD mip strict-fail.
        {
            const DWORD nrmGos = (dc.normalTexHandle != 0u && dc.normalTexHandle != 0xFFFFFFFFu && mcTextureManager)
                ? mcTextureManager->get_gosTextureHandle(dc.normalTexHandle) : 0u;
            const uint32_t nrmGlId = nrmGos ? gos_GetGLTextureId((uint32_t)nrmGos) : 0u;
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, nrmGlId);
            glBindSampler(7, 0);
            glActiveTexture(GL_TEXTURE0);
        }
        // The actual texture-black fix in Slice A is mech.frag's
        // textureLod(u_tex, v_uv, 0.0) — see memory/amd_auto_lod_strict_fail.md.
        // Sampler-state inheritance is defended by the glBindSampler(0,
        // s_sampler) above (REPEAT/LINEAR), which OVERRIDES texture-object
        // params anyway. So no per-texture glTexParameteri here. Keeping the
        // texture object's persistent state untouched avoids leaking mech-
        // specific filter/wrap onto a gosTexture handle that another renderer
        // may want to sample with auto-LOD or CLAMP later.

#ifdef MC2_VULKAN_ISLAND
        // VK-P14 MECH TIER-1: gate ON -> the island co-writes these mechs into the
        // shared GBuffer instead; skip the GL color draw (SSBOs + feed still built).
        if (!vkMechGateOn())
#endif
        glDrawElementsInstancedBaseVertex(
            GL_TRIANGLES,
            (GLsizei)pkt.indexCount,
            GL_UNSIGNED_INT,
            (void*)(uintptr_t)(pkt.firstIndex * sizeof(uint32_t)),
            (GLsizei)dc.instanceCount,
            pkt.baseVertex);

        ++drawnCalls;
    }
}

// Restore the GL state saved before the draw loop.
void MechFlushFrame::restoreGlState() {
    // Restore prior state in reverse order of save.
    // GLSTATE-GUARD-ADOPTION-3: SSBO 0/1/2 restore via guard .reset() (runs the
    // dtor immediately, same glBindBufferBase(slot, prev) it replaces) — MUST
    // fire before gos_InvalidateRenderStateCache() below (recon requirement);
    // relative order between slots is immaterial (independent binding points).
    guardSsbo0.reset();
    guardSsbo1.reset();
    guardSsbo2.reset();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit0);
    // Slice C2 + PBR-LAYERED-1: restore PBR detail sampler units 1-4.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit1);
    glBindSampler(1, (GLuint)prevSampler1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit2);
    glBindSampler(2, (GLuint)prevSampler2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit3);
    glBindSampler(3, (GLuint)prevSampler3);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit4);
    glBindSampler(4, (GLuint)prevSampler4);
    // AO-1: restore unit 6.
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit6);
    glBindSampler(6, (GLuint)prevSampler6);
    // NORMALS-1: restore unit 7.
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTexUnit7);
    glBindSampler(7, (GLuint)prevSampler7);
    glActiveTexture((GLenum)prevActiveTex);
    glBindSampler(0, (GLuint)prevSampler);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)prevElemBuf);
    glUseProgram((GLuint)prevProgram);
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(prevDepthMask);
    glDepthFunc((GLenum)prevDepthFunc);
    if (prevCull)  { glEnable(GL_CULL_FACE); glCullFace((GLenum)prevCullMode); }
    else             glDisable(GL_CULL_FACE);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
}

// Fence the ring slot and invalidate the engine render-state cache.
static void fenceAndInvalidate() {
    s_fence[s_frameSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // RENDER_STATES v1: even with the explicit save/restore above, the
    // engine's applyRenderStates cache mirrors a separate (slot,value)
    // table — it does NOT re-read GL state. Without invalidate, a
    // subsequent applyRenderStates call early-outs on matching cached
    // values while the actual texture binding / sampler / depth bits we
    // touched go unrechecked. Mirrors gos_static_prop_batcher.cpp:1791.
    gos_InvalidateRenderStateCache();
}

// Print the per-frame batcher and fallback summary (a free function: its block-scope extern must name the global).
static void emitBatcherStats(MechFlushFrame& f) {
    auto& buckets = f.buckets;
    auto& totalInstances = f.totalInstances;
    auto& drawnCalls = f.drawnCalls;
    // Per-reason stats output (MC2_MECH_BATCHER_STATS=1).
    if (s_mechBatcherTrace) {
        static const char* const kFallbackNames[] = {
            "UnregisteredType", "U8BoneOverflow", "RingOverflow",
            "TglGpuUnsupported", "ShaderInitFailure",
            "StaleLightRow"   // MECH-LIGHTROW-FRESHNESS-1
        };
        static_assert(sizeof(kFallbackNames) / sizeof(kFallbackNames[0])
                          == kGpuMechFallbackReasonCount,
                      "kFallbackNames must cover every GpuMechFallbackReason");
        uint32_t fallbackTotal = 0;
        for (int i = 0; i < kGpuMechFallbackReasonCount; ++i)
            fallbackTotal += s_fallbacksThisFrame[i];
        extern uint32_t g_mc2FrameCounter;
        MC2_DIAGFE(stderr,
            "[MECHBATCHER v1] event=summary frame=%u eligible=%u submitted=%zu "
            "buckets=%zu draw_calls=%u instances=%zu fallback_total=%u\n",
            g_mc2FrameCounter, s_eligibleActorsThisFrame, s_pendingSubmits.size(),
            buckets.size(), drawnCalls, totalInstances, fallbackTotal);
        for (int i = 0; i < kGpuMechFallbackReasonCount; ++i) {
            if (s_fallbacksThisFrame[i] > 0) {
                MC2_DIAGFE(stderr,
                    "[MECHBATCHER v1] event=fallback reason=%s count=%u\n",
                    kFallbackNames[i], s_fallbacksThisFrame[i]);
            }
        }
    }
}

// Keep this frame draw-call list and SSBO sizes for the shadow pass.
void MechFlushFrame::persistDrawCalls() {
    // Persist this frame's draw-call list and SSBO sizes for flushShadow()
    // to consume on the NEXT frame (Option B: persisted drawCalls).
    s_lastDrawCalls.clear();
    s_lastDrawCalls.reserve(drawCalls.size());
    for (const DrawCall& dc : drawCalls) {
        ShadowDrawEntry e;
        e.globalPacketIdx = dc.globalPacketIdx;
        e.instanceBase    = dc.instanceBase;
        e.instanceCount   = dc.instanceCount;
        s_lastDrawCalls.push_back(e);
    }
    s_lastTotalInstances = totalInstances;
    s_lastTotalBones     = totalBones;
}

// Copy the instance and bone palettes and the view matrix out for the VK mech island.
#ifdef MC2_VULKAN_ISLAND
void MechFlushFrame::snapshotForVkIsland() {
    // VK-P14 MECH TIER-1: snapshot this frame's instance + bone palette (host-visible
    // copies out of the mapped ring slot) + the world->clip / depth-bias the GL VS
    // uses, so the island can memcpy them into VK SSBOs without touching a fenced GL
    // buffer. Runs only when the gate is armed (empty otherwise -> feed reports 0).
    if (vkMechGateOn()) {
        s_vkMechInstSnapshot.assign(instDst, instDst + totalInstances);
        s_vkMechBoneSnapshot.assign(boneDst, boneDst + totalBones);
        // gos_GetMechFixBMVPMat4() is the row-major (GL_FALSE) kAxisSwap*worldToClip
        // the GL mech VS uploads as u_worldToClipGL (see the s_loc_terrainMVP upload).
        const float* mvp = gos_GetMechFixBMVPMat4();
        if (mvp) std::memcpy(s_vkMechWorldToClipGL, mvp, sizeof(s_vkMechWorldToClipGL));
        s_vkMechObjectDepthBias = mc2depth::OBJECT_DEPTH_BIAS;   // mclib/terrain_depth_bias.h
        s_vkMechFrameReady = (totalInstances > 0 && !s_vkMechRunSnapshot.empty());
    } else {
        s_vkMechFrameReady = false;
    }
}
#endif

// Persist per-actor facts for the extract snapshot.
static void persistExtraction() {
    // MECH-EXTRACTION-0: persist per-actor facts for batcher_getMechPendingCount/Entry.
    // Must run before s_pendingSubmits.clear() so ExtractRenderSnapshot can read this frame's data.
    static const bool s_mechExtractEnabled = mc2::gate::get<"MC2_SNAPSHOT_MECH_EXTRACT">();
    if (s_mechExtractEnabled) {
        s_mechExtractPersist.clear();
        s_mechExtractPersist.reserve(s_pendingSubmits.size());
        for (uint32_t i = 0u; i < static_cast<uint32_t>(s_pendingSubmits.size()); ++i) {
            const PendingSubmit& ps = s_pendingSubmits[i];
            ExtractedMechPacket pkt{};
            pkt.objectIdRaw = ps.desc.objectIdRaw;
            pkt.instanceIdx = i;
            // MECH-EXTRACTION-1: wire real materialIdx from the handle map populated
            // in Step 2.5 earlier this flush(). Sentinel when disabled or handle absent.
            if (s_mechMaterialGpuEnabled) {
                auto it = s_mechHandleToMaterialIdx.find(ps.desc.slot0TexHandle);
                pkt.materialIdx = (it != s_mechHandleToMaterialIdx.end())
                    ? it->second : 0xFFFFFFFFu;
            } else {
                pkt.materialIdx = 0xFFFFFFFFu;
            }
            pkt.texHandle   = ps.desc.slot0TexHandle;
            pkt.typeLodIdx  = ps.typeLodIdx;
            pkt.renderFlags = ps.desc.renderFlags;
            // GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: visual state for the debug dump.
            pkt.heat01      = ps.desc.heat01;
            pkt.damage01    = ps.desc.damage01;
            pkt.visualFlags = ps.desc.visualFlags;
            s_mechExtractPersist.push_back(pkt);
        }
    }
}

// Per-frame mech draw: bucket the pending submits, stream the instance + bone SSBOs, draw one instanced
// call per bucket, restore GL state, then persist the frame for the shadow pass, the VK island and
// the extract snapshot. Each phase is a helper above, in execution order.
void GpuMechBatcher::flush() {
    finalizePending();
    traceLatch();

    if (!g_useGpuMechs || !s_geometryFinalized || s_programLoadFailed ||
        s_pendingSubmits.empty()) {
        s_pendingSubmits.clear();
        s_eligibleActorsThisFrame = 0;
        std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
        // MECH-EXTRACTION-0: persist buffer is retained for frames with no pending submits;
        // it will be cleared in onMapUnload() or repopulated in the normal flush() path.
        return;
    }

    resolveSurfaceMaterial();
    MechFlushFrame f;
    f.buildBuckets();
    ensureRingCapacity(f.totalInstances, f.totalBones);
    if (!s_instanceMap || !s_boneMap) {
        s_pendingSubmits.clear();
        return;
    }

    f.advanceRing();
    f.writeBones();
    f.buildMaterialTable();
    f.writeInstances();
    f.validateMaterialTable();
    f.bindSsbos();
    f.saveGlState();
    applyMechPipeline();
    uploadShadingUniforms();
    uploadProjectionUniforms();
    f.drawBuckets();
    f.restoreGlState();
    fenceAndInvalidate();
    emitBatcherStats(f);
    f.persistDrawCalls();

#ifdef MC2_VULKAN_ISLAND
    f.snapshotForVkIsland();
#endif

    persistExtraction();
    s_pendingSubmits.clear();
    s_eligibleActorsThisFrame = 0;
    std::memset(s_fallbacksThisFrame, 0, sizeof(s_fallbacksThisFrame));
}

// ---------------------------------------------------------------------------
// MECH-EXTRACTION-0: snapshot API (gate: MC2_SNAPSHOT_MECH_EXTRACT=1)
// ---------------------------------------------------------------------------

uint32_t batcher_getMechPendingCount() {
    return static_cast<uint32_t>(s_mechExtractPersist.size());
}

bool batcher_getMechPendingEntry(uint32_t idx, ExtractedMechPacket* out) {
    if (!out || idx >= static_cast<uint32_t>(s_mechExtractPersist.size()))
        return false;
    *out = s_mechExtractPersist[idx];
    out->instanceIdx = idx;
    return true;
}

// V1A: per-frame mech submit count, latched at flush() entry.
// GpuMechBatcher submits only. Does NOT include MLR fallback draws or alive count.
uint64_t batcher_getLastFlushSubmitCount() {
    return s_lastFlushSubmitCount;
}
