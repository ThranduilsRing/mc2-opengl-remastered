#pragma once
#include "fx_geometry.h"
#include <cstddef>
#include <map>
#include <span>

namespace mc2::fx {
using MaterialId = uint64_t;
// All camera data is supplied by the frame owner, never by an MLR clipper.
struct FrameView {
    glm::mat4 world_to_clip{1};
    glm::vec3 camera{}, right{1, 0, 0}, up{0, 1, 0};
    int width = 0, height = 0;
};
struct RenderMaterial {
    std::string texture;
    uint32_t variant = 0, blend = 0;
    bool clamp = false;
    auto operator<=>(const RenderMaterial&) const = default;
};
// Scalar-packed indexed vertex, shared verbatim with vfx_particle.vert.
// No per-index expansion, padding to 64 bytes, RNG or event state.
struct RenderVertex {
    float position[3], color[4], uv[2];
    uint32_t flags;
};
static_assert(sizeof(RenderVertex) == 40);
static_assert(offsetof(RenderVertex, color) == 12);
static_assert(offsetof(RenderVertex, uv) == 28 && offsetof(RenderVertex, flags) == 36);
struct RenderDraw {
    MaterialId material = 0;
    uint32_t first = 0, count = 0;
};
static_assert(sizeof(RenderDraw) == 16);
struct FramePackets {
    std::vector<RenderVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<RenderDraw> draws;
    void clear() { vertices.clear(); indices.clear(); draws.clear(); }
};

// Owns persistent material identities and mesh copies until the resource epoch
// retires. Addresses are only lookup hints within one epoch, never public IDs.
class RenderResources {
public:
    MaterialId intern(const RenderMaterial& material);
    const RenderMaterial* material(MaterialId id) const;
    uint64_t mesh(const MeshShape& shape);
    uint64_t generation() const { return generation_; }
    void reset(); // caller retires outstanding frames before releasing resources
    void append(std::span<const Geometry> geometry, FramePackets& frame);
private:
    uint64_t generation_ = 1, next_material_ = 1, next_mesh_ = 1;
    std::map<RenderMaterial, MaterialId> material_ids_;
    std::map<MaterialId, RenderMaterial> materials_;
    std::map<const MeshShape*, uint64_t> mesh_ids_;
    std::map<uint64_t, MeshShape> meshes_;
};
// Compose serialized state masks in root-to-leaf order. Texture names replace
// the wire's pool index; the latter is never looked up at runtime.
Material compose_material(const Material& master, const Material& slave);
RenderMaterial render_material(const Material& material);

// Resolve once per unique persistent material per frame, retaining draw order.
// The owner uses this for texture service access; tests inject a counting sink.
using TextureResolver = uint32_t (*)(const RenderMaterial&, void*);
void resolve_materials(const RenderResources& resources, const FramePackets& frame,
    TextureResolver resolve, void* context, std::map<MaterialId, uint32_t>& textures);
}
