// VK terrain island: height snapshot and audit, the colormap / cement / material-normal imports and the shadow image copies.
// Declared in the class headers; shared names in vulkan_terrain_island_internal.h.
#include "vulkan_terrain_island_internal.h"
#include "gos_terrain_indirect.h"
#include "vk_one_shot.h"
#include "texture_decode.h"
#include "terrain_field_store.h"
#include "gameos.hpp"
#include <array>
#include <bit>
#include <filesystem>
#include <string>
#include <string_view>
#include "../../RenderCore/KtxLoader.h"
#include "Gates/gate_cold.h"
#include "terrain_surface_cook.h"
#include "Gates/render_options.h"
#include "../../3rdparty/glm/glm/gtc/packing.hpp"

// The field store names the atlas format with its own constants (its header stays free of the decoder); they are the same VkFormat numbers.
static_assert(mc2terrain::ColormapAtlas::kRgba8 == mc2tex::kVkFormatRgba8Unorm && mc2terrain::ColormapAtlas::kBc7Unorm == mc2tex::kVkFormatBc7Unorm
              && mc2terrain::ColormapAtlas::kBc7Srgb == mc2tex::kVkFormatBc7Srgb, "colormap atlas formats drifted from the decoder's VkFormat values");

#ifdef MC2_VULKAN_ISLAND

// Shared rendered-surface query: analytic cement, road alpha and vegetation exclusion.
extern float gos_terrain_indirect_HardSurfaceCoverage(float wx, float wy);
extern uint64_t gos_terrain_indirect_HardSurfaceVersion();
extern int g_terrainMaterialProfile; // existing mission import profile; never sent to the shader


namespace vk_terrain_detail {

// Copy the store's height field into the VK-owned buffer when the feed version changes
// (map load and every height patch), never per frame.
bool snapshot_height(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (feed.heightVersion == s->heightVersionCopied) return true; // unchanged
    if (!feed.heightCpu || feed.heightBytes == 0) return false;
    const auto gradients = mc2terrain::cookSurfaceGradients(
        {feed.heightCpu, size_t(feed.heightBytes) / sizeof(float)}, uint32_t(feed.mapSide));
    if (!gradients) return false;
    if (!ensure_height_capacity(s, static_cast<VkDeviceSize>(feed.heightBytes))) return false;
    // VK-TERRAIN-FENCE-DEFER-1: the height SSBO stays single-instance (version-gated
    // rare rewrite: mission load / crater deform). Under the deferred retire a prior
    // terrain submit may still be reading it — device-idle before rewriting in place.
    // (The old inline post-submit fence wait used to provide this guarantee for free.)
    // A rewrite (crater deform) replaces the buffer instead: the old one retires on the open-frame ticket and the descriptors repoint lazily per parity.
    if (s->heightVersionCopied != UINT64_MAX &&
        !replace_host_buffer(s, s->heightBuf, s->heightCapBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "terrain.heightBuf")) return false;
    if (!create_colormap_sampler(s) || !upload_sampled_image(s, s->macroGradientTexture,
            VK_FORMAT_R32G32_SFLOAT, gradients->side, gradients->side, gradients->texels.data(),
            gradients->texels.size() * sizeof(gradients->texels[0]))) return false;
    write_descriptors(s);
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->heightBuf.allocation(), &p) != VK_SUCCESS) return false;
    std::memcpy(p, feed.heightCpu, static_cast<size_t>(feed.heightBytes));
    vmaUnmapMemory(s->allocator, s->heightBuf.allocation());
    s->heightVersionCopied = feed.heightVersion;
    ++g_health.heightSnapshots;
    return true;
}

// ---------------------------------------------------------------------------
// COLORMAP-BASE sub-slice: VK-owned copy-once import of the GL colormap atlas.
// Pattern: read the GL texel
// bytes back on the GL side (RGBA8 -> glGetTexImage(GL_RGBA); BC7 ->
// glGetCompressedTexImage), stage to a VK buffer, vkCmdCopyBufferToImage into a
// sampled VK image, LINEAR/CLAMP_TO_EDGE sampler (mip0). Match VK format to the GL
// upload's channel order / block encoding; do NOT double-encode sRGB.
// ---------------------------------------------------------------------------

