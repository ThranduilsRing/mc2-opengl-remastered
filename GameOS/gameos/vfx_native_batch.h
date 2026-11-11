#pragma once
// One frame's native indexed VFX runs (gosFX packets, tube ribbons, mesh effects) gathered into one vertex/index/draw set, so the island
// records ONE render pass and ONE submit for them instead of one per bridge. No Vulkan; doctest: tests/unit/test_vfx_draw_plan.cpp.
// Each append rebases like the island would see a single call: indices become absolute vertex indices, draw ranges become absolute
// index ranges. A range or index that was invalid in its own call stays invalid (UINT32_MAX), so the island still skips that draw.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include "fx/fx_render.h"
#include "vulkan_vfx_island.h"

namespace mc2vfx {

struct NativeBatch {
    bool open = false;
    std::vector<mc2::fx::RenderVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<VfxParticleDraw> draws;
    float mvp[16]{};
    float camRight[3]{}, camUp[3]{};
    bool hasMvp = false, hasCam = false;
    int w = 0, h = 0;

    void begin() {   // capacity kept across frames
        open = true;
        vertices.clear(); indices.clear(); draws.clear();
        hasMvp = hasCam = false;
    }

    void append(const mc2::fx::RenderVertex* v, uint32_t vertexCount, const uint32_t* idx, uint32_t indexCount,
                const VfxParticleDraw* d, uint32_t drawCount, const float* worldToClip, const float* right, const float* up,
                int width, int height) {
        const auto vertexBase = static_cast<uint32_t>(vertices.size());
        const auto indexBase = static_cast<uint32_t>(indices.size());
        vertices.insert(vertices.end(), v, v + vertexCount);
        const size_t at = indices.size();
        indices.resize(at + indexCount);
        for (uint32_t i = 0; i < indexCount; ++i) indices[at + i] = idx[i] >= vertexCount ? UINT32_MAX : idx[i] + vertexBase;
        for (uint32_t gi = 0; gi < drawCount; ++gi) {
            VfxParticleDraw draw = d[gi];
            draw.start = static_cast<uint64_t>(draw.start) + draw.count > indexCount ? UINT32_MAX : draw.start + indexBase;
            draws.push_back(draw);
        }
        std::memcpy(mvp, worldToClip, sizeof(mvp));
        hasMvp = true;
        if (right && up) {
            std::memcpy(camRight, right, sizeof(camRight));
            std::memcpy(camUp, up, sizeof(camUp));
            hasCam = true;
        }
        w = width; h = height;
    }
};

}  // namespace mc2vfx
