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
#include <unordered_map>

namespace mc2prop {
struct MaterialSources {
    mc2tex::TextureArrayCpu normal, orm;
    std::vector<RenderCore::PropMaterialGpu> table;
    std::vector<int32_t> packetIds;
    std::unordered_map<std::string, uint32_t> normalLayers, ormLayers;
    // Per-object textures (MaterialFlags::kPerTexture rows): each file keeps its own extent, format and complete
    // mip chain and is addressed by index in the shader's texture table, never packed into a shared-extent array.
    std::vector<RenderCore::KtxImage> textures;
    std::unordered_map<std::string, uint32_t> textureIndex;
};

inline constexpr size_t kMaxMaterials = 512; // bounded table; a mission may contain the whole kit
inline constexpr uint32_t kMaxDetailEdge = 2048;
inline constexpr size_t kMaxTextures = 1024;   // must match uPropTex[] in prop_material_vk.glsl

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

// Mixed mod inputs keep the RGBA path. A fully cooked array stays in its GPU
// block format, including every authored mip; no runtime codec or resampling.
inline void decodeDetailArray(mc2tex::TextureArrayCpu& a, bool normal) {
    if (!a.layers || a.vkFormat == mc2tex::kVkFormatRgba8Unorm) return;
    for (uint32_t level = 0; level < a.levels.size(); ++level) {
        RenderCore::KtxImage img;
        img.width = mc2tex::mipExtent(a.width, level); img.height = mc2tex::mipExtent(a.height, level);
        img.vkFormat = a.vkFormat; img.isCompressed = true; img.mipCount = 1; img.mipByteOffsets = {0};
        const size_t bytes = mc2tex::bc7LevelBytes(img.width, img.height);
        std::vector<uint8_t> decoded;
        for (uint32_t layer = 0; layer < a.layers; ++layer) {
            img.pixels.assign(a.levels[level].begin() + layer*bytes, a.levels[level].begin() + (layer+1)*bytes);
            std::vector<uint8_t> pixels;
            if (!RenderCore::ktxDecodeBlocksToRgba8(img, 0, pixels)) return;
            if (normal) rebuildNormalZ(pixels), normalizeNormals(pixels);
            decoded.insert(decoded.end(), pixels.begin(), pixels.end());
        }
        a.levels[level] = std::move(decoded);
    }
    a.vkFormat = mc2tex::kVkFormatRgba8Unorm;
}

inline bool addMaterial(MaterialSources& dst, const ModelOverrideRecord& owner,
                         const ModelOverrideMaterial& input) {
    if (dst.table.size() >= kMaxMaterials) return false;
    // Per-object path: every named map is a cooked KTX2 (BC5 normal, BC7 ORM, BC7 albedo) that keeps its own extent
    // and mip chain. Maps of one slot need not share a size.
    {
        struct Want { const std::string& source; uint32_t format, altFormat; };
        const Want wants[3] = {{input.normalPath, 141, 141}, {input.ormPath, 145, 145}, {input.albedoPath, 145, 146}};
        bool any = false, all = true;
        for (const auto& w : wants) {
            if (w.source.empty()) continue;
            any = true;
            all = all && isKtx2(w.source);
        }
        if (any && all) {
            RenderCore::PropMaterialGpu gpu{};
            gpu.material.albedoTex = gpu.material.normalTex = gpu.material.metallicRoughnessTex =
                gpu.material.emissiveTex = RenderCore::kMaterialTexAbsent;
            uint32_t* slots[3] = {&gpu.material.normalTex, &gpu.material.metallicRoughnessTex, &gpu.material.albedoTex};
            std::vector<std::string> keys;
            for (int i = 0; i < 3; ++i) {
                if (wants[i].source.empty()) continue;
                const auto path = (std::filesystem::path(owner.manifestDir) / wants[i].source).generic_string();
                auto it = dst.textureIndex.find(path);
                if (it == dst.textureIndex.end()) {
                    RenderCore::KtxImage image;
                    if (dst.textures.size() >= kMaxTextures || !RenderCore::ktxLoadRgba8(path.c_str(), image) ||
                        (image.vkFormat != wants[i].format && image.vkFormat != wants[i].altFormat) ||
                        image.width <= 0 || image.height <= 0 || image.width > int(kMaxDetailEdge) || image.height > int(kMaxDetailEdge) ||
                        mc2tex::mipExtent(image.width, image.mipCount-1) != 1 ||
                        mc2tex::mipExtent(image.height, image.mipCount-1) != 1) {
                        for (const auto& k : keys) { dst.textureIndex.erase(k); dst.textures.pop_back(); }
                        return false;
                    }
                    image.vkFormat = wants[i].format;      // albedo bytes are shaded as authored (encoded), like the tier arrays
                    image.isSrgb = false;
                    it = dst.textureIndex.emplace(path, static_cast<uint32_t>(dst.textures.size())).first;
                    dst.textures.push_back(std::move(image));
                    keys.push_back(path);
                }
                *slots[i] = it->second;
            }
            gpu.material.flags = RenderCore::MaterialFlags::kPerTexture |
                (wants[0].source.empty() ? 0u : RenderCore::MaterialFlags::kNormalMap) |
                (wants[1].source.empty() ? 0u : RenderCore::MaterialFlags::kMetallicRoughness);
            gpu.material.baseColorFactor = 1; gpu.material.metallicFactor = input.metallic; gpu.material.roughnessFactor = input.roughness;
            gpu.uvScale[0] = input.uvScale[0]; gpu.uvScale[1] = input.uvScale[1];
            gpu.normalStrength = input.normalStrength; gpu.aoStrength = input.aoStrength;
            gpu.materialClass = input.materialClass; gpu.reserved[0] = input.alphaMode; gpu.reserved[1] = 1; // BC5 XY
            dst.table.push_back(gpu);
            return true;
        }
    }
    auto cooked = [&](std::string_view source, uint32_t format) -> std::optional<RenderCore::KtxImage> {
        if (!isKtx2(source)) return std::nullopt;
        RenderCore::KtxImage image;
        const auto path = (std::filesystem::path(owner.manifestDir) / source).generic_string();
        if (!RenderCore::ktxLoadRgba8(path.c_str(), image) || image.vkFormat != format ||
            image.width > kMaxDetailEdge || image.height > kMaxDetailEdge ||
            mc2tex::mipExtent(image.width, image.mipCount-1) != 1 ||
            mc2tex::mipExtent(image.height, image.mipCount-1) != 1) return std::nullopt;
        return image;
    };
    const auto n = cooked(input.normalPath, 141), o = cooked(input.ormPath, 145);
    auto compatible = [](const mc2tex::TextureArrayCpu& a, const RenderCore::KtxImage& image) {
        return !a.layers || (a.vkFormat == image.vkFormat && a.width == image.width && a.height == image.height &&
                             a.levels.size() == image.mipByteOffsets.size());
    };
    if (n && o && n->width == o->width && n->height == o->height &&
        compatible(dst.normal, *n) && compatible(dst.orm, *o)) {
        auto append = [&](auto& a, auto& layers, const auto& image, std::string_view source) {
            const auto path = (std::filesystem::path(owner.manifestDir) / source).generic_string();
            if (const auto it = layers.find(path); it != layers.end()) { const auto& [pathKey, cachedLayer] = *it; return cachedLayer; }
            const auto layer = a.layers++;
            a.width = image.width; a.height = image.height; a.vkFormat = image.vkFormat;
            a.levels.resize(image.mipCount);
            for (int level = 0; level < image.mipCount; ++level) {
                const auto begin = image.pixels.begin() + image.mipByteOffsets[level];
                const auto bytes = mc2tex::bc7LevelBytes(mc2tex::mipExtent(a.width, level), mc2tex::mipExtent(a.height, level));
                a.levels[level].insert(a.levels[level].end(), begin, begin + bytes);
            }
            layers.emplace(path, layer);
            return layer;
        };
        RenderCore::PropMaterialGpu gpu{};
        gpu.material.albedoTex = gpu.material.emissiveTex = RenderCore::kMaterialTexAbsent;
        gpu.material.normalTex = append(dst.normal, dst.normalLayers, *n, input.normalPath);
        gpu.material.metallicRoughnessTex = append(dst.orm, dst.ormLayers, *o, input.ormPath);
        gpu.material.flags = RenderCore::MaterialFlags::kNormalMap | RenderCore::MaterialFlags::kMetallicRoughness;
        gpu.material.baseColorFactor = 1; gpu.material.metallicFactor = input.metallic; gpu.material.roughnessFactor = input.roughness;
        gpu.uvScale[0] = input.uvScale[0]; gpu.uvScale[1] = input.uvScale[1];
        gpu.normalStrength = input.normalStrength; gpu.aoStrength = input.aoStrength;
        gpu.materialClass = input.materialClass; gpu.reserved[0] = input.alphaMode; gpu.reserved[1] = 1; // BC5 XY
        dst.table.push_back(gpu);
        return true;
    }
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
    decodeDetailArray(dst.normal, true); decodeDetailArray(dst.orm, false);
    for (auto& row : dst.table) row.reserved[1] = 0;
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

// The per-packet alpha class (0 opaque, 1 normal core/fringe coverage, 2 late fringe: the depth-writing core plus a
// fringe composited after vegetation) for a packet whose texture has alpha. Coverage semantics are explicit, never
// inferred from override provenance: an authored slot's explicit alpha mode decides (Mask = thin geometry that must
// survive later vegetation, Opaque = no test, Stock = stock coverage), a package's unmapped slots keep the stock coverage
// policy, and a record's explicit lateCoverageFringe opt-in (procedural fences, which carry no material package)
// selects the late fringe for every non-opaque packet. Class 2 shares class 1's LOD-aware coverage: a fixed hard cutoff
// loses minified wire entirely.
inline uint32_t packetAlphaClass(bool isOverride, const ModelOverrideRecord* source, uint32_t textureSlot) {
    const uint32_t late = isOverride && source && source->lateCoverageFringe ? 2u : 1u;
    if (isOverride && source && !source->materials.empty()) {
        for (const auto& m : source->materials)
            if (m.slot == textureSlot)
                switch (static_cast<RenderCore::SurfaceAlpha>(m.alphaMode)) {
                    case RenderCore::SurfaceAlpha::Opaque: return 0u;
                    case RenderCore::SurfaceAlpha::Mask:   return 2u;
                    default:                               return late;
                }
    }
    return late;
}
}
