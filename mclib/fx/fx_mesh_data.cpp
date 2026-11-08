#include "fx_mesh_data.h"
#include <algorithm>
#include <numeric>

namespace mc2::fx {
namespace {
template<size_t N> std::vector<std::array<float, N>> read_vectors(Reader& reader) {
    const auto count = reader.count(N * 4, 65536);
    std::vector<std::array<float, N>> values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) values.push_back(reader.scalars<N>());
    return values;
}
template<size_t N> void write_vectors(Writer& writer, const std::vector<std::array<float, N>>& values) {
    writer.integer(static_cast<uint32_t>(values.size()));
    for (const auto& value : values) writer.scalars(value);
}
}

void include(Bounds& bounds, const Vec3& point) {
    if (bounds.empty) { bounds.minimum = bounds.maximum = point; bounds.empty = false; }
    else for (size_t i = 0; i < 3; ++i) {
        bounds.minimum[i] = std::min(bounds.minimum[i], point[i]);
        bounds.maximum[i] = std::max(bounds.maximum[i], point[i]);
    }
}

MeshLayout mesh_layout(uint32_t id) {
    // Frozen on-disk MLR ordinals, including retired classes between families.
    switch (id) {
    case 87: return {};
    case 88: return {false, true};
    case 89: return {false, true, true};
    case 90: return {false, false, false, true};
    case 91: return {false, true, false, true};
    case 92: return {false, true, true, true};
    case 95: return {false, false, false, false, false, true};
    case 96: return {false, false, false, false, true};
    case 97: return {false, true, false, false, true};
    case 98: return {false, true, true, false, true};
    case 99: return {true};
    case 100: return {true, false, false, false, true};
    case 101: return {true, true};
    case 102: return {true, true, true};
    case 107: return {true, false, false, true};
    case 108: return {true, true, false, true};
    case 109: return {true, true, true, true};
    case 110: return {true, true, false, false, true};
    case 111: return {true, true, true, false, true};
    default: throw FormatError(0, "unsupported mesh class ordinal " + std::to_string(id));
    }
}

Material read_material(Reader& reader, int version) {
    Material result;
    for (auto& word : result.words) word = reader.integer();
    const int fog_words = version < 6 ? (version > 3 ? 4 : 2) : 0;
    for (int i = 0; i < fog_words; ++i) result.legacy_fog[i] = reader.integer();
    if (result.words[0] & 0x3fff) {
        result.texture = reader.string();
        result.texture_instance = reader.integer();
    }
    return result;
}

void write_material(Writer& writer, const Material& material, int version) {
    for (auto word : material.words) writer.integer(word);
    const int fog_words = version < 6 ? (version > 3 ? 4 : 2) : 0;
    for (int i = 0; i < fog_words; ++i) writer.integer(material.legacy_fog[i]);
    if (material.words[0] & 0x3fff) {
        writer.string(material.texture); writer.integer(material.texture_instance);
    }
}

