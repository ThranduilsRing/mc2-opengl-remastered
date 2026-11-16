// Native ribbon submission; the queue preserves the post-renderLists drain order.
#include "gos_particle_bridge_internal.h"
#include "vulkan_vfx_geometry.h"
#include "../../mclib/fx/fx_render.h"
#include "mc2fmt.h"
#include "mlr/mlrtexturepool.hpp"
#include "mlr/mlrtexture.hpp"
#include <algorithm>

namespace {
struct RibbonRecord {
    size_t floatAt, indexAt;   // into ribbonFloats / ribbonIndices
    unsigned numVerts, numIndices;
    unsigned mlrHandle;
    int blendMode;
};
std::vector<RibbonRecord> ribbons;
std::vector<float> ribbonFloats;
std::vector<unsigned short> ribbonIndices;
std::vector<mc2::fx::RenderVertex> vertices;
std::vector<uint32_t> elementIndices;
std::vector<VfxParticleDraw> draws;
struct ResolvedTexture { unsigned mlrHandle, texture; };
std::vector<ResolvedTexture> resolved;

void appendRibbon(std::vector<mc2::fx::RenderVertex>& vertices, std::vector<VfxParticleDraw>& draws,
                  const float* positions, const float* colors, const float* uvs, unsigned numVerts,
                  const unsigned short* indices, unsigned numIndices, unsigned texture, bool untextured, int blend) {
    if (!positions || !colors || !uvs || !indices || !numVerts || !numIndices || (!texture && !untextured)) return;
    if (std::any_of(indices, indices + numIndices, [&](auto i) { return i >= numVerts; })) return;
    VfxParticleDraw draw{};
    draw.start = static_cast<uint32_t>(elementIndices.size());
    draw.count = numIndices;
    draw.texture = texture;
    // Low byte: Vulkan blend recipe; bit 8: authored UV clamp.
    const unsigned mode = static_cast<unsigned>(blend) & 255u;
    if (mode > 5) return;
    if (groupLogEnabled()) {
        static bool seen[6]{};
        if (!seen[mode]) {
            seen[mode] = true;
            mc2::fmt::print_err("[VFX_TUBE] blend={} vertices={}\n", mode, numIndices);
        }
    }
    const bool additive = mode == 2;
    draw.additive = mode;
    draw.atlasColumns = kVfxCompactGeometryAtlas;
    const auto base = static_cast<uint32_t>(vertices.size());
    for (unsigned i = 0; i < numVerts; ++i) {
        mc2::fx::RenderVertex v{};
        std::copy_n(positions + i * 3, 3, v.position);
        std::copy_n(colors + i * 4, 4, v.color);
        std::copy_n(uvs + i * 2, 2, v.uv);
        constexpr uint32_t clampUv = 16u; // vfx_particle.frag geometry UV policy
        v.flags = VfxGeometryTube | (additive ? VfxGeometryOneOne : 0u) |
                  ((blend & 256) != 0 ? clampUv : 0u) |
                  (mode == 4 || mode == 5 ? 512u : 0u) |
                  (mode == 3 ? 1024u : 0u) | (untextured ? VfxGeometryUntextured : 0u);
        vertices.push_back(v);
    }
    for (unsigned n = 0; n < numIndices; ++n) elementIndices.push_back(base + indices[n]);
    draws.push_back(draw);
}
void submit(const std::vector<mc2::fx::RenderVertex>& vertices, const std::vector<VfxParticleDraw>& draws) {
    auto* pp = getGosPostProcess();
    const auto* mvp = gos_GetTerrainMVPMat4();
    if (mc2_vk_vfx_island_enabled() && pp && mvp && !vertices.empty())
        mc2_vk_vfx_indexed_run(vertices.data(), static_cast<uint32_t>(vertices.size()),
            elementIndices.data(), static_cast<uint32_t>(elementIndices.size()), draws.data(),
            static_cast<uint32_t>(draws.size()), mvp, nullptr, nullptr, pp->getWidth(), pp->getHeight());
}
}
extern "C" void gos_tube_ribbon_enqueue(const float* positions, const float* colors, const float* uvs,
    unsigned numVerts, const unsigned short* indices, unsigned numIndices, unsigned mlrHandle, int blendMode) {
    if (!positions || !colors || !uvs || !indices || !numVerts || !numIndices) return;
    ribbons.push_back({ribbonFloats.size(), ribbonIndices.size(), numVerts, numIndices, mlrHandle, blendMode});
    // One flat arena per frame (capacity kept) instead of four heap vectors per ribbon: positions, colours, uvs back to back.
    ribbonFloats.insert(ribbonFloats.end(), positions, positions + static_cast<size_t>(numVerts) * 3);
    ribbonFloats.insert(ribbonFloats.end(), colors, colors + static_cast<size_t>(numVerts) * 4);
    ribbonFloats.insert(ribbonFloats.end(), uvs, uvs + static_cast<size_t>(numVerts) * 2);
    ribbonIndices.insert(ribbonIndices.end(), indices, indices + numIndices);
}
extern "C" void gos_vfx_native_batch_begin() { mc2_vk_vfx_native_batch_begin(); }
extern "C" void gos_vfx_native_batch_end() {
    ZoneScopedN("VKIsl.Vfx.FrameBatch");
    mc2_vk_vfx_native_batch_end();
}
extern "C" void gos_tube_ribbon_flush_deferred() {
    if (ribbons.empty()) return;
    mc2::ScopedFlushTimer timer(2, "tube_deferred");
    ZoneScopedN("gos.TubeRibbon.FlushDeferred");
    vertices.clear(); elementIndices.clear(); draws.clear(); resolved.clear();
    // Enqueue carries MLR pool indices, not GOS handles. Resolve after the
    // camera's ResolveTextures/ForceLoadImages, just like card groups.
    { ZoneScopedN("Tube.Expand");
    for (const auto& r : ribbons) {
        auto entryIt = std::find_if(resolved.begin(), resolved.end(), [&](const auto& entry) { return entry.mlrHandle == r.mlrHandle; });
        if (entryIt == resolved.end()) {
            resolved.push_back({r.mlrHandle, 0});
            entryIt = resolved.end() - 1;
            if (r.mlrHandle && MidLevelRenderer::MLRTexturePool::Instance) {
                auto* entry = (*MidLevelRenderer::MLRTexturePool::Instance)[r.mlrHandle];
                if (entry && entry->GetImage()) entryIt->texture = entry->GetImage()->GetHandle();
            }
        }
        const float* pos = ribbonFloats.data() + r.floatAt;
        appendRibbon(vertices, draws, pos, pos + static_cast<size_t>(r.numVerts) * 3, pos + static_cast<size_t>(r.numVerts) * 7,
            r.numVerts, ribbonIndices.data() + r.indexAt, r.numIndices, entryIt->texture, r.mlrHandle == 0, r.blendMode);
    } }
    submit(vertices, draws);
    ribbons.clear(); ribbonFloats.clear(); ribbonIndices.clear();
}
