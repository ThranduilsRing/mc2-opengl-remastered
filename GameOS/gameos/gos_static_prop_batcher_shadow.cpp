#include "../../mclib/terrain_depth_bias.h"
// GpuStaticPropBatcher: the VK shadow-island geometry feed and the colour-prop island feeds (the GL prop shadow draws are gone).
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include <atomic>
#include "gos_static_prop_batcher_internal.h"
#include "mc2fmt.h"
#include "diagnostic_trace.h"
#include "view_registry.h"
#include "visual_tuning_profile.h"
#include "../../RenderCore/FramePackets.h"
#include "shadow_mover_feed.h"
#include "../../RenderCore/StaticPropOrigin.h"
#include "../../RenderCore/PropListRoute.h"
#include "../../RenderCore/PropRecordCache.h"

void GpuStaticPropBatcher::captureMoverShadowShape(const void* owner, TG_MultiShape* multi) {
    if (!multi || !multi->listOfShapes || !s_geometryFinalized || g_mechPreviewRenderDepth>0) return;
    auto& feed=mc2shadow::moverPropFeed();
    if (feed.paused()) return;
    if (feed.contains(owner,g_mc2FrameCounter.load(std::memory_order_relaxed))) return;
    static std::vector<GpuStaticPropInstance> leaves;
    leaves.clear();
    for (int i=0;i<multi->numTG_Shapes;++i) {
        const auto& rec=multi->listOfShapes[i];
        TG_Shape* shape=rec.node.get();
        if (!rec.processMe || !shape || shape->noShadow || !shape->myType || shape->myType->GetNodeType()!=SHAPE_NODE || shape->GetIsSpotlight()) continue;
        GpuStaticPropInstance leaf{};
        const uint32_t flags=shape->GetIsAlphaTest()?(1u<<2):0;
        if (GpuStaticPropBatcher::instance().buildRecipeFromShape(shape,glm::mat4(rec.shapeToWorld),0,0,flags,&leaf)) leaves.push_back(leaf);
    }
    feed.record(owner,leaves,g_mc2FrameCounter.load(std::memory_order_relaxed));
}

void GpuStaticPropBatcher::setDebugAddrMode(int mode) { debugAddrMode_ = mode; }

// VK-P8a SHADOW-INTEROP: caster-geometry feed for the Vulkan shadow island. Exposes
// the finalized shared VBO/IBO staging bytes + per-type packet ranges so the island
// can copy them ONCE per map into VK-owned buffers (STEP 0 feed decision, fallback
// (B): VK owns a copy). Read-only; buffers persist until onMapUnload() clears the
// staging vectors. Returns false if geometry is not yet finalized.
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
            s_feedPackets[i].baseVertex = static_cast<uint32_t>(s_packets[i].baseVertex);
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
    out->vertexStride    = static_cast<uint32_t>(kVertexStride);
    out->packets         = s_feedPackets.data();
    out->packetCount     = static_cast<uint32_t>(s_feedPackets.size());
    out->typeFirstPacket = s_feedTypeFirstPacket.data();
    out->typePacketCount = s_feedTypePacketCount.data();
    out->typeCount       = static_cast<uint32_t>(s_types.size());
    return 1;
}

// VK-P11 STATIC-PROP COLOR — flatten the full static-prop instance set into a single
// GpuStaticPropInstance array grouped by typeID, and hand out the world->clip matrix
// + reverse-Z depth bias. Called by the color island ONLY when its gate is ON (the
// GL flush() is skipped in that case).
extern "C" uint32_t mc2_static_prop_vertex_stride(void) { return static_cast<uint32_t>(kVertexStride); }

// Called by txmmgr once per frame at the point where the GL flush() is skipped because
// the VK color island handled the frame (flush() owns the clear on the GL path).
extern "C" void mc2_color_prop_island_end_frame() {
    // Mirror flush()'s end-of-frame bookkeeping (see flush(): s_bucketsByType.clear
    // + s_lastUploadedSlot reset + prev-frame per-type instance count snapshot).
    s_bucketsByType.clear();
    s_lastUploadedSlot = 0xFFFFFFFFu;
    s_typeInstanceCountPrevFrame.clear();
    for (const auto& [rangeType, range] : s_typeRanges)
        s_typeInstanceCountPrevFrame[rangeType] = range.instanceCount;
}

