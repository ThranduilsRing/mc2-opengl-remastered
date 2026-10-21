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

void put(std::vector<uint8_t>& bytes, uint64_t word, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<uint8_t>(word >> (8 * i)));
}
uint64_t get(std::span<const uint8_t> bytes, size_t offset, unsigned count) {
    uint64_t word = 0;
    for (unsigned i = 0; i < count; ++i) word |= uint64_t(bytes[offset + i]) << (8 * i);
    return word;
}
} // namespace

uint64_t surfaceHash(std::span<const uint8_t> bytes, uint64_t seed) {
    for (uint8_t byte : bytes) { seed ^= byte; seed *= 1099511628211ull; }
    return seed;
}

bool validSurfaceAsset(const SurfaceAsset& asset, uint64_t expectedSource) {
    if (!validSize(asset.width, asset.height) || asset.sourceHash != expectedSource || expectedSource == 0 ||
        asset.macroColor.size() != uint64_t(asset.width) * asset.height) return false;
    for (const auto& pixel : asset.macroColor)
        for (float channel : pixel)
            if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f) return false;
    return true;
}

std::optional<SurfaceAsset> decodeSurfaceAsset(std::span<const uint8_t> bytes, uint64_t expectedSource) {
    if (bytes.size() < kHeaderBytes || get(bytes, 0, 4) != kMagic ||
        get(bytes, 4, 4) != SurfaceAsset::kSchema || get(bytes, 8, 4) != SurfaceAsset::kEvaluator ||
        get(bytes, 20, 4) != SurfaceAsset::kLinear || get(bytes, 24, 8) != expectedSource || expectedSource == 0) return {};
    const auto width = static_cast<uint32_t>(get(bytes, 12, 4));
    const auto height = static_cast<uint32_t>(get(bytes, 16, 4));
    if (!validSize(width, height)) return {};
    const uint64_t payloadBytes = uint64_t(width) * height * kPixelBytes;
    if (get(bytes, 40, 8) != payloadBytes || bytes.size() != kHeaderBytes + payloadBytes ||
        surfaceHash(bytes.subspan(kHeaderBytes)) != get(bytes, 32, 8)) return {};
    SurfaceAsset candidate;
    candidate.width = width; candidate.height = height; candidate.sourceHash = expectedSource;
    candidate.macroColor.resize(size_t(width) * height);
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(candidate.macroColor.data(), bytes.data() + kHeaderBytes, size_t(payloadBytes));
    } else {
        size_t offset = kHeaderBytes;
        for (auto& pixel : candidate.macroColor)
            for (float& channel : pixel) {
                channel = std::bit_cast<float>(static_cast<uint32_t>(get(bytes, offset, 4)));
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
    put(bytes, kMagic, 4); put(bytes, SurfaceAsset::kSchema, 4); put(bytes, SurfaceAsset::kEvaluator, 4);
    put(bytes, asset.width, 4); put(bytes, asset.height, 4); put(bytes, SurfaceAsset::kLinear, 4);
    put(bytes, asset.sourceHash, 8); put(bytes, 0, 8); put(bytes, asset.macroColor.size() * kPixelBytes, 8);
    if constexpr (std::endian::native == std::endian::little) {
        bytes.resize(kHeaderBytes + asset.macroColor.size() * kPixelBytes);
        std::memcpy(bytes.data() + kHeaderBytes, asset.macroColor.data(), asset.macroColor.size() * kPixelBytes);
    } else {
        for (const auto& pixel : asset.macroColor)
            for (float channel : pixel) put(bytes, std::bit_cast<uint32_t>(channel), 4);
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

void setSurfaceMissionCook(std::vector<uint8_t> candidate) {
    missionCook = candidate.size() <= SurfaceAsset::kMaxBytes ? std::move(candidate) : std::vector<uint8_t>{};
}

std::span<const uint8_t> surfaceMissionCook() { return missionCook; }
} // namespace mc2terrain