// One-shot command buffer on the island's borrowed queue (mirror the spike helper).
VkCommandBuffer begin_oneshot(TerrainIsland* s) {
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = s->cpool.get(); cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cbi, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
void end_oneshot(TerrainIsland* s, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    mc2vk::submitAndWait(mc2vk::getOrCreateEngineVulkanContext(), si, 5ull * 1000 * 1000 * 1000, "terrain image copy-once");
    vkFreeCommandBuffers(s->device, s->cpool.get(), 1, &cb);
}

bool create_colormap_sampler(TerrainIsland* s) {
    if (s->colormapSamp) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // GL_CLAMP_TO_EDGE
    sci.maxLod = 0.25f;
    mc2vk::infra::materialSampler(sci);
    return mc2vk::create(s->colormapSamp, s->device, [&](auto* h) { return vkCreateSampler(s->device, &sci, nullptr, h); });
}

// Generic: build a VK sampled image from raw bytes (staging -> copy -> SHADER_READ).
// Frees the previous image/view (passed by ref). Reused by BOTH the colormap and cement
// copy-once imports. Match `fmt` to the GL upload's channel order (RGBA8 storage read
// back as GL_RGBA -> R8G8B8A8_UNORM; do NOT double-encode sRGB).
bool upload_sampled_image(TerrainIsland* s, mc2vk::ResidentTextureRef& outTexture,
                          VkFormat fmt, uint32_t width, uint32_t height, const void* bytes, VkDeviceSize byteCount) {
    auto texture=mc2vk::getOrCreateEngineVulkanContext().textures().acquirePixels(width,height,fmt,bytes,size_t(byteCount),false);
    if (!texture) return false;
    outTexture=std::move(texture); return true;
}

// Colormap-atlas wrapper over the generic uploader.
bool upload_colormap_image(TerrainIsland* s, VkFormat fmt, uint32_t width, uint32_t height,
                           const void* bytes, VkDeviceSize byteCount) {
    return upload_sampled_image(s, s->colormapTexture,
                                fmt, width, height, bytes, byteCount);
}

// Create a 1x1 opaque placeholder so binding 2 is ALWAYS a valid descriptor even
// before/without the real atlas (colormapReady stays false -> FS uses olive).
bool ensure_dummy_colormap(TerrainIsland* s) {
    if (!create_colormap_sampler(s)) return false;
    if (s->colormapTexture) return true; // something already bound
    const uint8_t px[4] = {107, 102, 71, 255}; // ~olive, RGBA8; irrelevant (flag=0)
    if (!upload_colormap_image(s, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, px, sizeof(px))) return false;
    s->colormapReady = false;
    return true;
}

// Copy-once import of the colormap atlas from the terrain field store (RGBA8 texels or BC7 blocks, the bytes GL uploaded), again
// whenever the store's version changes (a per-mission rebuild). Sets s->colormapReady on success. Fail-soft: leaves the dummy
// bound (colormapReady=false) so the FS falls back to olive.
void import_colormap(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (!feed.colormapBytes || feed.colormapVersion == 0) return;                          // atlas not built yet -> stay olive
    const std::array<float, 9> params = {feed.atlasTopLeftX, feed.atlasTopLeftY, feed.atlasOneOverWorldUnits,
        feed.shoreFieldBounds[0], feed.shoreFieldBounds[1], feed.shoreFieldBounds[2], feed.shoreFieldBounds[3],
        feed.shorelineWaterElev, float(feed.shoreFieldIsSdf != 0 && feed.shoreFieldTexels != nullptr)};
    if (feed.colormapVersion == s->colormapVersionImported && s->colormapReady &&
        feed.shoreFieldVersion == s->colormapShoreVersionImported && params == s->colormapCookParams) return;
    s->colormapReady = false; // a failed mission replacement must never reuse the previous mission's color
    s->colormapBreakup = false;

    // Consume exactly the atlas selected by the existing stock/mod import path. The cache is optional;
    // missing/stale cooks regenerate from these same bytes, including BC7's original sampling space.
    if (auto* fields = gos_TerrainFieldStore(); fields && fields->colormapAtlas().version == feed.colormapVersion) {
        const auto& atlas = fields->colormapAtlas();
        std::vector<uint8_t> decoded;
        std::span<const uint8_t> rgba = atlas.bytes;
        if (atlas.vkFormat == mc2terrain::ColormapAtlas::kBc7Unorm || atlas.vkFormat == mc2terrain::ColormapAtlas::kBc7Srgb) {
            decoded = mc2tex::decodeBc7(atlas.bytes, uint32_t(atlas.width), uint32_t(atlas.height));
            rgba = decoded;
        }
        mc2terrain::SurfaceCookInput input;
        input.rgba = rgba; input.srgb = atlas.vkFormat == mc2terrain::ColormapAtlas::kBc7Srgb;
        input.colorWidth = uint32_t(atlas.width); input.colorHeight = uint32_t(atlas.height);
        input.topLeftX = feed.atlasTopLeftX; input.topLeftY = feed.atlasTopLeftY;
        input.worldSpan = feed.atlasOneOverWorldUnits > 0 ? 1.0f / feed.atlasOneOverWorldUnits : 0;
        input.outputWidth = std::min(input.colorWidth * 2, mc2terrain::SurfaceAsset::kMaxSide);
        input.outputHeight = std::min(input.colorHeight * 2, mc2terrain::SurfaceAsset::kMaxSide);
        input.followCoast = params[8] != 0 && feed.shoreFieldW > 1 && feed.shoreFieldH > 1;
        if (input.followCoast) {
            input.shore = {feed.shoreFieldTexels, size_t(feed.shoreFieldW) * size_t(feed.shoreFieldH)};
            input.shoreWidth = uint32_t(feed.shoreFieldW); input.shoreHeight = uint32_t(feed.shoreFieldH);
            std::copy(feed.shoreFieldBounds, feed.shoreFieldBounds + 4, input.shoreBounds.begin());
        }
        input.waterElevation = feed.shorelineWaterElev;
        input.staticBreakup = true;
        // Write-through cache under the user data dir, keyed by source hash: stock, FST and mod maps alike.
        std::string cacheDir;
        {
            std::array<char, 1024> userDir{};
            const int userDirCap = static_cast<int>(userDir.size());
            if (gos_GetUserDataDirectory(userDir.data(), userDirCap)) cacheDir = (std::filesystem::path(userDir.data()) / "cache" / "terrain").generic_string();
        }
        const char* cookOrigin = "none";
        const auto resolveT0 = std::chrono::steady_clock::now();
        auto cooked = mc2terrain::resolveSurfaceAsset(input, mc2terrain::surfaceMissionCook(), cacheDir, &cookOrigin);
        const double resolveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - resolveT0).count();
        if (cooked) {
            const auto source = cooked->sourceHash;
            if (fields->setSurfaceAsset(std::move(*cooked), source)) {
                const auto& surface = fields->surfaceAsset();
                // The cook output comes from an 8-bit atlas and is only ever sampled at LOD 0, so RGBA8 holds it:
                // sRGB sources store the sRGB-encoded rgb (the sampler decodes to the same linear values the cook
                // computed, with sRGB's dark-end precision); alpha is the cooked breakup in [0,1]. Half the VRAM
                // of the former R16G16B16A16_SFLOAT upload (64 MB vs 128 MB at 4096^2).
                const auto packStart = std::chrono::steady_clock::now();
                const auto toByte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
                const auto encode = [&](float v) {
                    v = std::clamp(v, 0.0f, 1.0f);
                    return toByte(input.srgb ? (v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f) : v);
                };
                std::vector<uint32_t> packedColors(surface.macroColor.size());
                for (size_t i = 0; i < packedColors.size(); ++i) {
                    const auto& c = surface.macroColor[i];
                    packedColors[i] = uint32_t(encode(c[0])) | uint32_t(encode(c[1])) << 8 | uint32_t(encode(c[2])) << 16 |
                                      uint32_t(toByte(c[3])) << 24;
                }
                const double packMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-packStart).count();
                const auto uploadStart = std::chrono::steady_clock::now();
                if (upload_colormap_image(s, input.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, surface.width, surface.height,
                        packedColors.data(), packedColors.size() * sizeof(uint32_t))) {
                    s->colormapVersionImported = feed.colormapVersion;
                    s->colormapShoreVersionImported = feed.shoreFieldVersion;
                    s->colormapCookParams = params;
                    s->colormapReady = true;
                    s->colormapBreakup = true;
                    write_descriptors(s);
                    const double uploadMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-uploadStart).count();
                    vlog("COLORMAP-BASE: cooked macro color %ux%u source=%llu coast=%d origin=%s resolve_ms=%.1f pack_ms=%.1f enqueue_ms=%.1f.", surface.width, surface.height,
                         static_cast<unsigned long long>(source), int(input.followCoast), cookOrigin, resolveMs, packMs, uploadMs);
                    return;
                }
            }
        }
    }
    // Malformed/unsupported sources still cover the full map through the original raw atlas.
    // This is an import failure fallback; normal stock and mod missions always use the shared cook.
    VkFormat fmt;
    switch (feed.colormapVkFormat) {
        case mc2tex::kVkFormatBc7Srgb:    fmt = VK_FORMAT_BC7_SRGB_BLOCK; break;
        case mc2tex::kVkFormatBc7Unorm:   fmt = VK_FORMAT_BC7_UNORM_BLOCK; break;
        case mc2tex::kVkFormatRgba8Unorm: fmt = VK_FORMAT_R8G8B8A8_UNORM; break;
        default: {
            static decltype(feed.colormapVersion) s_rejectedVersion = 0;   // one log line per atlas version
            if (s_rejectedVersion != feed.colormapVersion)
                vlog("COLORMAP-BASE: colormap atlas format %u is not one the island uploads; staying on the fallback colour.", feed.colormapVkFormat);
            s_rejectedVersion = feed.colormapVersion;
            return;
        }
    }
    if (upload_colormap_image(s, fmt, static_cast<uint32_t>(feed.colormapWidth), static_cast<uint32_t>(feed.colormapHeight), feed.colormapBytes,
                              static_cast<VkDeviceSize>(feed.colormapByteCount))) {
        s->colormapVersionImported = feed.colormapVersion;
        s->colormapShoreVersionImported = feed.shoreFieldVersion;
        s->colormapCookParams = params;
        s->colormapReady = true;
        write_descriptors(s);
        vlog("COLORMAP-BASE: cook unavailable, using raw atlas %dx%d format %u (version %llu).",
             feed.colormapWidth, feed.colormapHeight, feed.colormapVkFormat, static_cast<unsigned long long>(feed.colormapVersion));
    }
    // On failure the dummy stays bound (colormapReady=false) -> FS uses olive.
}

