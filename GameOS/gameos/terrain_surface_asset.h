// Versioned render-only surface data. Source fingerprints cover resolved input bytes and cook parameters.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc2terrain {

using SurfaceColor = std::array<float, 4>;

struct SurfaceAsset {
    static constexpr uint32_t kSchema = 1;
    static constexpr uint32_t kEvaluator = 2;
    static constexpr uint32_t kMaxSide = 4096;
    static constexpr uint64_t kMaxBytes = 48ull + uint64_t(kMaxSide) * kMaxSide * 16;
    // Shader sample space: sRGB sources are decoded before filtering, UNORM sources are not.
    static constexpr uint32_t kLinear = 1;
    uint32_t width = 0, height = 0;
    uint64_t sourceHash = 0;
    std::vector<SurfaceColor> macroColor;
};

// Unnormalized central differences on the height lattice, with a one-cell clamp halo.
// Filtering these before normalization preserves the original bilinear normal field.
struct SurfaceGradients {
    uint32_t side = 0;
    std::vector<std::array<float, 2>> texels;
};

uint64_t surfaceHash(std::span<const uint8_t> bytes, uint64_t seed = 14695981039346656037ull);
bool validSurfaceAsset(const SurfaceAsset& asset, uint64_t expectedSource);
// Little-endian, fixed 48-byte header; no native struct layout or unchecked file-size allocations.
std::optional<SurfaceAsset> decodeSurfaceAsset(std::span<const uint8_t> bytes, uint64_t expectedSource);
std::vector<uint8_t> encodeSurfaceAsset(const SurfaceAsset& asset);

// On-disk write-through cache keyed by source hash (which embeds the evaluator version).
// Load validates via decodeSurfaceAsset (magic/schema/evaluator/key/size/payload hash).
// Store writes a temp file then renames, so a crash cannot leave a partial entry.
std::optional<SurfaceAsset> loadCachedSurfaceAsset(std::string_view dir, uint64_t expectedSource);
bool storeCachedSurfaceAsset(std::string_view dir, const SurfaceAsset& asset);

struct SurfaceCookInput;
// A cook is only a cache hint. The caller passes inputs already resolved by the mission's existing loaders.
std::optional<SurfaceAsset> resolveSurfaceAsset(const SurfaceCookInput& input, std::span<const uint8_t> candidate,
                                                const std::string& cacheDir = {}, const char** origin = nullptr);
void setSurfaceMissionCook(std::vector<uint8_t> candidate);
std::span<const uint8_t> surfaceMissionCook();

} // namespace mc2terrain
