// GpuStaticPropBatcher: the VK shadow-island geometry feed and the colour-prop island feeds (the GL prop shadow draws are gone).
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"
#include "mc2fmt.h"
#include "view_uniforms_gl.h"   // RenderCore::getCurrentView: the eye position of the VK prop specular
#include "../../RenderCore/FramePackets.h"

void GpuStaticPropBatcher::setDebugAddrMode(int mode) { debugAddrMode_ = mode; }

// VK-P8a SHADOW-INTEROP: caster-geometry feed for the Vulkan shadow island. Exposes
// the finalized shared VBO/IBO staging bytes + per-type packet ranges so the island
// can copy them ONCE per map into VK-owned buffers (STEP 0 feed decision, fallback
// (B): VK owns a copy). Read-only; buffers persist until onMapUnload() clears the
// staging vectors. Returns false if geometry is not yet finalized.
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_shadow_island_live.h"
extern "C" int mc2_shadow_island_get_geometry_feed(Mc2ShadowGeomFeed* out) {
    if (!out) return 0;
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return 0;
    if (s_sharedVboBytes == 0 || s_sharedIboBytes == 0 || s_packets.empty()) return 0;

    // Per-packet ranges (subset of GpuStaticPropPacket) + per-type packet spans.
    // Rebuilt when the packet/type count changes (finalize is once per map).
    static std::vector<Mc2ShadowPacket> s_feedPackets;
    static std::vector<uint32_t>        s_feedTypeFirstPacket;
    static std::vector<uint32_t>        s_feedTypePacketCount;
    static size_t s_feedPacketGen = SIZE_MAX;
    static size_t s_feedTypeGen   = SIZE_MAX;
    if (s_feedPacketGen != s_packets.size()) {
        s_feedPackets.resize(s_packets.size());
        for (size_t i = 0; i < s_packets.size(); ++i) {
            s_feedPackets[i].firstIndex = s_packets[i].firstIndex;
            s_feedPackets[i].indexCount = s_packets[i].indexCount;
            s_feedPackets[i].baseVertex = (uint32_t)s_packets[i].baseVertex;
        }
        s_feedPacketGen = s_packets.size();
    }
    if (s_feedTypeGen != s_types.size()) {
        s_feedTypeFirstPacket.resize(s_types.size());
        s_feedTypePacketCount.resize(s_types.size());
        for (size_t t = 0; t < s_types.size(); ++t) {
            s_feedTypeFirstPacket[t] = s_types[t].firstPacket;
            s_feedTypePacketCount[t] = s_types[t].packetCount;
        }
        s_feedTypeGen = s_types.size();
    }

    out->vboBytes        = s_sharedVboBytes;
    out->iboBytes        = s_sharedIboBytes;
    out->vertexStride    = (uint32_t)kVertexStride;
    out->packets         = s_feedPackets.data();
    out->packetCount     = (uint32_t)s_feedPackets.size();
    out->typeFirstPacket = s_feedTypeFirstPacket.data();
    out->typePacketCount = s_feedTypePacketCount.data();
    out->typeCount       = (uint32_t)s_types.size();
    return 1;
}

// VK-P11 STATIC-PROP COLOR — flatten the full static-prop instance set into a single
// GpuStaticPropInstance array grouped by typeID, and hand out the world->clip matrix
// + reverse-Z depth bias. Called by the color island ONLY when its gate is ON (the
// GL flush() is skipped in that case).
//
// CURRENT BEHAVIOUR (supersedes the history below): per type the feed is the
// persistent-store block FIRST, then this frame's dynamic submits from
// s_bucketsByType (excluding the reinjPos/reinjCount range copied from the store);
// mc2_color_prop_island_end_frame() performs flush()'s end-of-frame bookkeeping.
// The 'persistent store only' text below describes the original fix, kept for context.
//
// VK-PROP-COVERAGE FIX (source correction): originally this read s_bucketsByType.
// That map is a per-FRAME accumulator whose ONLY per-frame clear lives inside
// GpuStaticPropBatcher::flush() (line ~7661) — which is SKIPPED when the island owns
// the frame. So s_bucketsByType was simultaneously (a) unbounded-accumulating across
// island frames (measured on mc2_01: feed_total 968 -> 1.17M over ~10 frames as
// reinjectPersistentStatic() re-appended the whole store every frame with nothing
// clearing it), overflowing the island's instance/buffer capacity so only low-offset
// building runs survived -> "props are building-only"; and (b) on frames where the
// registry's frame-currency optimisation left it near-empty, feeding only 1-2 props.
// Either way the buckets are the WRONG source for a "draw one frame's full prop set"
// island that runs in place of flush().
//
// The AUTHORITATIVE, stable, all-population set is the persistent-static store
// (s_persistentStaticStore) — the registry recipes for every registered static prop:
// Buildings AND Trees AND Generic (all use the registry markVisible path; measured:
// 733 trees eligible, all via the store, dyn-submit=0). It is population-agnostic and
// is exactly what reinjectPersistentStatic() copies into the GL buckets each frame, so
// feeding it directly gives the island the SAME prop set GL flush() draws — trees,
// tents, misc props included — without the accumulation hazard. Read-only: does NOT
// mutate any batcher state. Output is a reused static scratch vector.
extern "C" uint32_t mc2_static_prop_vertex_stride(void) { return (uint32_t)kVertexStride; }