// ---------------------------------------------------------------------------
// CEMENT/ROAD atlas sub-slice: copy-once import of the GL cement/road overlay atlas
// (g_cementAtlasGLTex, runtime-assembled RGBA8, GL_REPEAT wrap) + a REPEAT sampler
// (binding 3), plus a map-load snapshot of the per-vertex cement-word SSBO (GL binding
// 25 -> here binding 4). REUSES the colormap copy-once primitive verbatim; the ONLY
// deltas are (a) a REPEAT (not CLAMP) sampler — cement tiles wrap via fract() UV — and
// (b) the extra cement-word SSBO snapshot fed to the FS's cement branch.
// ---------------------------------------------------------------------------

bool create_cement_sampler(TerrainIsland* s) {
    if (s->cementSamp) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    mc2vk::infra::materialSampler(sci);
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // ★CEMENT: GL_REPEAT wrap (gos_terrain_indirect.cpp:1291-1292) — cement tiles wrap.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.25f;
    return mc2vk::create(s->cementSamp, s->device, [&](auto* h) { return vkCreateSampler(s->device, &sci, nullptr, h); });
}

// 1x1 placeholder so binding 3 is ALWAYS a valid descriptor before the real atlas
// imports (cementReady stays false -> FS ignores the cement branch -> colormap base).
bool ensure_dummy_cement(TerrainIsland* s) {
    if (!create_cement_sampler(s)) return false;
    if (s->cementTexture) return true; // already bound
    const uint8_t px[4] = {128, 128, 128, 255}; // neutral grey; irrelevant (useCement=0)
    if (!upload_sampled_image(s, s->cementTexture,
                              VK_FORMAT_R8G8B8A8_UNORM, 1, 1, px, sizeof(px))) return false;
    s->cementReady = false;
    return true;
}

