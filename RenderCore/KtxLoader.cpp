// RenderCore/KtxLoader.cpp
// Minimal KTX2 RGBA8 loader -- Phase 0 (no Basis/supercompression transcoding).
// No GL headers. No external dependencies.

#include "KtxLoader.h"

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

// KTX2 header fields we need (all uint32, little-endian, file offsets 12-44).
// We do NOT use the struct approach for the full header because mixing
// uint32_t and uint64_t fields causes compiler padding before the uint64_t
// members, making sizeof() != the on-disk layout.
// Strategy: read the 9 essential uint32 fields (36 bytes), then fseek to
// file offset 80 (start of level index) to skip the rest of the header.
// KTX2 level index starts at file offset 80 by spec:
//   12 (magic) + 9*4 + 4*4 + 2*8 = 12 + 36 + 16 + 16 = 80.
struct Ktx2Fields {
    uint32_t vkFormat;
    uint32_t typeSize;
    uint32_t pixelWidth;
    uint32_t pixelHeight;
    uint32_t pixelDepth;
    uint32_t layerCount;
    uint32_t faceCount;
    uint32_t levelCount;
    uint32_t supercompressionScheme;
};
static_assert(sizeof(Ktx2Fields) == 36, "Ktx2Fields must be 36 bytes (all uint32)");

// Helper: read exactly `n` bytes; return false on short read or error.
static bool readExact(std::FILE* f, void* buf, std::size_t n)
{
    return std::fread(buf, 1, n, f) == n;
}

bool ktxLoadRgba8(const char* path, KtxImage& out)
{
    std::FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;

    // ---- 1. Magic check ----
    uint8_t magic[12];
    if (!readExact(f, magic, 12) || std::memcmp(magic, kKtx2Magic, 12) != 0) {
        std::fclose(f);
        return false;
    }

    // ---- 2. Read the 9 essential uint32 fields (36 bytes) ----
    Ktx2Fields hdr{};
    if (!readExact(f, &hdr, sizeof(hdr))) {
        std::fclose(f);
        return false;
    }

    // ---- 3. Validate ----
    const bool isRgba8 = (hdr.vkFormat == kVkFormatR8G8B8A8Unorm ||
                          hdr.vkFormat == kVkFormatR8G8B8A8Srgb);
    const bool isBc6h  = (hdr.vkFormat == kVkFormatBc6hUfloat ||
                          hdr.vkFormat == kVkFormatBc6hSfloat);
    const bool isBc7   = (hdr.vkFormat == kVkFormatBc7UnormBlock ||
                          hdr.vkFormat == kVkFormatBc7SrgbBlock);
    // BC6H and BC7 both use 4x4 blocks of 16 bytes — same block geometry.
    const bool isBlockCompressed = isBc6h || isBc7;
    if (!isRgba8 && !isBlockCompressed) {
        std::fclose(f);
        return false;
    }
    // typeSize for block-compressed formats is 1 by KTX2 spec; for RGBA8 it is
    // the size of the channel type (1 byte). Either way we require 1.
    if (hdr.typeSize != 1 ||
        hdr.pixelDepth != 0 ||
        hdr.faceCount  != 1 ||
        hdr.levelCount  < 1 ||
        hdr.supercompressionScheme != 0) {   // stored only -- no transcoding
        std::fclose(f);
        return false;
    }

    // ---- 4. Seek to level index at file offset 80 ----
    // Skips dfdByteOffset/Length, kvdByteOffset/Length (4*4=16 bytes) and
    // sgdByteOffset/Length (2*8=16 bytes). Total header = 12+36+16+16 = 80.
    if (std::fseek(f, 80, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }

    // ---- 5. Level index: read ALL levelCount entries ----
    // Each entry: byteOffset (uint64), byteLength (uint64), uncompressedByteLength (uint64)
    // Spec: level index is ordered largest-mip-first (mip 0 = entry[0]).
    struct LevelEntry {
        uint64_t byteOffset;
        uint64_t byteLength;
        uint64_t uncompressedByteLength;
    };
    std::vector<LevelEntry> entries(hdr.levelCount);
    if (!readExact(f, entries.data(),
                   sizeof(LevelEntry) * static_cast<std::size_t>(hdr.levelCount))) {
        std::fclose(f);
        return false;
    }

    // ---- 6. Validate each level's byteLength against expected RGBA8 size,
    //         and accumulate the concatenated buffer size (level 0 first). ----
    out.mipByteOffsets.clear();
    out.mipByteOffsets.resize(hdr.levelCount);
    uint64_t totalBytes = 0;
    for (uint32_t lvl = 0; lvl < hdr.levelCount; ++lvl) {
        const uint32_t lw = (hdr.pixelWidth  >> lvl) ? (hdr.pixelWidth  >> lvl) : 1u;
        const uint32_t lh = (hdr.pixelHeight >> lvl) ? (hdr.pixelHeight >> lvl) : 1u;
        uint64_t expected;
        if (isBlockCompressed) {
            // BC6H and BC7: identical 4x4 blocks, 16 bytes/block.
            // ceil(lw/4)*ceil(lh/4)*16.
            const uint64_t bw = (static_cast<uint64_t>(lw) + 3u) / 4u;
            const uint64_t bh = (static_cast<uint64_t>(lh) + 3u) / 4u;
            expected = bw * bh * 16u;
        } else {
            expected = static_cast<uint64_t>(lw) * static_cast<uint64_t>(lh) * 4u;
        }
        if (entries[lvl].byteLength != expected) {
            std::fclose(f);
            return false;   // pre-baked mip chain mismatch -> fail; caller falls back.
        }
        out.mipByteOffsets[lvl] = totalBytes;   // level 0 -> offset 0
        totalBytes += expected;
    }

    // ---- 7. Read each level's bytes into the concatenated buffer ----
    out.pixels.resize(static_cast<std::size_t>(totalBytes));
    for (uint32_t lvl = 0; lvl < hdr.levelCount; ++lvl) {
        if (std::fseek(f, static_cast<long>(entries[lvl].byteOffset), SEEK_SET) != 0) {
            std::fclose(f);
            return false;
        }
        const std::size_t off = static_cast<std::size_t>(out.mipByteOffsets[lvl]);
        const std::size_t len = static_cast<std::size_t>(entries[lvl].byteLength);
        if (!readExact(f, out.pixels.data() + off, len)) {
            std::fclose(f);
            return false;
        }
    }

    std::fclose(f);

    // ---- 8. Fill output ----
    out.width          = static_cast<int>(hdr.pixelWidth);
    out.height         = static_cast<int>(hdr.pixelHeight);
    out.isSrgb         = (hdr.vkFormat == kVkFormatR8G8B8A8Srgb ||
                          hdr.vkFormat == kVkFormatBc7SrgbBlock);
    // BC6H is always linear HDR; it has no sRGB variant.
    out.mipCount       = static_cast<int>(hdr.levelCount);
    out.vkFormat       = hdr.vkFormat;
    out.isCompressed   = isBlockCompressed;  // true for BC6H (143/144) and BC7 (145/146)
    out.blockSizeBytes = isBlockCompressed ? 16u : 0u;

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
