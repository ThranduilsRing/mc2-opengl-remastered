// RenderCore/KtxLoader.h
// Minimal KTX2 loader for RGBA8, BC6H, and stored BC7 textures.
// Supports:
//   VK_FORMAT_R8G8B8A8_UNORM       (37)
//   VK_FORMAT_R8G8B8A8_SRGB        (43)
//   VK_FORMAT_BC6H_UFLOAT_BLOCK   (143)  -- HDR skybox (HDRI-BC6H-1)
//   VK_FORMAT_BC6H_SFLOAT_BLOCK   (144)  -- HDR skybox signed variant
//   VK_FORMAT_BC7_UNORM_BLOCK     (145)
//   VK_FORMAT_BC7_SRGB_BLOCK      (146)
// Returns false for supercompressed files, missing files, or invalid headers.
//
// BC6H support (HDRI-BC6H-1): loader stores the raw block stream (isCompressed=true,
// vkFormat=143/144, blockSizeBytes=16) for the consumer to upload via
// glCompressedTexImage2D(GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT). BC6H blocks
// have identical geometry to BC7 (4x4, 16 bytes/block); no decoder is provided
// since BC6H is always uploaded to GL verbatim.
//
// BC7 support (COMPRESSION-BC7-STATICPROP-1): the loader stays GL-FREE and
// simply validates + concatenates the stored block-compressed mip chain. The
// consumer (gos_static_prop_batcher BC7 array path) uploads it verbatim via
// glCompressedTexSubImage3D. Per-level byteLength validation is format-aware.
//
// Phase 1+ (Basis UASTC/ETC1S transcoding) will extend this without
// changing the call site interface.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace RenderCore {

struct KtxImage {
    // Pixel data. Holds the FULL mip chain concatenated, level 0 first.
    // For RGBA8 this is raw RGBA bytes; for BC7 this is the stored block stream.
    // Legacy callers that read only mip 0 use pixels.data() (== offset 0),
    // width/height (mip-0 dims), and isSrgb — those remain valid unchanged.
    std::vector<uint8_t> pixels;
    int   width    = 0;            // mip-0 width
    int   height   = 0;            // mip-0 height
    bool  isSrgb   = false;        // vkFormat is an sRGB format (43 or 146)
    int   mipCount = 0;            // number of mip levels present in `pixels`
    std::vector<uint64_t> mipByteOffsets; // byte offset of each level in `pixels` (level 0 == 0)

    // COMPRESSION-BC7-STATICPROP-1 / HDRI-BC6H-1 format metadata.
    uint32_t vkFormat       = 0;   // raw VK_FORMAT value from the file header
    bool     isCompressed   = false; // true for BC6H (143/144) or BC7 (145/146); false for RGBA8
    uint32_t blockSizeBytes = 0;   // 16 for BC6H/BC7; 0 for uncompressed RGBA8
};

// The outcome of parsing a KTX2 container: `image` on success, otherwise `error` names the first check that failed.
struct KtxParseResult {
    std::optional<KtxImage> image;
    const char* error = nullptr;
};

// Parses a KTX2 container held in memory. A malformed or hostile file (a mod sidecar, a truncated cache file) is rejected,
// never trusted: the edges are bounded (kKtxMaxEdge), the level count cannot exceed a full mip chain, every level must have
// exactly the byte length its format and size imply and must lie inside the file, and only 2D, single-layer, single-face,
// non-supercompressed RGBA8 / BC6H / BC7 files are accepted.
inline constexpr uint32_t kKtxMaxEdge = 16384;
KtxParseResult ktxParse(std::span<const uint8_t> file);

// Reads a .ktx2 file and parses it with ktxParse. Returns false (silent) for a missing file or anything ktxParse rejects.
// Despite the historical name, this also accepts stored (non-supercompressed)
// BC7 KTX2 files — inspect out.isCompressed / out.vkFormat to dispatch upload.
bool ktxLoadRgba8(const char* path, KtxImage& out);

// CPU-side BC7 (BPTC) block decode (COMPRESSION-BC7-CPUDECODE-1).
// Decodes one mip level of an already-loaded BC7 KtxImage into a freshly
// allocated, tightly-packed RGBA8 buffer (4 bytes/pixel, top row first,
// no padding). This is the missing CPU-readable source that lets the engine
// source dimensions + texels from a .ktx2 alone (no companion .tga required).
//
//   img    : a KtxImage filled by ktxLoadRgba8 with img.isCompressed == true
//            and img.vkFormat 145/146 (BC7). Non-BC7 images return false.
//   level  : mip level to decode (0 == largest). Must be < img.mipCount.
//   outRgba: resized to mipWidth*mipHeight*4 and filled on success.
//   outW/outH (optional): receive the decoded level's dimensions.
//
// Returns false (and leaves outRgba untouched) on any precondition failure.
// GL-free, allocation only via the supplied vector, no global state.
bool ktxDecodeBc7ToRgba8(const KtxImage& img, int level,
                         std::vector<uint8_t>& outRgba,
                         int* outW = nullptr, int* outH = nullptr);

} // namespace RenderCore
