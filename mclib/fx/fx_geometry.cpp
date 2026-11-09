#include "fx_geometry.h"
#include "fx_tubes.h"

namespace mc2::fx {
namespace {
void align(glm::mat4& model, const glm::vec3& direction, bool x, bool y) {
    if (x) math::alignLocalAxisToWorldVector(model, direction, 2, y ? 1 : 0, y ? 0 : -1);
    else if (y) math::alignLocalAxisToWorldVector(model, direction, 2, 1, -1);
}
void extract(const Instance& effect, const glm::mat4& parent, const glm::vec3& camera,
    const std::vector<const Material*>& parent_materials, std::vector<Geometry>& result) {
    const auto& record = effect.record();
    const auto type = record.class_id - first_effect_class;
    auto materials = parent_materials;
    materials.push_back(&record.material);
    const auto world = effect.local_to_parent * parent;
    const auto local_camera = math::transformPoint(camera, glm::inverse(world));
    if (type == 10 || type == 11) {
        const auto& p = effect.singleton();
        Geometry geometry;
        geometry.materials = materials;
        geometry.effect = &record;
        auto model = world;
        align(model, camera - math::getTranslation(model), record.alignment[0], record.alignment[1]);
        if (type == 10) {
            geometry.kind = GeometryKind::quads;
            for (size_t i = 0; i < p.vertices.size(); ++i)
                geometry.vertices.push_back({math::transformPoint(p.vertices[i], model), p.color, p.uvs[i]});
        } else {
            geometry.kind = GeometryKind::mesh; geometry.model = model;
            geometry.shape = &record.geometry.at(0).shape;
            geometry.scale = glm::vec3(p.scale); geometry.color = p.color;
        }
        result.push_back(std::move(geometry));
    } else if (type >= 2 && type <= 8) {
        Geometry geometry;
        geometry.materials = materials;
        geometry.effect = &record;
        geometry.kind = type == 2 ? GeometryKind::points : type == 6 ? GeometryKind::quads : GeometryKind::triangles;
        for (size_t slot = 0; slot < effect.active_count(); ++slot) {
            const auto& p = effect.particles()[slot];
            if (p.age >= 1) continue;
            // EffectCloud forwards its incoming DrawInfo unchanged to particles.
            if (type == 8) { extract(*p.effect, parent, camera, parent_materials, result); continue; }
            if (type == 2) {
                geometry.vertices.push_back({math::transformPoint(p.position, world), p.color, {}});
                continue;
            }
            auto model = math::rotationMatrix(p.rotation);
            align(model, local_camera - p.position, record.alignment[1], record.alignment[2]);
            math::buildTranslation(model, p.position);
            if (type == 7) {
                Geometry mesh;
                mesh.materials = materials;
                mesh.kind = GeometryKind::mesh; mesh.effect = &record; mesh.model = model * world;
                mesh.scale = glm::vec3(p.scale); mesh.color = p.color; mesh.shape = &record.geometry.at(0).shape;
                result.push_back(std::move(mesh)); continue;
            }
            std::vector<glm::vec3> vertices;
            if (type == 4) vertices = {{0, -0.5f * p.scale * p.radius, 0},
                {p.scale * p.angle * p.radius, p.scale * p.radius * 0.5f, 0},
                {-p.scale * p.angle * p.radius, p.scale * p.radius * 0.5f, 0}};
            else if (type == 6) vertices = {{p.scale * p.half_x, -p.scale * p.half_y, 0},
                {-p.scale * p.half_x, -p.scale * p.half_y, 0}, {-p.scale * p.half_x, p.scale * p.half_y, 0},
                {p.scale * p.half_x, p.scale * p.half_y, 0}};
            else for (const auto& v : p.vertices) vertices.push_back(v * p.scale);
            const auto base = geometry.vertices.size();
            for (size_t i = 0; i < vertices.size(); ++i) {
                // Preserve the two separate transforms used by the particle bridge.
                const auto local = math::transformPoint(vertices[i], model);
                geometry.vertices.push_back({math::transformPoint(local, world),
                    type == 5 && i == 0 ? p.center_color : p.color, type == 6 ? p.uvs[i] : glm::vec2(0)});
            }
            if (type == 5) for (size_t i = 1; i + 1 < vertices.size(); ++i)
                for (size_t index : {base, base + i + 1, base + i}) geometry.indices.push_back(static_cast<uint16_t>(index));
        }
        if (!geometry.vertices.empty()) result.push_back(std::move(geometry));
    } else if (type == 12 && effect.active_count() > 1) {
        Geometry geometry;
        geometry.materials = materials;
        geometry.effect = &record;
        const auto vertices = tube_template(record.profile_type);
        auto positions = effect.tube_vertices();
        if (record.profile_type == 1) {
            size_t slot = effect.head_profile();
            for (size_t compact = 0; compact < effect.active_count(); ++compact) {
                const auto& p = effect.profiles()[slot];
                const auto profile_camera = math::transformPoint(camera, glm::inverse(p.profile_to_world));
                const float scale = effect.curve("m_pScale", p.age, p.seed);
                const auto offset = p.direction * effect.curve("m_pDisplacement", p.age, p.seed);
                glm::mat4 rotation{1.0f};
                math::alignLocalAxisToWorldVector(rotation, profile_camera - offset, 2, 1, -1);
                glm::mat4 model{1.0f};
                for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) model[c][r] = scale * rotation[c][r];
                math::buildTranslation(model, offset);
                model = model * p.profile_to_world;
                model = model * glm::inverse(world);
                for (size_t i = 0; i < vertices.size(); ++i) positions[compact * vertices.size() + i] = math::transformPoint(vertices[i], model);
                slot = slot ? slot - 1 : effect.profiles().size() - 1;
            }
        }
        const auto count = vertices.size() * effect.active_count();
        for (size_t i = 0; i < count; ++i) geometry.vertices.push_back({math::transformPoint(positions[i], world),
            effect.tube_colors()[i], effect.tube_uvs()[i]});
        geometry.indices = tube_indices(record.profile_type, record.inside_out, effect.active_count());
        result.push_back(std::move(geometry));
    } else if (type == 13) {
        for (size_t i = 0; i < effect.particles().size(); ++i) {
            const auto& p = effect.particles()[i];
            if (p.age >= 1) continue;
            Geometry geometry;
            geometry.materials = materials;
            geometry.kind = GeometryKind::mesh; geometry.effect = &record;
            geometry.shape = &record.geometry[i].shape;
            geometry.model = p.debris_transform * world;
            geometry.color = {1, 1, 1, p.color.a};
            result.push_back(std::move(geometry));
        }
    }
    for (const auto& child : effect.children()) extract(*child, world, camera, materials, result);
}
}
std::vector<Geometry> generate_geometry(const Instance& effect, const glm::mat4& parent, const glm::vec3& camera) {
    std::vector<Geometry> result;
    extract(effect, parent, camera, {}, result);
    return result;
}
}
