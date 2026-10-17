// tools/asset_viewer/MeshGpu.h
// Upload a CPU MeshData to Vulkan buffers (vertex + index buffer per submesh + per-submesh albedo).
// No engine headers; toolvk (tools/common/tool_vk_gpu.h) + C++17.
#pragma once
#include "TglMeshLoader.h"
#include "tool_vk_gpu.h"
#include <string>
#include <vector>
#include <cstdint>

struct GpuSubMesh {
    toolvk::Buffer vb, ib;
    uint32_t albedo = 0;   // toolvk texture id, 0 = none
    int      indexCount = 0;
    bool     ownsAlbedo = false;
    // Lowercased stem of the source texture name (no extension, no path).
    // e.g. "a_2civliving" or "a_2civlivingx".
    std::string texStem;
    // True when this submesh's shapes have node names starting with "SpotLight_".
    // These are forward headlight/spotlight beam geometry that obscure the model.
    bool isSpotlight = false;
};

class MeshGpu {
public:
    // Upload all submeshes from MeshData to GL.
    // Resolves per-submesh albedo at the preferred tier first, then falls back.
    // preferredTier must be one of {128, 256, 512, 1024}; default 512.
    void upload(const MeshData& m, const std::string& deployDir, int preferredTier = 512);

    // Reload albedo textures at a new resolution tier (hot-swap).
    // VAO/VBO/EBO are untouched; only textures are freed (if owned) and re-resolved.
    void reloadAlbedo(const std::string& deployDir, int preferredTier);

    // The submeshes in draw order; the caller issues one toolvk::Pass::draw per entry (skipping isSpotlight when lights are hidden).
    const std::vector<GpuSubMesh>& subs() const { return subs_; }

    // Free all GPU objects.
    void destroy();

    bool valid() const { return !subs_.empty(); }

    const float* bmin() const { return bmin_; }
    const float* bmax() const { return bmax_; }

private:
    std::vector<GpuSubMesh> subs_;
    float bmin_[3]{};
    float bmax_[3]{};
};