// Copy-once import of the cement atlas from the terrain field store (RGBA8, the texels GL uploaded), again whenever the store's
// version changes (a per-mission rebuild). Fail-soft: on failure the dummy stays bound and the FS uses the colormap base.
void import_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (!feed.cementAtlasRgba || feed.cementAtlasSide <= 0 || feed.cementAtlasVersion == 0) return;   // atlas not built yet
    if (feed.cementAtlasVersion == s->cementVersionImported && s->cementReady) return;              // already imported
    const uint32_t side = static_cast<uint32_t>(feed.cementAtlasSide);
    if (upload_sampled_image(s, s->cementTexture, VK_FORMAT_R8G8B8A8_UNORM, side, side,
                             feed.cementAtlasRgba, static_cast<VkDeviceSize>(side) * side * 4u)) {
        s->cementVersionImported = feed.cementAtlasVersion;
        s->cementReady = true;
        write_descriptors(s);
        vlog("CEMENT/ROAD: imported the cement atlas %ux%u from the terrain field store (version %llu).", side, side,
             static_cast<unsigned long long>(feed.cementAtlasVersion));
    }
    // On failure the dummy stays bound (cementReady=false) -> FS uses colormap base.
}

// TERRAIN-SHORELINE-V3: binding 14 is always a valid descriptor; the dummy reads as "no modulation" and the FS ignores it
// anyway while uT.shorelineHasMask is 0. Shares the colormap's linear clamp sampler (GL samples the mask the same way).
bool ensure_dummy_shore_mask(TerrainIsland* s) {
    if (!create_colormap_sampler(s)) return false;
    if (s->shoreMaskTexture) return true;
    const uint8_t px[4] = {0, 255, 255, 0};
    if (!upload_sampled_image(s, s->shoreMaskTexture, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, px, sizeof(px))) return false;
    s->shoreMaskReady = false;
    return true;
}

// Material weights share the macro atlas UV convention and clamp sampler.
bool ensure_dummy_materials(TerrainIsland* s) {
    if (!create_colormap_sampler(s)) return false;
    if (s->materialTexture) return true;
    const uint8_t rock[4] = {};
    return upload_sampled_image(s, s->materialTexture, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, rock, sizeof(rock));
}

void import_materials(TerrainIsland* s) {
    const auto* fields = gos_TerrainFieldStore();
    if (!fields) return;
    if (s->materialPending && mc2vk::getOrCreateEngineVulkanContext().textures().ready(s->materialPending.get())) {
        s->materialTexture = std::move(s->materialPending);   // the old texture retires through the residency service, no GPU idle
        write_descriptors(s);
    }
    const auto& surface = fields->surfaceAsset();
    const auto& types = fields->terrainTypes();
    const auto& control = fields->controlMap();
    const bool baseSame = fields->surfaceVersion() == s->materialSurfaceVersion && types.version == s->materialTypesVersion &&
                          control.version == s->materialControlVersion && g_terrainMaterialProfile == s->materialProfile;
    const uint64_t coverageVersion = gos_terrain_indirect_HardSurfaceVersion();
    if (baseSame && coverageVersion == s->materialCoverageVersion) return;
    const uint32_t w = std::min(surface.width, 1024u);
    const uint32_t h = std::min(surface.height, 1024u);
    if (!baseSame || s->materialW != w || s->materialH != h) {
        s->materialBase = mc2terrain::cookSurfaceMaterials(surface, types.texels, uint32_t(types.width),
                              control.texels, uint32_t(control.width), w, h, g_terrainMaterialProfile == 1);
        s->materialW = w; s->materialH = h;
        s->materialProfile = g_terrainMaterialProfile;
    }
    const auto& base = s->materialBase;
    const float span = s->colormapCookParams[2] > 0 ? 1.0f / s->colormapCookParams[2] : 0;
    // Composite the hard-surface coverage over texels [x0,x1) x [y0,y1) of the cached base weights.
    const auto composite = [&](uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) for (uint32_t x = x0; x < x1; ++x) {
            const float wx = s->colormapCookParams[0] + (float(x)+0.5f) / w * span;
            const float wy = s->colormapCookParams[1] - (float(y)+0.5f) / h * span;
            const float coverage = gos_terrain_indirect_HardSurfaceCoverage(wx, wy);
            const size_t i = (size_t(y)*w+x)*4;
            for (size_t k=0;k<3;++k) s->materialFinal[i+k] = uint8_t(std::lround(base[i+k] * (1.0f-coverage)));
            s->materialFinal[i+3] = std::max(base[i+3], uint8_t(std::lround(coverage*255)));
        }
    };
    const auto dirty = gos_terrain_indirect::TakeHardSurfaceDirty();
    const bool incremental = baseSame && !base.empty() && s->materialFinal.size() == base.size();   // coverage-only change
    if (!base.empty() && !(incremental && !dirty.all)) {
        s->materialFinal.resize(base.size());
        composite(0, w, 0, h);
    } else if (incremental) {
        for (int r = 0; r < dirty.count; ++r) {
            const auto& q = dirty.rects[r];   // world rect {x0, y0, x1, y1}; invert the texel -> world map above
            const auto tx = [&](float wx) { return (wx - s->colormapCookParams[0]) / span * w - 0.5f; };
            const auto ty = [&](float wy) { return (s->colormapCookParams[1] - wy) / span * h - 0.5f; };
            const auto clampTo = [](float v, uint32_t hi) { return uint32_t(std::clamp(v, 0.0f, float(hi))); };
            if (!(span > 0)) break;
            composite(clampTo(std::floor(tx(q[0])), w), clampTo(std::ceil(tx(q[2])) + 1.0f, w),
                      clampTo(std::floor(ty(q[3])), h), clampTo(std::ceil(ty(q[1])) + 1.0f, h));
        }
    }
    // Coverage-only updates swap in once the new texture is resident; a new mission replaces it at once
    // (the previous texture retires through the residency service; no GPU idle).
    if (base.empty()) {
        s->materialTexture = {}; s->materialPending = {};   // discard the previous mission even when the cook fails
        ensure_dummy_materials(s);
    } else if (incremental) {
        auto texture = mc2vk::getOrCreateEngineVulkanContext().textures().acquirePixels(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                                                                         s->materialFinal.data(), s->materialFinal.size(), false);
        if (texture) s->materialPending = std::move(texture);
    } else {
        s->materialPending = {};
        s->materialTexture = {};
        if (!upload_sampled_image(s, s->materialTexture, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                                  s->materialFinal.data(), s->materialFinal.size())) ensure_dummy_materials(s);
    }
    s->materialSurfaceVersion = fields->surfaceVersion();
    s->materialTypesVersion = types.version;
    s->materialControlVersion = control.version;
    s->materialCoverageVersion = coverageVersion;
    write_descriptors(s);
    vlog("MATERIALS: %s %ux%u semantic weights (%zu bytes).", incremental ? "patched" : "baked", w, h, s->materialFinal.size());
}

