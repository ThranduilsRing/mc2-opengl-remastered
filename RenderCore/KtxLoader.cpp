// RenderCore/KtxLoader.cpp
// Minimal KTX2 RGBA8 loader -- Phase 0 (no Basis/supercompression transcoding).
// No GL headers. No external dependencies.

#include "KtxLoader.h"
#include "../GameOS/gameos/utils/mc2log.h"
#include "../mclib/mc2fmt.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
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
#include "../3rdparty/bcdec/bcdec.h"
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
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t readU64(const uint8_t* p)
{
    return static_cast<uint64_t>(readU32(p)) | (static_cast<uint64_t>(readU32(p + 4)) << 32);
}

// The byte length level `lvl` of a w x h image must have: 4x4 blocks of 16 bytes when block-compressed, else 4 bytes a pixel.
static uint64_t levelBytes(uint32_t w, uint32_t h, uint32_t lvl, uint32_t blockSize)
{
    const uint64_t lw = (w >> lvl) ? (w >> lvl) : 1u;
    const uint64_t lh = (h >> lvl) ? (h >> lvl) : 1u;
    return blockSize ? ((lw + 3u) / 4u) * ((lh + 3u) / 4u) * blockSize : lw * lh * 4u;
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
    // A Basis payload (vkFormat 0, or BasisLZ / Zstd supercompression) is transcoded to BC blocks by the vendored transcoder.
    if (vkFormat == 0 || supercompression != 0) return ktxParseBasis(file);

    const bool isRgba8 = vkFormat == kVkFormatR8G8B8A8Unorm || vkFormat == kVkFormatR8G8B8A8Srgb;
    const bool isBc6h = vkFormat == kVkFormatBc6hUfloat || vkFormat == kVkFormatBc6hSfloat;
    const bool isBc7 = vkFormat == kVkFormatBc7UnormBlock || vkFormat == kVkFormatBc7SrgbBlock;
    const bool isBc1 = vkFormat >= 131 && vkFormat <= 134;
    const bool isBc3 = vkFormat == 137 || vkFormat == 138;
    const bool isBc5 = vkFormat == 141;
    const bool blockCompressed = isBc1 || isBc3 || isBc5 || isBc6h || isBc7;
    const uint32_t blockSize = isBc1 ? 8u : blockCompressed ? 16u : 0u;   // both 4x4 blocks of 16 bytes
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
    if (size - kLevelIndexOffset < std::size_t{levels} * kLevelEntryBytes) { r.error = "truncated level index"; return r; }

    KtxImage img;
    // Embedded metadata shares the sky's provider identity and cannot override
    // a mod sky independently. Older skies simply use the EXR scan fallback.
    const uint32_t kvOffset = readU32(h + 44), kvSize = readU32(h + 48);
    if (kvOffset <= size && kvSize <= size - kvOffset) {
        size_t at = kvOffset, end = at + kvSize;
        constexpr char key[] = "MC2.sunAzimuth";
        while (end - at >= 4) {
            const uint32_t length = readU32(bytes + at); at += 4;
            if (length > end - at) break;
            if (length == sizeof(key) + 5 && std::memcmp(bytes + at, key, sizeof(key)) == 0) {
                const uint8_t valid = bytes[at + sizeof(key)];
                const float az = std::bit_cast<float>(readU32(bytes + at + sizeof(key) + 1));
                if (valid <= 1 && std::isfinite(az) && std::abs(az) <= 3.141593f) {
                    img.hasSunMetadata = true;
                    if (valid) img.sunAzimuth = az;
                }
            }
            const size_t padded = (static_cast<size_t>(length) + 3) & ~size_t{3};
            if (padded > end - at) break;
            at += padded;
        }
    }
    img.mipByteOffsets.resize(levels);
    uint64_t total = 0;
    for (uint32_t lvl = 0; lvl < levels; ++lvl) {
        const uint8_t* e = bytes + kLevelIndexOffset + std::size_t{lvl} * kLevelEntryBytes;
        const uint64_t offset = readU64(e);
        const uint64_t length = readU64(e + 8);
        if (length != levelBytes(width, height, lvl, blockSize)) { r.error = "level byte length does not match the format"; return r; }
        if (offset > size || length > size - offset) { r.error = "level data runs past the end of the file"; return r; }
        if (readU64(e + 16) != length || offset < kLevelIndexOffset + levels * kLevelEntryBytes) {
            r.error = "invalid stored length or level overlaps header"; return r;
        }
        for (uint32_t prior = 0; prior < lvl; ++prior) {
            const uint8_t* p = bytes + kLevelIndexOffset + prior * kLevelEntryBytes;
            const uint64_t po = readU64(p), pl = readU64(p + 8);
            if (offset < po + pl && po < offset + length) { r.error = "overlapping levels"; return r; }
        }
        img.mipByteOffsets[lvl] = total;
        total += length;
    }
    img.pixels.resize(static_cast<std::size_t>(total));
    for (uint32_t lvl = 0; lvl < levels; ++lvl) {
        const uint8_t* e = bytes + kLevelIndexOffset + std::size_t{lvl} * kLevelEntryBytes;
        std::memcpy(img.pixels.data() + img.mipByteOffsets[lvl], bytes + readU64(e), static_cast<std::size_t>(readU64(e + 8)));
    }
    img.width = static_cast<int>(width);
    img.height = static_cast<int>(height);
    img.isSrgb = vkFormat == 43 || vkFormat == 132 || vkFormat == 134 || vkFormat == 138 || vkFormat == 146;   // BC6H has no sRGB variant
    img.mipCount = static_cast<int>(levels);
    img.vkFormat = vkFormat;
    img.isCompressed = blockCompressed;
    img.blockSizeBytes = blockSize;
    img.alpha = (vkFormat == 131 || vkFormat == 132 || isBc5 || isBc6h) ? KtxAlpha::Opaque
              : isBc1 ? KtxAlpha::Cutout : KtxAlpha::Straight;
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
            bytes.resize(static_cast<std::size_t>(n));
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

bool ktxDecodeBlocksToRgba8(const KtxImage& img, int level,
                         std::vector<uint8_t>& outRgba,
                         int* outW, int* outH)
{
    const bool bc1 = img.vkFormat >= 131 && img.vkFormat <= 134;
    const bool bc3 = img.vkFormat == 137 || img.vkFormat == 138;
    const bool bc5 = img.vkFormat == 141;
    const bool bc7 = img.vkFormat == 145 || img.vkFormat == 146;
    const uint32_t bs = bc1 ? 8u : 16u;
    if (!img.isCompressed || !(bc1 || bc3 || bc5 || bc7) || img.blockSizeBytes != bs ||
        img.width <= 0 || img.height <= 0 || static_cast<uint32_t>(img.width) > kKtxMaxEdge || static_cast<uint32_t>(img.height) > kKtxMaxEdge ||
        level < 0 || level >= 32 || level >= img.mipCount || static_cast<size_t>(level) >= img.mipByteOffsets.size()) return false;
    const uint32_t w = std::max(1u, static_cast<uint32_t>(img.width) >> level);
    const uint32_t h = std::max(1u, static_cast<uint32_t>(img.height) >> level);
    const uint32_t blocksWide = (w + 3u) / 4u, blocksHigh = (h + 3u) / 4u;
    const uint64_t levelBytes = static_cast<uint64_t>(blocksWide) * blocksHigh * bs;
    const uint64_t levelOffset = img.mipByteOffsets[level];
    if (levelOffset > img.pixels.size() || levelBytes > img.pixels.size() - levelOffset) return false;
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
            const uint8_t* block = src + (static_cast<std::size_t>(by) * blocksWide + bx) * bs;
            const uint32_t px = bx * 4u;
            const uint32_t py = by * 4u;
            const uint32_t bw = (px + 4u <= w) ? 4u : (w - px); // valid columns
            const uint32_t bh = (py + 4u <= h) ? 4u : (h - py); // valid rows

            if (bc1) bcdec_bc1(block, blockTmp, 16);
            else if (bc3) bcdec_bc3(block, blockTmp, 16);
            else if (bc7) bcdec_bc7(block, blockTmp, 16);
            else {
                uint8_t rg[32]; bcdec_bc5(block, rg, 8);
                for (int p = 0; p < 16; ++p) {
                    blockTmp[p*4] = rg[p*2]; blockTmp[p*4+1] = rg[p*2+1];
                    blockTmp[p*4+2] = 0; blockTmp[p*4+3] = 255;
                }
            }
            if (img.vkFormat == 131 || img.vkFormat == 132)
                for (int p = 0; p < 16; ++p) blockTmp[p*4+3] = 255;
            for (uint32_t r = 0; r < bh; ++r)
                std::memcpy(outRgba.data() + static_cast<size_t>(py + r) * rowPitch + px * 4u,
                            blockTmp + r * 16, bw * 4u);
        }
    }

    if (outW) *outW = static_cast<int>(w);
    if (outH) *outH = static_cast<int>(h);
    return true;
}

