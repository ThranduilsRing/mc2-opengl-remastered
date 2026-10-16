// RenderCore/KtxLoader.cpp
// Minimal KTX2 RGBA8 loader -- Phase 0 (no Basis/supercompression transcoding).
// No GL headers. No external dependencies.

#include "KtxLoader.h"
#include "../GameOS/gameos/utils/mc2log.h"
#include "../GameOS/include/mc2fmt.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// COMPRESSION-BC7-CPUDECODE-1: single-header public-domain BC1-7/BC6H decoder
// (bcdec.h v0.97, Sergii "iOrange" Kudlai, MIT / public domain). Zero deps,
// GL-free — matches this loader's self-contained design. We only use BC7.
// Define the implementation in THIS translation unit (it is the sole consumer).
#define BCDEC_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244)  // vendored decoder: bit-math int->uint8 stores, not ours to rewrite
#endif
#include "bcdec.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace RenderCore {

// KTX2 file magic: \xab KTX 20\xbb \r\n \x1a\n
static constexpr uint8_t kKtx2Magic[12] = {
    0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a
};

// VK_FORMAT constants for uncompressed RGBA8
static constexpr uint32_t kVkFormatR8G8B8A8Unorm = 37;
static constexpr uint32_t kVkFormatR8G8B8A8Srgb  = 43;
// VK_FORMAT constants for stored BC6H (BPTC float) blocks — HDR
static constexpr uint32_t kVkFormatBc6hUfloat = 143;  // VK_FORMAT_BC6H_UFLOAT_BLOCK
static constexpr uint32_t kVkFormatBc6hSfloat = 144;  // VK_FORMAT_BC6H_SFLOAT_BLOCK
// VK_FORMAT constants for stored BC7 (BPTC) blocks
static constexpr uint32_t kVkFormatBc7UnormBlock = 145;
static constexpr uint32_t kVkFormatBc7SrgbBlock  = 146;

// KTX2 layout: 12-byte magic, nine uint32 header fields (format .. supercompression), four uint32 + two uint64 index fields we
// do not use, then the level index at offset 80: one {byteOffset, byteLength, uncompressedByteLength} uint64 triple per level,
// largest level first. All little-endian.
static constexpr std::size_t kHeaderFieldsOffset = 12;
static constexpr std::size_t kLevelIndexOffset   = 80;
static constexpr std::size_t kLevelEntryBytes    = 24;

static uint32_t readU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t readU64(const uint8_t* p)
{
    return (uint64_t)readU32(p) | ((uint64_t)readU32(p + 4) << 32);
}

// The byte length level `lvl` of a w x h image must have: 4x4 blocks of 16 bytes when block-compressed, else 4 bytes a pixel.
static uint64_t levelBytes(uint32_t w, uint32_t h, uint32_t lvl, bool blockCompressed)
{
    const uint64_t lw = (w >> lvl) ? (w >> lvl) : 1u;
    const uint64_t lh = (h >> lvl) ? (h >> lvl) : 1u;
    return blockCompressed ? ((lw + 3u) / 4u) * ((lh + 3u) / 4u) * 16u : lw * lh * 4u;
}

KtxParseResult ktxParse(std::span<const uint8_t> file)
{
    const uint8_t* bytes = file.data();
    const std::size_t size = file.size();
    KtxParseResult r;
    if (!bytes || size < kLevelIndexOffset || std::memcmp(bytes, kKtx2Magic, sizeof(kKtx2Magic)) != 0) {
        r.error = "not a KTX2 file (no magic or shorter than the header)";
        return r;
    }
    const uint8_t* h = bytes + kHeaderFieldsOffset;
    const uint32_t vkFormat = readU32(h + 0);
    const uint32_t typeSize = readU32(h + 4);
    const uint32_t width = readU32(h + 8);
    const uint32_t height = readU32(h + 12);
    const uint32_t depth = readU32(h + 16);
    const uint32_t layers = readU32(h + 20);
    const uint32_t faces = readU32(h + 24);
    const uint32_t levels = readU32(h + 28);
    const uint32_t supercompression = readU32(h + 32);

    const bool isRgba8 = vkFormat == kVkFormatR8G8B8A8Unorm || vkFormat == kVkFormatR8G8B8A8Srgb;
    const bool isBc6h = vkFormat == kVkFormatBc6hUfloat || vkFormat == kVkFormatBc6hSfloat;
    const bool isBc7 = vkFormat == kVkFormatBc7UnormBlock || vkFormat == kVkFormatBc7SrgbBlock;
    const bool blockCompressed = isBc6h || isBc7;   // both 4x4 blocks of 16 bytes
    if (!isRgba8 && !blockCompressed) { r.error = "unsupported vkFormat"; return r; }
    // typeSize is 1 for both the 8-bit RGBA formats and the block formats.
    if (typeSize != 1 || depth != 0 || layers > 1 || faces != 1 || supercompression != 0) {
        r.error = "not a stored 2D single-layer single-face image";
        return r;
    }
    if (width == 0 || height == 0 || width > kKtxMaxEdge || height > kKtxMaxEdge) { r.error = "edge out of range"; return r; }
    uint32_t fullChain = 1;
    for (uint32_t m = width > height ? width : height; m > 1; m >>= 1) ++fullChain;
    if (levels < 1 || levels > fullChain) { r.error = "level count out of range"; return r; }
    if (size - kLevelIndexOffset < (std::size_t)levels * kLevelEntryBytes) { r.error = "truncated level index"; return r; }

    KtxImage img;
    img.mipByteOffsets.resize(levels);
    uint64_t total = 0;
    for (uint32_t lvl = 0; lvl < levels; ++lvl) {
        const uint8_t* e = bytes + kLevelIndexOffset + (std::size_t)lvl * kLevelEntryBytes;
        const uint64_t offset = readU64(e);
        const uint64_t length = readU64(e + 8);
        if (length != levelBytes(width, height, lvl, blockCompressed)) { r.error = "level byte length does not match the format"; return r; }
        if (offset > size || length > size - offset) { r.error = "level data runs past the end of the file"; return r; }
        img.mipByteOffsets[lvl] = total;
        total += length;
    }
    img.pixels.resize((std::size_t)total);
    for (uint32_t lvl = 0; lvl < levels; ++lvl) {
        const uint8_t* e = bytes + kLevelIndexOffset + (std::size_t)lvl * kLevelEntryBytes;
        std::memcpy(img.pixels.data() + img.mipByteOffsets[lvl], bytes + readU64(e), (std::size_t)readU64(e + 8));
    }
    img.width = (int)width;
    img.height = (int)height;
    img.isSrgb = vkFormat == kVkFormatR8G8B8A8Srgb || vkFormat == kVkFormatBc7SrgbBlock;   // BC6H has no sRGB variant
    img.mipCount = (int)levels;
    img.vkFormat = vkFormat;
    img.isCompressed = blockCompressed;
    img.blockSizeBytes = blockCompressed ? 16u : 0u;
    r.image = std::move(img);
    return r;
}