// Copy-once import of the mission's shoreline mask sidecar from the terrain field store, again whenever its version changes.
// A mission without a sidecar keeps the dummy (pure elevation bands, as GL without one).
void import_shore_mask(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    const bool have = feed.shorelineMaskRgba && feed.shorelineMaskW > 0 && feed.shorelineMaskH > 0 && feed.shorelineMaskVersion != 0;
    auto& textures = mc2vk::getOrCreateEngineVulkanContext().textures();
    if (feed.shorelineMaskVersion == s->shoreMaskVersionImported && s->shoreMaskTexture) {
        s->shoreMaskReady = have && textures.ready(s->shoreMaskTexture.get());
        return;
    }
    const uint32_t zero = 0; // same absent-field representation as water
    const auto w = have ? static_cast<uint32_t>(feed.shorelineMaskW) : 1u;
    const auto h = have ? static_cast<uint32_t>(feed.shorelineMaskH) : 1u;
    s->shoreMaskReady = false;
    // Same bytes, format and key as water's mask: one service-owned GPU image.
    if (!upload_sampled_image(s, s->shoreMaskTexture, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                              have ? static_cast<const void*>(feed.shorelineMaskRgba) : &zero, static_cast<VkDeviceSize>(w) * h * 4u)) {
        s->shoreMaskReady = false;
        vlog("SHORELINE: shoreline mask %ux%u upload failed; bands draw unmodulated.", w, h);
        return; // retain old descriptor, disable its use, retry next frame
    }
    s->shoreMaskVersionImported = feed.shorelineMaskVersion;
    s->shoreMaskReady = have && textures.ready(s->shoreMaskTexture.get());
    vlog("SHORELINE: mask %ux%u version=%llu mask_handle=%u.", w, h,
         static_cast<unsigned long long>(feed.shorelineMaskVersion), s->shoreMaskTexture.get().raw());
    write_descriptors(s);
}

// ---------------------------------------------------------------------------
// VK-TERRAIN-DETAIL: 2D_ARRAY sampled-image uploader (RGBA8, all layers at level 0).
// Mirrors upload_sampled_image but with arrayLayers=layerCount and a 2D_ARRAY view.
// `bytes` is tightly packed layer-major (layer0 WxH RGBA8, then layer1, ...), exactly
// what glGetTexImage(GL_TEXTURE_2D_ARRAY, level0) returns.
bool upload_sampled_image_array(TerrainIsland* s, mc2vk::ResidentTextureRef& outTexture,
                                VkFormat fmt, uint32_t width, uint32_t height, uint32_t layers,
                                const void* bytes, VkDeviceSize byteCount) {
    auto texture=mc2vk::getOrCreateEngineVulkanContext().textures().acquirePixels(width,height,fmt,bytes,size_t(byteCount),false,layers,true);
    if (!texture) return false;
    outTexture=std::move(texture); return true;
}

bool create_matnormal_sampler(TerrainIsland* s) {
    if (s->matNormalSamp) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    mc2vk::infra::materialSampler(sci);
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR (mag)
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // GL_REPEAT wrap (gameos_graphics.cpp:2174-2175) — material detail normals tile.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;  // both imports are mip-complete; the explicit-gradient detail taps pick the level
    return mc2vk::create(s->matNormalSamp, s->device, [&](auto* h) { return vkCreateSampler(s->device, &sci, nullptr, h); });
}

// 1x1x1 flat-normal placeholder so binding 6 is ALWAYS a valid descriptor before the
// real array imports (matNormalReady stays false -> FS uses macro-slope-only normal).
bool ensure_dummy_matnormal(TerrainIsland* s) {
    if (!create_matnormal_sampler(s)) return false;
    if (s->matNormalTexture) return true;
    const uint8_t px[4] = {128, 128, 255, 255}; // (0,0,1) tangent-space flat normal
    if (!upload_sampled_image_array(s, s->matNormalTexture,
                                    VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, px, sizeof(px))) return false;
    s->matNormalReady = false;
    s->matNormalLayers = 1;
    return true;
}

