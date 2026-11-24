// Shared offline and mission-load evaluator of the existing terrain macro-color response.
#pragma once
#include "terrain_surface_asset.h"

namespace mc2terrain {
struct SurfaceCookInput {
    std::span<const SurfaceColor> color;
    // The original resolved atlas can exceed the cooked image size. Sample bytes directly
    // rather than expanding a 5120+ square source to a second float image at load time.
    std::span<const uint8_t> rgba;
    bool srgb = false;
    static constexpr uint32_t kMaxSourceSide = 16384;
    uint32_t colorWidth = 0, colorHeight = 0;
    float topLeftX = 0, topLeftY = 0, worldSpan = 0;
    std::span<const float> shore;
    uint32_t shoreWidth = 0, shoreHeight = 0;
    std::array<float, 4> shoreBounds{}; // north-west x/y, width/height in world units
    float waterElevation = 0;
    bool followCoast = false;
    bool staticBreakup = false; // encoded in macroColor alpha; editable tint stays in the shader
    uint32_t outputWidth = 0, outputHeight = 0;
};

std::vector<SurfaceColor> surfaceColorsFromRgba(std::span<const uint8_t> rgba, bool srgb);
bool validSurfaceCookInput(const SurfaceCookInput& input);
uint64_t surfaceCookSourceHash(const SurfaceCookInput& input);
// UVs have the original atlas convention (north at v=0); caller validates input once per cook.
SurfaceColor evaluateSurfaceMacro(const SurfaceCookInput& input, float u, float v);
float evaluateSurfaceBreakup(float worldX, float worldY);
// Maximum vertical deviation from the native fixed TL-BR fine mesh, per stride.
// Non-divisible footprints stay fine so a coarse capture cannot truncate coverage.
std::array<float, 6> terrainPatchHeightErrors(std::span<const float> heights, uint32_t side,
    uint32_t originX, uint32_t originY, uint32_t quadsX, uint32_t quadsY);
// Padded (side + 2) lattice, same indexing as SurfaceGradients::texels; 1 = open, lower = occluded.
std::vector<float> cookTerrainShapeAo(std::span<const float> heights, uint32_t side, float cellWorld);
// bakeAo = false leaves SurfaceGradients::ao empty (a height patch keeps the mission's earlier bake).
std::optional<SurfaceGradients> cookSurfaceGradients(std::span<const float> heights, uint32_t side, bool bakeAo = true);
std::optional<SurfaceAsset> cookSurface(const SurfaceCookInput& input);
// RGBA8 = grass, dirt, snow, concrete; rock is the remaining weight. Terrain
// material indices and authored control weights override the legacy color import.
std::vector<uint8_t> cookSurfaceMaterials(const SurfaceAsset& surface,
    std::span<const float> materials, uint32_t side, std::span<const uint8_t> control, uint32_t controlSide,
    uint32_t width, uint32_t height, bool sandProfile = false);
} // namespace mc2terrain
