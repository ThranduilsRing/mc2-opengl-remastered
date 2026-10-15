// texture_decode.h - backend-neutral decode and upload policy for gos textures.
//
// WHAT: turns a gos texture source (a file, the raw bytes the texture cache holds, or a sized placeholder) into level-0 pixels plus
// the decisions every backend upload needs: the resolved alpha class (gos_Texture_Detect never survives decode) and whether the
// texture is mipmapped. Malformed or unsupported sources are rejected with a log line, never a STOP.
// WHY: the GL texture object used to decode inside its upload, so the Vulkan islands could only obtain texels by reading the GL
// texture back. With decode and policy here (no GL, no state of its own), GL uploads the result and keeps it as the CPU copy the
// Vulkan islands import through gos_GetTextureRgba.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "gameos.hpp"

namespace mc2tex {

enum class PixelLayout : uint8_t { RGB8, RGBA8 };

struct DecodedTexture {
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
// legacy colour key baked into opaque pixels), Detect resolves to Alpha or Solid from the alpha channel, and a Solid RGBA8 texture
// gets alpha forced to 255 (zero-alpha texels of a "solid" texture would otherwise blend). Sets t.format.
void applyFormatPolicy(DecodedTexture& t, gos_TextureFormat requested, bool keyMagenta);

// Decoders. Return false (with a log line) when the source is missing, undecodable, or not RGB8 / RGBA8.
bool decodeFile(const char* path, gos_TextureFormat requested, DWORD hints, DecodedTexture& out);
bool decodeMemory(const uint8_t* bytes, size_t size, const char* nameForLog, gos_TextureFormat requested, DWORD hints,
                  DecodedTexture& out);

// A sized texture with no source: filled with the debug colour so an unresolved texture is visible. Keeps the requested format.
// Returns false (with a log line) for a non-positive size.
bool makePlaceholder(int width, int height, gos_TextureFormat requested, DWORD hints, DecodedTexture& out);

}  // namespace mc2tex

// The level-0 RGBA8 texels of a gos texture handle (the bytes its upload took, or for a pre-compressed texture a one-time readback
// made by the texture object), for a backend that uploads its own copy. Null when the handle is invalid or not loaded yet
// (retry later), or when that readback failed (*unavailable set, logged; retrying will not help).
// The vector is immutable once published: a content change publishes a new one, so pointer identity tracks the content.
// Implemented by the renderer's texture list (gameos_graphics.cpp).
std::shared_ptr<const std::vector<uint8_t>> gos_GetTextureRgba(uint32_t gosHandle, uint32_t* width, uint32_t* height,
                                                               bool* unavailable = nullptr);