// Called by txmmgr once per frame at the point where the GL flush() is skipped because
// the VK color island handled the frame (flush() owns the clear on the GL path).
extern "C" void mc2_color_prop_island_end_frame() {
    // Mirror flush()'s end-of-frame bookkeeping (see flush(): s_bucketsByType.clear
    // + s_lastUploadedSlot reset + prev-frame per-type instance count snapshot).
    s_bucketsByType.clear();
    s_reinjectedThisFrame = false;
    s_lastUploadedSlot = 0xFFFFFFFFu;
    s_typeInstanceCountPrevFrame.clear();
    for (const auto& kv : s_typeRanges)
        s_typeInstanceCountPrevFrame[kv.first] = kv.second.instanceCount;
}

extern "C" int mc2_color_prop_island_get_frame_feed(Mc2ColorPropFeed* out) {
    if (!out) return 0;
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return 0;
    static std::vector<GpuStaticPropInstance> s_colorFeedFlat;
    s_colorFeedFlat.clear();

    // Grouped by typeID for correct per-type geometry addressing (the island issues
    // one type-run per contiguous typeID block via firstInstance). Deterministic
    // ordering (ascending typeID) so the flattening is stable frame-to-frame.
    // Per type: persistent-store block first, then this frame's DYNAMIC submits
    // (buildings failing isStaticEligible etc.) from s_bucketsByType, excluding the
    // range reinjectPersistentStatic() copied from the store (else double-draw).
    // Read point: inside the island run, which txmmgr calls after renderLists() has
    // issued all submits and the registry reinject for the frame.
    std::vector<uint32_t> typeIds;
    typeIds.reserve(s_persistentStaticStore.size() + s_bucketsByType.size());
    for (const auto& kv : s_persistentStaticStore) typeIds.push_back(kv.first);
    for (const auto& kv : s_bucketsByType) typeIds.push_back(kv.first);
    std::sort(typeIds.begin(), typeIds.end());
    typeIds.erase(std::unique(typeIds.begin(), typeIds.end()), typeIds.end());
    for (uint32_t tid : typeIds) {
        auto it = s_persistentStaticStore.find(tid);
        if (it != s_persistentStaticStore.end()) {
            const std::vector<GpuStaticPropInstance>& insts = it->second;
            s_colorFeedFlat.insert(s_colorFeedFlat.end(), insts.begin(), insts.end());
        }
        auto bt = s_bucketsByType.find(tid);
        if (bt != s_bucketsByType.end()) {
            const PerTypeBucket& b = bt->second;
            const uint32_t n  = (uint32_t)b.instances.size();
            const uint32_t rp = std::min(b.reinjPos, n);
            const uint32_t re = std::min(rp + b.reinjCount, n);
            s_colorFeedFlat.insert(s_colorFeedFlat.end(), b.instances.begin(), b.instances.begin() + rp);
            s_colorFeedFlat.insert(s_colorFeedFlat.end(), b.instances.begin() + re, b.instances.end());
        }
    }
    if (s_colorFeedFlat.empty()) return 0;

    out->instances112 = s_colorFeedFlat.data();
    out->count        = (uint32_t)s_colorFeedFlat.size();
    const float* mvp = gos_GetObjectFixBMVPMat4();
    if (mvp) std::memcpy(out->worldToClipGL, mvp, sizeof(out->worldToClipGL));
    else     std::memset(out->worldToClipGL, 0, sizeof(out->worldToClipGL));
    // OBJECT_DEPTH_BIAS mirror (shaders/include/terrain_depth_bias.hglsl:62).
    out->objectDepthBias = 0.0001f;

    // VK-OFFLINE-PROOFS-1: golden feed dump. Fires once per process (see
    // vk_feed_dump.h). No-op unless MC2_VK_FEED_DUMP is set.
    if (mc2vkfeeddump::enabled() && mc2vkfeeddump::readyToDump() && !mc2vkfeeddump::latch("prop")) {
        if (FILE* f = mc2vkfeeddump::openAppend(/*truncateFirst=*/false)) {
            mc2::fmt::print(f, "{{\"island\":\"prop\",\"count\":{:d},\"objectDepthBias\":{:.6f},\"instances\":[", mc2::fmt::as_u32(out->count), out->objectDepthBias);
            const GpuStaticPropInstance* insts =
                reinterpret_cast<const GpuStaticPropInstance*>(out->instances112);
            for (uint32_t i = 0; i < out->count; ++i) {
                const GpuStaticPropInstance& gi = insts[i];
                // Translation lives in row 3 of a row-major 4x4 (col-major layout:
                // indices 12,13,14 are the last column == translation for either
                // convention used consistently across the island's mirror).
                mc2::fmt::print(f, "{}{{\"typeID\":{:d},\"posX\":{:.4f},\"posY\":{:.4f},\"posZ\":{:.4f},\"flags\":{:d}}}",
                    mc2::fmt::safe(i ? "," : ""), mc2::fmt::as_u32(gi.typeID),
                    gi.modelMatrix[12], gi.modelMatrix[13], gi.modelMatrix[14],
                    mc2::fmt::as_u32(gi.flags));
            }
            mc2::fmt::print(f, "]}}\n");
            std::fclose(f);
        }
        mc2vkfeeddump::latch("prop") = true;
    }
    return 1;
}