MeshShape read_shape(Reader& reader, int version) {
    if (version < 3 || version > 9) reader.fail("unsupported mesh MLR version");
    MeshShape shape;
    const auto count = reader.count(48, 65536);
    shape.primitives.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        MeshPrimitive mesh;
        mesh.class_id = reader.integer();
        MeshLayout layout;
        try { layout = mesh_layout(mesh.class_id); }
        catch (const FormatError&) { reader.fail("unsupported mesh class ordinal " + std::to_string(mesh.class_id)); }
        mesh.positions = read_vectors<3>(reader);
        mesh.texcoords = read_vectors<2>(reader);
        const auto lengths = reader.count(1);
        mesh.lengths.reserve(lengths);
        for (uint32_t j = 0; j < lengths; ++j) mesh.lengths.push_back(static_cast<uint8_t>(reader.integer(1)));
        mesh.draw_mode = reader.integer();
        mesh.materials.push_back(read_material(reader, version));
        const auto indices = reader.count(2);
        mesh.indices.reserve(indices);
        for (uint32_t j = 0; j < indices; ++j) {
            const auto index = reader.integer(2);
            if (index >= mesh.positions.size()) reader.fail("mesh vertex index out of range");
            mesh.indices.push_back(static_cast<uint16_t>(index));
        }
        if (layout.dual || layout.detail) mesh.materials.push_back(read_material(reader, version));
        if (layout.detail) {
            for (size_t j = 0; j < (layout.triangles ? 6u : 4u); ++j) mesh.detail[j] = reader.scalar();
        }
        if (layout.multi) {
            const auto passes = reader.integer(1);
            // The stored count includes the already decoded base pass.
            if (passes < 1 || passes > 8 || passes - 1 > reader.remaining() / 28) reader.fail("invalid material pass count");
            for (uint32_t j = 1; j < passes; ++j) {
                mesh.materials.push_back(read_material(reader, version));
                mesh.extra_texcoords.push_back(read_vectors<2>(reader));
            }
        }
        if (layout.colored) {
            const auto colors = reader.count(4, 65536);
            mesh.colors.reserve(colors);
            for (uint32_t j = 0; j < colors; ++j) mesh.colors.push_back(reader.integer());
            if (mesh.colors.size() != mesh.positions.size()) reader.fail("mesh color count mismatch");
        }
        if (layout.lit) {
            mesh.normals = read_vectors<3>(reader);
            if (mesh.normals.size() != mesh.positions.size()) reader.fail("mesh normal count mismatch");
        }
        if (layout.triangles && indices % 3) reader.fail("incomplete triangle indices");
        if (!layout.triangles) {
            if (std::any_of(mesh.lengths.begin(), mesh.lengths.end(), [](auto n) { return n < 3; }) ||
                std::accumulate(mesh.lengths.begin(), mesh.lengths.end(), size_t{0}) != indices)
                reader.fail("invalid polygon lengths");
        }
        const auto uv_count = mesh.positions.size() * (layout.dual ? 2 : 1);
        if (!mesh.texcoords.empty() && mesh.texcoords.size() != uv_count)
            reader.fail("mesh texture coordinate count mismatch");
        for (const auto& uv : mesh.extra_texcoords)
            if (uv.size() != mesh.positions.size()) reader.fail("material pass UV count mismatch");
        for (const auto& point : mesh.positions) { include(mesh.bounds, point); include(shape.bounds, point); }
        shape.primitives.push_back(std::move(mesh));
    }
    return shape;
}

void write_shape(Writer& writer, const MeshShape& shape, int version) {
    writer.integer(static_cast<uint32_t>(shape.primitives.size()));
    for (const auto& mesh : shape.primitives) {
        const auto layout = mesh_layout(mesh.class_id);
        writer.integer(mesh.class_id);
        write_vectors(writer, mesh.positions); write_vectors(writer, mesh.texcoords);
        writer.integer(static_cast<uint32_t>(mesh.lengths.size()));
        for (auto length : mesh.lengths) writer.integer(length, 1);
        writer.integer(mesh.draw_mode);
        write_material(writer, mesh.materials[0], version);
        writer.integer(static_cast<uint32_t>(mesh.indices.size()));
        for (auto index : mesh.indices) writer.integer(index, 2);
        if (layout.dual || layout.detail) write_material(writer, mesh.materials[1], version);
        if (layout.detail)
            for (size_t i = 0; i < (layout.triangles ? 6u : 4u); ++i) writer.scalar(mesh.detail[i]);
        if (layout.multi) {
            writer.integer(static_cast<uint32_t>(mesh.materials.size()), 1);
            for (size_t i = 1; i < mesh.materials.size(); ++i) {
                write_material(writer, mesh.materials[i], version);
                write_vectors(writer, mesh.extra_texcoords[i - 1]);
            }
        }
        if (layout.colored) {
            writer.integer(static_cast<uint32_t>(mesh.colors.size()));
            for (auto color : mesh.colors) writer.integer(color);
        }
        if (layout.lit) write_vectors(writer, mesh.normals);
    }
}
}
