#include "terrain_surface_asset.h"

#include "cook_cache.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>

namespace mc2terrain {
namespace {
constexpr uint32_t kMagic = 0x31465354; // TSF1
constexpr size_t kHeaderBytes = 48;
constexpr uint64_t kPixelBytes = 16;
std::vector<uint8_t> missionCook;

bool validSize(uint32_t w, uint32_t h) {
    return w > 0 && h > 0 && w <= SurfaceAsset::kMaxSide && h <= SurfaceAsset::kMaxSide;
}

using mc2cache::putLE;
using mc2cache::getLE;
} // namespace

uint64_t surfaceHash(std::span<const uint8_t> bytes, uint64_t seed) {
    for (uint8_t byte : bytes) { seed ^= byte; seed *= 1099511628211ull; }
    return seed;
}

bool validSurfaceAsset(const SurfaceAsset& asset, uint64_t expectedSource) {
    if (!validSize(asset.width, asset.height) || asset.sourceHash != expectedSource || expectedSource == 0 ||
        asset.macroColor.size() != static_cast<uint64_t>(asset.width) * asset.height) return false;
    for (const auto& pixel : asset.macroColor)
        for (float channel : pixel)
            if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f) return false;
    return true;
}

std::optional<SurfaceAsset> decodeSurfaceAsset(std::span<const uint8_t> bytes, uint64_t expectedSource) {
    if (bytes.size() < kHeaderBytes || getLE(bytes, 0, 4) != kMagic ||
        getLE(bytes, 4, 4) != SurfaceAsset::kSchema || getLE(bytes, 8, 4) != SurfaceAsset::kEvaluator ||
        getLE(bytes, 20, 4) != SurfaceAsset::kLinear || getLE(bytes, 24, 8) != expectedSource || expectedSource == 0) return {};
    const auto width = static_cast<uint32_t>(getLE(bytes, 12, 4));
    const auto height = static_cast<uint32_t>(getLE(bytes, 16, 4));
    if (!validSize(width, height)) return {};
    const uint64_t payloadBytes = static_cast<uint64_t>(width) * height * kPixelBytes;
    if (getLE(bytes, 40, 8) != payloadBytes || bytes.size() != kHeaderBytes + payloadBytes ||
        surfaceHash(bytes.subspan(kHeaderBytes)) != getLE(bytes, 32, 8)) return {};
    SurfaceAsset candidate;
    candidate.width = width; candidate.height = height; candidate.sourceHash = expectedSource;
    candidate.macroColor.resize(static_cast<size_t>(width) * height);
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(candidate.macroColor.data(), bytes.data() + kHeaderBytes, static_cast<size_t>(payloadBytes));
    } else {
        size_t offset = kHeaderBytes;
        for (auto& pixel : candidate.macroColor)
            for (float& channel : pixel) {
                channel = std::bit_cast<float>(static_cast<uint32_t>(getLE(bytes, offset, 4)));
                offset += 4;
            }
    }
    if (!validSurfaceAsset(candidate, expectedSource)) return {};
    return candidate;
}

std::vector<uint8_t> encodeSurfaceAsset(const SurfaceAsset& asset) {
    if (!validSurfaceAsset(asset, asset.sourceHash)) return {};
    std::vector<uint8_t> bytes;
    bytes.reserve(kHeaderBytes + asset.macroColor.size() * kPixelBytes);
    putLE(bytes, kMagic, 4); putLE(bytes, SurfaceAsset::kSchema, 4); putLE(bytes, SurfaceAsset::kEvaluator, 4);
    putLE(bytes, asset.width, 4); putLE(bytes, asset.height, 4); putLE(bytes, SurfaceAsset::kLinear, 4);
    putLE(bytes, asset.sourceHash, 8); putLE(bytes, 0, 8); putLE(bytes, asset.macroColor.size() * kPixelBytes, 8);
    if constexpr (std::endian::native == std::endian::little) {
        bytes.resize(kHeaderBytes + asset.macroColor.size() * kPixelBytes);
        std::memcpy(bytes.data() + kHeaderBytes, asset.macroColor.data(), asset.macroColor.size() * kPixelBytes);
    } else {
        for (const auto& pixel : asset.macroColor)
            for (float channel : pixel) putLE(bytes, std::bit_cast<uint32_t>(channel), 4);
    }
    const uint64_t hash = surfaceHash(std::span<const uint8_t>(bytes).subspan(kHeaderBytes));
    for (unsigned i = 0; i < 8; ++i) bytes[32 + i] = static_cast<uint8_t>(hash >> (8 * i));
    return bytes;
}

namespace {
// Each macro entry is up to ~256 MiB.
constexpr mc2cache::CacheKind kMacroKind{"macro-", ".tsf", 6, SurfaceAsset::kMaxBytes};
}

std::optional<SurfaceAsset> loadCachedSurfaceAsset(std::string_view dir, uint64_t expectedSource) {
    if (expectedSource == 0) return {};
    const auto bytes = mc2cache::readEntry(dir, kMacroKind, expectedSource);
    if (!bytes || bytes->size() < kHeaderBytes) return {};
    return decodeSurfaceAsset(*bytes, expectedSource);
}

bool storeCachedSurfaceAsset(std::string_view dir, const SurfaceAsset& asset) {
    const auto bytes = encodeSurfaceAsset(asset);
    return !bytes.empty() && mc2cache::writeEntry(dir, kMacroKind, asset.sourceHash, bytes);
}

