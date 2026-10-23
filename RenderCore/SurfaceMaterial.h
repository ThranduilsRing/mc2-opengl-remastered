// RenderCore/SurfaceMaterial.h -- the one runtime material schema (C++ side).
//
// WHAT: the material class and alpha-mode vocabulary shared by the content cooker
// (tools/content_pipeline/ir.py MATERIAL_CLASSES / CLASS_DEFAULTS; a pytest pins the
// two lists together), the override registry (models.json "materials" v2) and the
// shaders (shaders/include/surface_material.hglsl SurfaceMaterial.materialClass).
// Props, buildings and imported mechs evaluate their inputs into SurfaceMaterial
// { linear baseColor, render-space normal, roughness, metallic, ao, linear emission,
// alphaMode, materialClass }; lighting composition consumes it unchanged.
// WHY: metal and roughness come from an explicit, reviewed per-part class (or
// authored maps), never from colour; LegacyPrelit marks stock art that keeps its
// vertex-lit appearance and gets zero new specular.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace RenderCore {

enum class SurfaceClass : uint32_t {
    LegacyPrelit = 0, Dielectric, PaintedMetal, BareMetal, Rust,
    Concrete, Glass, Grating, Wood, Fabric, Rubber, Emissive,
    Count
};

inline constexpr std::array<std::string_view, static_cast<size_t>(SurfaceClass::Count)> kSurfaceClassNames = {
    "legacy_prelit", "dielectric", "painted_metal", "bare_metal", "rust",
    "concrete", "glass", "grating", "wood", "fabric", "rubber", "emissive",
};

struct SurfaceClassDefaults { float metallic; float roughness; };
// Used when an authored slot carries no ORM map / scalar. Same table as ir.CLASS_DEFAULTS.
inline constexpr std::array<SurfaceClassDefaults, static_cast<size_t>(SurfaceClass::Count)> kSurfaceClassDefaults = {{
    {0.0f, 1.0f}, {0.0f, 0.8f}, {0.0f, 0.55f}, {1.0f, 0.45f}, {0.0f, 0.9f},
    {0.0f, 0.9f}, {0.0f, 0.08f}, {1.0f, 0.5f}, {0.0f, 0.8f}, {0.0f, 0.95f}, {0.0f, 0.9f},
    {0.0f, 0.9f},
}};

constexpr bool surfaceClassMayBeMetallic(SurfaceClass c) {
    return c == SurfaceClass::BareMetal || c == SurfaceClass::Grating;
}

inline std::optional<SurfaceClass> surfaceClassFromName(std::string_view name) {
    for (size_t i = 1; i < kSurfaceClassNames.size(); ++i)   // LegacyPrelit is never authored by name
        if (kSurfaceClassNames[i] == name) return static_cast<SurfaceClass>(i);
    return std::nullopt;
}

// Stock = the stock coverage policy (soft fringe pass, stock alpha); Mask = depth-
// writing cutout at alphaCutoff; Opaque = never alpha-tested; Blend is reserved.
enum class SurfaceAlpha : uint32_t { Stock = 0, Opaque = 1, Mask = 2, Blend = 3 };

inline std::optional<SurfaceAlpha> surfaceAlphaFromName(std::string_view name) {
    if (name == "STOCK") return SurfaceAlpha::Stock;
    if (name == "OPAQUE") return SurfaceAlpha::Opaque;
    if (name == "MASK") return SurfaceAlpha::Mask;
    if (name == "BLEND") return SurfaceAlpha::Blend;
    return std::nullopt;
}

}  // namespace RenderCore
