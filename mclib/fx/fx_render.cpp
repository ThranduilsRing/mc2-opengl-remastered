#include "fx_render.h"
#include <algorithm>
#include <limits>

namespace mc2::fx {
Material compose_material(const Material& master, const Material& slave) {
    Material result = master;
    for (size_t base : {0u, 3u}) {
        const auto delta = slave.words[base + 1] & master.words[base + 2];
        result.words[base] = (master.words[base] & ~delta) | (slave.words[base] & delta);
        result.words[base + 1] = delta;
        result.words[base + 2] = master.words[base + 2] & slave.words[base + 2];
        if (base == 0 && (delta & 0x3fffu)) {
            result.texture = slave.texture;
            result.texture_instance = slave.texture_instance;
        }
    }
    return result;
}
RenderMaterial render_material(const Material& material) {
    constexpr uint32_t recipes[] = {3, 2, 1, 4, 0, 5};
    const auto alpha = (material.words[0] >> 14) & 7u;
    if (alpha >= std::size(recipes)) throw std::invalid_argument("FX invalid alpha recipe");
    return {material.texture, material.texture_instance, recipes[alpha], (material.words[0] & (1u << 21)) != 0};
}
MaterialId RenderResources::intern(const RenderMaterial& material) {
    auto [it, inserted] = material_ids_.try_emplace(material, next_material_);
    if (inserted) { materials_.emplace(next_material_, material); ++next_material_; }
    const auto& [key, id] = *it;
    return id;
}
const RenderMaterial* RenderResources::material(MaterialId id) const {
    const auto it = materials_.find(id);
    if (it == materials_.end()) return nullptr;
    const auto& [key, value] = *it;
    return &value;
}
uint64_t RenderResources::mesh(const MeshShape& shape) {
    auto [it, inserted] = mesh_ids_.try_emplace(&shape, next_mesh_);
    if (inserted) { meshes_.emplace(next_mesh_, shape); ++next_mesh_; }
    const auto& [key, id] = *it;
    return id;
}
void RenderResources::reset() {
    ++generation_;
    // IDs are never reused, even when a catalog gets allocated at the old address.
    material_ids_.clear(); materials_.clear(); mesh_ids_.clear(); meshes_.clear();
}
namespace {
Material compose(std::span<const Material* const> chain) {
    Material result;
    result.words = {0, 0, 0xffffffffu, 0, 0, 0xffffffffu};
    for (auto* material : chain) result = compose_material(result, *material);
    return result;
}
uint32_t flags(const RenderMaterial& material, bool tube) {
    return (tube ? 8u : 4u) | (material.clamp ? 16u : 0u) |
        (material.texture.empty() ? 32u : 0u) | (material.blend == 2 ? 2u : 0u) |
        (material.blend == 4 || material.blend == 5 ? 512u : 0u) | (material.blend == 3 ? 1024u : 0u);
}
RenderVertex vertex(const glm::vec3& p, const glm::vec4& c, const glm::vec2& uv, uint32_t flags) {
    return {{p.x, p.y, p.z}, {c.r, c.g, c.b, c.a}, {uv.x, uv.y}, flags};
}
std::vector<uint16_t> triangles(const MeshPrimitive& primitive) {
    if (primitive.lengths.empty()) return primitive.indices;
    std::vector<uint16_t> out;
    size_t offset = 0;
    for (auto length : primitive.lengths) {
        if (length < 3 || offset + length > primitive.indices.size()) throw std::invalid_argument("FX polygon range");
        for (size_t i = 1; i + 1 < length; ++i)
            out.insert(out.end(), {primitive.indices[offset], primitive.indices[offset + i], primitive.indices[offset + i + 1]});
        offset += length;
    }
    if (offset != primitive.indices.size()) throw std::invalid_argument("FX polygon count");
    return out;
}
}
void RenderResources::append(std::span<const Geometry> geometry, FramePackets& frame) {
    for (const auto& g : geometry) {
        const Material inherited = compose(g.materials);
        if (g.kind == GeometryKind::mesh) {
            if (!g.shape) continue;
            const auto& shape = meshes_.at(mesh(*g.shape));
            for (const auto& primitive : shape.primitives) {
                const auto indices = triangles(primitive);
                for (size_t pass = 0; pass < primitive.materials.size(); ++pass) {
                    const auto mat = render_material(compose_material(inherited, primitive.materials[pass]));
                    const auto first = static_cast<uint32_t>(frame.indices.size());
                    const auto base = static_cast<uint32_t>(frame.vertices.size());
                    for (size_t i = 0; i < primitive.positions.size(); ++i) {
                        const auto& p = primitive.positions[i];
                        const auto position = math::transformPoint(glm::vec3(p[0], p[1], p[2]) * g.scale, g.model);
                        glm::vec4 color = g.color;
                        if (!primitive.colors.empty()) {
                            const uint32_t c = primitive.colors.at(i);
                            color *= glm::vec4((c >> 16) & 255u, (c >> 8) & 255u, c & 255u, c >> 24) / 255.0f;
                        }
                        glm::vec2 uv{};
                        if (pass && pass - 1 < primitive.extra_texcoords.size()) {
                            const auto& u = primitive.extra_texcoords[pass - 1].at(i); uv = {u[0], u[1]};
                        } else if (!primitive.texcoords.empty()) {
                            const auto index = primitive.texcoords.size() == primitive.positions.size() * 2 ? i + primitive.positions.size() * std::min(pass, size_t{1}) : i;
                            const auto& u = primitive.texcoords.at(index); uv = {u[0], u[1]};
                            if (pass && mesh_layout(primitive.class_id).detail)
                                uv = uv * glm::vec2(primitive.detail[2], primitive.detail[3]) + glm::vec2(primitive.detail[0], primitive.detail[1]);
                        }
                        frame.vertices.push_back(vertex(position, color, uv, flags(mat, false)));
                    }
                    for (auto index : indices) {
                        if (index >= primitive.positions.size()) throw std::invalid_argument("FX mesh index");
                        frame.indices.push_back(base + index);
                    }
                    frame.draws.push_back({intern(mat), first, static_cast<uint32_t>(indices.size())});
                }
            }
        } else {
            const auto mat = render_material(inherited);
            const auto first = static_cast<uint32_t>(frame.indices.size());
            const auto base = static_cast<uint32_t>(frame.vertices.size());
            if (g.kind == GeometryKind::points) {
                for (const auto& v : g.vertices) for (uint32_t corner = 0; corner < 3; ++corner) {
                    frame.indices.push_back(static_cast<uint32_t>(frame.vertices.size()));
                    frame.vertices.push_back(vertex(v.position, v.color,
                        {corner == 1 ? 2.4f : 0.0f, corner == 2 ? 2.4f : 0.0f}, flags(mat, true) | 32u | 128u));
                }
                frame.draws.push_back({intern(mat), first, static_cast<uint32_t>(frame.indices.size()) - first});
                continue;
            }
            for (const auto& v : g.vertices) frame.vertices.push_back(vertex(v.position, v.color, v.uv, flags(mat, true)));
            if (!g.indices.empty()) {
                for (auto index : g.indices) {
                    if (index >= g.vertices.size()) throw std::invalid_argument("FX geometry index");
                    frame.indices.push_back(base + index);
                }
            } else if (g.kind == GeometryKind::quads) {
                for (uint32_t i = 0; i + 3 < g.vertices.size(); i += 4)
                    for (auto index : {0u, 1u, 2u, 0u, 2u, 3u}) frame.indices.push_back(base + i + index);
            } else {
                for (uint32_t i = 0; i < g.vertices.size(); ++i) frame.indices.push_back(base + i);
            }
            frame.draws.push_back({intern(mat), first, static_cast<uint32_t>(frame.indices.size()) - first});
        }
    }
}
void resolve_materials(const RenderResources& resources, const FramePackets& frame,
    TextureResolver resolve, void* context, std::map<MaterialId, uint32_t>& textures) {
    textures.clear();
    for (const auto& draw : frame.draws) {
        if (textures.contains(draw.material)) continue;
        if (const auto* mat = resources.material(draw.material)) textures.emplace(draw.material, resolve(*mat, context));
    }
}
}
