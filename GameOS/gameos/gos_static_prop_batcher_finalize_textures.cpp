// Assemble CPU albedo arrays and per-packet layers for native prop consumers.
// and the per-packet layer tables. The orchestrator is in gos_static_prop_batcher_finalize.cpp.
#include <algorithm>
#include "load_scope.h"
#include "gos_static_prop_batcher_internal.h"
#include "../../mclib/texture_assets.h"
#include "gos_static_prop_finalize_frame.h"
#include "mc2fmt.h"
#include "vegetation_cutout.h"
#include "prop_array_buckets.h"
#include "diagnostic_trace.h"

#include <cstring>

// Tree foliage tone (owner 11-23: corrected mips read too light/lime; 0 = authored colour, 0.85 = old far-tree luminance, 0.43 = midpoint; owner 11-23 picked option C = 0.64).
static constexpr float kTreeFoliageTone = 0.64f;

// Build the per-group texture arrays and publish the per-packet and per-type layer tables.
bool GpuStaticPropBatcher::buildTextureArrays(StaticPropFinalizeFrame& f) {
    auto& typeCount = f.typeCount; auto& layerForType = f.layerForType; auto& layerForPacket = f.layerForPacket; auto& uvScaleXByPacket = f.uvScaleXByPacket;
    auto& uvScaleYByPacket = f.uvScaleYByPacket;
    layerForType.assign(typeCount, -1);
    for (auto& a : albedoArrays_) a = {};
    albedoKey_ = {};
    // 2026-05-11 per-packet rework: layerForPacket[globalPacketIdx] gives the
    // group-relative texture-array layer for that packet. The previous
    // per-type layerForType is kept for backward-compat but now records the
    // FIRST packet's layer (used only by zero-packet guards in stale call
    // sites). Multi-packet types had their detail collapsed under per-type
    // layering — see memory/substrate_coalesce_armed_multi_packet_limitation.md.
    layerForPacket.assign(s_packets.size(), -1);
    uvScaleXByPacket.assign(s_packets.size(), 1.0f);
    uvScaleYByPacket.assign(s_packets.size(), 1.0f);

    struct UniqueTex {
        uint32_t sourceHandle;
        int  w;
        int  h;
        DWORD  nodeIdx;   // legacy texture manager node index for KTX2 sidecar lookup
        std::shared_ptr<const std::vector<uint8_t>> rgba;
        bool tree = false;
    };

    static uint32_t s_albedoKeySerial = 0;
    for (uint8_t group = 0; group <= 1; ++group) {
        std::vector<DWORD> newlyPinnedThisBuild;

        std::vector<UniqueTex> uniques;             // unique textures in this group
        std::unordered_map<uint32_t, int32_t> sourceToLayer;
        // Track packets that landed in this group, for the second-pass uvScale
        // assignment. Each entry: { globalPacketIdx, layer }.
        std::vector<std::pair<uint32_t, int32_t>> packetsInGroup;

        // 5.10.a — walk types in sorted order belonging to this group; for
        // each type, walk ALL its packets. Each packet gets its own layer
        // (deduped by sourceHandle) — this is the per-packet rework's core change.
        for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
            MC2_LOAD_SCOPE("props.finalizeTextureType");
            const uint32_t typeID = s_sortedTypeOrder[i];
            auto& type = s_types[typeID];
            if (type.alphaClass != group) continue;

            // Zero-packet guard mirrors batcher_getTypeDrawInfo pattern;
            // leave layer = -1 (already initialized) and continue.
            if (type.packetCount == 0u) {
                continue;
            }

            for (uint32_t pIdx = 0; pIdx < type.packetCount; ++pIdx) {
                const uint32_t globalPktIdx = type.firstPacket + pIdx;
                const auto& pkt = s_packets[globalPktIdx];
                const TG_TypeShape* src = type.source;
                if (!src || !src->listOfTextures ||
                    pkt.textureSlot >= src->numTextures) {
                    MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed "
                                 "reason=malformed_type type=%u pkt=%u group=%s\n",
                                 typeID, pIdx, group == 0u ? "off" : "on");
                    coalesceRollbackTexBuild(newlyPinnedThisBuild); albedoKey_ = {};
                    s_coalesceLayoutReady = false;
                    s_coalesceEnabled     = false;
                    s_coalesceArmed       = false;
                    return true;
                }

                const DWORD nodeIdx =
                    src->listOfTextures[pkt.textureSlot].mcTextureNodeIndex;
                if (mc2::textures::ready() && nodeIdx != 0xFFFFFFFFu) {
                    mc2::textures::pin(nodeIdx);
                    newlyPinnedThisBuild.push_back(nodeIdx);
                }

                DWORD gosHandle = (mc2::textures::ready() && nodeIdx != 0xFFFFFFFFu)
                    ? mc2::textures::sourceId(nodeIdx)
                    : src->listOfTextures[pkt.textureSlot].gosTextureHandle;
                // Snapshot first-packet handle for the runtime eviction
                // detect (Step 11.4 still per-type). Multi-packet types use
                // their first packet as the canary — sufficient for the
                // mid-mission cache-evict guard (multi-textured types
                // typically share underlying texture lifetime).
                if (pIdx == 0) {
                    type.lastSeenGosHandle = gosHandle;
                }

                const uint32_t sourceHandle = static_cast<uint32_t>(gosHandle);

                auto it = sourceToLayer.find(sourceHandle);
                if (it != sourceToLayer.end()) {
                    const auto& [srcKey, srcLayer] = *it;
                    uniques[srcLayer].tree |= type.treeCanopy[1] > 0.0f;
                    layerForPacket[globalPktIdx] = srcLayer;
                    // VK-P11 MATERIAL FIX: FIRST-VALID-LAYER-WINS (was pIdx==0 only).
                    // A multi-packet type whose packet 0 hit the no-dims skip left
                    // layerForType==-1 (the "no_primary" teal bucket) even though a
                    // later packet had a valid layer. Record the first VALID (>=0)
                    // layer so the VK material feed textures the type.
                    if (layerForType[typeID] < 0) layerForType[typeID] = srcLayer;
                    packetsInGroup.emplace_back(globalPktIdx, srcLayer);
                    continue;
                }

                const auto texels = gos_GetTextureTexels(gosHandle);
                auto rgba = texels.bytes;
                if (rgba && (texels.vkFormat == mc2tex::kVkFormatBc7Unorm || texels.vkFormat == mc2tex::kVkFormatBc7Srgb))
                    rgba = std::make_shared<const std::vector<uint8_t>>(mc2tex::decodeBc7(*rgba, texels.width, texels.height));

            int W = texels.status == GosTexels::Status::Ready && rgba &&
                rgba->size() == static_cast<size_t>(texels.width) * texels.height * 4 ? static_cast<int>(texels.width) : 0;
            int H = W > 0 ? static_cast<int>(texels.height) : 0;
            // If the retained source has no pixels, the texture is genuinely
            // unavailable at finalize-time (no MC node, never cached,
            // and no decoded source). Skip this packet via layer=-1 sentinel
            // rather than disarming the whole coalesce build — graceful
            // degradation: other valid packets still arm.
            if (W <= 0 || H <= 0) {
                COALESCE_TRACE("skip_packet type=%u pkt=%u group=%s reason=no_dims "
                               "nodeIdx=%lu gosHandle=%lu sourceHandle=%u",
                               typeID, pIdx, group == 0u ? "off" : "on",
                               static_cast<unsigned long>(nodeIdx),
                               static_cast<unsigned long>(gosHandle), sourceHandle);
                // [SEAMPROBE] stage 10: layer-assignment census. When the
                // override prop/tree packets land here, they get layer=-1 and
                // are dropped from the multidraw (the bdactor.cpp:262 skip
                // rule) -> override never rasterizes. Gated, capped.
                {
                    static int s_seamLayerSkip = 0;
                    if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">() && s_seamLayerSkip < 200) {
                        ++s_seamLayerSkip;
                        mc2::fmt::print_err("[SEAMPROBE] sample=first200 layer=-1 SKIP type={:d} pkt={:d} nodeIdx={:d} gosHandle={:d} sourceHandle={:d} W={:d} H={:d} override={:d} uniques={:d}\n", mc2::fmt::as_u32(typeID), mc2::fmt::as_u32(pIdx), mc2::fmt::as_u32(static_cast<unsigned long>(nodeIdx)), mc2::fmt::as_u32(static_cast<unsigned long>(gosHandle)), mc2::fmt::as_u32(sourceHandle), mc2::fmt::as_i32(static_cast<int>(W)), mc2::fmt::as_i32(static_cast<int>(H)), mc2::fmt::as_i32(static_cast<int>(type.isOverride)), mc2::fmt::as_size(uniques.size()));
                        std::fflush(stderr);
                    }
                }
                continue;
            }

            if (s_coalesceForceDisarm == CoalesceForceDisarm::SizeMismatch &&
                group == 0u && uniques.size() == 1u) {
                MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=size_mismatch "
                             "group=off expected=%dx%d got=%dx%d (forced)\n",
                             uniques[0].w, uniques[0].h,
                             uniques[0].w + 1, uniques[0].h + 1);
                coalesceRollbackTexBuild(newlyPinnedThisBuild); albedoKey_ = {};
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return true;
            }

            // v3.8 mixed-size: append unique with its actual W,H.
            // Array allocation uses max(W,H) across all uniques.
            const int32_t layer = static_cast<int32_t>(uniques.size());
            uniques.push_back({sourceHandle, W, H, nodeIdx, std::move(rgba), type.treeCanopy[1] > 0.0f});
            sourceToLayer[sourceHandle] = layer;
            layerForPacket[globalPktIdx] = layer;
            if (layerForType[typeID] < 0) layerForType[typeID] = layer; // VK-P11: first-valid wins
            packetsInGroup.emplace_back(globalPktIdx, layer);
            } // end per-packet loop
        }

        if (uniques.empty()) {
            // Empty group — Step 11.7.g/h skip the bind+draw pair when
            // count == 0 (out-CRIT-5). No array allocated; no pins to
            // promote (newlyPinnedThisBuild is empty).
            continue;
        }

        // PROP-ARRAY-BUCKETS: one array per size bucket (prop_array_buckets.h), both groups: a 1024 atlas no longer
        // inflates the group's 128/256 layers.
        // Within a bucket the array is the members' max W x H; a smaller member sits in the upper-left of its layer
        // and its per-packet uvScale maps [0,1] UV to that sub-region (as for the whole group before).
        std::vector<mc2::prop_buckets::Dims> memberDims;
        memberDims.reserve(uniques.size());
        for (const auto& u : uniques) memberDims.push_back({u.w, u.h});
        const auto plan = mc2::prop_buckets::planBuckets(memberDims, mc2::prop_buckets::kMaxBuckets);
        if (mc2_diag::tagRequested("PROP_ARRAY"))   // MC2_DIAG_TAGS=PROP_ARRAY: each member's native size and its bucket array
            for (size_t k = 0; k < uniques.size(); ++k) {
                const char* name = uniques[k].nodeIdx != 0xFFFFFFFFu && mc2::textures::ready() ? mc2::textures::name(uniques[k].nodeIdx) : nullptr;
                mc2::fmt::print_err("[PROP_ARRAY v1] group={} bucket={:d} layer={:d} w={:d} h={:d} array={:d}x{:d} tex={}\n",
                                    group == 0u ? "off" : "on", mc2::fmt::as_u32(plan.bucket[k]), mc2::fmt::as_i32(plan.layer[k]),
                                    mc2::fmt::as_i32(uniques[k].w), mc2::fmt::as_i32(uniques[k].h),
                                    mc2::fmt::as_i32(plan.bucketDims[plan.bucket[k]].w), mc2::fmt::as_i32(plan.bucketDims[plan.bucket[k]].h),
                                    name ? name : "(unnamed)");
            }
        for (const auto& d : plan.bucketDims) {
            if (d.w <= 0 || d.h <= 0) {
                // Defensive — shouldn't happen because skip-path filtered W/H<=0.
                MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=alloc_failed "
                             "(group=%s maxDims=%dx%d after skip-no-dims pass)\n",
                             group == 0u ? "off" : "on", d.w, d.h);
                coalesceRollbackTexBuild(newlyPinnedThisBuild); albedoKey_ = {};
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return true;
            }
        }

        auto& bucketArrays = albedoArrays_[group];
        bucketArrays.assign(plan.bucketDims.size(), {});
        for (uint32_t b = 0; b < plan.bucketDims.size(); ++b) {
        std::vector<size_t> members;
        for (size_t k = 0; k < uniques.size(); ++k) if (plan.bucket[k] == b) members.push_back(k);
        const int maxW = plan.bucketDims[b].w, maxH = plan.bucketDims[b].h;
        mc2tex::TextureArrayCpu& cpuArray = bucketArrays[b];
        cpuArray.width = static_cast<uint32_t>(maxW);
        cpuArray.height = static_cast<uint32_t>(maxH);
        cpuArray.layers = static_cast<uint32_t>(members.size());
        cpuArray.vkFormat = mc2tex::kVkFormatRgba8Unorm;
        const size_t layerBytes0 = static_cast<size_t>(maxW) * static_cast<size_t>(maxH) * 4u;
        std::vector<std::vector<uint8_t>> layerTexels(members.size(), std::vector<uint8_t>(layerBytes0, 0));
        auto blitRows = [](std::vector<uint8_t>& layer, uint32_t layerW, const uint8_t* src, uint32_t w, uint32_t h) {
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(layer.data() + static_cast<size_t>(y) * layerW * 4u, src + static_cast<size_t>(y) * w * 4u, static_cast<size_t>(w) * 4u);
        };
        for (size_t k = 0; k < members.size(); ++k) {
            MC2_LOAD_SCOPE("props.finalizeTextureLayer");
            const auto& u = uniques[members[k]];
            blitRows(layerTexels[k], cpuArray.width, u.rgba->data(), static_cast<uint32_t>(u.w), static_cast<uint32_t>(u.h));
            if (u.tree) mc2::vegetation::toneFoliageRgb(layerTexels[k], kTreeFoliageTone);
        }
        // PROP-ALPHA-MIP-1: a solid cutout layer of the alpha-ON group (canopy, alpha-mixed wall) keeps its coverage at the
        // shader's per-mip cut instead of box-filtering down to mid alpha, which the rising cut sends to the thin blended fringe
        // (far trees read see-through). Thin lattice (isSolidCutout false) keeps the plain box mips so fences stay see-through.
        // BUILDING-GLASS-1: enclosed rectangular windows of a solid cutout wall become opaque dark glass (both groups: a stock
        // texture whose alpha flag is set late sits in the alpha-OFF array yet is still cut per packet).
        for (size_t k = 0; k < layerTexels.size(); ++k) {
            const auto& u = uniques[members[k]];
            if (u.tree) continue;
            bool anyClear = false;
            for (size_t i = 3; i < layerTexels[k].size() && !anyClear; i += 4) anyClear = layerTexels[k][i] < 128;
            if (!anyClear || !mc2::vegetation::isSolidCutout(layerTexels[k], cpuArray.width, cpuArray.height,
                                                             static_cast<uint32_t>(std::min(u.w, u.h)))) continue;
            const size_t n = mc2::vegetation::fillEnclosedGlassHoles(layerTexels[k], cpuArray.width, cpuArray.height,
                                                                     static_cast<uint32_t>(u.w), static_cast<uint32_t>(u.h));
            if (n && mc2_diag::tagRequested("PROP_GLASS")) mc2::fmt::print_err("[PROP_GLASS v1] group={} layer={:d} texels={:d}\n", group == 0u ? "off" : "on", mc2::fmt::as_i32(plan.layer[members[k]]), mc2::fmt::as_u64(n));
        }
        std::vector<float> keepCoverage(members.size(), -1.0f);
        if (group == 1u)
            for (size_t k = 0; k < layerTexels.size(); ++k)
                // Leaf atlases have separate tiny leaves, so erosion mistakes them for wire.
                if (uniques[members[k]].tree || mc2::vegetation::isSolidCutout(layerTexels[k], cpuArray.width, cpuArray.height,
                                                   static_cast<uint32_t>(std::min(uniques[members[k]].w, uniques[members[k]].h)))) {
                    keepCoverage[k] = mc2::vegetation::cutoutCoverage(layerTexels[k]);
                    mc2::vegetation::dilateCutoutRgb(layerTexels[k], cpuArray.width, cpuArray.height);
                }

        int maxLevel = 0;
        {
            int w = maxW, h = maxH;
            while (w > 1 || h > 1) {
                ++maxLevel;
                if (w > 1) w >>= 1;
                if (h > 1) h >>= 1;
            }
        }
        const uint32_t levelCount = static_cast<uint32_t>(maxLevel) + 1u;
        cpuArray.levels.assign(levelCount, {});
        for (uint32_t lvl = 0; lvl < levelCount; ++lvl) {
            MC2_LOAD_SCOPE("props.finalizeTextureMip");
            const uint32_t lw = mc2tex::mipExtent(cpuArray.width, lvl), lh = mc2tex::mipExtent(cpuArray.height, lvl);
            const size_t layerBytes = static_cast<size_t>(lw) * lh * 4u;
            std::vector<uint8_t>& level = cpuArray.levels[lvl];
            level.resize(layerBytes * layerTexels.size());
            for (size_t k = 0; k < layerTexels.size(); ++k) std::memcpy(level.data() + k * layerBytes, layerTexels[k].data(), layerBytes);
            if (lvl + 1u < levelCount)
                for (size_t k = 0; k < layerTexels.size(); ++k) {
                    if (keepCoverage[k] < 0.0f) { layerTexels[k] = mc2tex::boxDownsampleRgba8(layerTexels[k], lw, lh); continue; }
                    layerTexels[k] = mc2::vegetation::downsampleCutoutRgba8(layerTexels[k], lw, lh);
                    // A tree layer is cut at a fixed 0.5 in the colour, depth and shadow passes (color_prop_tier1.frag propCoverage), so its
                    // mips keep coverage at 0.5; other cutouts follow the rising cut.
                    mc2::vegetation::preserveCutoutAlpha(layerTexels[k], keepCoverage[k],
                        uniques[members[k]].tree ? 128L : mc2::vegetation::propCutThreshold(lvl + 1u));
                    mc2::vegetation::dilateCutoutRgb(layerTexels[k], mc2tex::mipExtent(cpuArray.width, lvl + 1u), mc2tex::mipExtent(cpuArray.height, lvl + 1u));
                }
        }
        COALESCE_TRACE("group_built group=%s bucket=%u layers=%zu maxDims=%dx%d",
                       group == 0u ? "off" : "on", b, members.size(), maxW, maxH);
        }
        albedoKey_[group] = ++s_albedoKeySerial;
        // 2026-05-11 per-packet rework: uvScale is per-PACKET, derived from
        // each packet's unique texture dimensions vs its bucket array's max dims.
        // PROP-ARRAY-BUCKETS: the packet's (and the type's) layer becomes (bucket, layer-in-bucket).
        for (const auto& [globalPktIdx, layer] : packetsInGroup) {
            if (layer < 0) continue;
            const size_t k = static_cast<size_t>(layer);
            const auto& u = uniques[k];
            const auto& d = plan.bucketDims[plan.bucket[k]];
            uvScaleXByPacket[globalPktIdx] = static_cast<float>(u.w) / static_cast<float>(d.w);
            uvScaleYByPacket[globalPktIdx] = static_cast<float>(u.h) / static_cast<float>(d.h);
            layerForPacket[globalPktIdx] = mc2::prop_buckets::encodeLayer(plan.bucket[k], plan.layer[k]);
        }
        for (const uint32_t typeID : s_sortedTypeOrder) {
            if (s_types[typeID].alphaClass != group || layerForType[typeID] < 0) continue;
            const size_t k = static_cast<size_t>(layerForType[typeID]);
            layerForType[typeID] = mc2::prop_buckets::encodeLayer(plan.bucket[k], plan.layer[k]);
        }

        COALESCE_TRACE("group_built group=%s uniques=%zu buckets=%zu",
                       group == 0u ? "off" : "on",
                       uniques.size(), plan.bucketDims.size());

        s_coalescePinnedNodes.insert(s_coalescePinnedNodes.end(),
                                      newlyPinnedThisBuild.begin(),
                                      newlyPinnedThisBuild.end());
    }

    // v2: persist per-packet texArrayLayer before layerForPacket goes stale.
    // layerForPacket is fully populated here; the PerDrawEntry loop below reads
    // but does not modify it. -1 entries = packets with unavailable textures.
    s_packetTexArrayLayer.assign(layerForPacket.begin(), layerForPacket.end());
    // FENCE-UVSCALE-1: uvScaleXByPacket/uvScaleYByPacket are fully populated by
    // the per-group loop above (same "packetsInGroup" pass that fills
    // layerForPacket) -- persist alongside the layer sidecar for the VK feed.
    s_packetUvScaleX.assign(uvScaleXByPacket.begin(), uvScaleXByPacket.end());
    s_packetUvScaleY.assign(uvScaleYByPacket.begin(), uvScaleYByPacket.end());

    // VK-P11 STATIC-PROP MATERIAL: persist per-type group-relative albedo layer +
    // alpha-class for the VK color island's copy-once material feed. layerForType[]
    // is fully populated above (group-relative layer into s_texArrayOff/On); the
    // island only textures alpha-OFF (class 0) types from s_texArrayOff, so we keep
    // the raw group-relative layer and the class side-by-side. Compiled always (cheap
    // pair of vector copies); read only when MC2_VK_PROP_MATERIAL is armed.
    s_typeAlbedoLayer.assign(layerForType.begin(), layerForType.end());
    s_typeAlphaClass.assign(s_types.size(), 0);
    for (size_t t = 0; t < s_types.size(); ++t)
        s_typeAlphaClass[t] = static_cast<int32_t>(s_types[t].alphaClass);

    // 2026-05-11 per-packet rework: build s_sortedPacketOrder, s_alphaOff/OnCmdCount,
    // s_perDrawSsbo (one entry per packet in sorted order), and s_cmdToBucketSsbo
    // (one uint typeID per cmd). Fragment shader reads
    // entries[v_drawID + u_drawIDBase] where v_drawID = gl_DrawID = sorted packet
    // slot, u_drawIDBase = 0 for alpha-OFF group, s_alphaOffCmdCount for alpha-ON.
    s_sortedPacketOrder.clear();
    s_alphaOffCmdCount = 0;
    s_alphaOnCmdCount  = 0;
    return false;
}
