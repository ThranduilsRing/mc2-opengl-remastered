#pragma once

#include "fx_format.h"

namespace mc2::fx {
using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
struct Bounds {
    Vec3 minimum{}, maximum{};
    bool empty = true;
};

struct Material {
    // Render state/delta/permission, process state/delta/permission, in wire order.
    // Texture bits retain the authored value; texture names are the native identity.
    std::array<uint32_t, 6> words{};
    std::array<uint32_t, 4> legacy_fog{};
    std::string texture;
    uint32_t texture_instance = 0;
};

struct MeshPrimitive {
    uint32_t class_id = 87, draw_mode = 0;
    std::vector<Vec3> positions;
    std::vector<Vec2> texcoords;
    std::vector<uint8_t> lengths;
    std::vector<uint16_t> indices;
    std::vector<Material> materials;
    std::vector<std::vector<Vec2>> extra_texcoords;
    std::array<float, 6> detail{}; // offset, scale, optional triangle fade distances
    std::vector<uint32_t> colors; // authored packed ARGB; no lossy unpack/repack
    std::vector<Vec3> normals;
    Bounds bounds;
};

struct MeshShape {
    std::vector<MeshPrimitive> primitives;
    Bounds bounds;
};
struct MeshLayout {
    bool triangles = false, colored = false, lit = false;
    bool dual = false, detail = false, multi = false;
};
MeshLayout mesh_layout(uint32_t class_id);
Material read_material(Reader& reader, int version);
void write_material(Writer& writer, const Material& material, int version);
MeshShape read_shape(Reader& reader, int version);
void write_shape(Writer& writer, const MeshShape& shape, int version);
void include(Bounds& bounds, const Vec3& point);
}
