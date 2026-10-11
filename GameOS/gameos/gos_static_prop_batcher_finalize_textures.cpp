// GpuStaticPropBatcher: the texture-array phase of finalizeGeometry -- per-group array build, the BC7 / ORM bucket arrays
// and the per-packet layer tables. The orchestrator is in gos_static_prop_batcher_finalize.cpp.
#include "gos_static_prop_batcher_internal.h"
#include "gos_static_prop_finalize_frame.h"
#include "mc2fmt.h"


// Build the per-group texture arrays and publish the per-packet and per-type layer tables.
bool GpuStaticPropBatcher::buildTextureArrays(StaticPropFinalizeFrame& f) {
    auto& typeCount = f.typeCount; auto& layerForType = f.layerForType; auto& layerForPacket = f.layerForPacket; auto& uvScaleXByPacket = f.uvScaleXByPacket;
    auto& uvScaleYByPacket = f.uvScaleYByPacket;
    // Step 5.10 — build per-group GL_TEXTURE_2D_ARRAY (§5.4).
    // layerForType[typeID] = group-relative layer in {s_texArrayOff,
    // s_texArrayOn}; -1 sentinel for zero-packet / no-dim types
    // (Step 5.11 emits a no-op draw entry but keeps array indexing
    // aligned).
    //
    // v3.8 mixed-size handling: stock content has both 32x32 and 64x64
    // textures in the same alpha group. Strict same-size assertion
    // would disarm coalesce on every mc2_NN mission. Instead, allocate
    // the array at MAX dimensions across the group's uniques and blit
    // each smaller texture into the upper-left sub-region (0,0,W,H) of
    // its layer. Per-type uvScaleX/Y in PerDrawEntry (§5.4) maps
    // [0,1] UV → the actual texture sub-region in the larger layer.
    // Shader applies `fract(v_uv) * uvScale` before sampling so
    // GL_REPEAT semantics work for tiling textures.
    layerForType.assign(typeCount, -1);
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
        GLuint glTexId;
        GLint  w;
        GLint  h;
        DWORD  nodeIdx;   // MC_TextureManager node index for KTX2 sidecar lookup
    };

    // COMPRESSION-BC7-STATICPROP-2 — bucket bookkeeping is rebuilt every
    // finalizeGeometry() under the gate. Release any prior arrays + size the
    // per-packet bucket index now (parallel to layerForPacket). Gate-OFF leaves
    // all of this empty (and untouched below), preserving byte-identical output.
    staticPropReleaseBuckets();
    if (s_staticPropBc7Enabled) {
        s_packetBucketIndex.assign(s_packets.size(), -1);
    }

    // COMPRESSION-BC7-STATICPROP-2 — build ONE uniform-dim GL_TEXTURE_2D_ARRAY
    // from a list of same-dimension uniques. Tries BC7 first (all-or-nothing per
    // array — a GL array has a single internalformat); falls back to the RGBA8
    // upload path on any condition miss. Returns the GL handle (0 on hard
    // failure) and sets outIsBc7. This is the per-bucket equivalent of the
    // group-level BC7/RGBA8 logic below; bucketDims == every layer's dims so
    // uvScale is always 1.0 and no sub-region blits occur.
    auto buildBucketArray =
        [&](const std::vector<UniqueTex>& bUniques, GLint bw, GLint bh,
            uint8_t group, bool& outIsBc7, size_t& outBytes) -> GLuint {
        outIsBc7 = false;
        outBytes = 0;
        GLuint arr = 0;

        if (s_materialKtxEnabled && mcTextureManager &&
            GLEW_ARB_texture_compression_bptc) {
            std::vector<RenderCore::KtxImage> bc7Imgs(bUniques.size());
            bool bc7Ok = true;
            const char* failReason = nullptr;
            int bc7MipCount = 0;
            uint32_t bc7VkFormat = 0;
            for (size_t k = 0; bc7Ok && k < bUniques.size(); ++k) {
                const auto& u = bUniques[k];
                if (u.nodeIdx == 0xFFFFFFFFu) { bc7Ok = false; failReason = "load_fail"; break; }
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
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
                if (img.width != bw || img.height != bh) {
                    bc7Ok = false; failReason = "dim_mismatch"; break;
                }
                if (k == 0) { bc7VkFormat = img.vkFormat; bc7MipCount = img.mipCount; }
                if (img.mipCount != bc7MipCount) { bc7Ok = false; failReason = "dim_mismatch"; break; }
            }
            if (bc7Ok) {
                // linear BC7 (UNORM): matches GL_RGBA8 baseline; engine has no GL_FRAMEBUFFER_SRGB
                const GLenum internalformat = GL_COMPRESSED_RGBA_BPTC_UNORM;
                const int levels = (bc7MipCount > 0) ? bc7MipCount : 1;
                // TEX-CLASS: asset-pool -- static-prop BC7 albedo 2D_ARRAY (content)
                glGenTextures(1, &arr);
                glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, internalformat,
                               bw, bh, static_cast<GLsizei>(bUniques.size()));
                for (size_t k = 0; k < bUniques.size(); ++k) {
                    const RenderCore::KtxImage& img = bc7Imgs[k];
                    for (int lvl = 0; lvl < levels; ++lvl) {
                        const int lw = (bw >> lvl) ? (bw >> lvl) : 1;
                        const int lh = (bh >> lvl) ? (bh >> lvl) : 1;
                        const GLsizei imageSize =
                            static_cast<GLsizei>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);
                        glCompressedTexSubImage3D(
                            GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(k),
                            lw, lh, 1, internalformat, imageSize,
                            img.pixels.data() +
                                img.mipByteOffsets[static_cast<size_t>(lvl)]);
                        outBytes += static_cast<size_t>(imageSize);
                    }
                }
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  levels - 1);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                outIsBc7 = true;
                mc2::fmt::print_err("[STATICPROP_BC7] bucket group={:d} dims={:d}x{:d} layers={:d} format=BC7 levels={:d} srcVkFormat={:d} bytes={:d}\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::as_i32(bw), mc2::fmt::as_i32(bh), mc2::fmt::as_size(bUniques.size()), mc2::fmt::as_i32(levels), mc2::fmt::as_u32(bc7VkFormat), mc2::fmt::as_size(outBytes));
                std::fflush(stderr);
                return arr;
            } else {
                mc2::fmt::print_err("[STATICPROP_BC7] bucket fallback group={:d} dims={:d}x{:d} reason={}\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::as_i32(bw), mc2::fmt::as_i32(bh), mc2::fmt::safe(failReason ? failReason : "unknown"));
                std::fflush(stderr);
            }
        }

        // ---- RGBA8 fallback (mirrors the group-level RGBA8 path; uniform-dim
        //      so every layer fills the whole level → no sub-region blits) ----
        // TEX-CLASS: asset-pool -- static-prop albedo/normal 2D_ARRAY (content)
        glGenTextures(1, &arr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, bw, bh,
                     static_cast<GLsizei>(bUniques.size()),
                     0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        std::vector<uint8_t> pixelBuf(static_cast<size_t>(bw) * bh * 4u);
        std::vector<std::pair<size_t, RenderCore::KtxImage>> ktxMipLayers;
        for (size_t k = 0; k < bUniques.size(); ++k) {
            const auto& u = bUniques[k];
            bool ktxUsed = false;
            if (s_materialKtxEnabled && u.nodeIdx != 0xFFFFFFFFu && mcTextureManager) {
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (srcName && *srcName) {
                    const std::string ktxPath = deriveTieredKtxPath(srcName, texTier());
                    RenderCore::KtxImage ktxImg;
                    const bool ktxOk = RenderCore::ktxLoadRgba8(ktxPath.c_str(), ktxImg);
                    // Reject compressed sidecars here — block bytes are not RGBA8.
                    const bool dimOk = ktxOk && !ktxImg.isCompressed &&
                                       ktxImg.width == u.w && ktxImg.height == u.h;
                    if (dimOk) {
                        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                        u.w, u.h, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                        ktxImg.pixels.data());
                        ktxUsed = true;
                        outBytes += ktxImg.pixels.size();
                        if (ktxImg.mipCount > 1) ktxMipLayers.emplace_back(k, std::move(ktxImg));
                    }
                }
            }
            if (!ktxUsed) {
                glBindTexture(GL_TEXTURE_2D, u.glTexId);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
                glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                u.w, u.h, 1, GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
                outBytes += static_cast<size_t>(bw) * bh * 4u;
            }
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        GLint maxLevel = 0;
        { GLint w = bw, h = bh; while (w > 1 || h > 1) { ++maxLevel; if (w>1) w>>=1; if (h>1) h>>=1; } }
        if (s_materialKtxEnabled && !ktxMipLayers.empty()) {
            for (const auto& lm : ktxMipLayers) {
                const size_t layerK = lm.first;
                const RenderCore::KtxImage& img = lm.second;
                const int lastLvl = (img.mipCount - 1 < maxLevel) ? (img.mipCount - 1) : maxLevel;
                for (int lvl = 1; lvl <= lastLvl; ++lvl) {
                    int lw = img.width  >> lvl; if (lw < 1) lw = 1;
                    int lh = img.height >> lvl; if (lh < 1) lh = 1;
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(layerK),
                                    lw, lh, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                    img.pixels.data() +
                                        img.mipByteOffsets[static_cast<size_t>(lvl)]);
                }
            }
        }
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  maxLevel);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        mc2::fmt::print_err("[STATICPROP_BC7] bucket group={:d} dims={:d}x{:d} layers={:d} format=RGBA8 bytes={:d}\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::as_i32(bw), mc2::fmt::as_i32(bh), mc2::fmt::as_size(bUniques.size()), mc2::fmt::as_size(outBytes));
        std::fflush(stderr);
        return arr;
    };

    // STATICPROP-MATERIAL-ORM-1 — build the per-bucket LINEAR ORM sibling array.
    // bUniques / bw / bh are the SAME inputs the albedo buildBucketArray got, so
    // ORM layer k corresponds to albedo layer k (the layer-alignment invariant).
    // Tries a BC7-linear fast path (all-or-nothing per array — a GL array has a
    // single internalformat) when every layer has a compressed sidecar matching
    // bw x bh with identical mipCount; otherwise an RGBA8 array where each layer
    // is either the loaded uncompressed sidecar (dims must equal bw x bh) or a
    // 1x1-replicated neutral (255,255,255,255) texel so the array stays DENSE/
    // bindable. outHasMap[k] = true only where layer k got a REAL ORM map.
    // ORM is LINEAR: bptcInternalFormatFor(false) for BC7, GL_RGBA8 otherwise.
    auto buildOrmBucketArray =
        [&](const std::vector<UniqueTex>& bUniques, GLint bw, GLint bh,
            uint8_t group, std::vector<bool>& outHasMap) -> GLuint {
        outHasMap.assign(bUniques.size(), false);
        GLuint arr = 0;
        const size_t layerCount = bUniques.size();

        if (mcTextureManager && GLEW_ARB_texture_compression_bptc) {
            std::vector<RenderCore::KtxImage> imgs(layerCount);
            bool ok = true;
            int  mipCount = 0;
            for (size_t k = 0; ok && k < layerCount; ++k) {
                const auto& u = bUniques[k];
                if (u.nodeIdx == 0xFFFFFFFFu) { ok = false; break; }
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (!srcName || !*srcName) { ok = false; break; }
                const std::string ormPath = deriveOrmSidecar(srcName);
                if (!RenderCore::ktxLoadRgba8(ormPath.c_str(), imgs[k])) { ok = false; break; }
                const RenderCore::KtxImage& img = imgs[k];
                if (!img.isCompressed ||
                    (img.vkFormat != 145u && img.vkFormat != 146u)) { ok = false; break; }
                if (img.width != bw || img.height != bh) { ok = false; break; }
                if (k == 0) mipCount = img.mipCount;
                if (img.mipCount != mipCount) { ok = false; break; }
            }
            if (ok) {
                const GLenum internalformat = bptcInternalFormatFor(false); // LINEAR
                const int levels = (mipCount > 0) ? mipCount : 1;
                // TEX-CLASS: asset-pool -- static-prop albedo/normal 2D_ARRAY (content)
                glGenTextures(1, &arr);
                glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, internalformat,
                               bw, bh, static_cast<GLsizei>(layerCount));
                for (size_t k = 0; k < layerCount; ++k) {
                    const RenderCore::KtxImage& img = imgs[k];
                    for (int lvl = 0; lvl < levels; ++lvl) {
                        const int lw = (bw >> lvl) ? (bw >> lvl) : 1;
                        const int lh = (bh >> lvl) ? (bh >> lvl) : 1;
                        const GLsizei imageSize =
                            static_cast<GLsizei>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);
                        glCompressedTexSubImage3D(
                            GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(k),
                            lw, lh, 1, internalformat, imageSize,
                            img.pixels.data() +
                                img.mipByteOffsets[static_cast<size_t>(lvl)]);
                    }
                    outHasMap[k] = true;
                }
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  levels - 1);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                mc2::fmt::print_err("[STATICPROP_ORM] bucket group={:d} dims={:d}x{:d} layers={:d} format=BC7-linear levels={:d} realMaps={:d}\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::as_i32(bw), mc2::fmt::as_i32(bh), mc2::fmt::as_size(layerCount), mc2::fmt::as_i32(levels), mc2::fmt::as_size(layerCount));
                std::fflush(stderr);
                return arr;
            }
        }

        // ---- RGBA8 fallback (per-layer: real sidecar or neutral fill) ----
        // TEX-CLASS: asset-pool -- static-prop albedo/normal 2D_ARRAY (content)
        glGenTextures(1, &arr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, bw, bh,
                     static_cast<GLsizei>(layerCount),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        std::vector<uint8_t> neutral(static_cast<size_t>(bw) * bh * 4u, 255u);
        size_t realMaps = 0;
        std::vector<std::pair<size_t, RenderCore::KtxImage>> ormMipLayers;
        for (size_t k = 0; k < layerCount; ++k) {
            const auto& u = bUniques[k];
            bool used = false;
            if (u.nodeIdx != 0xFFFFFFFFu && mcTextureManager) {
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (srcName && *srcName) {
                    const std::string ormPath = deriveOrmSidecar(srcName);
                    // STATICPROP-ORM-VISUAL-CHECK-1: observability — log every ORM
                    // unique probed (src path + expected sidecar + dims) so an
                    // operator can author/place a matching .orm.ktx2. Gated; off by default.
                    static const bool s_ormProbeTrace = mc2::gate::get<"MC2_STATICPROP_ORM_TRACE">();
                    if (s_ormProbeTrace) {
                        mc2::fmt::print_err("[STATICPROP_ORM_PROBE] group={:d} layer={:d} dims={:d}x{:d} src={} sidecar={}\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::as_size(k), mc2::fmt::as_i32(bw), mc2::fmt::as_i32(bh), mc2::fmt::safe(srcName), ormPath.c_str());
                        std::fflush(stderr);
                    }
                    RenderCore::KtxImage img;
                    const bool loadOk = RenderCore::ktxLoadRgba8(ormPath.c_str(), img);
                    // Reject compressed sidecars (block bytes are not RGBA8) and
                    // dim mismatches (must equal the albedo unique's dims).
                    const bool dimOk = loadOk && !img.isCompressed &&
                                       img.width == bw && img.height == bh;
                    if (loadOk && !dimOk) {
                        mc2::fmt::print_err("[STATICPROP_ORM] reject layer={:d} group={:d} sidecar={} reason={} got={:d}x{:d} want={:d}x{:d}\n", mc2::fmt::as_size(k), mc2::fmt::as_u32((unsigned)group), ormPath.c_str(), mc2::fmt::safe(img.isCompressed ? "compressed_in_rgba8_path" : "dim_mismatch"), mc2::fmt::as_i32(img.width), mc2::fmt::as_i32(img.height), mc2::fmt::as_i32(bw), mc2::fmt::as_i32(bh));
                        std::fflush(stderr);
                    }
                    if (dimOk) {
                        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                        bw, bh, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                        img.pixels.data());
                        used = true;
                        outHasMap[k] = true;
                        ++realMaps;
                        if (img.mipCount > 1) ormMipLayers.emplace_back(k, std::move(img));
                    }
                }
            }
            if (!used) {
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                bw, bh, 1, GL_RGBA, GL_UNSIGNED_BYTE, neutral.data());
            }
        }
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        GLint maxLevel = 0;
        { GLint w = bw, h = bh; while (w > 1 || h > 1) { ++maxLevel; if (w>1) w>>=1; if (h>1) h>>=1; } }
        for (const auto& lm : ormMipLayers) {
            const size_t layerK = lm.first;
            const RenderCore::KtxImage& img = lm.second;
            const int lastLvl = (img.mipCount - 1 < maxLevel) ? (img.mipCount - 1) : maxLevel;
            for (int lvl = 1; lvl <= lastLvl; ++lvl) {
                int lw = img.width  >> lvl; if (lw < 1) lw = 1;
                int lh = img.height >> lvl; if (lh < 1) lh = 1;
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(layerK),
                                lw, lh, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                img.pixels.data() +
                                    img.mipByteOffsets[static_cast<size_t>(lvl)]);
            }
        }
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  maxLevel);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        mc2::fmt::print_err("[STATICPROP_ORM] bucket group={:d} dims={:d}x{:d} layers={:d} format=RGBA8 realMaps={:d}\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::as_i32(bw), mc2::fmt::as_i32(bh), mc2::fmt::as_size(layerCount), mc2::fmt::as_size(realMaps));
        std::fflush(stderr);
        return arr;
    };

    for (uint8_t group = 0; group <= 1; ++group) {
        std::vector<DWORD> newlyPinnedThisBuild;
        GLuint& outArray = (group == 0u) ? s_texArrayOff : s_texArrayOn;

        std::vector<UniqueTex> uniques;             // unique textures in this group
        std::unordered_map<GLuint, int32_t> glTexIdToLayer;
        // Track packets that landed in this group, for the second-pass uvScale
        // assignment. Each entry: { globalPacketIdx, layer }.
        std::vector<std::pair<uint32_t, int32_t>> packetsInGroup;

        // 5.10.a — walk types in sorted order belonging to this group; for
        // each type, walk ALL its packets. Each packet gets its own layer
        // (deduped by glTexId) — this is the per-packet rework's core change.
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
                    coalesceRollbackTexBuild(newlyPinnedThisBuild);
                    s_coalesceLayoutReady = false;
                    s_coalesceEnabled     = false;
                    s_coalesceArmed       = false;
                    return true;
                }

                const DWORD nodeIdx =
                    src->listOfTextures[pkt.textureSlot].mcTextureNodeIndex;
                if (mcTextureManager && nodeIdx != 0xFFFFFFFFu) {
                    mcTextureManager->pinNode(nodeIdx);
                    newlyPinnedThisBuild.push_back(nodeIdx);
                }

                DWORD gosHandle = (mcTextureManager && nodeIdx != 0xFFFFFFFFu)
                    ? mcTextureManager->get_gosTextureHandle(nodeIdx)
                    : src->listOfTextures[pkt.textureSlot].gosTextureHandle;
                // Snapshot first-packet handle for the runtime eviction
                // detect (Step 11.4 still per-type). Multi-packet types use
                // their first packet as the canary — sufficient for the
                // mid-mission cache-evict guard (multi-textured types
                // typically share underlying texture lifetime).
                if (pIdx == 0) {
                    type.lastSeenGosHandle = gosHandle;
                }

                const GLuint glTexId = static_cast<GLuint>(gos_GetGLTextureId(gosHandle));

                auto it = glTexIdToLayer.find(glTexId);
                if (it != glTexIdToLayer.end()) {
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

            // v3.8 dimension fetch: use mcTextureManager metadata first
            // (avoids the lazy-GL-upload trap where glGetTexLevelParameteriv
            // returns 0x0 for textures that have a handle but haven't been
            // glTexImage2D'd yet). MC2 textures are square (only a single
            // `width` field exists in MC_TextureNode). Fall back to GL if
            // metadata is unavailable.
            GLint W = (mcTextureManager && nodeIdx != 0xFFFFFFFFu)
                ? static_cast<GLint>(mcTextureManager->getWidth(nodeIdx))
                : 0;
            GLint H = W;
            if (W <= 0) {
                glBindTexture(GL_TEXTURE_2D, glTexId);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &W);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &H);
            }
            // If both metadata AND GL fail, the texture is genuinely
            // unavailable at finalize-time (no MC node, never cached,
            // and no GL upload). Skip this packet via layer=-1 sentinel
            // rather than disarming the whole coalesce build — graceful
            // degradation: other valid packets still arm.
            if (W <= 0 || H <= 0) {
                COALESCE_TRACE("skip_packet type=%u pkt=%u group=%s reason=no_dims "
                               "nodeIdx=%lu gosHandle=%lu glTexId=%u",
                               typeID, pIdx, group == 0u ? "off" : "on",
                               (unsigned long)nodeIdx,
                               (unsigned long)gosHandle, glTexId);
                // [SEAMPROBE] stage 10: layer-assignment census. When the
                // override prop/tree packets land here, they get layer=-1 and
                // are dropped from the multidraw (the bdactor.cpp:262 skip
                // rule) -> override never rasterizes. Gated, capped.
                {
                    static int s_seamLayerSkip = 0;
                    if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">() && s_seamLayerSkip < 200) {
                        ++s_seamLayerSkip;
                        mc2::fmt::print_err("[SEAMPROBE] sample=first200 layer=-1 SKIP type={:d} pkt={:d} nodeIdx={:d} gosHandle={:d} glTexId={:d} W={:d} H={:d} override={:d} uniques={:d}\n", mc2::fmt::as_u32(typeID), mc2::fmt::as_u32(pIdx), mc2::fmt::as_u32((unsigned long)nodeIdx), mc2::fmt::as_u32((unsigned long)gosHandle), mc2::fmt::as_u32(glTexId), mc2::fmt::as_i32((int)W), mc2::fmt::as_i32((int)H), mc2::fmt::as_i32((int)type.isOverride), mc2::fmt::as_size(uniques.size()));
                        std::fflush(stderr);
                    }
                }
                // MODEL-OVERRIDE-GPU-BATCHER-SEAM-PROBE-1 fix: a modder glTF
                // render-override imports with unresolved "NULLTXM" texture
                // handles (nodeIdx=0xFFFFFFFF, no GL upload -> W<=0 here). The
                // stock skip rule (layer=-1 -> dropped from the coalesce
                // multidraw, see bdactor.cpp:262) makes the override geometry
                // invisible. For override-backed types ONLY, route the packet
                // to an already-built valid layer in this group (layer 0) so it
                // RASTERIZES with that texture (wrong albedo, but visible) —
                // the MVP success criterion is a box that draws, not correct
                // texturing. Stock/damage-shape packets are UNAFFECTED: they
                // keep the layer=-1 skip (no isOverride flag), so the orange-
                // ghost destroyed-building regression cannot recur. If no valid
                // layer exists yet in this group, fall through to the skip.
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
                coalesceRollbackTexBuild(newlyPinnedThisBuild);
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return true;
            }

            // v3.8 mixed-size: append unique with its actual W,H.
            // Array allocation uses max(W,H) across all uniques.
            const int32_t layer = static_cast<int32_t>(uniques.size());
            uniques.push_back({glTexId, W, H, nodeIdx});
            glTexIdToLayer[glTexId] = layer;
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
        if (s_staticPropBc7Enabled) {
            struct DimKey { GLint w, h; };
            std::vector<DimKey>               dimKeys;       // unique dims, stable order of first appearance
            std::vector<std::vector<int32_t>> bucketLayers;  // per dim: list of group-relative layer indices
            std::vector<int32_t>              layerToBucketLocal(uniques.size(), -1);   // group-rel layer -> local bucket
            std::vector<int32_t>              layerToBucketRelLayer(uniques.size(), -1); // group-rel layer -> bucket-relative layer

            for (size_t lyr = 0; lyr < uniques.size(); ++lyr) {
                const GLint w = uniques[lyr].w, h = uniques[lyr].h;
                int found = -1;
                for (size_t b = 0; b < dimKeys.size(); ++b) {
                    if (dimKeys[b].w == w && dimKeys[b].h == h) { found = static_cast<int>(b); break; }
                }
                if (found < 0) {
                    found = static_cast<int>(dimKeys.size());
                    dimKeys.push_back({w, h});
                    bucketLayers.emplace_back();
                }
                layerToBucketLocal[lyr]    = found;
                layerToBucketRelLayer[lyr] = static_cast<int32_t>(bucketLayers[found].size());
                bucketLayers[found].push_back(static_cast<int32_t>(lyr));
            }

            // Build one GL array per dim bucket; record into the global vectors.
            // localBucketToGlobal maps this group's local bucket index -> global
            // s_bucketArrays index (used to tag packets).
            std::vector<int32_t> localBucketToGlobal(dimKeys.size(), -1);
            size_t groupTotalBytes = 0;
            for (size_t b = 0; b < dimKeys.size(); ++b) {
                std::vector<UniqueTex> sub;
                sub.reserve(bucketLayers[b].size());
                for (int32_t lyr : bucketLayers[b]) sub.push_back(uniques[static_cast<size_t>(lyr)]);

                bool isBc7 = false; size_t bytes = 0;
                const GLuint arr = buildBucketArray(sub, dimKeys[b].w, dimKeys[b].h,
                                                    group, isBc7, bytes);
                groupTotalBytes += bytes;

                const int32_t globalBucket = static_cast<int32_t>(s_bucketArrays.size());
                localBucketToGlobal[b] = globalBucket;
                s_bucketArrays.push_back(arr);
                StaticPropBucketInfo info{};
                info.group      = group;
                info.w          = static_cast<uint16_t>(dimKeys[b].w);
                info.h          = static_cast<uint16_t>(dimKeys[b].h);
                info.layerCount = static_cast<uint32_t>(sub.size());
                info.isBc7      = isBc7;
                info.glArray    = arr;
                s_bucketInfo.push_back(info);

                // STATICPROP-MATERIAL-ORM-1 — build the layer-aligned ORM sibling
                // for THIS bucket from the SAME `sub` uniques, so ORM array index
                // == albedo array index (globalBucket) and ORM layer k == albedo
                // layer k. Gate-OFF builds nothing (vectors stay empty); gate-ON
                // keeps s_ormBucketArrays.size() == s_bucketArrays.size().
                if (s_ormSlotsEnabled) {
                    std::vector<bool> ormHasMap;
                    const GLuint ormArr = buildOrmBucketArray(sub, dimKeys[b].w, dimKeys[b].h,
                                                              group, ormHasMap);
                    s_ormBucketArrays.push_back(ormArr);
                    s_ormLayerHasMap.push_back(std::move(ormHasMap));
                }
            }

            // Reassign layerForPacket -> bucket-relative layer, tag each packet
            // with its global bucket index, and set uvScale = 1.0 (uniform-dim).
            for (const auto& kv : packetsInGroup) {
                const uint32_t globalPktIdx = kv.first;
                const int32_t  grpLayer     = kv.second;   // group-relative layer
                if (grpLayer < 0) continue;
                const int32_t localB   = layerToBucketLocal[static_cast<size_t>(grpLayer)];
                const int32_t relLayer = layerToBucketRelLayer[static_cast<size_t>(grpLayer)];
                layerForPacket[globalPktIdx]      = relLayer;            // bucket-relative
                s_packetBucketIndex[globalPktIdx] = localBucketToGlobal[static_cast<size_t>(localB)];
                uvScaleXByPacket[globalPktIdx]    = 1.0f;
                uvScaleYByPacket[globalPktIdx]    = 1.0f;
            }

            mc2::fmt::print_err("[STATICPROP_BC7] group={:d} buckets={:d} total_bytes={:d}\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::as_size(dimKeys.size()), mc2::fmt::as_size(groupTotalBytes));
            std::fflush(stderr);

            s_coalescePinnedNodes.insert(s_coalescePinnedNodes.end(),
                                          newlyPinnedThisBuild.begin(),
                                          newlyPinnedThisBuild.end());
            continue;
        }

        // 5.10.d — find max dimensions across the group's uniques.
        // The texture array is allocated at maxW × maxH; smaller
        // textures get blitted into upper-left sub-region of their
        // layer; per-type uvScale maps [0,1] UV → sub-region.
        GLint maxW = 0, maxH = 0;
        for (const auto& u : uniques) {
            if (u.w > maxW) maxW = u.w;
            if (u.h > maxH) maxH = u.h;
        }
        if (maxW <= 0 || maxH <= 0) {
            // Defensive — shouldn't happen because skip-path filtered W/H<=0.
            MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=alloc_failed "
                         "(group=%s maxDims=%dx%d after skip-no-dims pass)\n",
                         group == 0u ? "off" : "on", maxW, maxH);
            coalesceRollbackTexBuild(newlyPinnedThisBuild);
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return true;
        }

        // -------------------------------------------------------------------
        // COMPRESSION-BC7-STATICPROP-1 — BC7-compressed array fast path.
        // Gate: MC2_MATERIAL_KTX=1 AND MC2_STATICPROP_BC7=1.
        // A GL array has ONE internalformat, so BC7 is ALL-OR-NOTHING per
        // array. Additionally we require the group to be UNIFORM-DIM (every
        // layer fills the whole level) so compressed sub-region uploads never
        // violate 4x4 block alignment at small mips. If any condition fails we
        // fall through to the existing RGBA8 path below, byte-identical.
        // -------------------------------------------------------------------
        if (s_materialKtxEnabled && s_staticPropBc7Enabled && mcTextureManager) {
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
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
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

            if (bc7Ok && !GLEW_ARB_texture_compression_bptc) {
                bc7Ok = false; failReason = "no_bptc_support";
            }

            if (bc7Ok) {
                // linear BC7 (UNORM): matches GL_RGBA8 baseline; engine has no GL_FRAMEBUFFER_SRGB
                const GLenum internalformat = GL_COMPRESSED_RGBA_BPTC_UNORM;
                const int levels = (bc7MipCount > 0) ? bc7MipCount : 1;

                // TEX-CLASS: asset-pool -- static-prop material 2D_ARRAY (content)
                glGenTextures(1, &outArray);
                glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, internalformat,
                               maxW, maxH, static_cast<GLsizei>(uniques.size()));

                size_t bc7TotalBytes = 0;
                for (size_t k = 0; k < uniques.size(); ++k) {
                    const RenderCore::KtxImage& img = bc7Imgs[k];
                    for (int lvl = 0; lvl < levels; ++lvl) {
                        const int lw = (maxW >> lvl) ? (maxW >> lvl) : 1;
                        const int lh = (maxH >> lvl) ? (maxH >> lvl) : 1;
                        const GLsizei imageSize =
                            static_cast<GLsizei>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);
                        glCompressedTexSubImage3D(
                            GL_TEXTURE_2D_ARRAY, lvl,
                            0, 0, static_cast<GLint>(k),
                            lw, lh, 1,
                            internalformat, imageSize,
                            img.pixels.data() +
                                img.mipByteOffsets[static_cast<size_t>(lvl)]);
                        bc7TotalBytes += static_cast<size_t>(imageSize);
                    }
                }

                // Sampler: match the RGBA8 path. NO glGenerateMipmap (compressed
                // mips come straight from the cooked KTX). MAX_LEVEL = levels-1.
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  levels - 1);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

                for (const auto& kv : packetsInGroup) {
                    const uint32_t globalPktIdx = kv.first;
                    if (kv.second < 0) continue;
                    uvScaleXByPacket[globalPktIdx] = 1.0f;
                    uvScaleYByPacket[globalPktIdx] = 1.0f;
                }

                mc2::fmt::print_err("[STATICPROP_BC7] group={:d} layers={:d} dims={:d}x{:d} levels={:d} internalformat=0x{:x} bytes={:d}\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::as_size(uniques.size()), mc2::fmt::as_i32(maxW), mc2::fmt::as_i32(maxH), mc2::fmt::as_i32(levels), mc2::fmt::as_u32((unsigned)internalformat), mc2::fmt::as_size(bc7TotalBytes));
                mc2::fmt::print_err("[STATICPROP_BC7] summary group={:d} built BC7 array (srcVkFormat={:d}, {:d} layers)\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::as_u32(bc7VkFormat), mc2::fmt::as_size(uniques.size()));
                std::fflush(stderr);

                COALESCE_TRACE("group_built_bc7 group=%s uniques=%zu maxDims=%dx%d levels=%d",
                               group == 0u ? "off" : "on",
                               uniques.size(), maxW, maxH, levels);

                s_coalescePinnedNodes.insert(s_coalescePinnedNodes.end(),
                                              newlyPinnedThisBuild.begin(),
                                              newlyPinnedThisBuild.end());
                continue;
            } else {
                mc2::fmt::print_err("[STATICPROP_BC7] fallback group={:d} reason={}\n", mc2::fmt::as_u32((unsigned)group), mc2::fmt::safe(failReason ? failReason : "unknown"));
                std::fflush(stderr);
            }
        }

        // 5.10.d — allocate the array at max dims.
        // TEX-CLASS: asset-pool -- static-prop material 2D_ARRAY (content)
        glGenTextures(1, &outArray);
        glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8,
                     maxW, maxH,
                     static_cast<GLsizei>(uniques.size()),
                     0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);

        // 5.10.e — per-unique blit. Each unique blits at sub-region
        // (0,0,W,H) of its layer; the unused (W..maxW, H..maxH) area
        // is left at the glTexImage3D-zeroed state. Shader uvScale
        // ensures sampling never reaches that area for the type's UVs.
        size_t maxBytes = static_cast<size_t>(maxW) *
                          static_cast<size_t>(maxH) * 4u;
        std::vector<uint8_t> pixelBuf(maxBytes);
        // KTX2 layers that carry a pre-baked mip chain: cache the loaded image
        // keyed by layer index so we can OVERWRITE the auto-generated mips with
        // the sharp pre-baked levels after the single glGenerateMipmap pass.
        std::vector<std::pair<size_t, RenderCore::KtxImage>> s_ktxMipLayers;
        for (size_t k = 0; k < uniques.size(); ++k) {
            const auto& u = uniques[k];
            bool ktxUsed = false;
            if (s_materialKtxEnabled && u.nodeIdx != 0xFFFFFFFFu && mcTextureManager) {
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (srcName && *srcName) {
                    // Derive tiered .ktx2 path: data/tgl/<tier>/foo.ktx2
                    const std::string ktxPath = deriveTieredKtxPath(srcName, texTier());
                    RenderCore::KtxImage ktxImg;
                    const bool ktxOk = RenderCore::ktxLoadRgba8(ktxPath.c_str(), ktxImg);
                    // This RGBA8 upload path cannot consume compressed (BC7) KTX2:
                    // its bytes are block data, not RGBA8 texels. If a BC7 sidecar
                    // is present but this group fell back to the RGBA8 path (e.g.
                    // mixed-dim, so the BC7 array path was declined), reject it here
                    // (dimOk=false) so we fall through to the original GL texture
                    // upload below instead of mis-uploading block data as RGBA8.
                    const bool dimOk = ktxOk && !ktxImg.isCompressed &&
                                       ktxImg.width == u.w && ktxImg.height == u.h;
                    // Always-on debug: log attempt result so we can diagnose path/dim issues.
                    mc2::fmt::print_err("[KTX_SIDECAR] node={:d} srcName={} path={} load={:d} ktxDims={:d}x{:d} glDims={:d}x{:d} match={:d}\n",
                        mc2::fmt::as_u32((unsigned long)u.nodeIdx), mc2::fmt::safe(srcName), ktxPath.c_str(),
                        mc2::fmt::as_i32((int)ktxOk),
                        mc2::fmt::as_i32(ktxOk ? ktxImg.width : 0), mc2::fmt::as_i32(ktxOk ? ktxImg.height : 0),
                        mc2::fmt::as_i32(u.w), mc2::fmt::as_i32(u.h), mc2::fmt::as_i32((int)dimOk));
                    std::fflush(stderr);
                    if (dimOk) {
                        // KTX2 pixels are RGBA order; GL_RGBA matches.
                        glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
                        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                                        0, 0, static_cast<GLint>(k),
                                        u.w, u.h, 1,
                                        GL_RGBA, GL_UNSIGNED_BYTE, ktxImg.pixels.data());
                        ktxUsed = true;
                        // Stash for post-glGenerateMipmap pre-baked mip overwrite.
                        if (ktxImg.mipCount > 1) {
                            s_ktxMipLayers.emplace_back(k, std::move(ktxImg));
                        }
                        COALESCE_TRACE("ktx_sidecar_hit type=? node=%lu path=%s",
                                       (unsigned long)u.nodeIdx, ktxPath.c_str());
                    }
                }
            }
            if (!ktxUsed) {
                glBindTexture(GL_TEXTURE_2D, u.glTexId);
                const size_t need = static_cast<size_t>(u.w) *
                                    static_cast<size_t>(u.h) * 4u;
                if (pixelBuf.size() < need) pixelBuf.resize(need);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
                glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                                0, 0, static_cast<GLint>(k),
                                u.w, u.h, 1,
                                GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
            }
        }

        // 5.10.f — mipmap + sampler params (forestall AMD strict-fail).
        // GL_CLAMP_TO_EDGE on the array (was GL_REPEAT pre-v3.8) so
        // sub-region textures don't bleed into the unused area at the
        // edge. Tiling textures rely on the shader's `fract(v_uv) *
        // uvScale` to keep sampling within the sub-region.
        glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        GLint maxLevel = 0;
        {
            GLint w = maxW, h = maxH;
            while (w > 1 || h > 1) {
                ++maxLevel;
                if (w > 1) w >>= 1;
                if (h > 1) h >>= 1;
            }
        }
        // 5.10.f.ktx — OVERWRITE auto-generated mips with the pre-baked KTX2
        // mip chain for layers that carried one. glGenerateMipmap above filled
        // mips for ALL layers (KTX + non-KTX) from level 0; here we replace the
        // blurry auto-gen mips on KTX layers with the sharp baked levels.
        // Non-KTX layers keep their auto-generated mips. Only overwrite levels
        // that exist both in the baked chain AND within the array's maxLevel.
        if (s_materialKtxEnabled && !s_ktxMipLayers.empty()) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
            int totalMipsUploaded = 0;
            for (const auto& lm : s_ktxMipLayers) {
                const size_t layerK = lm.first;
                const RenderCore::KtxImage& img = lm.second;
                const int lastLvl = (img.mipCount - 1 < maxLevel)
                                        ? (img.mipCount - 1) : maxLevel;
                int uploaded = 0;
                for (int lvl = 1; lvl <= lastLvl; ++lvl) {
                    int lw = img.width  >> lvl; if (lw < 1) lw = 1;
                    int lh = img.height >> lvl; if (lh < 1) lh = 1;
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl,
                                    0, 0, static_cast<GLint>(layerK),
                                    lw, lh, 1,
                                    GL_RGBA, GL_UNSIGNED_BYTE,
                                    img.pixels.data() +
                                        img.mipByteOffsets[static_cast<size_t>(lvl)]);
                    ++uploaded;
                }
                totalMipsUploaded += uploaded;
                mc2::fmt::print_err("[KTX2_MIP] uploaded {:d} pre-baked mips for layer {:d} ({:d}x{:d})\n", mc2::fmt::as_i32(uploaded), mc2::fmt::as_size(layerK), mc2::fmt::as_i32(img.width), mc2::fmt::as_i32(img.height));
            }
            mc2::fmt::print_err("[KTX2_MIP] summary: {:d} KTX layers, {:d} pre-baked mips overwritten\n", mc2::fmt::as_size(s_ktxMipLayers.size()), mc2::fmt::as_i32(totalMipsUploaded));
            std::fflush(stderr);
        }

        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  maxLevel);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        // 2026-05-11 per-packet rework: uvScale is per-PACKET, derived from
        // each packet's unique texture dimensions vs the array max dims.
        const float invMaxW = 1.0f / static_cast<float>(maxW);
        const float invMaxH = 1.0f / static_cast<float>(maxH);
        for (const auto& kv : packetsInGroup) {
            const uint32_t globalPktIdx = kv.first;
            const int32_t layer = kv.second;
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
