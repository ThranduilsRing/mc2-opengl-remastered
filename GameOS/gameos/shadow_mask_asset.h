// shadow_mask_asset.h -- the static shadow bake's mask as a cooked asset: BC4 blocks of every mip, keyed by the bake's content key.
//
// WHAT: a fixed 48-byte little-endian header (magic TSM1, schema, evaluator, texels, mips, key, payload hash, payload bytes) and the BC4 blocks of mip
// 0..mips-1 in order, each level row-major in 4x4 blocks (a level smaller than 4 texels still takes one block). Cached on disk through cook_cache
// (kind "shadow-", ".tsm", newest 4 kept). Also the BC4 block packing the GPU encoder mirrors, so a doctest pins the bit layout.
// WHY: a warm load replaces the bake (about 60-110 ms of GPU on stock maps) with a disk read and one upload; BC4 halves the R8 mask's VRAM.
#pragma once

#include "cook_cache.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mc2shadow {

struct ShadowMaskAsset {
    static constexpr uint32_t kSchema = 2;   // 2: word-wise payload hash
    static constexpr uint32_t kEvaluator = 1;   // bump when the bake's output for the same inputs changes
    static constexpr uint32_t kMaxTexels = 16384;
    static constexpr uint64_t kMaxBytes = 48ull + uint64_t(kMaxTexels) * kMaxTexels / 2 * 4 / 3 + 4096;
    uint32_t texels = 0;   // mip 0 is texels x texels, a whole number of 4x4 blocks
    uint32_t mips = 0;
    uint64_t key = 0;
    std::vector<uint8_t> blocks;   // 8 bytes per BC4 block, every level
};

inline constexpr mc2cache::CacheKind kShadowMaskKind{"shadow-", ".tsm", 4, ShadowMaskAsset::kMaxBytes};

// Bytes of the BC4 chain of a texels^2 image with `mips` levels.
uint64_t bc4ChainBytes(uint32_t texels, uint32_t mips);

std::vector<uint8_t> encodeShadowMaskAsset(const ShadowMaskAsset& asset);
// Nothing unless magic, schema, evaluator, key, sizes and the payload hash all check out.
std::optional<ShadowMaskAsset> decodeShadowMaskAsset(std::span<const uint8_t> bytes, uint64_t expectedKey);

std::optional<ShadowMaskAsset> loadCachedShadowMask(std::string_view dir, uint64_t key);
bool storeCachedShadowMask(std::string_view dir, const ShadowMaskAsset& asset);

// One BC4 block from 16 values in [0, 255], row-major (the 8-value mode: endpoint 0 = max, endpoint 1 = min). LOCKSTEP with shadow_bake_bc4.comp.
std::array<uint8_t, 8> encodeBc4Block(const std::array<uint8_t, 16>& texels);
std::array<uint8_t, 16> decodeBc4Block(const std::array<uint8_t, 8>& block);

}  // namespace mc2shadow