bool ktxDecodeBc7ToRgba8(const KtxImage& img, int level, std::vector<uint8_t>& out, int* w, int* h) {
    return (img.vkFormat == 145 || img.vkFormat == 146) && ktxDecodeBlocksToRgba8(img, level, out, w, h);
}

std::shared_ptr<const KtxImage> ktxStackBc7Layers(std::span<const KtxImage> layers) {
    if (layers.empty()) return {};
    const auto& first = layers[0];
    int full = 1;
    for (int e = first.width; e > 1; e >>= 1) ++full;
    for (const auto& l : layers)
        if (!l.isCompressed || l.vkFormat != 145 || l.blockSizeBytes != 16 || l.width <= 0 || l.width != l.height || l.width != first.width ||
            l.mipCount != full || l.mipByteOffsets.size() != static_cast<size_t>(full)) return {};
    auto array = std::make_shared<KtxImage>();
    array->width = first.width; array->height = first.height; array->vkFormat = first.vkFormat;
    array->isCompressed = true; array->blockSizeBytes = 16; array->mipCount = full;
    for (int level = 0; level < full; ++level) {
        array->mipByteOffsets.push_back(array->pixels.size());
        const size_t blocks = (static_cast<size_t>(std::max(1, first.width >> level)) + 3) / 4, bytes = blocks * blocks * 16;
        for (const auto& l : layers) {
            if (l.mipByteOffsets[level] + bytes > l.pixels.size()) return {};
            const auto begin = l.pixels.begin() + static_cast<ptrdiff_t>(l.mipByteOffsets[level]);
            array->pixels.insert(array->pixels.end(), begin, begin + static_cast<ptrdiff_t>(bytes));
        }
    }
    return array;
}

} // namespace RenderCore
