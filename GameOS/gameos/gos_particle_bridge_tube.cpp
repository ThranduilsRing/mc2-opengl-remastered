// Native ribbon submission; the queue preserves the post-renderLists drain order.
#include "gos_particle_bridge_internal.h"
#include "vulkan_vfx_geometry.h"
#include "mlr/mlrtexturepool.hpp"
#include "mlr/mlrtexture.hpp"
#include <algorithm>

namespace {
struct RibbonRecord {
    std::vector<float> positions, colors, uvs;
    std::vector<unsigned short> indices;
    unsigned gosHandle;
    int blendMode;
};
std::vector<RibbonRecord> ribbons;

void appendRibbon(std::vector<VfxGeometryVertex>& vertices, std::vector<VfxParticleDraw>& draws,
                  const float* positions, const float* colors, const float* uvs, unsigned numVerts,
                  const unsigned short* indices, unsigned numIndices, unsigned texture, int blend) {
    if (!positions || !colors || !uvs || !indices || !numVerts || !numIndices) return;
    if (std::any_of(indices, indices + numIndices, [&](auto i) { return i >= numVerts; })) return;
    VfxParticleDraw draw{};
    draw.start = static_cast<uint32_t>(vertices.size());
    draw.count = numIndices;
    draw.texture = texture;
    // Low byte: Vulkan blend recipe; bit 8: authored UV clamp.
    const unsigned mode = static_cast<unsigned>(blend) & 255u;
    if (mode > 5) return;
    if (groupLogEnabled()) {
        static bool seen[6][2]{};
        const unsigned untextured = texture == 0;
        if (!seen[mode][untextured]) {
            seen[mode][untextured] = true;
            mc2::fmt::print_err("[VFX_TUBE] blend={} untextured={} vertices={}\n", mode, untextured, numIndices);
        }
    }
    const bool additive = mode == 2;
    draw.additive = mode;
    draw.atlasColumns = kVfxGeometryAtlas;
    for (unsigned n = 0; n < numIndices; ++n) {
        const auto i = indices[n];
        VfxGeometryVertex v{};
        std::copy_n(positions + i * 3, 3, v.position);
        std::copy_n(colors + i * 4, 4, v.color);
        if (mode == 3) v.color[3] = 1.0f;
        std::copy_n(uvs + i * 2, 2, v.uv);
        constexpr uint32_t clampUv = 16u; // vfx_particle.frag geometry UV policy
        v.flags = VfxGeometryTube | (additive ? VfxGeometryOneOne : 0u) |
                  ((blend & 256) != 0 ? clampUv : 0u) |
                  (texture == 0 ? VfxGeometryUntextured : 0u) |
                  (mode == 3 ? 256u : 0u);
        vertices.push_back(v);
    }
    draws.push_back(draw);
}
void submit(const std::vector<VfxGeometryVertex>& vertices, const std::vector<VfxParticleDraw>& draws) {
#ifdef MC2_VULKAN_ISLAND
    auto* pp = getGosPostProcess();
    const auto* mvp = gos_GetTerrainMVPMat4();
    if (mc2_vk_vfx_island_enabled() && pp && mvp && !vertices.empty())
        mc2_vk_vfx_island_run(vertices.data(), static_cast<uint32_t>(vertices.size()), draws.data(),
            static_cast<uint32_t>(draws.size()), mvp, nullptr, nullptr, pp->getWidth(), pp->getHeight());
#endif
}
}
extern "C" void gos_tube_ribbon_flush(const float* positions, const float* colors, const float* uvs,
    unsigned numVerts, const unsigned short* indices, unsigned numIndices, unsigned gosHandle, int blendMode) {
    mc2::ScopedFlushTimer timer(1, "tube");
    std::vector<VfxGeometryVertex> vertices;
    std::vector<VfxParticleDraw> draws;
    appendRibbon(vertices, draws, positions, colors, uvs, numVerts, indices, numIndices, gosHandle, blendMode);
    submit(vertices, draws);
}
extern "C" void gos_tube_ribbon_enqueue(const float* positions, const float* colors, const float* uvs,
    unsigned numVerts, const unsigned short* indices, unsigned numIndices, unsigned gosHandle, int blendMode) {
    if (!positions || !colors || !uvs || !indices || !numVerts || !numIndices) return;
    ribbons.push_back({{positions, positions + size_t(numVerts)*3}, {colors, colors + size_t(numVerts)*4},
                       {uvs, uvs + size_t(numVerts)*2}, {indices, indices + numIndices}, gosHandle, blendMode});
}
extern "C" void gos_tube_ribbon_flush_deferred() {
    if (ribbons.empty()) return;
    mc2::ScopedFlushTimer timer(2, "tube_deferred");
    ZoneScopedN("gos.TubeRibbon.FlushDeferred");
    std::vector<VfxGeometryVertex> vertices;
    std::vector<VfxParticleDraw> draws;
    // Enqueue carries MLR pool indices, not GOS handles. Resolve after the
    // camera's ResolveTextures/ForceLoadImages, just like card groups.
    for (const auto& r : ribbons) {
        unsigned texture = 0;
        if (r.gosHandle && MidLevelRenderer::MLRTexturePool::Instance) {
            auto* entry = (*MidLevelRenderer::MLRTexturePool::Instance)[r.gosHandle];
            if (entry && entry->GetImage()) texture = entry->GetImage()->GetHandle();
        }
        appendRibbon(vertices, draws, r.positions.data(), r.colors.data(), r.uvs.data(),
            static_cast<unsigned>(r.positions.size()/3), r.indices.data(), static_cast<unsigned>(r.indices.size()), texture, r.blendMode);
    }
    submit(vertices, draws);
    ribbons.clear();
}