bool ktxLoadRgba8(const char* path, KtxImage& out)
{
    std::FILE* f = path ? std::fopen(path, "rb") : nullptr;
    if (!f)
        return false;
    std::vector<uint8_t> bytes;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long n = std::ftell(f);
        if (n > 0 && std::fseek(f, 0, SEEK_SET) == 0) {
            bytes.resize((std::size_t)n);
            if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        }
    }
    std::fclose(f);
    KtxParseResult parsed = ktxParse(bytes);
    if (!parsed.image) {
        // A present but malformed sidecar is mod content the caller silently replaces with its fallback; say why.
        MC2_LOG(Assets, Warn, "[TEXTURE_DECODE v1] rejected {}: {}", mc2::fmt::safe(path), parsed.error ? parsed.error : "unreadable");
        return false;
    }
    out = std::move(*parsed.image);
    return true;
}

bool ktxDecodeBc7ToRgba8(const KtxImage& img, int level,
                         std::vector<uint8_t>& outRgba,
                         int* outW, int* outH)
{
    // ---- Preconditions ----
    if (!img.isCompressed ||
        (img.vkFormat != kVkFormatBc7UnormBlock && img.vkFormat != kVkFormatBc7SrgbBlock) ||
        img.blockSizeBytes != 16u ||
        level < 0 || level >= img.mipCount ||
        static_cast<std::size_t>(level) >= img.mipByteOffsets.size()) {
        return false;
    }

    // Level dimensions (same halving rule the header validation used).
    const uint32_t w = (static_cast<uint32_t>(img.width)  >> level) ? (static_cast<uint32_t>(img.width)  >> level) : 1u;
    const uint32_t h = (static_cast<uint32_t>(img.height) >> level) ? (static_cast<uint32_t>(img.height) >> level) : 1u;

    const uint32_t blocksWide = (w + 3u) / 4u;
    const uint32_t blocksHigh = (h + 3u) / 4u;
    const uint64_t levelBytes = static_cast<uint64_t>(blocksWide) * blocksHigh * 16u;

    // Bounds-check the stored block stream for this level.
    const std::size_t levelOffset = static_cast<std::size_t>(img.mipByteOffsets[level]);
    if (levelOffset + levelBytes > img.pixels.size()) {
        return false;
    }
    const uint8_t* src = img.pixels.data() + levelOffset;

    // ---- Decode block-by-block into the tightly-packed RGBA8 output ----
    outRgba.assign(static_cast<std::size_t>(w) * h * 4u, 0u);
    const std::size_t rowPitch = static_cast<std::size_t>(w) * 4u;

    // Scratch for one fully-decoded 4x4 block (16 px * 4 bytes), used only for
    // edge blocks where the 4x4 extends past the image bounds; interior blocks
    // decode straight into the destination at the correct pitch.
    uint8_t blockTmp[4 * 4 * 4];

    for (uint32_t by = 0; by < blocksHigh; ++by) {
        for (uint32_t bx = 0; bx < blocksWide; ++bx) {
            const uint8_t* block = src + (static_cast<std::size_t>(by) * blocksWide + bx) * 16u;
            const uint32_t px = bx * 4u;
            const uint32_t py = by * 4u;
            const uint32_t bw = (px + 4u <= w) ? 4u : (w - px); // valid columns
            const uint32_t bh = (py + 4u <= h) ? 4u : (h - py); // valid rows

            if (bw == 4u && bh == 4u) {
                // Interior/aligned block: decode directly into the destination.
                uint8_t* dst = outRgba.data() + static_cast<std::size_t>(py) * rowPitch + px * 4u;
                bcdec_bc7(block, dst, static_cast<int>(rowPitch));
            } else {
                // Edge block: decode into scratch, copy the valid sub-rectangle.
                bcdec_bc7(block, blockTmp, 4 * 4);
                for (uint32_t r = 0; r < bh; ++r) {
                    std::memcpy(outRgba.data() + static_cast<std::size_t>(py + r) * rowPitch + px * 4u,
                                blockTmp + static_cast<std::size_t>(r) * (4 * 4),
                                static_cast<std::size_t>(bw) * 4u);
                }
            }
        }
    }

    if (outW) *outW = static_cast<int>(w);
    if (outH) *outH = static_cast<int>(h);
    return true;
}

} // namespace RenderCore
