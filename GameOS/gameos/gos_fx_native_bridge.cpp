// Native packet/texture boundary. No MLR or gosFX headers, handles or camera.
#include "gos_particle_bridge.h"
#include "vulkan_vfx_geometry.h"
#include "../../mclib/fx/fx_render.h"
#include "../../mclib/texture_assets.h"
#include <map>

namespace {
std::map<mc2::fx::RenderMaterial, DWORD> assets;
std::map<mc2::fx::MaterialId, uint32_t> resolved;
std::vector<VfxParticleDraw> draws;
uint32_t resolve(const mc2::fx::RenderMaterial& material, void*) {
    if (material.texture.empty()) return 0;
    if (!mc2::textures::ready()) return 0xffffffffu;
    auto it = assets.find(material);
    if (it == assets.end()) {
        const auto path = "data/tgl/128/" + material.texture + ".tga";
        const bool fx = material.texture.size() >= 2 &&
            (material.texture[0] == 'f' || material.texture[0] == 'F') &&
            (material.texture[1] == 'x' || material.texture[1] == 'X');
        const auto asset = mc2::textures::load(path.c_str(), gos_Texture_Detect,
            fx ? 0 : gosHint_DisableMipmap, material.variant);
        if (asset == 0xffffffffu) return 0xffffffffu;
        const auto [stored, inserted] = assets.emplace(material, asset);
        it = stored;
    }
    const auto& [key, asset] = *it;
    const auto source = mc2::textures::sourceId(asset);
    return source == 0 ? 0xffffffffu : source;
}
}
int gos_fx_native_flush(const mc2::fx::RenderResources& resources,
    const mc2::fx::FramePackets& frame, const mc2::fx::FrameView& view) {
    if (!mc2_vk_vfx_island_enabled() || frame.indices.empty()) return 0;
    mc2::fx::resolve_materials(resources, frame, resolve, nullptr, resolved);
    draws.clear();
    for (const auto& packet : frame.draws) {
        const auto* material = resources.material(packet.material);
        const auto it = resolved.find(packet.material);
        if (!material || it == resolved.end()) continue;
        const auto& [key, texture] = *it;
        if (texture == 0xffffffffu) continue;
        VfxParticleDraw draw{};
        draw.start = packet.first; draw.count = packet.count;
        draw.additive = material->blend; draw.texture = texture;
        draw.atlasColumns = kVfxCompactGeometryAtlas;
        draws.push_back(draw);
    }
    return mc2_vk_vfx_indexed_run(frame.vertices.data(), static_cast<uint32_t>(frame.vertices.size()),
        frame.indices.data(), static_cast<uint32_t>(frame.indices.size()), draws.data(),
        static_cast<uint32_t>(draws.size()), &view.world_to_clip[0][0], &view.right.x, &view.up.x, view.width, view.height);
}
void gos_fx_native_reset() {
    if (mc2::textures::ready()) for (const auto& [material, asset] : assets) mc2::textures::release(asset);
    assets.clear(); resolved.clear(); draws.clear();
}
