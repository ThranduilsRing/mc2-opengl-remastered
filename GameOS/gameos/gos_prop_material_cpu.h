// gos_prop_material_cpu.h -- CPU assembly of authored static-prop material inputs (SurfaceMaterial rows).
//
// WHAT: turns a model override's material slots (models.json materials v1/v2) into the native prop
// material table plus compact normal / ORM texture arrays the Vulkan colour island uploads. Maps are
// .tga or .ktx2 (the content cook contract: BC5 normal, BC7 linear ORM, full mip chain); a slot with no
// maps is a class-scalar material (metal/roughness from the declared class, full coverage).
// WHY: presence is decoded-and-valid, never declared-only: a missing or malformed optional map rejects
// that slot (logged by the caller) and the packet keeps the legacy response with no added specular.
#pragma once
#include "texture_decode.h"
#include "../../RenderCore/PropMaterialGpu.h"
#include "../../RenderCore/SurfaceMaterial.h"
#include "../../mclib/model_override_registry.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string_view>

namespace mc2prop {
struct MaterialSources {
    mc2tex::TextureArrayCpu normal, orm;
    std::vector<RenderCore::PropMaterialGpu> table;
    std::vector<int32_t> packetIds;
};

inline constexpr size_t kMaxMaterials = 64;
inline constexpr uint32_t kMaxDetailEdge = 2048;

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

// BC5 stores X/Y only; rebuild Z (tangent space, so Z >= 0) before the RGBA8 array sees it.
inline void rebuildNormalZ(std::vector<uint8_t>& pixels) {
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const float x = pixels[i] / 127.5f - 1.0f, y = pixels[i+1] / 127.5f - 1.0f;
        const float z = std::sqrt(std::max(0.0f, 1.0f - x*x - y*y));
        pixels[i+2] = static_cast<uint8_t>(std::clamp(std::lround((z * 0.5f + 0.5f) * 255.0f), 0l, 255l));
        pixels[i+3] = 255;
    }
}

// Appends one layer. A complete cooked mip chain is used as stored (normals renormalized after the
// block decode); otherwise the chain is box-filtered here.
inline void appendLayer(mc2tex::TextureArrayCpu& a, uint32_t width, uint32_t height,
                        std::vector<uint8_t> pixels, bool normal,
                        const std::vector<std::vector<uint8_t>>* cooked = nullptr) {
    a.width = width; a.height = height; a.vkFormat = mc2tex::kVkFormatRgba8Unorm;
    uint32_t w = width, h = height, level = 0;
    for (;;) {
        if (a.levels.size() <= level) a.levels.emplace_back();
        a.levels[level].insert(a.levels[level].end(), pixels.begin(), pixels.end());
        if (w == 1 && h == 1) break;
        ++level;
        if (cooked && level < cooked->size()) {
            pixels = (*cooked)[level];
            if (normal) rebuildNormalZ(pixels), normalizeNormals(pixels);
        } else {
            pixels = mc2tex::boxDownsampleRgba8(pixels, w, h);
            if (normal) normalizeNormals(pixels);
        }
        w = mc2tex::mipExtent(width, level);
        h = mc2tex::mipExtent(height, level);
    }
    ++a.layers;
}

inline bool isKtx2(std::string_view path) {
    return path.size() > 5 && (path.substr(path.size() - 5) == ".ktx2" || path.substr(path.size() - 5) == ".KTX2");
}