namespace {
// Separate kind/version from the float cook. Encoding changes must bump this magic.
constexpr uint32_t kPackedMagic = 0x31415350; // PSA1
constexpr uint32_t materialSide(uint32_t side) { return std::min(side, 1024u); }
constexpr uint64_t kPackedMaxBytes = kHeaderBytes + static_cast<uint64_t>(SurfaceAsset::kMaxSide) * SurfaceAsset::kMaxSide * 4 +
    kHeaderBytes + static_cast<uint64_t>(materialSide(SurfaceAsset::kMaxSide)) * materialSide(SurfaceAsset::kMaxSide) * kPixelBytes;
constexpr mc2cache::CacheKind kPackedKind{"macro-packed-", ".tsp", 6, kPackedMaxBytes};
}

bool storeCachedPackedSurface(std::string_view dir, const SurfaceAsset& surface, bool srgb, std::span<const uint8_t> rgba) {
    if (!validSurfaceAsset(surface, surface.sourceHash) || rgba.size() != static_cast<uint64_t>(surface.width) * surface.height * 4) return false;
    SurfaceAsset materials;
    materials.width = materialSide(surface.width); materials.height = materialSide(surface.height);
    materials.sourceHash = surface.sourceHash;
    materials.macroColor.resize(static_cast<size_t>(materials.width) * materials.height);
    // Match cookSurfaceMaterials' center/nearest lookup exactly, before any RGBA8 quantization.
    for (uint32_t y = 0; y < materials.height; ++y) for (uint32_t x = 0; x < materials.width; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / materials.width;
        const float v = (static_cast<float>(y) + 0.5f) / materials.height;
        materials.macroColor[static_cast<size_t>(y) * materials.width + x] = surface.macroColor[
            static_cast<size_t>(std::min(static_cast<uint32_t>(v * surface.height), surface.height - 1)) * surface.width +
            std::min(static_cast<uint32_t>(u * surface.width), surface.width - 1)];
    }
    const auto floats = encodeSurfaceAsset(materials);
    std::vector<uint8_t> bytes;
    bytes.reserve(kHeaderBytes + rgba.size() + floats.size());
    putLE(bytes, kPackedMagic, 4); putLE(bytes, SurfaceAsset::kSchema, 4); putLE(bytes, SurfaceAsset::kEvaluator, 4);
    putLE(bytes, surface.width, 4); putLE(bytes, surface.height, 4); putLE(bytes, srgb ? 1 : 0, 4);
    putLE(bytes, surface.sourceHash, 8); putLE(bytes, 0, 8); putLE(bytes, rgba.size(), 8);
    bytes.insert(bytes.end(), rgba.begin(), rgba.end()); bytes.insert(bytes.end(), floats.begin(), floats.end());
    const std::span<const uint8_t> data = bytes;
    const auto hash = surfaceHash(data.subspan(40), surfaceHash(data.first(32)));
    for (unsigned i = 0; i < 8; ++i) bytes[32 + i] = static_cast<uint8_t>(hash >> (8 * i));
    return mc2cache::writeEntry(dir, kPackedKind, surface.sourceHash, bytes);
}

std::optional<PackedSurfaceAsset> loadCachedPackedSurface(std::string_view dir, uint64_t source, bool srgb) {
    if (source == 0) return {};
    const auto bytes = mc2cache::readEntry(dir, kPackedKind, source);
    if (!bytes || bytes->size() < kHeaderBytes) return {};
    const std::span<const uint8_t> data = *bytes;
    if (getLE(data, 0, 4) != kPackedMagic || getLE(data, 4, 4) != SurfaceAsset::kSchema ||
        getLE(data, 8, 4) != SurfaceAsset::kEvaluator || getLE(data, 20, 4) != (srgb ? 1u : 0u) || getLE(data, 24, 8) != source) return {};
    const auto w = static_cast<uint32_t>(getLE(data, 12, 4)), h = static_cast<uint32_t>(getLE(data, 16, 4));
    if (!validSize(w, h)) return {};
    const uint64_t rgbaSize = static_cast<uint64_t>(w) * h * 4;
    const uint64_t floatsSize = kHeaderBytes + static_cast<uint64_t>(materialSide(w)) * materialSide(h) * 16;
    if (getLE(data, 40, 8) != rgbaSize || data.size() != kHeaderBytes + rgbaSize + floatsSize ||
        surfaceHash(data.subspan(40), surfaceHash(data.first(32))) != getLE(data, 32, 8)) return {};
    auto materials = decodeSurfaceAsset(data.subspan(kHeaderBytes + static_cast<size_t>(rgbaSize)), source);
    if (!materials || materials->width != materialSide(w) || materials->height != materialSide(h)) return {};
    PackedSurfaceAsset result;
    result.width = w; result.height = h; result.materials = std::move(*materials);
    result.rgba.assign(data.begin() + kHeaderBytes, data.begin() + kHeaderBytes + static_cast<size_t>(rgbaSize));
    return result;
}

void setSurfaceMissionCook(std::vector<uint8_t> candidate) {
    missionCook = candidate.size() <= SurfaceAsset::kMaxBytes ? std::move(candidate) : std::vector<uint8_t>{};
}

std::span<const uint8_t> surfaceMissionCook() { return missionCook; }
} // namespace mc2terrain