// VK-P11 STATIC-PROP MATERIAL — expose the alpha-OFF albedo 2D_ARRAY + per-type
// layer table so the color island can copy-once import it and sample real textures.
// Read-only; buffers persist until onMapUnload(). Returns 0 (fail soft -> island
// keeps flat teal) when geometry is not finalized, the alpha-OFF array is unbuilt,
// or the group was BC7-bucketed (bucket-relative layers invalidate the single-array
// import — materials for BC7 are a follow-on).
extern "C" int mc2_color_prop_island_get_material_feed(Mc2ColorPropMaterialFeed* out) {
    if (!out) return 0;
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return 0;
    if (s_texArrayOff == 0) return 0;                 // alpha-OFF array not built
    if (s_typeAlbedoLayer.empty() ||
        s_typeAlbedoLayer.size() != s_types.size()) return 0;

    out->glTexArrayOff   = s_texArrayOff;
    // VK-P11 STATIC-PROP MATERIAL alpha-ON: expose the alpha-ON group array (0 if not
    // built). typeAlbedoLayer[] already carries the group-relative layer for class-1
    // types, so the island routes them into this array with the same layer table.
    out->glTexArrayOn    = s_texArrayOn;
    {
        const auto& batcher = GpuStaticPropBatcher::instance();
        out->albedoOff = batcher.albedoArray(0).levels.empty() ? nullptr : &batcher.albedoArray(0);
        out->albedoOn  = batcher.albedoArray(1).levels.empty() ? nullptr : &batcher.albedoArray(1);
    }
    // BC7-bucketed: per-bucket sibling arrays with bucket-relative layers. The
    // single-array (group-relative) import + layer table below do not describe that
    // layout, so signal the island to fail soft (keep teal) rather than mis-sample.
    out->bucketed        = s_bucketArrays.empty() ? 0u : 1u;
    out->typeAlbedoLayer = s_typeAlbedoLayer.data();
    out->typeAlphaClass  = s_typeAlphaClass.data();
    out->typeCount       = (uint32_t)s_typeAlbedoLayer.size();
    // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: per-packet layer (see header comment).
    // s_packetTexArrayLayer is populated in finalizeGeometry() from the SAME
    // per-packet, group-relative layerForPacket[] GL's coalesce PerDrawEntry table
    // uses — it is the correct source, unlike the per-type "first-valid-wins" table.
    out->packetAlbedoLayer = s_packetTexArrayLayer.empty() ? nullptr : s_packetTexArrayLayer.data();
    out->packetCount       = (uint32_t)s_packetTexArrayLayer.size();
    // FENCE-UVSCALE-1: same per-packet indexing as packetAlbedoLayer, populated
    // in finalizeGeometry() from uvScaleXByPacket/uvScaleYByPacket (the same
    // source PerDrawEntry.uvScaleX/Y draws from on the GL side). nullptr if the
    // sidecar size mismatches packetAlbedoLayer's -- island falls back to 1.0.
    out->packetUvScaleX = (s_packetUvScaleX.size() == s_packetTexArrayLayer.size())
                              ? s_packetUvScaleX.data() : nullptr;
    out->packetUvScaleY = (s_packetUvScaleY.size() == s_packetTexArrayLayer.size())
                              ? s_packetUvScaleY.data() : nullptr;

    // Terrain sun (MC2 world, surface->sun) — same vector the GL prop VS uses for the
    // directional term (STATIC-PROP-TERRAIN-SUN-DIFFUSE). Provides computePBR's L.
    extern void gos_GetTerrainLightDir(float*, float*, float*);
    float tsx = 0.0f, tsy = 0.0f, tsz = 1.0f;
    gos_GetTerrainLightDir(&tsx, &tsy, &tsz);
    out->terrainSunMC2[0] = tsx;
    out->terrainSunMC2[1] = tsy;
    out->terrainSunMC2[2] = tsz;

    out->typeHotColors = (s_typeHotColors.size() == s_types.size() * 12) ? s_typeHotColors.data() : nullptr;

    return 1;
}

