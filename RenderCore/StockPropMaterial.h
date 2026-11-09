#pragma once
#include "PropMaterialGpu.h"
#include "SurfaceMaterial.h"

namespace RenderCore {
// Stock/mod albedo-only props: classify by source texture, never by pixel colour.
// Painted tanks and vehicles are dielectrics; only explicitly bare metal conducts.
inline SurfaceClass stockPropClass(std::string_view name) {
    auto has = [name](std::string_view token) {
        for (size_t i = 0; i + token.size() <= name.size(); ++i) {
            size_t j = 0;
            for (; j < token.size(); ++j) {
                char c = name[i + j];
                if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (c != token[j]) break;
            }
            if (j == token.size()) return true;
        }
        return false;
    };
    if (has("tent") || has("canvas")) return SurfaceClass::Fabric;
    if (has("concrete") || has("cement") || has("wall")) return SurfaceClass::Concrete;
    if (has("grating") || has("chainlink")) return SurfaceClass::Grating;
    if (has("baremetal") || has("bare_metal")) return SurfaceClass::BareMetal;
    if (has("rust")) return SurfaceClass::Rust;
    if (has("wood") || has("crate")) return SurfaceClass::Wood;
    if (has("tank") || has("quonset") || has("vehicle") || has("tower")) return SurfaceClass::PaintedMetal;
    return SurfaceClass::Dielectric;
}

inline PropMaterialGpu stockPropMaterial(SurfaceClass c) {
    PropMaterialGpu row;
    row.material.albedoTex = row.material.normalTex = row.material.metallicRoughnessTex =
        row.material.emissiveTex = kMaterialTexAbsent;
    row.material.baseColorFactor = 1.0f;
    row.materialClass = static_cast<uint32_t>(c);
    row.material.metallicFactor = surfaceClassMayBeMetallic(c) ? 1.0f : 0.0f;
    switch (c) {
    case SurfaceClass::Concrete: case SurfaceClass::Fabric: case SurfaceClass::Rust:
    case SurfaceClass::Rubber: row.material.roughnessFactor = 0.9f; break;
    case SurfaceClass::BareMetal: case SurfaceClass::Grating: row.material.roughnessFactor = 0.7f; break;
    case SurfaceClass::PaintedMetal: case SurfaceClass::Wood: row.material.roughnessFactor = 0.8f; break;
    default: row.material.roughnessFactor = 0.85f; break;
    }
    return row;
}
}
