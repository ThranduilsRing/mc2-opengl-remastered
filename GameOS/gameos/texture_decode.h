// texture_decode.h - backend-neutral decode and upload policy for gos textures.
//
// WHAT: turns a gos texture source (a file, the raw bytes the texture cache holds, or a sized placeholder) into level-0 pixels plus
// the decisions every backend upload needs: the resolved alpha class (gos_Texture_Detect never survives decode) and whether the
// texture is mipmapped. Malformed or unsupported sources are rejected with a log line, never a STOP. Pre-compressed (.ktx2 BC7)
// textures keep their blocks: the container is parsed by RenderCore::ktxParse, and the blocks are handed on as they are.
// WHY: the GL texture object used to decode inside its upload, so the Vulkan islands could only obtain texels by reading the GL
// texture back. With decode and policy here (no GL, no state of its own), GL uploads the result and keeps it as the CPU copy the
// Vulkan islands import through gos_GetTextureTexels.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "gameos.hpp"
#include "../../RenderCore/KtxLoader.h"

namespace mc2tex {

enum class PixelLayout : uint8_t { RGB8, RGBA8 };

// Fill a distinct texture-lock buffer in BGRA order. A missing mirror is transparent black.
void writeBgraLockPixels(std::span<uint8_t> destination, std::span<const uint8_t> rgba);

struct DecodedTexture {
    bool isSrgb = false;
    uint32_t sourceVkFormat = 0;
    RenderCore::KtxAlpha sourceAlpha = RenderCore::KtxAlpha::Straight;
    std::vector<std::vector<uint8_t>> mipLevels; // RGBA8 levels after alpha policy; pixels remains mip zero
    int width = 0;
    int height = 0;
    PixelLayout layout = PixelLayout::RGBA8;
    gos_TextureFormat format = gos_Texture_Solid;  // resolved alpha class; the requested one for a placeholder
    bool wantMipmaps = false;
    std::vector<uint8_t> pixels;                   // width * height * (3 | 4) bytes, row-major, R first
};

// Mipmaps are opt-in (gosHint_MipmapFilter0): many HUD / GUI loads pass hints = 0 and must stay unfiltered; DisableMipmap wins.
bool wantsMipmaps(DWORD hints);

// The per-pixel policy after decode: PNG magenta (0xFF00FF) becomes transparent black when keyMagenta is set (modder PNGs carry the
// legacy colour key baked into opaque pixels). Keyed textures also apply this policy to TGA/raw art (promoting RGB8 to RGBA8).
// Detect resolves to Alpha or Solid from the alpha channel, and a Solid RGBA8 texture
// gets alpha forced to 255 (zero-alpha texels of a "solid" texture would otherwise blend). Sets t.format.
void applyFormatPolicy(DecodedTexture& t, gos_TextureFormat requested, bool keyMagenta);

// Decoders. Empty (with a log line) when the source is missing, undecodable, or not RGB8 / RGBA8.
std::optional<DecodedTexture> decodeFile(const char* path, gos_TextureFormat requested, DWORD hints);
std::optional<DecodedTexture> decodeMemory(const uint8_t* bytes, size_t size, const char* nameForLog, gos_TextureFormat requested,
                                           DWORD hints);

// A sized texture with no source: filled with the debug colour so an unresolved texture is visible. Keeps the requested format.
// Empty (with a log line) for a non-positive size.
std::optional<DecodedTexture> makePlaceholder(int width, int height, gos_TextureFormat requested, DWORD hints);

// VK_FORMAT values the texel accessor reports (the numbering both KTX2 and Vulkan use).
inline constexpr uint32_t kVkFormatRgba8Unorm = 37;
inline constexpr uint32_t kVkFormatBc7Unorm = 145;
inline constexpr uint32_t kVkFormatBc7Srgb = 146;

// The byte size of one BC7 level of a w x h image (4x4 blocks of 16 bytes); 0 for an empty size.
size_t bc7LevelBytes(uint32_t width, uint32_t height);

// The byte size of a tightly packed BC7 chain of `mipCount` levels, level 0 first; 0 for an empty size or count.
size_t bc7ChainBytes(uint32_t width, uint32_t height, uint32_t mipCount);

// Decodes level 0 (bc7LevelBytes(width, height) bytes of blocks, optionally followed by the lower levels of a chain) to RGBA8 on
// the CPU, for a consumer that only takes RGBA8.
// The bytes come out as stored: an sRGB-encoded level stays sRGB-encoded. Empty when fewer bytes than level 0 are given.
std::vector<uint8_t> decodeBc7(const std::vector<uint8_t>& blocks, uint32_t width, uint32_t height);

// The extent of mip level `level` of a `size` edge: halved per level, never below 1.
constexpr uint32_t mipExtent(uint32_t size, uint32_t level) {
    const uint32_t e = level < 32u ? size >> level : 0u;
    return e ? e : 1u;
}

// The next mip level of an RGBA8 (UNORM) level: each texel the rounded mean of its 2x2 source footprint, per channel, on the
// stored bytes. A 1-texel edge pairs the texel with itself; an odd edge above 1 leaves its last row / column out (the floor
// halving every level takes; drivers differ there, so parity is claimed for even edges only).
// WHY: the static-prop albedo arrays are GL_RGBA8 (not sRGB), mipmapped by glGenerateMipmap, whose filter on that format is
// this box; a CPU copy of the chain built with it lets a backend without GL upload the same texels.
// Empty when `src` is not width * height * 4 bytes or an edge is 0 or above RenderCore::kKtxMaxEdge.
std::vector<uint8_t> boxDownsampleRgba8(const std::vector<uint8_t>& src, uint32_t width, uint32_t height);

// A 2D texture array as CPU levels: level n holds every layer at mipExtent(width, n) x mipExtent(height, n), layer after layer,
// RGBA8 texels (R first) or BC7 blocks. WHY: the producer that assembles an array keeps it in this form, so each backend uploads
// the same bytes and none reads another's texture object back.
struct TextureArrayCpu {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 0;
    uint32_t vkFormat = 0;  // kVkFormatRgba8Unorm, kVkFormatBc7Unorm or kVkFormatBc7Srgb
    std::vector<std::vector<uint8_t>> levels;
};

// The byte size one level of `a` must have (all layers); 0 for an unknown format.
size_t textureArrayLevelBytes(const TextureArrayCpu& a, uint32_t level);

}  // namespace mc2tex

// The level-0 texels of a gos texture handle as its upload took them, for a backend that uploads its own copy: RGBA8 for decoded
// textures, the BC7 blocks themselves for a pre-compressed (.ktx2) texture.
struct GosTexels {
    enum class Status : uint8_t {
        Ready,        // `bytes` holds the texels
        Pending,      // invalid handle or not loaded yet: retry later
        Unavailable,  // the texture has no CPU texels (a render target): retrying will not help
    };
    Status status = Status::Pending;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t vkFormat = 0;  // mc2tex::kVkFormatRgba8Unorm, kVkFormatBc7Unorm or kVkFormatBc7Srgb
    uint32_t mipCount = 1;  // BC7 only: `bytes` holds this many levels, level 0 first; every other flavour is one level
    // Immutable once published: a content change publishes a new vector, so pointer identity tracks the content.
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};

// Implemented by the renderer's texture list (gameos_graphics.cpp).
GosTexels gos_GetTextureTexels(uint32_t gosHandle);
// Render-owner generation: advances on CPU source publication/destruction, including handle reuse.
uint64_t gos_GetTextureGeneration();
