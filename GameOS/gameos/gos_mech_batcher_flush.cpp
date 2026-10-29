// GpuMechBatcher: submitActor and GpuMechBatcher::flush. flush() is CPU only: it builds the instance, bone and run feeds
// (the persisted snapshots) the Vulkan mech island and the VK shadow cascades consume.
// Declared in the class headers; shared names in gos_mech_batcher_internal.h.
#include <atomic>
#include "gos_mech_batcher_internal.h"
#include "../../mclib/texture_assets.h"
#include "mc2fmt.h"
#include "ui_texture.h"

// ---------------------------------------------------------------------------
// submitActor (Task 6) — bone staging + texture capture
// ---------------------------------------------------------------------------
bool GpuMechBatcher::submitActor(const GpuMechSubmitDesc& desc) {
    s_lastFailWasLateReg = false;

    if (!s_geometryFinalized) return false;
    if (!desc.mechShape) return false;
    // A vehicle submits with no mechType: its record is keyed by the live shape's type shape, at lod 0.
    const void* typeKey = desc.mechType ? static_cast<const void*>(desc.mechType) : static_cast<const void*>(desc.mechShape->myMultiType);
    if (!typeKey) return false;

    const TypeLodKey key{typeKey, desc.mechType ? desc.currentLOD : 0};
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
        const int mdCount = mc2mechanim::ImportedGpuModelDelta(static_cast<const void*>(desc.mechShape), &md);
        const float lift = mc2mechanim::ImportedGpuLift(static_cast<const void*>(desc.mechShape));
        const int   liftAxis = mc2mechanim::ImportedGpuLiftAxis();   // 0/1/2 = Stuff.x/y/z
        const int   liftIdx = 3 + 4 * (liftAxis & 3);                // F[3]/F[7]/F[11]
        float Msw[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        if (desc.mechShape->GetNumShapes() > 0) {
            const float* e = static_cast<const float*>(glm::value_ptr(desc.mechShape->listOfShapes[0].shapeToWorld));
            for (int k = 0; k < 12; ++k) Msw[k] = e[k];
        }
        for (int b = 0; b < mdCount && b < static_cast<int>(rec.numBones) && md; ++b) {
            const float* D = md + static_cast<size_t>(b) * 16;
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
        for (int i = 0; i < numShapes && i < static_cast<int>(rec.numBones); ++i) {
            const TG_ShapeRec& sr = desc.mechShape->listOfShapes[i];
            const float* e = static_cast<const float*>(glm::value_ptr(sr.shapeToWorld));
            GpuMechBone bone;
            bone.row0[0]=e[0]; bone.row0[1]=e[4]; bone.row0[2]=e[ 8]; bone.row0[3]=0.0f;
            bone.row1[0]=e[1]; bone.row1[1]=e[5]; bone.row1[2]=e[ 9]; bone.row1[3]=0.0f;
            bone.row2[0]=e[2]; bone.row2[1]=e[6]; bone.row2[2]=e[10]; bone.row2[3]=0.0f;
            bone.row3[0]=e[3]; bone.row3[1]=e[7]; bone.row3[2]=e[11]; bone.row3[3]=1.0f;
            ps.bones.push_back(bone);
        }
    }
    while (static_cast<int>(ps.bones.size()) < static_cast<int>(rec.numBones)) {
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
    const auto numTextures = static_cast<uint32_t>(desc.mechShape->GetNumTextures());
    uint32_t lastTextureSlot = 0;
    uint32_t lastTextureHandle = 0;
    for (uint32_t p = 0; p < rec.packetCount; ++p) {
        const GpuMechPacket& pkt = s_packets[rec.firstPacket + p];
        if (pkt.textureSlot == 0) {
            ps.packetTexHandles[p] = desc.slot0TexHandle;
        } else if (pkt.textureSlot < numTextures) {
            // Adjacent packets often share a material. Keep this lookup local
            // to one actor so paint, LOD and reload need no cache invalidation.
            // A failed realization must still be retried by the next packet.
            if (pkt.textureSlot != lastTextureSlot || !lastTextureHandle) {
                lastTextureSlot = pkt.textureSlot;
                lastTextureHandle = mc2::textures::sourceId(desc.mechShape->GetTextureHandle(pkt.textureSlot));
            }
            ps.packetTexHandles[p] = lastTextureHandle;
        }
    }

    s_pendingSubmits.push_back(std::move(ps));

    // [SPOTLIGHT_REAL_TRACE v1] T0.2 (b) — per-instance counter. Fires once
    // per submitActor when the actor's mechType has at least one SpotLight_
    // TypeNode (recorded at registerTypeLod time). Per-frame increment is
    // unconditional; 600-frame summary is env-gated in flush() below.
    if (s_spotlightRealTypeNodeCount.find(typeKey)
            != s_spotlightRealTypeNodeCount.end()) {
        ++s_spotlightReal_drawWindow;
        ++s_spotlightReal_drawMono;
        if (!s_spotlightReal_firstHitDraw) {
            s_spotlightReal_firstHitDraw = true;
            MC2_DIAGFE(stderr,
                "[SPOTLIGHT_REAL_TRACE v1] event=first_hit site=mech_submit "
                "type=%p lod=%d\n",
                static_cast<const void*>(desc.mechType), desc.currentLOD);
            std::fflush(stderr);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// flush (Task 7) — bucket-sorted compaction into the feed snapshots
// ---------------------------------------------------------------------------
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
    uint32_t aoTexHandle;   // AO-1: AO mc2::textures::ready() slot (0=none; per-type, so consistent within a bucket).
    uint32_t normalTexHandle; // NORMALS-1: normal-map slot (0=none; per-type).
};

// Locals the flush phases hand to each other: the bucketed submits, the bone bases and the draw-call list.
struct MechFlushFrame {
    size_t totalBones = 0;
    std::map<BucketKey, std::vector<uint32_t>> buckets;  // key -> [submitIdx list]
    size_t totalInstances = 0;
    std::vector<uint32_t> actorBoneBase;
    std::vector<DrawCall> drawCalls;
    uint32_t drawnCalls = 0;

    void buildBuckets();
    void writeBones();
    void buildMaterialTable();
    void writeInstances();
    void validateMaterialTable();
    void collectRuns();
    void snapshotFrame();
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
            static_cast<unsigned long long>(s_spotlightReal_drawFrameCount),
            static_cast<unsigned long long>(s_spotlightReal_drawWindow),
            static_cast<unsigned long long>(s_spotlightReal_drawMono),
            static_cast<unsigned long long>(s_spotlightReal_typeNodeEvents),
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


    for (uint32_t si = 0; si < static_cast<uint32_t>(s_pendingSubmits.size()); ++si) {
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

// Append each actor bone palette to the bone snapshot and record its base offset.
void MechFlushFrame::writeBones() {
    // Step 4: Write the bone feed once per actor; record each actor's boneBase offset.
    actorBoneBase.assign(s_pendingSubmits.size(), 0u);
    s_vkMechBoneSnapshot.clear();
    s_vkMechBoneSnapshot.reserve(totalBones);
    for (uint32_t si = 0; si < static_cast<uint32_t>(s_pendingSubmits.size()); ++si) {
        actorBoneBase[si] = static_cast<uint32_t>(s_vkMechBoneSnapshot.size());
        for (const auto& b : s_pendingSubmits[si].bones)
            s_vkMechBoneSnapshot.push_back(b);
    }
}

// Assign each bucket a material-table row; the table grows within a mission.
void MechFlushFrame::buildMaterialTable() {
    // Step 2.5: Build mech MaterialGpu table (same bucket iteration order as Step 5).
    // s_mechDrawMaterialIdx is per-draw (bucket-iteration order) — rebuilt every flush.
    // s_mechMaterialTable / s_mechHandleToMaterialIdx PERSIST within a mission
    // (texHandle->idx is stable; reset by onMapUnload).
    s_mechDrawMaterialIdx.clear();
    if (s_mechMaterialGpuEnabled) {
        if (s_mechMaterialTable.empty())
            s_mechMaterialTable.push_back(RenderCore::MaterialGpu{}); // index 0 = sentinel/not-assigned
        for (const auto& [key, subs] : buckets) {
            const uint32_t texHandle = key.texHandle;
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
            }
            s_mechDrawMaterialIdx.push_back(mIdx);
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

// Write the instance feed in bucket order and collect the draw calls.
void MechFlushFrame::writeInstances() {
    // Step 5: Write the instance feed in bucket order; collect draw calls.
    drawCalls.reserve(buckets.size());
    s_vkMechInstSnapshot.clear();
    s_vkMechInstSnapshot.reserve(totalInstances);

    auto unpack = [](uint32_t argb, float out[4]) {
        out[0] = ((argb >> 16) & 0xFF) / 255.f;  // r
        out[1] = ((argb >>  8) & 0xFF) / 255.f;  // g
        out[2] = ((argb >>  0) & 0xFF) / 255.f;  // b
        out[3] = ((argb >> 24) & 0xFF) / 255.f;  // a
    };

    uint32_t dcIdx    = 0;
    for (const auto& [key, subs] : buckets) {

        DrawCall dc;
        dc.globalPacketIdx = key.globalPacketIdx;
        dc.texHandle       = key.texHandle;
        dc.materialFlags   = key.materialFlags;
        dc.instanceBase    = static_cast<uint32_t>(s_vkMechInstSnapshot.size());
        dc.instanceCount   = static_cast<uint32_t>(subs.size());
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
            s_vkMechInstSnapshot.push_back(inst);
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
        for (uint32_t i = 0; i < static_cast<uint32_t>(drawCalls.size()); ++i) {
            const uint32_t mIdx = (i < static_cast<uint32_t>(s_mechDrawMaterialIdx.size())) ? s_mechDrawMaterialIdx[i] : 0u;
            if (mIdx == 0u || mIdx >= static_cast<uint32_t>(s_mechMaterialTable.size())) continue;
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

// One run per draw call, with the LIVE slot-0 paint texture (and the per-type AO / normal maps) resolved this frame.
void MechFlushFrame::collectRuns() {
    s_vkMechRunSnapshot.clear();
    drawnCalls = 0;
    for (const DrawCall& dc : drawCalls) {
        const GpuMechPacket& pkt = s_packets[dc.globalPacketIdx];

        // dc.texHandle is the mc2::textures::ready() slot index, NOT a gos handle
        // (TG_TinyTexture::gosTextureHandle is a misnamed slot index per
        // memory/mc2_texture_handle_is_live.md). Resolve to the live gos handle THIS FRAME.
        const DWORD liveGosHandle = (dc.texHandle != 0xFFFFFFFFu && mc2::textures::ready())
            ? mc2::textures::sourceId(dc.texHandle)
            : 0u;
        Mc2MechDrawRun run{};
        run.firstIndex    = pkt.firstIndex;
        run.indexCount    = pkt.indexCount;
        run.baseVertex    = pkt.baseVertex;
        run.instanceBase  = dc.instanceBase;
        run.instanceCount = dc.instanceCount;
        run.materialFlags = dc.materialFlags;
        run.slot0Tex      = gos_GetMaterialTextureId(liveGosHandle);   // LIVE per-actor paint scheme this frame
        // VK-PARITY-MECH-1: per-type AO / normal maps.
        run.aoTex = (dc.aoTexHandle != 0u && dc.aoTexHandle != 0xFFFFFFFFu && mc2::textures::ready())
            ? gos_GetMaterialTextureId(mc2::textures::sourceId(dc.aoTexHandle)) : 0u;
        run.normalTex = (dc.normalTexHandle != 0u && dc.normalTexHandle != 0xFFFFFFFFu && mc2::textures::ready())
            ? gos_GetMaterialTextureId(mc2::textures::sourceId(dc.normalTexHandle)) : 0u;
        s_vkMechRunSnapshot.push_back(run);
        ++drawnCalls;
    }
}

// Publish the frame: the projection and depth bias the feed carries, the geometry generation the runs index and the
// shadow caster counts the state dump reports.
void MechFlushFrame::snapshotFrame() {
    // gos_GetMechFixBMVPMat4() is the row-major kAxisSwap*worldToClip the island's vertex stage consumes.
    const float* mvp = gos_GetMechFixBMVPMat4();
    if (mvp) std::memcpy(s_vkMechWorldToClipGL, mvp, sizeof(s_vkMechWorldToClipGL));
    s_vkMechObjectDepthBias = mc2depth::OBJECT_DEPTH_BIAS;   // mclib/terrain_depth_bias.h
    s_vkMechSnapshotGeomGen = s_vkMechGeomGen;
    s_vkMechFrameReady = (totalInstances > 0 && !s_vkMechRunSnapshot.empty());
    s_vkMechShadowReady = s_vkMechFrameReady;
    static const bool s_shadowEnabled = mc2::gate::get<"MC2_SHADOW_ENABLE">();
    if (s_shadowEnabled) {
        s_shadowTypesDrawn = static_cast<int>(drawCalls.size());
        s_shadowInstDrawn  = static_cast<int>(totalInstances);
    }
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
        extern std::atomic<uint32_t> g_mc2FrameCounter;
        MC2_DIAGFE(stderr,
            "[MECHBATCHER v1] event=summary frame=%u eligible=%u submitted=%zu "
            "buckets=%zu draw_calls=%u instances=%zu fallback_total=%u\n",
            g_mc2FrameCounter.load(std::memory_order_relaxed), s_eligibleActorsThisFrame, s_pendingSubmits.size(),
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

// The frame-graph executor's record of the MechOpaque pass: bumps the apply-state ledger (the Vulkan mech island owns the pipeline).
void GpuMechBatcher::executorApplyMechOpaqueState() {
    mc2_framegraph_executor_bump_apply_state(static_cast<unsigned>(RenderCore::framegraph::ApplyPassId::MechOpaque));
}

// Per-frame mech flush: bucket the pending submits and build the instance, bone and run feeds the Vulkan mech island
// and the VK shadow cascades consume, then persist the frame for the extract snapshot. Each phase is a helper above,
// in execution order. Nothing here touches the GPU: the island does the drawing.
void GpuMechBatcher::flush() {
    // An empty current frame must not replay last frame's caster poses.
    s_vkMechShadowReady = false;
    finalizePending();
    traceLatch();

    if (!s_geometryFinalized || s_pendingSubmits.empty()) {
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
    f.writeBones();
    f.buildMaterialTable();
    f.writeInstances();
    f.validateMaterialTable();
    f.collectRuns();
    emitBatcherStats(f);
    f.snapshotFrame();

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