inline bool addMaterial(MaterialSources& dst, const ModelOverrideRecord& owner,
                         const ModelOverrideMaterial& input) {
    if (dst.table.size() >= kMaxMaterials) return false;
    auto read = [&](std::string_view source) -> std::optional<mc2tex::DecodedTexture> {
        if (source.empty()) return std::nullopt;
        const auto path = (std::filesystem::path(owner.manifestDir) / source).generic_string();
        // KTX2: decode the whole cooked chain; TGA: level 0, chain built in appendLayer.
        return mc2tex::decodeFile(path.c_str(), gos_Texture_Alpha, isKtx2(source) ? gosHint_MipmapFilter0 : 0);
    };
    auto normal = read(input.normalPath), orm = read(input.ormPath);
    if ((!input.normalPath.empty() && !normal) || (!input.ormPath.empty() && !orm)) return false;

    RenderCore::PropMaterialGpu gpu{};
    gpu.material.albedoTex = RenderCore::kMaterialTexAbsent;
    gpu.material.normalTex = RenderCore::kMaterialTexAbsent;
    gpu.material.metallicRoughnessTex = RenderCore::kMaterialTexAbsent;
    gpu.material.emissiveTex = RenderCore::kMaterialTexAbsent;
    gpu.material.baseColorFactor = 1;
    gpu.material.metallicFactor = input.metallic;
    gpu.material.roughnessFactor = input.roughness;
    gpu.uvScale[0] = input.uvScale[0]; gpu.uvScale[1] = input.uvScale[1];
    gpu.normalStrength = input.normalStrength; gpu.aoStrength = input.aoStrength;
    gpu.materialClass = input.materialClass;
    gpu.reserved[0] = input.alphaMode;

    const auto* ref = normal ? &*normal : orm ? &*orm : nullptr;
    if (!ref) {                       // class-scalar material: no layers, full coverage
        dst.table.push_back(gpu);
        return true;
    }
    const uint32_t w = static_cast<uint32_t>(ref->width), h = static_cast<uint32_t>(ref->height);
    // One compact detail array per map. Authoring cooks a common extent; do not
    // silently stretch mismatched maps or accept an sRGB normal/ORM source.
    if (!w || !h || w > kMaxDetailEdge || h > kMaxDetailEdge || (w & (w-1)) || (h & (h-1)) ||
        (dst.normal.layers && (dst.normal.width != w || dst.normal.height != h))) return false;
    auto valid = [&](const auto& t) {
        return !t || (!t->isSrgb && t->layout == mc2tex::PixelLayout::RGBA8 &&
            t->width == static_cast<int>(w) && t->height == static_cast<int>(h) &&
            t->pixels.size() == static_cast<size_t>(w)*h*4);
    };
    if (!valid(normal) || !valid(orm)) return false;
    auto chain = [](const auto& t) -> const std::vector<std::vector<uint8_t>>* {
        return t && t->mipLevels.size() > 1 ? &t->mipLevels : nullptr;
    };
    auto neutral = [&](uint8_t r, uint8_t g, uint8_t b) {
        std::vector<uint8_t> bytes(static_cast<size_t>(w)*h*4, 255);
        for (size_t i = 0; i < bytes.size(); i += 4) { bytes[i]=r; bytes[i+1]=g; bytes[i+2]=b; }
        return bytes;
    };
    if (normal && normal->sourceVkFormat == 141u) rebuildNormalZ(normal->pixels);   // BC5
    gpu.material.normalTex = normal ? dst.normal.layers : RenderCore::kMaterialTexAbsent;
    gpu.material.metallicRoughnessTex = orm ? dst.orm.layers : RenderCore::kMaterialTexAbsent;
    gpu.material.flags = (normal ? RenderCore::MaterialFlags::kNormalMap : 0u) |
        (orm ? RenderCore::MaterialFlags::kMetallicRoughness : 0u);
    appendLayer(dst.normal, w, h, normal ? std::move(normal->pixels) : neutral(128,128,255), true, chain(normal));
    appendLayer(dst.orm, w, h, orm ? std::move(orm->pixels) : neutral(255,255,255), false, chain(orm));
    dst.table.push_back(gpu);
    return true;
}

// The per-packet alpha class (0 opaque, 1 stock coverage, 2 depth-writing cutout) for a packet whose
// texture has alpha. An authored slot's explicit alpha mode wins. A package that declares materials
// owns its unmapped slots too: those are legacy (stock-derived) slots and keep the stock coverage
// policy. An override with no material package at all is a cutout (procedural fences); stock is coverage.
inline uint32_t packetAlphaClass(bool isOverride, const ModelOverrideRecord* source, uint32_t textureSlot) {
    if (isOverride && source && !source->materials.empty()) {
        for (const auto& m : source->materials)
            if (m.slot == textureSlot)
                switch (static_cast<RenderCore::SurfaceAlpha>(m.alphaMode)) {
                    case RenderCore::SurfaceAlpha::Opaque: return 0u;
                    case RenderCore::SurfaceAlpha::Mask:   return 2u;
                    default:                               return 1u;
                }
        return 1u;
    }
    return isOverride ? 2u : 1u;
}
}
