// Native ribbon submission; the queue preserves the post-renderLists drain order.
#include "gos_particle_bridge_internal.h"
#include "vulkan_vfx_geometry.h"
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
    if (!positions || !colors || !uvs || !indices || !numVerts || !numIndices || !texture) return;
    if (std::any_of(indices, indices + numIndices, [&](auto i) { return i >= numVerts; })) return;
    VfxParticleDraw draw{};
    draw.start = static_cast<uint32_t>(vertices.size());
    draw.count = numIndices;
    draw.texture = texture;
    draw.additive = blend == 1 ? 2u : 0u; // authored OneOne tubes ignore vertex alpha
    draw.atlasColumns = kVfxGeometryAtlas;
    for (unsigned n = 0; n < numIndices; ++n) {
        const auto i = indices[n];
        VfxGeometryVertex v{};
        std::copy_n(positions + i * 3, 3, v.position);
        std::copy_n(colors + i * 4, 4, v.color);
        std::copy_n(uvs + i * 2, 2, v.uv);
        v.flags = VfxGeometryTube | (blend == 1 ? VfxGeometryOneOne : 0u);
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
    if (!positions || !colors || !uvs || !indices || !numVerts || !numIndices || !gosHandle) return;
    ribbons.push_back({{positions, positions + size_t(numVerts)*3}, {colors, colors + size_t(numVerts)*4},
                       {uvs, uvs + size_t(numVerts)*2}, {indices, indices + numIndices}, gosHandle, blendMode});
}
extern "C" void gos_tube_ribbon_flush_deferred() {
    mc2::fx_cost_split::roll_frame_and_maybe_emit();
    if (ribbons.empty()) return;
    mc2::ScopedFlushTimer timer(2, "tube_deferred");
    ZoneScopedN("gos.TubeRibbon.FlushDeferred");
    mc2::fx_cost_split::Scope cost(mc2::fx_cost_split::B_TUBE_BRIDGE_FLUSH);
    std::vector<VfxGeometryVertex> vertices;
    std::vector<VfxParticleDraw> draws;
    for (const auto& r : ribbons) appendRibbon(vertices, draws, r.positions.data(), r.colors.data(), r.uvs.data(),
        static_cast<unsigned>(r.positions.size()/3), r.indices.data(), static_cast<unsigned>(r.indices.size()), r.gosHandle, r.blendMode);
    submit(vertices, draws);
    ribbons.clear();
}