// VK-PROP-AMBIENT-IBL-1: real ObjectLights ambient + IBL-SH feed. GL-owner accessors
// (this runs in GL context, mirrors mc2_mech_island_get_frame_feed's setup at
// gos_mech_batcher.cpp:2910-2915).
extern "C" int mc2_color_prop_island_get_light_feed(Mc2ColorPropLightFeed* out) {
    if (!out) return 0;
    // VK-PROP-LIGHTFEED-NATIVE-1: native CPU feed (mech item-9 pattern, mirrors
    // mc2_mech_island_get_frame_feed's lightDataCpu export) — the byte source of
    // the GL SSBO upload. VK-PERF-2 cleanup deleted the GL SSBO name/size export
    // with the audit-gated readback it existed for (clean audit banked).
    out->lightDataCpu = nullptr;
    out->lightDataCpuBytes = 0;
    if (mcTextureManager) {
        out->lightDataCpu      = mcTextureManager->getLightDataPtr();
        out->lightDataCpuBytes = mcTextureManager->getLightDataBytes();
    }
    // s_currentShSet: the live per-mission SH-L2 set (RenderCore::IblShRegistry),
    // same table the GL static-prop VS uniform upload uses (u_iblSh[9]).
    // IBL-SUN-ROTATE-1: routed through the SAME accessor as the two GL upload
    // sites, so VK and the debug state dump see the rotated set when the gate
    // is on and cannot silently disagree with GL. Gate OFF -> identical bytes.
    std::memcpy(out->iblSh, iblShCoeffsForUpload(), sizeof(out->iblSh));
    // The StaticPropIblSh option (default on). Mirrors the GL per-frame upload rule
    // exactly: on -> g_iblShStrength; off -> 0.0.
    out->iblShStrength = iblShStrengthNightScaled();
    bool isNight = false;
    float nightFactor = 0.0f;
    gos_GetNightState(&isNight, &nightFactor);
    out->isNight      = isNight ? 1 : 0;
    out->nightFactor  = nightFactor;
    out->ambientV1Strength = staticPropAmbientV1Enabled() ? 1.0f : 0.0f;
    // The GL upload zeroes the strength when the view-uniform block is off; the island always has the eye position.
    out->pbrV1Strength = pbrV1Enabled() ? g_pbrV1Strength : 0.0f;
    out->pbrV1RoughnessOverride = g_pbrV1RoughnessOverrideEnabled ? g_pbrV1RoughnessOverrideValue : -1.0f;
    const RenderCore::ViewUniforms vu = RenderCore::framePackets().copyMainView().uniforms;   // by value: readers copy synchronously (FramePackets.h)
    for (int k = 0; k < 4; ++k) out->cameraWorldPos[k] = vu.cameraWorldPos[k];
    // The same pool set and knobs GL static_prop.frag receives (UploadObjectLightPoolUniforms).
    gos_terrain_lighting::ObjectLightPoolParams pp{};
    gos_terrain_lighting::GetObjectLightPoolParams(&pp);
    out->poolLights48   = pp.pool;
    out->numPoolLights  = pp.pool ? pp.count : 0;
    out->poolSoft       = pp.soft;
    out->poolMoverDim   = pp.moverDim;
    out->poolMoverScale = pp.moverScale;
    out->poolMoverMax   = pp.moverMax;
    out->poolCompress   = pp.compress;
    out->poolObjScale   = pp.objScale;
    return 1;
}
#endif // MC2_VULKAN_ISLAND