// The terrain detail-normal array, layers 0-5 (the only ones terrain_tier1.frag samples). Preferred: the cooked BC7 layers with
// their authored mip chains from the field store. Otherwise a copy-once import of the slot textures' own texels (RGBA8, mips built
// at upload), again whenever the slot set is rebuilt. Absent optional slots get the flat normal, as in GL's array.
// Fail-soft: a slot that is not RGBA8 at slot 0's size is rejected with a log line and the dummy stays bound.
void import_matnormal(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if(mc2::options::lowCost()) return; // Low/Potato keep the flat-normal placeholder.
    const auto* fields = gos_TerrainFieldStore();
    if (fields && fields->materialNormals().image) {
        const auto& cooked = fields->materialNormals();
        // Latch the version once tried, but keep falling through to the slot-texture import when the cooked array is not the live one
        // (refused, or the fallback is still Pending): returning here would leave the placeholder bound for good.
        const bool cookedLive = s->matNormalReady && s->matNormalVersionImported == 0;
        if (cooked.version == s->matNormalCookedVersion && cookedLive) return;
        const bool cookedTried = cooked.version == s->matNormalCookedVersion;
        s->matNormalCookedVersion = cooked.version;
        const uint32_t side = uint32_t(cooked.image->width);
        RenderCore::TextureKey key;
        key.source = RenderCore::makeTextureSourceId("terrain-detail-normals", "bc7-array", cooked.version);
        key.format = cooked.image->vkFormat; key.hints = gosHint_MipmapFilter0;
        if (cookedTried) {
            // already refused at this version: straight to the slot-texture import below
        } else if (auto tex = mc2vk::getOrCreateEngineVulkanContext().textures().acquire(key, *cooked.image, {side, side, side, side}, cooked.layers, true)) {
            s->matNormalTexture = std::move(tex);
            s->matNormalLayers = cooked.layers;
            s->matNormalReady = true;
            s->matNormalVersionImported = 0;   // a later map without the cooked layers imports its slot textures again
            write_descriptors(s);
            vlog("VK-TERRAIN-DETAIL: imported the cooked detail-normal array %ux%u x%u layers, %d mips (vkFormat %u).",
                 side, side, cooked.layers, cooked.image->mipCount, cooked.image->vkFormat);
            return;
        }
        if (!cookedTried)
            vlog("VK-TERRAIN-DETAIL: the cooked detail-normal array was refused; importing the slot textures.");
    } else {
        s->matNormalCookedVersion = 0;
    }
    const uint64_t version = feed.matNormalVersion;
    if (version == 0) return;                        // array not built yet -> macro-slope only
    if (version == s->matNormalVersionImported && s->matNormalReady) return; // already imported
    if (version == s->matNormalVersionRejected) return;
    constexpr uint32_t kSlots = 6;
    GosTexels slots[kSlots];
    for (uint32_t i = 0; i < kSlots; ++i) {
        if (feed.matNormalHandles[i] == 0) continue;   // 0 = the invalid gos handle
        slots[i] = gos_GetTextureTexels(feed.matNormalHandles[i]);
        if (slots[i].status == GosTexels::Status::Pending) return;   // retried next frame
    }
    const uint32_t w = slots[0].width, h = slots[0].height;
    const size_t layerBytes = static_cast<size_t>(w) * h * 4u;
    for (uint32_t i = 0; i < kSlots; ++i) {
        const bool absent = feed.matNormalHandles[i] == 0;
        if (absent && i >= 5) continue;            // GL requires slots 0-4 only
        if (absent || slots[i].status != GosTexels::Status::Ready || slots[i].vkFormat != mc2tex::kVkFormatRgba8Unorm ||
            slots[i].width != w || slots[i].height != h || w == 0 || h == 0 || !slots[i].bytes ||
            slots[i].bytes->size() != layerBytes) {
            vlog("VK-TERRAIN-DETAIL: detail-normal slot %u rejected (%ux%u fmt=%u, slot 0 %ux%u); macro slope only.",
                 i, slots[i].width, slots[i].height, slots[i].vkFormat, w, h);
            s->matNormalVersionRejected = version;
            return;
        }
    }
    std::vector<uint8_t> px(layerBytes * kSlots);
    for (uint32_t i = 0; i < kSlots; ++i) {
        uint8_t* dst = px.data() + static_cast<size_t>(i) * layerBytes;
        if (slots[i].bytes) {
            // The slot texels are the TGA's own bytes (B, G, R, A); the shader reads .rgb as the normal's x, y, z.
            std::memcpy(dst, slots[i].bytes->data(), layerBytes);
            for (size_t t = 0; t < layerBytes; t += 4) std::swap(dst[t], dst[t + 2]);
        } else {
            for (size_t t = 0; t < layerBytes; t += 4) { dst[t] = 128; dst[t + 1] = 128; dst[t + 2] = 255; dst[t + 3] = 255; }
        }
    }
    if (auto tex = mc2vk::getOrCreateEngineVulkanContext().textures().acquirePixels(w, h, VK_FORMAT_R8G8B8A8_UNORM, px.data(), px.size(),
                                                                                   true, kSlots, true)) {
        s->matNormalTexture = std::move(tex);
        s->matNormalVersionImported = version;
        s->matNormalLayers = kSlots;
        s->matNormalReady = true;
        write_descriptors(s);
        vlog("VK-TERRAIN-DETAIL: imported the detail-normal array %ux%u x%u layers from the slot textures.", w, h, kSlots);
    }
    // On failure the dummy stays bound (matNormalReady=false) -> FS uses macro-slope only.
}

// Copy the store's cement words into the VK-owned buffer when the feed version changes
// (map load), mirroring snapshot_height. A map without words gets an all-zero field.
bool snapshot_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (feed.cementBytes == 0) return false;  // no map yet
    if (feed.cementVersion == s->cementVersionCopied && s->cementWordsReady) return true;
    // Ensure capacity (grow-only, like the height buffer).
    if (!s->cementBuf || static_cast<VkDeviceSize>(feed.cementBytes) > s->cementCapBytes) {
        const VkDeviceSize cap = std::bit_ceil(static_cast<uint64_t>(feed.cementBytes) + feed.cementBytes / 4 + 4096);
        if (!replace_host_buffer(s, s->cementBuf, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "terrain.cementBuf"))
            return false;
        s->cementCapBytes = cap;
        s->cementVersionCopied = UINT64_MAX; // force re-snapshot into the new buffer
        write_descriptors(s);
    }
    // VK-TERRAIN-FENCE-DEFER-1: single-instance rare rewrite (map-load version bump)
    // — device-idle before writing in place (see snapshot_height).
    if (s->cementVersionCopied != UINT64_MAX &&
        !replace_host_buffer(s, s->cementBuf, s->cementCapBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "terrain.cementBuf")) return false;
    write_descriptors(s);
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->cementBuf.allocation(), &p) != VK_SUCCESS) return false;
    // The descriptor spans the grow-only allocation. Clear the unused road plane
    // too when an edit/load publishes only cement, so old art cannot survive it.
    std::memset(p, 0, static_cast<size_t>(s->cementCapBytes));
    if (feed.cementCpu) std::memcpy(p, feed.cementCpu, static_cast<size_t>(feed.cementBytes));
    vmaUnmapMemory(s->allocator, s->cementBuf.allocation());
    s->cementVersionCopied = feed.cementVersion;
    s->cementWordsReady = true;
    return true;
}

