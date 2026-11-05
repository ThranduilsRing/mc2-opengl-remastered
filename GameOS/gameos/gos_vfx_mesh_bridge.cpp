// CPU mesh cache and native Vulkan VFX submission. No renderer-owned GL objects.
#include "gos_vfx_mesh_bridge.h"
#include "vulkan_vfx_geometry.h"
#include "gos_postprocess.h"
#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>
#include <span>
#include "mlr/mlrtexturepool.hpp"
#include "mlr/mlrtexture.hpp"
extern const float* gos_GetTerrainMVPMat4();

namespace {
struct Mesh {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 2>> uvs;
    std::vector<uint16_t> indices;
};
std::unordered_map<uint64_t, Mesh> meshes;
std::vector<GosVfxMeshInstance> pending;
}

namespace {
void flush(std::span<const GosVfxMeshUpload> uploads, std::span<const GosVfxMeshInstance> instances) {
    for (const auto& up : uploads) {
        if (meshes.contains(up.meshId) || !up.positions || !up.indices || !up.vertexCount || !up.indexCount) continue;
        Mesh mesh;
        mesh.positions.resize(up.vertexCount);
        mesh.uvs.resize(up.vertexCount);
        for (unsigned i = 0; i < up.vertexCount; ++i) {
            std::copy_n(up.positions + i * 3, 3, mesh.positions[i].data());
            if (up.uvs) std::copy_n(up.uvs + i * 2, 2, mesh.uvs[i].data());
        }
        mesh.indices.assign(up.indices, up.indices + up.indexCount);
        if (std::any_of(mesh.indices.begin(), mesh.indices.end(), [&](auto i) { return i >= up.vertexCount; })) continue;
        meshes.emplace(up.meshId, std::move(mesh));
    }
    pending.insert(pending.end(), instances.begin(), instances.end());
}
}

extern "C" void gos_vfx_mesh_flush_deferred() {
    const auto instances = std::move(pending);
    pending.clear();
    if (instances.empty()) return;
#ifdef MC2_VULKAN_ISLAND
    if (!mc2_vk_vfx_island_enabled()) return;
    const float* mvp = gos_GetTerrainMVPMat4();
    auto* pp = getGosPostProcess();
    if (!mvp || !pp) return;
    std::vector<VfxGeometryVertex> vertices;
    std::vector<VfxParticleDraw> draws;
    for (const auto& inst : instances) {
        const auto it = meshes.find(inst.meshId);
        if (it == meshes.end()) continue;
        const auto& [meshKey, mesh] = *it;
        VfxParticleDraw draw{};
        draw.start = static_cast<uint32_t>(vertices.size());
        draw.count = static_cast<uint32_t>(mesh.indices.size());
        if (inst.mlrTexHandle && MidLevelRenderer::MLRTexturePool::Instance) {
            auto* entry = (*MidLevelRenderer::MLRTexturePool::Instance)[inst.mlrTexHandle];
            if (entry && entry->GetImage()) draw.texture = entry->GetImage()->GetHandle();
        }
        const unsigned blend = static_cast<unsigned>(inst.blendMode);
        draw.additive = blend == 3 ? 0u : blend;
        draw.atlasColumns = kVfxGeometryAtlas;
        for (auto i : mesh.indices) {
            VfxGeometryVertex v{};
            const auto& p = mesh.positions[i];
            // modelToWorld is the same column-major affine the GL shader read.
            for (unsigned a = 0; a < 3; ++a)
                v.position[a] = inst.scale * (inst.modelToWorld[a] * p[0] + inst.modelToWorld[4+a] * p[1] +
                                               inst.modelToWorld[8+a] * p[2]) + inst.modelToWorld[12+a];
            std::copy_n(inst.rgba, 4, v.color);
            if (blend == 2 || blend == 3) v.color[3] = 1.0f;
            std::copy_n(mesh.uvs[i].data(), 2, v.uv);
            v.flags = VfxGeometryMesh | (inst.mlrTexHandle == 0 ? VfxGeometryUntextured : 0u);
            vertices.push_back(v);
        }
        draws.push_back(draw);
    }
    if (!vertices.empty()) mc2_vk_vfx_island_run(vertices.data(), static_cast<uint32_t>(vertices.size()),
        draws.data(), static_cast<uint32_t>(draws.size()), mvp, nullptr, nullptr, pp->getWidth(), pp->getHeight());
#endif
}

extern "C" void gos_vfx_mesh_fixture(const float origin[3]) {
    constexpr float positions[] = {-20, 0, 0, 20, 0, 0, 0, 35, 0};
    constexpr float uvs[] = {0, 0, 1, 0, 0.5f, 1};
    constexpr uint16_t indices[] = {0, 1, 2};
    constexpr uint64_t id = UINT64_MAX;
    const GosVfxMeshUpload upload{id, positions, uvs, indices, 3, 3};
    GosVfxMeshInstance instances[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        auto& inst = instances[i];
        inst.meshId = id;
        inst.modelToWorld[0] = inst.modelToWorld[5] = inst.modelToWorld[10] = inst.modelToWorld[15] = 1;
        std::copy_n(origin, 3, inst.modelToWorld + 12);
        inst.modelToWorld[12] += i == 0 ? -30.0f : 30.0f;
        inst.scale = i == 0 ? 1.0f : 0.75f;
        inst.rgba[0] = i == 0 ? 1.0f : 0.0f;
        inst.rgba[1] = i == 0 ? 0.0f : 1.0f;
        inst.rgba[2] = 1.0f;
        inst.rgba[3] = 0.75f;
        inst.blendMode = static_cast<int>(i);
    }
    gos_vfx_mesh_flush(&upload, 1, instances, 2);
}

// Preserve the existing C ABI at the boundary; the native implementation uses spans.
extern "C" void gos_vfx_mesh_flush(const GosVfxMeshUpload* uploads,
                                   unsigned int numUploads,
                                   const GosVfxMeshInstance* instances,
                                   unsigned int numInstances) {
    flush({uploads, uploads ? numUploads : 0u}, {instances, instances ? numInstances : 0u});
}
