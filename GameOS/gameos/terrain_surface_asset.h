// Versioned render-only surface data. Source fingerprints cover resolved input bytes and cook parameters.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mc2terrain {

using SurfaceColor = std::array<float, 4>;

struct SurfaceAsset {
    static constexpr uint32_t kSchema = 1;
    static constexpr uint32_t kEvaluator = 1;
    static constexpr uint32_t kMaxSide = 4096;
    static constexpr uint64_t kMaxBytes = 48ull + uint64_t(kMaxSide) * kMaxSide * 16;
    // Shader sample space: sRGB sources are decoded before filtering, UNORM sources are not.
    static constexpr uint32_t kLinear = 1;
    uint32_t width = 0, height = 0;
    uint64_t sourceHash = 0;
    std::vector<SurfaceColor> macroColor;
};

uint64_t surfaceHash(std::span<const uint8_t> bytes, uint64_t seed = 14695981039346656037ull);
bool validSurfaceAsset(const SurfaceAsset& asset, uint64_t expectedSource);
// Little-endian, fixed 48-byte header; no native struct layout or unchecked file-size allocations.
std::optional<SurfaceAsset> decodeSurfaceAsset(std::span<const uint8_t> bytes, uint64_t expectedSource);
std::vector<uint8_t> encodeSurfaceAsset(const SurfaceAsset& asset);

struct SurfaceCookInput;
// A cook is only a cache hint. The caller passes inputs already resolved by the mission's existing loaders.
std::optional<SurfaceAsset> resolveSurfaceAsset(const SurfaceCookInput& input, std::span<const uint8_t> candidate);
void setSurfaceMissionCook(std::vector<uint8_t> candidate);
std::span<const uint8_t> surfaceMissionCook();

} // namespace mc2terrain
