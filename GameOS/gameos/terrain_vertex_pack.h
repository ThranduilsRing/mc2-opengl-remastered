#pragma once

#include "gos_terrain_lighting.h"
#include <vector>

namespace gos_terrain_lighting {

// The source revision covers elevation, normal, shadow and water. Window XY,
// source presence and transient calc bits are checked separately: no candidate
// vertex is packed on a clean frame, including camera re-entry.
class TerrainVertexPack {
public:
    void reset(uint32_t count) {
        vertices.assign(count, {});
        versions.assign(count, 0);
        sourceRevisions_.assign(count, 0);
        sourcePresent_.assign(count, false);
    }
    template<class Vertex> bool update(const Vertex& v, uint64_t revision) {
        const auto index = static_cast<uint32_t>(v.vertexNum);
        auto& out = vertices[index];
        const uint32_t calc = v.pVertex ?
            ((v.calcThisFrame & 1u) ? GPU_VERT_CALCFRAME_LIGHT : 0u) |
            ((v.calcThisFrame & 2u) ? GPU_VERT_CALCFRAME_WATER : 0u) : 0u;
        if (versions[index] != 0 && sourceRevisions_[index] == revision &&
            sourcePresent_[index] == (v.pVertex != nullptr) &&
            out.xy[0] == v.vx && out.xy[1] == v.vy &&
            (out.flags & (GPU_VERT_CALCFRAME_LIGHT | GPU_VERT_CALCFRAME_WATER)) == calc)
            return false;

        out.xy[0] = v.vx;
        out.xy[1] = v.vy;
        out.elevation = v.pVertex ? v.pVertex->elevation : 0.0f;
        out.hazeFactor = 0.0f;
        out.normal[0] = v.pVertex ? v.pVertex->vertexNormal.x : 0.0f;
        out.normal[1] = v.pVertex ? v.pVertex->vertexNormal.y : 0.0f;
        out.normal[2] = v.pVertex ? v.pVertex->vertexNormal.z : 1.0f;
        out.flags = calc;
        if (v.pVertex) {
            if (v.pVertex->shadow) out.flags |= GPU_VERT_SHADOW;
            if (v.pVertex->water & 1u) out.flags |= GPU_VERT_WATER;
            if (v.pVertex->water & 128u) out.flags |= GPU_VERT_WATER_ANIM_NEG;
            if (v.pVertex->water & 64u) out.flags |= GPU_VERT_WATER_ANIM_POS;
        }
        sourceRevisions_[index] = revision;
        sourcePresent_[index] = v.pVertex != nullptr;
        versions[index] = ++serial_;
        return true;
    }
    std::vector<GpuTerrainVertexInput> vertices;
    std::vector<uint64_t> versions;
private:
    std::vector<uint64_t> sourceRevisions_;
    std::vector<bool> sourcePresent_;
    uint64_t serial_ = 0;
};
} // namespace gos_terrain_lighting