static uint64_t propLodKey(const GpuStaticPropInstance& inst) {
    const StaticPropOrigin o = staticPropOriginStuff(inst.modelMatrix);
    uint32_t px, py, pz;
    std::memcpy(&px, &o.x, 4);
    std::memcpy(&py, &o.y, 4);
    std::memcpy(&pz, &o.z, 4);
    return (((static_cast<uint64_t>(px) * 0x9E3779B1ull) ^ (static_cast<uint64_t>(py) * 0x85EBCA77ull) ^ (static_cast<uint64_t>(pz) * 0xC2B2AE3Dull)) << 8) ^ inst.typeID;
}

void GpuStaticPropBatcher::forgetLodState(const GpuStaticPropInstance& inst) {
    s_lodState.erase(propLodKey(inst));
}

void mc2_static_prop_retype_far_tier(std::span<GpuStaticPropInstance> instances, bool resort) {
    if (s_lodMembers.empty()) return;
    bool any = false;
    for (GpuStaticPropInstance& inst : instances) {
        if (!s_lodMembers.contains(inst.typeID)) continue;
        const PropLodMember& m = s_lodMembers.at(inst.typeID);
        if (m.farType != inst.typeID) { inst.typeID = m.farType; any = true; }
    }
    if (any && resort) std::sort(instances.begin(), instances.end(), [](const GpuStaticPropInstance& a, const GpuStaticPropInstance& b) { return a.typeID < b.typeID; });
}

int mc2_static_prop_lod_skips_late(uint32_t typeID) {
    if (!s_lodMembers.contains(typeID)) return 0;
    const PropLodMember& m = s_lodMembers.at(typeID);
    return PropList::tierSkipsFringe(m.tier) ? 1 : 0;
}

