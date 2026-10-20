// Assemble CPU albedo arrays and per-packet layers for native prop consumers.
// and the per-packet layer tables. The orchestrator is in gos_static_prop_batcher_finalize.cpp.
#include "gos_static_prop_batcher_internal.h"
#include "../../mclib/texture_assets.h"
#include "gos_static_prop_finalize_frame.h"
#include "mc2fmt.h"

#include <cstring>

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

                const auto texels = gos_GetTextureTexels(gosHandle);
                auto rgba = texels.bytes;
                if (rgba && (texels.vkFormat == mc2tex::kVkFormatBc7Unorm || texels.vkFormat == mc2tex::kVkFormatBc7Srgb))
                    rgba = std::make_shared<const std::vector<uint8_t>>(mc2tex::decodeBc7(*rgba, texels.width, texels.height));
                const uint32_t sourceHandle = static_cast<uint32_t>(gosHandle);

                auto it = sourceToLayer.find(sourceHandle);
                if (it != sourceToLayer.end()) {
                    layerForPacket[globalPktIdx] = it->second;
                    // VK-P11 MATERIAL FIX: FIRST-VALID-LAYER-WINS (was pIdx==0 only).
                    // A multi-packet type whose packet 0 hit the no-dims skip left
                    // layerForType==-1 (the "no_primary" teal bucket) even though a
                    // later packet had a valid layer. Record the first VALID (>=0)
                    // layer so the VK material feed textures the type.
                    if (layerForType[typeID] < 0) layerForType[typeID] = it->second;
                    packetsInGroup.emplace_back(globalPktIdx, it->second);
                    continue;
                }

            int W = texels.status == GosTexels::Status::Ready && rgba &&
                rgba->size() == size_t(texels.width) * texels.height * 4 ? static_cast<int>(texels.width) : 0;
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
                if (type.isOverride && !uniques.empty()) {
                    layerForPacket[globalPktIdx] = 0;
                    if (layerForType[typeID] < 0) layerForType[typeID] = 0; // VK-P11: first-valid wins
                    packetsInGroup.emplace_back(globalPktIdx, 0);
                    if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">()) {
                        mc2::fmt::print_err("[SEAMPROBE] OVERRIDE_ROUTE type={:d} pkt={:d} -> layer=0 (default) group={}\n", mc2::fmt::as_u32(typeID), mc2::fmt::as_u32(pIdx), mc2::fmt::safe(group == 0u ? "off" : "on"));
                        std::fflush(stderr);
                    }
                    continue;
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
            uniques.push_back({sourceHandle, W, H, nodeIdx, std::move(rgba)});
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

        // ====================================================================
        // COMPRESSION-BC7-STATICPROP-2 — bucketed build (gate ON only).
        // Partition this group's uniques by (w,h). Each bucket is uniform-dim
        // by construction → eligible for an independent BC7 or RGBA8 array.
        // layerForPacket becomes BUCKET-RELATIVE (layer within the bucket
        // array, same semantics the shader already consumes). uvScale = 1.0.
        // The gate-OFF group-build below is left UNTOUCHED and never runs here.
        // ====================================================================

        // 5.10.d — find max dimensions across the group's uniques.
        // The texture array is allocated at maxW × maxH; smaller
        // textures get blitted into upper-left sub-region of their
        // layer; per-type uvScale maps [0,1] UV → sub-region.
        int maxW = 0, maxH = 0;
        for (const auto& u : uniques) {
            if (u.w > maxW) maxW = u.w;
            if (u.h > maxH) maxH = u.h;
        }
        if (maxW <= 0 || maxH <= 0) {
            // Defensive — shouldn't happen because skip-path filtered W/H<=0.
            MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=alloc_failed "
                         "(group=%s maxDims=%dx%d after skip-no-dims pass)\n",
                         group == 0u ? "off" : "on", maxW, maxH);
            coalesceRollbackTexBuild(newlyPinnedThisBuild); albedoKey_ = {};
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return true;
        }

        if (s_materialKtxEnabled && s_staticPropBc7Enabled && mc2::textures::ready()) {
            std::vector<RenderCore::KtxImage> bc7Imgs(uniques.size());
            bool bc7Ok      = true;
            const char* failReason = nullptr;
            uint32_t bc7VkFormat = 0;   // shared format across the array
            int      bc7MipCount = 0;   // shared mip count across the array

            // Uniform-dim check first (cheap, no file IO).
            for (const auto& u : uniques) {
                if (u.w != maxW || u.h != maxH) { bc7Ok = false; failReason = "mixed_dims"; break; }
            }

            for (size_t k = 0; bc7Ok && k < uniques.size(); ++k) {
                const auto& u = uniques[k];
                if (u.nodeIdx == 0xFFFFFFFFu) { bc7Ok = false; failReason = "load_fail"; break; }
                const char* srcName = mc2::textures::name(u.nodeIdx);
                if (!srcName || !*srcName) { bc7Ok = false; failReason = "load_fail"; break; }
                const std::string ktxPath = deriveTieredKtxPath(srcName, texTier());
                if (!RenderCore::ktxLoadRgba8(ktxPath.c_str(), bc7Imgs[k])) {
                    bc7Ok = false; failReason = "load_fail"; break;
                }
                const RenderCore::KtxImage& img = bc7Imgs[k];
                if (!img.isCompressed ||
                    (img.vkFormat != 145u && img.vkFormat != 146u)) {
                    bc7Ok = false; failReason = "not_all_bc7"; break;
                }
                if (img.width != u.w || img.height != u.h) {
                    bc7Ok = false; failReason = "dim_mismatch"; break;
                }
                if (k == 0) { bc7VkFormat = img.vkFormat; bc7MipCount = img.mipCount; }
                // Require a consistent mip count so glTexStorage3D's level count
                // is valid for every layer.
                if (img.mipCount != bc7MipCount) { bc7Ok = false; failReason = "dim_mismatch"; break; }
            }

            if (bc7Ok) {
                const uint32_t sourceFormat = mc2tex::kVkFormatBc7Unorm;
                const int levels = (bc7MipCount > 0) ? bc7MipCount : 1;

                mc2tex::TextureArrayCpu& cpuArray = albedoArrays_[group];
                cpuArray = {};
                cpuArray.width = static_cast<uint32_t>(maxW);
                cpuArray.height = static_cast<uint32_t>(maxH);
                cpuArray.layers = static_cast<uint32_t>(uniques.size());
                cpuArray.vkFormat = mc2tex::kVkFormatBc7Unorm;
                cpuArray.levels.assign(static_cast<size_t>(levels), {});
                size_t bc7TotalBytes = 0;
                for (size_t k = 0; k < uniques.size(); ++k) {
                    const RenderCore::KtxImage& img = bc7Imgs[k];
                    for (int lvl = 0; lvl < levels; ++lvl) {
                        const int lw = (maxW >> lvl) ? (maxW >> lvl) : 1;
                        const int lh = (maxH >> lvl) ? (maxH >> lvl) : 1;
                        const int imageSize =
                            static_cast<int>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);
                        const uint8_t* blocks = img.pixels.data() + img.mipByteOffsets[static_cast<size_t>(lvl)];
                        cpuArray.levels[static_cast<size_t>(lvl)].insert(cpuArray.levels[static_cast<size_t>(lvl)].end(), blocks, blocks + imageSize);
                        bc7TotalBytes += static_cast<size_t>(imageSize);
                    }
                }

                for (const auto& [globalPktIdx, grpLayer] : packetsInGroup) {
                    if (grpLayer < 0) continue;
                    uvScaleXByPacket[globalPktIdx] = 1.0f;
                    uvScaleYByPacket[globalPktIdx] = 1.0f;
                }

                mc2::fmt::print_err("[STATICPROP_BC7] group={:d} layers={:d} dims={:d}x{:d} levels={:d} internalformat=0x{:x} bytes={:d}\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::as_size(uniques.size()), mc2::fmt::as_i32(maxW), mc2::fmt::as_i32(maxH), mc2::fmt::as_i32(levels), mc2::fmt::as_u32(sourceFormat), mc2::fmt::as_size(bc7TotalBytes));
                mc2::fmt::print_err("[STATICPROP_BC7] summary group={:d} built BC7 array (srcVkFormat={:d}, {:d} layers)\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::as_u32(bc7VkFormat), mc2::fmt::as_size(uniques.size()));
                std::fflush(stderr);

                COALESCE_TRACE("group_built_bc7 group=%s uniques=%zu maxDims=%dx%d levels=%d",
                               group == 0u ? "off" : "on",
                               uniques.size(), maxW, maxH, levels);

                albedoKey_[group] = ++s_albedoKeySerial;
                s_coalescePinnedNodes.insert(s_coalescePinnedNodes.end(),
                                              newlyPinnedThisBuild.begin(),
                                              newlyPinnedThisBuild.end());
                continue;
            } else {
                mc2::fmt::print_err("[STATICPROP_BC7] fallback group={:d} reason={}\n", mc2::fmt::as_u32(static_cast<unsigned>(group)), mc2::fmt::safe(failReason ? failReason : "unknown"));
                std::fflush(stderr);
            }
        }

        mc2tex::TextureArrayCpu& cpuArray = albedoArrays_[group];
        cpuArray = {};
        cpuArray.width = static_cast<uint32_t>(maxW);
        cpuArray.height = static_cast<uint32_t>(maxH);
        cpuArray.layers = static_cast<uint32_t>(uniques.size());
        cpuArray.vkFormat = mc2tex::kVkFormatRgba8Unorm;
        const size_t layerBytes0 = static_cast<size_t>(maxW) * static_cast<size_t>(maxH) * 4u;
        std::vector<std::vector<uint8_t>> layerTexels(uniques.size(), std::vector<uint8_t>(layerBytes0, 0));
        auto blitRows = [](std::vector<uint8_t>& layer, uint32_t layerW, const uint8_t* src, uint32_t w, uint32_t h) {
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(layer.data() + static_cast<size_t>(y) * layerW * 4u, src + static_cast<size_t>(y) * w * 4u, static_cast<size_t>(w) * 4u);
        };
        std::vector<uint8_t> pixelBuf;
        std::vector<std::pair<size_t, RenderCore::KtxImage>> ktxMipLayers;
        for (size_t k = 0; k < uniques.size(); ++k) {
            const auto& u = uniques[k];
            bool ktxUsed = false;
            if (s_materialKtxEnabled && u.nodeIdx != 0xFFFFFFFFu && mc2::textures::ready()) {
                const char* srcName = mc2::textures::name(u.nodeIdx);
                if (srcName && *srcName) {
                    // Derive tiered .ktx2 path: data/tgl/<tier>/foo.ktx2
                    const std::string ktxPath = deriveTieredKtxPath(srcName, texTier());
                    RenderCore::KtxImage ktxImg;
                    const bool ktxOk = RenderCore::ktxLoadRgba8(ktxPath.c_str(), ktxImg);
                    // A BC7 sidecar in a group that took this RGBA8 path (e.g. mixed dims declined the BC7 array) holds blocks, not
                    // texels: rejected here, so the texture's own pixels are used.
                    const bool dimOk = ktxOk && !ktxImg.isCompressed &&
                                       ktxImg.width == u.w && ktxImg.height == u.h &&
                                       ktxImg.pixels.size() >= static_cast<size_t>(u.w) * static_cast<size_t>(u.h) * 4u;
                    mc2::fmt::print_err("[KTX_SIDECAR] node={:d} srcName={} path={} load={:d} ktxDims={:d}x{:d} glDims={:d}x{:d} match={:d}\n",
                        mc2::fmt::as_u32(static_cast<unsigned long>(u.nodeIdx)), mc2::fmt::safe(srcName), ktxPath.c_str(),
                        mc2::fmt::as_i32(static_cast<int>(ktxOk)),
                        mc2::fmt::as_i32(ktxOk ? ktxImg.width : 0), mc2::fmt::as_i32(ktxOk ? ktxImg.height : 0),
                        mc2::fmt::as_i32(u.w), mc2::fmt::as_i32(u.h), mc2::fmt::as_i32(static_cast<int>(dimOk)));
                    std::fflush(stderr);
                    if (dimOk) {
                        blitRows(layerTexels[k], cpuArray.width, ktxImg.pixels.data(), static_cast<uint32_t>(u.w), static_cast<uint32_t>(u.h));
                        ktxUsed = true;
                        if (ktxImg.mipCount > 1) ktxMipLayers.emplace_back(k, std::move(ktxImg));
                        COALESCE_TRACE("ktx_sidecar_hit type=? node=%lu path=%s",
                                       static_cast<unsigned long>(u.nodeIdx), ktxPath.c_str());
                    }
                }
            }
            if (!ktxUsed) {
                pixelBuf.assign(u.rgba->begin(), u.rgba->end());
                blitRows(layerTexels[k], cpuArray.width, pixelBuf.data(), static_cast<uint32_t>(u.w), static_cast<uint32_t>(u.h));
            }
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
        int bakedLevels = 0;
        for (uint32_t lvl = 0; lvl < levelCount; ++lvl) {
            const uint32_t lw = mc2tex::mipExtent(cpuArray.width, lvl), lh = mc2tex::mipExtent(cpuArray.height, lvl);
            const size_t layerBytes = static_cast<size_t>(lw) * lh * 4u;
            std::vector<uint8_t>& level = cpuArray.levels[lvl];
            level.resize(layerBytes * layerTexels.size());
            for (size_t k = 0; k < layerTexels.size(); ++k) std::memcpy(level.data() + k * layerBytes, layerTexels[k].data(), layerBytes);
            // A pre-baked level replaces its layer at that level only: the chain below it keeps deriving from level 0, as
            // glGenerateMipmap did before the baked levels were uploaded over it.
            if (lvl > 0)
                for (const auto& [layerK, img] : ktxMipLayers) {
                    if (static_cast<int>(lvl) >= img.mipCount || lvl >= img.mipByteOffsets.size()) continue;
                    const uint32_t kw = mc2tex::mipExtent(static_cast<uint32_t>(img.width), lvl);
                    const uint32_t kh = mc2tex::mipExtent(static_cast<uint32_t>(img.height), lvl);
                    const size_t off = img.mipByteOffsets[lvl];
                    if (kw > lw || kh > lh || off + static_cast<size_t>(kw) * kh * 4u > img.pixels.size()) {
                        mc2::fmt::print_err("[KTX2_MIP] layer {:d} level {:d} rejected: {:d}x{:d} does not fit the {:d}x{:d} array level or its bytes\n",
                            mc2::fmt::as_size(layerK), mc2::fmt::as_u32(lvl), mc2::fmt::as_u32(kw), mc2::fmt::as_u32(kh), mc2::fmt::as_u32(lw), mc2::fmt::as_u32(lh));
                        continue;
                    }
                    std::vector<uint8_t> patched = layerTexels[layerK];
                    blitRows(patched, lw, img.pixels.data() + off, kw, kh);
                    std::memcpy(level.data() + layerK * layerBytes, patched.data(), layerBytes);
                    ++bakedLevels;
                }
            if (lvl + 1u < levelCount)
                for (auto& layer : layerTexels) layer = mc2tex::boxDownsampleRgba8(layer, lw, lh);
        }
        if (!ktxMipLayers.empty())
            mc2::fmt::print_err("[KTX2_MIP] summary: {:d} KTX layers, {:d} pre-baked mips used\n", mc2::fmt::as_size(ktxMipLayers.size()), mc2::fmt::as_i32(bakedLevels));

        albedoKey_[group] = ++s_albedoKeySerial;
        // 2026-05-11 per-packet rework: uvScale is per-PACKET, derived from
        // each packet's unique texture dimensions vs the array max dims.
        const float invMaxW = 1.0f / static_cast<float>(maxW);
        const float invMaxH = 1.0f / static_cast<float>(maxH);
        for (const auto& [globalPktIdx, layer] : packetsInGroup) {
            if (layer < 0) continue;
            const auto& u = uniques[static_cast<size_t>(layer)];
            uvScaleXByPacket[globalPktIdx] = static_cast<float>(u.w) * invMaxW;
            uvScaleYByPacket[globalPktIdx] = static_cast<float>(u.h) * invMaxH;
        }

        COALESCE_TRACE("group_built group=%s uniques=%zu maxDims=%dx%d",
                       group == 0u ? "off" : "on",
                       uniques.size(), maxW, maxH);

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