// VK-TERRAIN-VISUALDISPLACE-FAR-1: copy the store's 4x visual-heightfield bake (the GL
// binding-26 bytes) into the VK-owned buffer when the feed version changes. Version-gated
// mission-load copy, mirroring snapshot_cement — the
// bake is uploaded once per mission (gos_TerrainLodChunk_UploadVisualHeightFull), so
// this is a cheap no-op on every other frame. Fail-soft: on any failure
// visualHeightReady stays false -> UBO visualMode forced 0 -> VS draws undisplaced.
bool snapshot_visual_height(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (!feed.visualHeightCpu || feed.visualHeightBytes == 0) return false;  // bake not live
    if (feed.visualHeightVersion == s->visualHeightVersionCopied && s->visualHeightReady) return true;
    // Ensure capacity (grow-only, like the cement buffer).
    if (!s->visualHeightBuf || static_cast<VkDeviceSize>(feed.visualHeightBytes) > s->visualHeightCapBytes) {
        const VkDeviceSize cap = std::bit_ceil(static_cast<uint64_t>(feed.visualHeightBytes) + feed.visualHeightBytes / 4 + 4096);
        if (!replace_host_buffer(s, s->visualHeightBuf, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "terrain.visualHeightBuf"))
            return false;
        s->visualHeightCapBytes = cap;
        s->visualHeightVersionCopied = UINT64_MAX; // force re-snapshot into the new buffer
        write_descriptors(s);
    }
    // VK-TERRAIN-FENCE-DEFER-1: single-instance rare rewrite (once-per-mission bake)
    // — device-idle before writing in place (see snapshot_height).
    if (s->visualHeightVersionCopied != UINT64_MAX &&
        !replace_host_buffer(s, s->visualHeightBuf, s->visualHeightCapBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "terrain.visualHeightBuf")) return false;
    write_descriptors(s);
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->visualHeightBuf.allocation(), &p) != VK_SUCCESS) return false;
    std::memcpy(p, feed.visualHeightCpu, static_cast<size_t>(feed.visualHeightBytes));
    vmaUnmapMemory(s->allocator, s->visualHeightBuf.allocation());
    s->visualHeightVersionCopied = feed.visualHeightVersion;
    s->visualHeightReady = true;
    static bool s_first = false;
    if (!s_first) {
        vlog("VK-TERRAIN-VISUALDISPLACE-FAR-1: snapshotted the visual-height bake "
             "(V=%d bytes=%llu ver=%llu); VK far bands now Z-swap like GL mode 2.",
             feed.visualSide, static_cast<unsigned long long>(feed.visualHeightBytes),
             static_cast<unsigned long long>(feed.visualHeightVersion));
        s_first = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shadow maps the terrain samples: the static sun map (binding 5), the single dynamic map (binding 7) and the cascades (bindings 8, 9). All of them
// are the VK shadow island's images; this island only owns 1x1 dummies that keep each binding valid before the first product.
// ---------------------------------------------------------------------------

// Depth-COMPARE sampler: compareOp=LESS_OR_EQUAL, LINEAR HW PCF, CLAMP_TO_BORDER
// white=1.0 (out-of-map -> lit). Mirrors GL sampler2DShadow.


// A D32 depth-compare image of `dim` texels for one binding's dummy (static: shadowImg/View/Dim, dynamic: dynShadow*), idempotent when unchanged.
// Left in DEPTH SHADER_READ so the binding is always valid.




// VK-TERRAIN-CSM-NEAR-1: (Re)create a D32 depth-COMPARE ARRAY image at dim x layers
// (idempotent when unchanged). Mirrors ensure_shadow_image_slot but with arrayLayers>1
// and a 2D_ARRAY view (so the frag can bind it as sampler2DArrayShadow).




// DIRECT-VIEW-ONLY near-cascade array (binding 8) + full-map cascade (binding 9). The VK
// shadow island renders both cascades natively into VK-owned D32 depth targets
// (MC2_VK_CSM_ARRAY_INTEROP / MC2_VK_CSM_FULLMAP_INTEROP, under MC2_VK_SHADOW_INTEROP)
// and terrain binds their VkImageViews directly, with no per-frame GL depth readback.
//
// Fail-soft: on ANY decline (csm not built this frame, or the shadow-island direct producer
// not yet ready during the ~1-2 warmup frames before its first CSM product) this returns
// false, the caller forces UBO csmEnabled=0, and the frag falls back to the legacy
// single-map dynamic-shadow path. This is NEVER a hole and NEVER stale: all
// ready/external/direct flags are reset every frame at the top, so a declined frame binds
// the 1x1 dummies (ensure_dummy_csm) and the next ready frame re-points bindings 8/9 at the
// live external views. The old copy-mode gave CSM immediately during warmup via a GL
// readback; the direct-only path accepts 1-2 warmup frames on the legacy path instead.


// 1x1 dummies so bindings 5 (static) and 7 (dynamic) are valid from frame 1.


// The static sun shadow map (binding 5): the VK shadow island's own map, used only when its build is of the feed's generation, so the map and the
// static light matrix in the UBO come from the same engine build. Between a rebake and its build (one frame) terrain has no static shadow.
bool bind_static_shadow(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    s->shadowReady=false;
    s->staticVisibilityExternalView=VK_NULL_HANDLE;
    if (!feed.lightingEnabled || !feed.shadowsEnabled || !mc2_vk_shadow_island_bake_ready()) return false;
    s->staticVisibilityExternalView=reinterpret_cast<VkImageView>(mc2_vk_shadow_island_get_bake_mask_view());
    s->shadowReady=s->staticVisibilityExternalView!=VK_NULL_HANDLE;
    return s->shadowReady;
}

// The single dynamic shadow map (binding 7) for frames without cascades (ShadowCSM=FALSE): the VK shadow island's own depth view when it produced one this
// frame, else no dynamic shadow on terrain this frame. There is no GL-copy fallback: it was a per-frame GL depth readback, which the co-write path treats
// as fatal, so the first frame of a ShadowCSM=FALSE mission (shadow island not yet up) latched the whole terrain island off and the VK scene was never
// cleared again (objects smeared over a frame with no terrain).



// CONCRETE-ROAD-PBR-DETAIL: the concrete / road material DETAIL array (binding 18). Layer 0 = poured-concrete pad detail, layer 1 =
// asphalt road/runway detail. Each texel is (normal.x, normal.y, roughness, AO) from CC0 PBR sets; the albedo of those sets is never
// loaded, MC2's own colour stays the colour source. Cooked by tools/mc2texcook/cook_concrete_detail.py (BC7 KTX2), decoded to RGBA8
// here and mip-chained by the residency service. Absent files leave the flat dummy bound and uT.concreteDetail = 0 (base game look).
bool ensure_dummy_concrete_detail(TerrainIsland* s) {
    if (!s->concreteDetailSamp) {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        mc2vk::infra::materialSampler(sci);
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        if (!mc2vk::create(s->concreteDetailSamp, s->device, [&](auto* h) { return vkCreateSampler(s->device, &sci, nullptr, h); })) return false;
    }
    if (s->concreteDetailTexture) return true;
    const uint8_t px[8] = {128, 128, 200, 255, 128, 128, 200, 255};   // flat normal, mid roughness, no occlusion, x2 layers
    return upload_sampled_image_array(s, s->concreteDetailTexture, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 2, px, sizeof(px));
}

void import_concrete_detail(TerrainIsland* s) {
    if (mc2::options::lowCost()) return; // No unused 2K detail residency on shared DDR4.
    if (s->concreteDetailPending) {                      // the upload is asynchronous: until it lands the residency service hands out a placeholder view
        auto& tx = mc2vk::getOrCreateEngineVulkanContext().textures();
        if (!tx.ready(s->concreteDetailPending.get())) return;
        s->concreteDetailTexture = std::move(s->concreteDetailPending);
        s->concreteDetailReady = true;
        write_descriptors(s);
        vlog("CONCRETE-DETAIL: the pad + road detail array is resident and bound.");
        return;
    }
    if (s->concreteDetailTried) return;
    s->concreteDetailTried = true;                       // one attempt per run; a missing sidecar is the base game look
    static const char* const kFiles[2] = {"data/terrain_layers/concrete_detail_pad.ktx2", "data/terrain_layers/concrete_detail_road.ktx2"};
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    auto appendLayer = [&](std::string_view file) {
        const std::string f(file);
        RenderCore::KtxImage img;
        std::vector<uint8_t> rgba;
        int lw = 0, lh = 0;
        if (!RenderCore::ktxLoadRgba8(f.c_str(), img) || !img.isCompressed || !RenderCore::ktxDecodeBc7ToRgba8(img, 0, rgba, &lw, &lh) ||
            lw <= 0 || lh <= 0 || (w != 0 && (lw != w || lh != h)))
            return false;
        w = lw; h = lh;
        px.insert(px.end(), rgba.begin(), rgba.end());
        return true;
    };
    for (const char* f : kFiles) {
        if (!appendLayer(f)) {
            vlog("CONCRETE-DETAIL: %s missing or unusable; concrete/road keep the base look.", f);
            return;
        }
    }
    // TERRAIN-CLIFF-ROCK: the cliff rock set (albedo ratio + height, normal + roughness + AO) rides the same array as layers 2/3.
    static const char* const kRock[2] = {"data/terrain_layers/cliff_rock_ah.ktx2", "data/terrain_layers/cliff_rock_nra.ktx2"};
    const size_t concreteBytes = px.size();
    const bool rock = mc2::gate::get<"MC2_TERRAIN_CLIFF_ROCK">() && appendLayer(kRock[0]) && appendLayer(kRock[1]);
    if (!rock) {
        px.resize(concreteBytes);
        vlog("CLIFF-ROCK: rock set absent, unusable or switched off; steep faces keep the base look.");
    }
    const uint32_t layers = rock ? 4u : 2u;
    auto tex = mc2vk::getOrCreateEngineVulkanContext().textures().acquirePixels(uint32_t(w), uint32_t(h), VK_FORMAT_R8G8B8A8_UNORM, px.data(), px.size(), true, layers, true);
    if (tex) {
        s->concreteDetailPending = std::move(tex);
        s->cliffRockPresent = rock;
        vlog("CONCRETE-DETAIL: imported pad + road%s detail array %dx%d x%u layers.", rock ? " + cliff rock" : "", w, h, layers);
    }
}

} // namespace

#endif // MC2_VULKAN_ISLAND
