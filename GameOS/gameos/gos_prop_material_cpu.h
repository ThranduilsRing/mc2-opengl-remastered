#pragma once
#include "texture_decode.h"
#include "../../RenderCore/PropMaterialGpu.h"
#include "../../mclib/model_override_registry.h"
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <string_view>

namespace mc2prop {
struct MaterialSources {
    mc2tex::TextureArrayCpu normal, orm;
    std::vector<RenderCore::PropMaterialGpu> table;
    std::vector<int32_t> packetIds;
};

// Normal mips average vectors and renormalize; a cancelling footprint is flat.
inline void normalizeNormals(std::vector<uint8_t>& pixels) {
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        float n[3]; float len2 = 0;
        for (int c = 0; c < 3; ++c) { n[c] = pixels[i+c] / 127.5f - 1.0f; len2 += n[c]*n[c]; }
        if (len2 < 0.0001f) { n[0] = n[1] = 0; n[2] = 1; }
        else { const float r = 1.0f / std::sqrt(len2); for (float& v : n) v *= r; }
        for (int c = 0; c < 3; ++c)
            pixels[i+c] = static_cast<uint8_t>(std::clamp(std::lround((n[c] * 0.5f + 0.5f) * 255.0f), 0l, 255l));
    }
}

inline void appendLayer(mc2tex::TextureArrayCpu& a, uint32_t width, uint32_t height,
                        std::vector<uint8_t> pixels, bool normal) {
    a.width = width; a.height = height; a.vkFormat = mc2tex::kVkFormatRgba8Unorm;
    uint32_t w = width, h = height, level = 0;
    for (;;) {
        if (a.levels.size() <= level) a.levels.emplace_back();
        a.levels[level].insert(a.levels[level].end(), pixels.begin(), pixels.end());
        if (w == 1 && h == 1) break;
        pixels = mc2tex::boxDownsampleRgba8(pixels, w, h);
        if (normal) normalizeNormals(pixels);
        ++level;
        w = mc2tex::mipExtent(width, level);
        h = mc2tex::mipExtent(height, level);
    }
    ++a.layers;
}

inline bool addMaterial(MaterialSources& dst, const ModelOverrideRecord& owner,
                         const ModelOverrideMaterial& input) {
    auto read = [&](std::string_view source) -> std::optional<mc2tex::DecodedTexture> {
        if (source.empty()) return std::nullopt;
        const auto path = (std::filesystem::path(owner.manifestDir) / source).generic_string();
        return mc2tex::decodeFile(path.c_str(), gos_Texture_Alpha, 0);
    };
    auto normal = read(input.normalPath), orm = read(input.ormPath);
    if ((!input.normalPath.empty() && !normal) || (!input.ormPath.empty() && !orm)) return false;
    const auto* ref = normal ? &*normal : orm ? &*orm : nullptr;
    if (!ref) return false;
    if (dst.table.size() >= 64) return false;
    const uint32_t w = static_cast<uint32_t>(ref->width), h = static_cast<uint32_t>(ref->height);
    // One compact detail array per map. Authoring cooks a common extent; do not
    // silently stretch mismatched maps or accept an sRGB normal/ORM source.
    if (!w || !h || w > 2048 || h > 2048 || (w & (w-1)) || (h & (h-1)) ||
        (dst.normal.layers && (dst.normal.width != w || dst.normal.height != h))) return false;
    auto valid = [&](const auto& t) {
        return !t || (!t->isSrgb && t->layout == mc2tex::PixelLayout::RGBA8 &&
            t->width == static_cast<int>(w) && t->height == static_cast<int>(h) &&
            t->pixels.size() == static_cast<size_t>(w)*h*4);
    };
    if (!valid(normal) || !valid(orm)) return false;
    auto neutral = [&](uint8_t r, uint8_t g, uint8_t b) {
        std::vector<uint8_t> bytes(static_cast<size_t>(w)*h*4, 255);
        for (size_t i = 0; i < bytes.size(); i += 4) { bytes[i]=r; bytes[i+1]=g; bytes[i+2]=b; }
        return bytes;
    };
    RenderCore::PropMaterialGpu gpu{};
    gpu.material.albedoTex = RenderCore::kMaterialTexAbsent;
    gpu.material.normalTex = normal ? dst.normal.layers : RenderCore::kMaterialTexAbsent;
    gpu.material.metallicRoughnessTex = orm ? dst.orm.layers : RenderCore::kMaterialTexAbsent;
    gpu.material.emissiveTex = RenderCore::kMaterialTexAbsent;
    gpu.material.flags = (normal ? RenderCore::MaterialFlags::kNormalMap : 0u) |
        (orm ? RenderCore::MaterialFlags::kMetallicRoughness : 0u);
    gpu.material.baseColorFactor = 1;
    gpu.material.metallicFactor = input.metallic;
    gpu.material.roughnessFactor = input.roughness;
    gpu.uvScale[0]=input.uvScale[0]; gpu.uvScale[1]=input.uvScale[1];
    gpu.normalStrength=input.normalStrength; gpu.aoStrength=input.aoStrength;
    gpu.materialClass=input.materialClass;
    appendLayer(dst.normal, w, h, normal ? std::move(normal->pixels) : neutral(128,128,255), true);
    appendLayer(dst.orm, w, h, orm ? std::move(orm->pixels) : neutral(255,255,255), false);
    dst.table.push_back(gpu);
    return true;
}
}
