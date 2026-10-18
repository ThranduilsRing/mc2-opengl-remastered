// tools/asset_viewer/MeshGpu.cpp
// Uploads MeshData to Vulkan buffers and resolves albedo textures via the decoder registry.
#include "MeshGpu.h"
#include "TextureDecoderRegistry.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>

// ---------------------------------------------------------------------------
// Albedo resolution helpers
// ---------------------------------------------------------------------------

// Return the filename stem, lowercased ("A_2CivLiving.tga" -> "a_2civliving").
static std::string stemLower(const std::string& textureName) {
    namespace fs = std::filesystem;
    std::string s = fs::path(textureName).stem().string();
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Try tiers in order starting from preferredTier, then the remaining ones.
// Returns the first existing path, or "" if none found.
// Valid tiers: 128, 256, 512, 1024.
static std::string resolveTexPath(const std::string& deployDir,
                                   const std::string& stem,
                                   int preferredTier) {
    // Build the ordered list: preferred first, then the rest in descending order.
    static const int kAllTiers[] = { 512, 256, 128, 1024 };
    // Try preferred tier first.
    auto tryPath = [&](int tier) -> std::string {
        std::string p = deployDir + "/data/tgl/" + std::to_string(tier) + "/" + stem + ".ktx2";
        if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return p; }
        return {};
    };

    std::string p = tryPath(preferredTier);
    if (!p.empty()) return p;

    // Fall back through the remaining tiers.
    for (int tier : kAllTiers) {
        if (tier == preferredTier) continue;
        p = tryPath(tier);
        if (!p.empty()) return p;
    }
    return {};
}

// ---------------------------------------------------------------------------
// MeshGpu::upload
// ---------------------------------------------------------------------------
void MeshGpu::upload(const MeshData& m, const std::string& deployDir, int preferredTier) {
    destroy();

    for (const SubMesh& sub : m.submeshes) {
        if (sub.verts.empty()) continue;

        GpuSubMesh gs{};
        gs.indexCount = (int)sub.idx.size();

        // Interleaved MeshVertex (stride 32): loc0 position, loc1 normal, loc2 uv.
        if (!toolvk::createBuffer(gs.vb, sub.verts.data(), sub.verts.size() * sizeof(MeshVertex), false) ||
            !toolvk::createBuffer(gs.ib, sub.idx.data(), sub.idx.size() * sizeof(uint32_t), true)) {
            toolvk::destroyBuffer(gs.vb); toolvk::destroyBuffer(gs.ib);
            continue;
        }

        // Carry the spotlight flag from the CPU submesh.
        gs.isSpotlight = sub.isSpotlight;

        // Resolve albedo.
        if (!sub.textureName.empty()) {
            gs.texStem = stemLower(sub.textureName);

            std::string path = resolveTexPath(deployDir, gs.texStem, preferredTier);
            if (!path.empty()) {
                DecodedTexture dt = textureDecoderRegistry().load(path);
                if (dt.texId) {
                    gs.albedo     = dt.texId;
                    gs.ownsAlbedo = dt.ownsTexture;
                }
            }
        }

        subs_.push_back(gs);
    }

    // Copy bounds
    for (int i = 0; i < 3; ++i) { bmin_[i] = m.bmin[i]; bmax_[i] = m.bmax[i]; }
}

// ---------------------------------------------------------------------------
// MeshGpu::reloadAlbedo — hot-swap textures at a new tier; geometry unchanged.
// ---------------------------------------------------------------------------
void MeshGpu::reloadAlbedo(const std::string& deployDir, int preferredTier) {
    for (GpuSubMesh& gs : subs_) {
        // Free the current owned texture.
        if (gs.albedo && gs.ownsAlbedo) {
            toolvk::destroyTexture(gs.albedo);
        }
        gs.albedo     = 0;
        gs.ownsAlbedo = false;

        if (!gs.texStem.empty()) {
            std::string path = resolveTexPath(deployDir, gs.texStem, preferredTier);
            if (!path.empty()) {
                DecodedTexture dt = textureDecoderRegistry().load(path);
                if (dt.texId) {
                    gs.albedo     = dt.texId;
                    gs.ownsAlbedo = dt.ownsTexture;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// MeshGpu::destroy
// ---------------------------------------------------------------------------
void MeshGpu::destroy() {
    for (GpuSubMesh& gs : subs_) {
        toolvk::destroyBuffer(gs.vb);
        toolvk::destroyBuffer(gs.ib);
        if (gs.albedo && gs.ownsAlbedo) toolvk::destroyTexture(gs.albedo);
    }
    subs_.clear();
    for (int i = 0; i < 3; ++i) bmin_[i] = bmax_[i] = 0.0f;
}
