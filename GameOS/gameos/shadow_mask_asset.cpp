// shadow_mask_asset.cpp -- see the header.
#include "shadow_mask_asset.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mc2shadow {

namespace {
constexpr uint32_t kMagic = 0x314D5354;   // TSM1
constexpr size_t kHeaderBytes = 48;

using mc2cache::putLE;
using mc2cache::getLE;
// Integrity only (a torn or flipped file is a miss), eight bytes a step: a byte-wise FNV over a 67 MB chain cost more than the read.
uint64_t payloadHash(std::span<const uint8_t> bytes) {
    uint64_t h = 14695981039346656037ull ^ bytes.size();
    size_t i = 0;
    for (; i + 8 <= bytes.size(); i += 8) {
        uint64_t w;
        std::memcpy(&w, bytes.data() + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < bytes.size(); ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
    return h;
}
bool validShape(uint32_t texels, uint32_t mips) {
    if (texels == 0 || texels % 4 != 0 || texels > ShadowMaskAsset::kMaxTexels || mips == 0) return false;
    uint32_t full = 1;
    while ((texels >> full) >= 1 && full < 32) ++full;
    return mips <= full;
}
}  // namespace

uint64_t bc4ChainBytes(uint32_t texels, uint32_t mips) {
    uint64_t bytes = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint64_t side = std::max<uint64_t>(texels >> m, 1);
        const uint64_t blocks = (side + 3) / 4;
        bytes += blocks * blocks * 8;
    }
    return bytes;
}

std::vector<uint8_t> encodeShadowMaskAsset(const ShadowMaskAsset& asset) {
    if (!validShape(asset.texels, asset.mips) || asset.key == 0 || asset.blocks.size() != bc4ChainBytes(asset.texels, asset.mips)) return {};
    std::vector<uint8_t> bytes;
    bytes.reserve(kHeaderBytes + asset.blocks.size());
    putLE(bytes, kMagic, 4); putLE(bytes, ShadowMaskAsset::kSchema, 4); putLE(bytes, ShadowMaskAsset::kEvaluator, 4);
    putLE(bytes, asset.texels, 4); putLE(bytes, asset.mips, 4); putLE(bytes, 0, 4);
    putLE(bytes, asset.key, 8); putLE(bytes, payloadHash(asset.blocks), 8); putLE(bytes, asset.blocks.size(), 8);
    bytes.insert(bytes.end(), asset.blocks.begin(), asset.blocks.end());
    return bytes;
}

std::optional<ShadowMaskAsset> decodeShadowMaskAsset(std::span<const uint8_t> bytes, uint64_t expectedKey) {
    if (expectedKey == 0 || bytes.size() < kHeaderBytes || getLE(bytes, 0, 4) != kMagic || getLE(bytes, 4, 4) != ShadowMaskAsset::kSchema ||
        getLE(bytes, 8, 4) != ShadowMaskAsset::kEvaluator || getLE(bytes, 24, 8) != expectedKey) return {};
    ShadowMaskAsset a;
    a.texels = static_cast<uint32_t>(getLE(bytes, 12, 4));
    a.mips = static_cast<uint32_t>(getLE(bytes, 16, 4));
    a.key = expectedKey;
    if (!validShape(a.texels, a.mips)) return {};
    const uint64_t payload = bc4ChainBytes(a.texels, a.mips);
    if (getLE(bytes, 40, 8) != payload || bytes.size() != kHeaderBytes + payload) return {};
    const auto body = bytes.subspan(kHeaderBytes);
    if (payloadHash(body) != getLE(bytes, 32, 8)) return {};
    a.blocks.assign(body.begin(), body.end());
    return a;
}

std::optional<ShadowMaskAsset> loadCachedShadowMask(std::string_view dir, uint64_t key) {
    const auto bytes = mc2cache::readEntry(dir, kShadowMaskKind, key);
    if (!bytes) return {};
    return decodeShadowMaskAsset(*bytes, key);
}

bool storeCachedShadowMask(std::string_view dir, const ShadowMaskAsset& asset) {
    const auto bytes = encodeShadowMaskAsset(asset);
    return !bytes.empty() && mc2cache::writeEntry(dir, kShadowMaskKind, asset.key, bytes);
}

std::array<uint8_t, 8> encodeBc4Block(const std::array<uint8_t, 16>& texels) {
    const uint8_t hi = *std::max_element(texels.begin(), texels.end());
    const uint8_t lo = *std::min_element(texels.begin(), texels.end());
    std::array<uint8_t, 8> block{hi, lo, 0, 0, 0, 0, 0, 0};
    if (hi == lo) return block;   // every index 0 = endpoint 0
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        // Sevenths of the way from endpoint 1 (min) to endpoint 0 (max), then the palette index of that step.
        const int step = static_cast<int>(std::lround(7.0 * (texels[static_cast<size_t>(i)] - lo) / double(hi - lo)));
        const uint64_t idx = step == 7 ? 0u : step == 0 ? 1u : static_cast<uint64_t>(8 - step);
        bits |= idx << (3 * i);
    }
    for (int b = 0; b < 6; ++b) block[static_cast<size_t>(2 + b)] = static_cast<uint8_t>(bits >> (8 * b));
    return block;
}

std::array<uint8_t, 16> decodeBc4Block(const std::array<uint8_t, 8>& block) {
    const int e0 = block[0], e1 = block[1];
    std::array<int, 8> pal{e0, e1, 0, 0, 0, 0, 0, 0};
    if (e0 > e1) {
        for (int i = 1; i <= 6; ++i) pal[static_cast<size_t>(i + 1)] = ((7 - i) * e0 + i * e1) / 7;
    } else {
        for (int i = 1; i <= 4; ++i) pal[static_cast<size_t>(i + 1)] = ((5 - i) * e0 + i * e1) / 5;
        pal[6] = 0; pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int b = 0; b < 6; ++b) bits |= uint64_t(block[static_cast<size_t>(2 + b)]) << (8 * b);
    std::array<uint8_t, 16> out{};
    for (int i = 0; i < 16; ++i) out[static_cast<size_t>(i)] = static_cast<uint8_t>(pal[static_cast<size_t>((bits >> (3 * i)) & 7u)]);
    return out;
}

}  // namespace mc2shadow