extern "C" int mc2_color_prop_island_get_frame_feed(Mc2ColorPropFeed* out) {
    ZoneScopedN("VKIsl.Props.MainFeed");
    if (!out) return 0;
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return 0;
    static std::vector<GpuStaticPropInstance> s_colorFeedFlat;
    size_t feedCursor = 0;

    // Grouped by typeID for correct per-type geometry addressing (the island issues
    // one type-run per contiguous typeID block via firstInstance). Deterministic
    // ordering (ascending typeID) so the flattening is stable frame-to-frame.
    // Per type: persistent-store block first, then this frame's DYNAMIC submits
    // (buildings failing isStaticEligible etc.) from s_bucketsByType.
    // Read point: inside the island run, which txmmgr calls after renderLists() has
    // issued all submits for the frame.
    // Distance LOD (model-override statics only; MC2_VK_PROP_LOD=0 disables): each instance of a LOD-capable
    // type is re-typed to the LOD its camera distance selects, with a +-8% band so it never flickers at a
    // boundary. Re-typed instances are collected per target type and appended in that type's slot below, so
    // every (type, LOD) is still one contiguous run (one instanced draw) and the HZB pass sees the same feed.
    static const bool s_lodOn = mc2::gate::get<"MC2_VK_PROP_LOD">();
    static std::unordered_map<uint32_t, std::vector<GpuStaticPropInstance>> s_lodOut;
    static std::unordered_map<uint32_t, PropLodTierCache<GpuStaticPropInstance, kMaxPropLods>> s_lodCache;
    std::unordered_map<uint32_t, const std::vector<GpuStaticPropInstance>*> persistentTiers;
    static const bool s_statsOn = mc2_diag::tagRequested("TREE_LOD_STATS");
    const bool lodActive = s_lodOn && !s_lodGroups.empty();
    { ZoneScopedN("VKIsl.Props.MainFeed.LODRoute");
    for (auto& [lodTid, lodVec] : s_lodOut) lodVec.clear();
    if (lodActive) {
        // Overlapping targets retain the original interleaved group routing.
        std::unordered_map<uint32_t, unsigned> targetUses;
        for (const auto& [src, g] : s_lodGroups)
            for (int tier = 0; tier < g.count; ++tier) ++targetUses[g.target[tier]];
        const RenderCore::ViewPacket mainView = RenderCore::framePackets().copyMainView();
        const RenderCore::ViewUniforms& vu = mainView.uniforms;
        const float cx = vu.cameraWorldPos[0], cy = vu.cameraWorldPos[1], cz = vu.cameraWorldPos[2];
        constexpr float kBand = 0.08f;
        // Tree chains switch on projected size: d = clip w / (clip-y row length) is the distance a 90 degree lens would
        // need for the same pixels per world unit (zoom-independent), scaled so a 2160p view is the reference.
        const float* m = vu.worldToClipGL;   // row-major storage, row-vector transform
        const float rowY = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
        const float vpScale = mainView.viewport[3] > 0 ? 2160.0f / static_cast<float>(mainView.viewport[3]) : 1.0f;
        float maxTreeD = 0.0f;
        bool staticBlend = false;
        auto route = [&](const GpuStaticPropInstance* b, const GpuStaticPropInstance* e, const PropLodGroup& g,
                         const size_t* slots, const auto& outputs, uint8_t* snapshots = nullptr, bool staticOnly = false) {
            for (; b != e; ++b) {
                const StaticPropOrigin o = staticPropOriginRender(b->modelMatrix);
                const float dx = o.x - cx, dy = o.y - cy, dz = o.z - cz;
                float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (g.tree && rowY > 1e-6f) d = propLodProjectedDistance(m, o, rowY, vpScale);
                if (s_statsOn && g.tree) maxTreeD = (std::max)(maxTreeD, d);
                uint8_t& lod = slots ? s_lodState.dense(*slots++) : s_lodState[propLodKey(*b)];
                propLodAdvance(lod, d, std::span<const float>(g.dist, static_cast<size_t>(g.count)), kBand);
                int blendTier = -1;
                float blend = 0.0f;
                if (staticOnly) for (int tier = 1; tier < g.count; ++tier) {
                    const float lo = g.dist[tier] * (1.0f - kBand), hi = g.dist[tier] * (1.0f + kBand);
                    if (d > lo && d < hi) { blendTier = tier; blend = (d - lo) / (hi - lo); break; }
                }
                if (staticOnly && blendTier < 0) {
                    lod = 0;
                    while (lod + 1 < g.count && d >= g.dist[lod + 1]) ++lod;
                }
                staticBlend |= blendTier >= 0;
                if (snapshots) { *snapshots++ = lod; continue; }
                GpuStaticPropInstance inst = *b;
                if (blendTier >= 0) {
                    // fogRGB.w is unused by Vulkan props. Values outside [0,1] encode
                    // complementary spatial coverage; dynamic submissions never get it.
                    inst.typeID = g.target[blendTier - 1]; inst.fogRGB[3] = -2.0f - blend;
                    outputs[blendTier - 1]->push_back(inst);
                    inst.typeID = g.target[blendTier]; inst.fogRGB[3] = 2.0f + blend;
                    outputs[blendTier]->push_back(inst);
                    continue;
                }
                inst.typeID = g.target[lod];
                outputs[lod]->push_back(inst);
            }
        };
        for (const auto& [src, g] : s_lodGroups) {
            bool overlapping = false;
            for (int tier = 0; tier < g.count; ++tier) overlapping |= targetUses[g.target[tier]] > 1;
            std::array<std::vector<GpuStaticPropInstance>*, kMaxPropLods> outputs{};
            for (int tier = 0; tier < g.count; ++tier) outputs[tier] = &s_lodOut[g.target[tier]];
            if (auto it = s_persistentStaticStore.find(src); it != s_persistentStaticStore.end()) {
                const auto& [stTid, stVec] = *it;
                auto& slots = s_persistentLodSlots[src];
                const bool rebound = slots.revision != s_lodState.revision() || slots.indices.size() != stVec.size();
                slots.bind(s_lodState, std::span<const GpuStaticPropInstance>(stVec), propLodKey);
                if (rebound) {
                    for (auto cached = s_lodCache.begin(); cached != s_lodCache.end(); ) {
                        if (!s_lodGroups.count(cached->first)) cached = s_lodCache.erase(cached);
                        else ++cached;
                    }
                }
                if (overlapping) route(stVec.data(), stVec.data() + stVec.size(), g, slots.indices.data(), outputs, nullptr, true);
                else {
                    static std::vector<uint8_t> snapshots;
                    snapshots.resize(stVec.size());
                    staticBlend = false;
                    route(stVec.data(), stVec.data() + stVec.size(), g, slots.indices.data(), outputs, snapshots.data(), true);
                    if (staticBlend) {
                        route(stVec.data(), stVec.data() + stVec.size(), g, slots.indices.data(), outputs, nullptr, true);
                    } else {
                        auto& cache = s_lodCache[src];
                        cache.refresh(std::span<const GpuStaticPropInstance>(stVec), rebound,
                            [&](size_t i) { return snapshots[i]; },
                            [&](GpuStaticPropInstance inst, uint8_t tier) { inst.typeID = g.target[tier]; return inst; });
                        for (int tier = 0; tier < g.count; ++tier) persistentTiers[g.target[tier]] = &cache.tiers[tier];
                    }
                }
            }
            if (auto bt = s_bucketsByType.find(src); bt != s_bucketsByType.end()) {
                const auto& [bkTid, pb] = *bt;
                const GpuStaticPropInstance* d0 = pb.instances.data();
                const uint32_t n  = static_cast<uint32_t>(pb.instances.size());
                route(d0, d0 + n, g, nullptr, outputs);
            }
        }
        static uint32_t s_statsTick = 0;
        if (s_statsOn && (++s_statsTick <= 3u || s_statsTick % 120u == 0)) {
            uint64_t inst[kMaxPropLods] = {}, tris[kMaxPropLods] = {};
            auto countTier = [&](uint32_t tid, const auto& vec) {
                if (!s_lodMembers.contains(tid) || tid >= s_types.size() || !s_types[tid].source) return;
                const size_t tier = s_lodMembers.at(tid).tier;
                inst[tier] += vec.size(); tris[tier] += vec.size() * static_cast<uint64_t>(s_types[tid].source->GetNumTypeTriangles());
            };
            for (const auto& [tid, vec] : s_lodOut) countTier(tid, vec);
            for (const auto& [tid, vec] : persistentTiers) countTier(tid, *vec);
            mc2::fmt::print_err("[TREE_LOD_STATS] frame={} lodActive={} d={} rowY={} viewport={}x{} inst={} {} {} {} {} tris={} {} {} {} {}\n", mainView.frame, lodActive, maxTreeD, rowY, mainView.viewport[2], mainView.viewport[3], mc2::fmt::as_u64(inst[0]), mc2::fmt::as_u64(inst[1]), mc2::fmt::as_u64(inst[2]), mc2::fmt::as_u64(inst[3]), mc2::fmt::as_u64(inst[4]),
                mc2::fmt::as_u64(tris[0]), mc2::fmt::as_u64(tris[1]), mc2::fmt::as_u64(tris[2]), mc2::fmt::as_u64(tris[3]), mc2::fmt::as_u64(tris[4]));
        }
    }
    }
    static std::vector<uint32_t> typeIds;
    typeIds.clear();
    { ZoneScopedN("VKIsl.Props.MainFeed.TypeOrder");
    typeIds.reserve(s_persistentStaticStore.size() + s_bucketsByType.size());
    for (const auto& [storeType, storeInsts] : s_persistentStaticStore) typeIds.push_back(storeType);
    for (const auto& [bucketType, bucketRef] : s_bucketsByType) typeIds.push_back(bucketType);
    if (lodActive) for (const auto& [lodTid, lodVec] : s_lodOut) if (!lodVec.empty()) typeIds.push_back(lodTid);
    if (lodActive) for (const auto& [tid, vec] : persistentTiers) if (!vec->empty()) typeIds.push_back(tid);
    std::sort(typeIds.begin(), typeIds.end());
    typeIds.erase(std::unique(typeIds.begin(), typeIds.end()), typeIds.end());
    }
    { ZoneScopedN("VKIsl.Props.MainFeed.Flatten");
    for (uint32_t tid : typeIds) {
        if (lodActive) if (auto p = persistentTiers.find(tid); p != persistentTiers.end())
            appendPropFeed(s_colorFeedFlat, feedCursor, std::span<const GpuStaticPropInstance>(*p->second));
        if (lodActive) if (auto lo = s_lodOut.find(tid); lo != s_lodOut.end()) {
            const auto& [loTid, loVec] = *lo;
            appendPropFeed(s_colorFeedFlat, feedCursor, std::span<const GpuStaticPropInstance>(loVec));
        }
        if (lodActive && s_lodGroups.count(tid)) continue; // already re-typed above
        auto it = s_persistentStaticStore.find(tid);
        if (it != s_persistentStaticStore.end()) {
            const auto& [storeKey, insts] = *it;
            appendPropFeed(s_colorFeedFlat, feedCursor, std::span<const GpuStaticPropInstance>(insts));
        }
        auto bt = s_bucketsByType.find(tid);
        if (bt != s_bucketsByType.end()) {
            const auto& [bucketKey, b] = *bt;
            appendPropFeed(s_colorFeedFlat, feedCursor, std::span<const GpuStaticPropInstance>(b.instances));
        }
    }
    }
    s_colorFeedFlat.resize(feedCursor);
    if (s_colorFeedFlat.empty()) return 0;

    out->instances112 = s_colorFeedFlat.data();
    out->count        = static_cast<uint32_t>(s_colorFeedFlat.size());
    const float* mvp = gos_GetObjectFixBMVPMat4();
    if (mvp) std::memcpy(out->worldToClipGL, mvp, sizeof(out->worldToClipGL));
    else     std::memset(out->worldToClipGL, 0, sizeof(out->worldToClipGL));
    // OBJECT_DEPTH_BIAS mirror (shaders/include/terrain_depth_bias.hglsl:62).
    out->objectDepthBias = mc2depth::OBJECT_DEPTH_BIAS_REL;

    // VK-OFFLINE-PROOFS-1: golden feed dump. Fires once per process (see
    // vk_feed_dump.h). No-op unless MC2_VK_FEED_DUMP is set.
    if (mc2vkfeeddump::enabled() && mc2vkfeeddump::readyToDump() && !mc2vkfeeddump::latch("prop")) {
        if (FILE* f = mc2vkfeeddump::openAppend(/*truncateFirst=*/false)) {
            mc2::fmt::print(f, "{{\"island\":\"prop\",\"count\":{:d},\"objectDepthBias\":{:.6f},\"instances\":[", mc2::fmt::as_u32(out->count), out->objectDepthBias);
            const GpuStaticPropInstance* insts =
                reinterpret_cast<const GpuStaticPropInstance*>(out->instances112);
            for (uint32_t i = 0; i < out->count; ++i) {
                const GpuStaticPropInstance& gi = insts[i];
                const StaticPropOrigin go = staticPropOriginRender(gi.modelMatrix);
                mc2::fmt::print(f, "{}{{\"typeID\":{:d},\"posX\":{:.4f},\"posY\":{:.4f},\"posZ\":{:.4f},\"flags\":{:d}}}",
                    mc2::fmt::safe(i ? "," : ""), mc2::fmt::as_u32(gi.typeID),
                    go.x, go.y, go.z,
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
    if (GpuStaticPropBatcher::instance().albedoKey(0) == 0) return 0;   // alpha-OFF array not built
    if (s_typeAlbedoLayer.empty() ||
        s_typeAlbedoLayer.size() != s_types.size()) return 0;

    out->arrayKeyOff     = GpuStaticPropBatcher::instance().albedoKey(0);
    const auto& authored = GpuStaticPropBatcher::instance().materialSources();
    out->normal = &authored.normal; out->orm = &authored.orm; out->textures = authored.textures.data(); out->textureCount = static_cast<uint32_t>(authored.textures.size());
    out->materials = authored.table.data();
    out->materialCount = static_cast<uint32_t>(authored.table.size());
    out->packetMaterialIds = authored.packetIds.size() == s_packetTexArrayLayer.size()
        ? authored.packetIds.data() : nullptr;
    // VK-P11 STATIC-PROP MATERIAL alpha-ON: expose the alpha-ON group array (0 if not
    // built). typeAlbedoLayer[] already carries the group-relative layer for class-1
    // types, so the island routes them into this array with the same layer table.
    out->arrayKeyOn      = GpuStaticPropBatcher::instance().albedoKey(1);
    {
        const auto& batcher = GpuStaticPropBatcher::instance();
        // PROP-ARRAY-BUCKETS: each group is albedoOffCount / albedoOnCount size buckets.
        const auto& off = batcher.albedoArrays(0);
        const auto& on = batcher.albedoArrays(1);
        out->albedoOff = off.empty() || off[0].levels.empty() ? nullptr : off.data();
        out->albedoOffCount = out->albedoOff ? static_cast<uint32_t>(off.size()) : 0u;
        out->albedoOn  = on.empty() || on[0].levels.empty() ? nullptr : on.data();
        out->albedoOnCount = out->albedoOn ? static_cast<uint32_t>(on.size()) : 0u;
    }
    // BC7-bucketed: per-bucket sibling arrays with bucket-relative layers. The
    // single-array (group-relative) import + layer table below do not describe that
    // layout, so signal the island to fail soft (keep teal) rather than mis-sample.
    out->bucketed        = 0u;
    out->typeAlbedoLayer = s_typeAlbedoLayer.data();
    out->typeAlphaClass  = s_typeAlphaClass.data();
    out->typeCount       = static_cast<uint32_t>(s_typeAlbedoLayer.size());
    // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: per-packet layer (see header comment).
    // s_packetTexArrayLayer is populated in finalizeGeometry() from the SAME
    // per-packet, group-relative layerForPacket[] GL's coalesce PerDrawEntry table
    // uses — it is the correct source, unlike the per-type "first-valid-wins" table.
    out->packetAlbedoLayer = s_packetTexArrayLayer.empty() ? nullptr : s_packetTexArrayLayer.data();
    out->packetCount       = static_cast<uint32_t>(s_packetTexArrayLayer.size());
    // FENCE-UVSCALE-1: same per-packet indexing as packetAlbedoLayer, populated
    // in finalizeGeometry() from uvScaleXByPacket/uvScaleYByPacket (the same
    // source PerDrawEntry.uvScaleX/Y draws from on the GL side). nullptr if the
    // sidecar size mismatches packetAlbedoLayer's -- island falls back to 1.0.
    out->packetUvScaleX = (s_packetUvScaleX.size() == s_packetTexArrayLayer.size())
                              ? s_packetUvScaleX.data() : nullptr;
    out->packetUvScaleY = (s_packetUvScaleY.size() == s_packetTexArrayLayer.size())
                              ? s_packetUvScaleY.data() : nullptr;

    out->typeHotColors = (s_typeHotColors.size() == s_types.size() * 12) ? s_typeHotColors.data() : nullptr;

    return 1;
}

// VK-PROP-AMBIENT-IBL-1: real ObjectLights ambient + IBL-SH feed. GL-owner accessors
// (this runs in GL context, mirrors mc2_mech_island_get_frame_feed's setup at
// gos_mech_batcher.cpp:2910-2915).
// Light contract (prop_light_contract.h): camera S/A/F and the sun direction as the row carried it,
// published once per frame by GameCamera::renderTerrainSceneState.
static struct { float S[3], A[3], D[3]; float F; bool published; } g_propContract{};
void gos_SetPropLightContract(const float* S, const float* A, float F, const float* D) {
    std::copy_n(S, 3, g_propContract.S); std::copy_n(A, 3, g_propContract.A);
    std::copy_n(D, 3, g_propContract.D); g_propContract.F = F; g_propContract.published = true;
}

extern "C" int mc2_color_prop_island_get_light_feed(Mc2ColorPropLightFeed* out) {
    if (!out) return 0;
    // VK-PROP-LIGHTFEED-NATIVE-1: native CPU feed (mech item-9 pattern, mirrors
    // mc2_mech_island_get_frame_feed's lightDataCpu export) — the byte source of
    // the GL SSBO upload. VK-PERF-2 cleanup deleted the GL SSBO name/size export
    // with the audit-gated readback it existed for (clean audit banked).
    out->lights = mc2::vkLights::captureFeed();
    // The L1 record owns day/night coefficients; night strength is already resolved there.
    const auto& environment = visualTuning_environment();
    std::copy_n(g_propContract.S, 3, out->sunColor);
    out->sunColor[3] = g_propContract.published ? 1.0f : 0.0f;
    std::copy_n(g_propContract.A, 3, out->ambientColor);
    out->ambientColor[3] = g_propContract.F;
    std::copy_n(g_propContract.D, 3, out->specDir);
    out->specDir[3] = 0.0f;
    // The SH coefficients themselves ride the EnvironmentUBO (mission_environment_gpu.cpp); only the class strength is fed here.
    // Retain the existing option and profile strength, applied once.
    out->iblShStrength = iblShEnabled() ? g_iblShStrength * environment.unitAmbientScale : 0.0f;
    bool isNight = false;
    float nightFactor = 0.0f;
    gos_GetNightState(&isNight, &nightFactor);
    out->isNight      = isNight ? 1 : 0;
    out->nightFactor  = nightFactor;
    out->ambientV1Strength = 0.0f; // ABI slot retained; the duplicate fill is retired.
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
