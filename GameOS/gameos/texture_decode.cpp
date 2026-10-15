// texture_decode.cpp - backend-neutral decode and upload policy for gos textures (see texture_decode.h).
// No GL and no state of its own: every call works on the caller's DecodedTexture.

#include "texture_decode.h"

#include <cstring>

#include "Gates/gate.h"
#include "mc2fmt.h"
#include "utils/Image.h"
#include "utils/mc2log.h"

namespace mc2tex {
namespace {

bool endsWithPng(const char* path) {
    const size_t n = path ? std::strlen(path) : 0;
    return n >= 4 && _stricmp(path + n - 4, ".png") == 0;
}

bool hasPngSignature(const uint8_t* bytes, size_t size) {
    return size >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
}

// The in-memory TGA loader trusts its header; a cached file can be truncated or a mod file malformed, so the header and (for
// an uncompressed image) the pixel payload are checked against the byte count first. RLE payloads are bounded by the loader.
bool tgaFits(const uint8_t* bytes, size_t size) {
    constexpr size_t kHeader = 18;
    if (size <= kHeader) return false;
    const size_t descLen = bytes[0];
    const uint8_t imageType = bytes[2];
    const size_t w = (size_t)bytes[12] | ((size_t)bytes[13] << 8);
    const size_t h = (size_t)bytes[14] | ((size_t)bytes[15] << 8);
    const size_t bpp = bytes[16];
    if (w == 0 || h == 0 || (bpp != 16 && bpp != 24 && bpp != 32)) return false;
    if (imageType == 2) return size >= kHeader + descLen + w * h * (bpp / 8);
    return imageType == 10 && size > kHeader + descLen;
}

// Copies the decoded image into `out` and applies the format policy. Only RGB8 / RGBA8 sources are accepted: the lock / unlock
// and mirror paths assume 8-bit channels.
bool finishDecode(const Image& img, const char* nameForLog, bool isPng, gos_TextureFormat requested, DWORD hints,
                  DecodedTexture& out) {
    const FORMAT fmt = img.getFormat();
    if (fmt != FORMAT_RGB8 && fmt != FORMAT_RGBA8) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected {}: unsupported pixel format {:d} (need RGB8 or RGBA8)",
                mc2::fmt::safe(nameForLog ? nameForLog : "NO NAME"), mc2::fmt::as_i32((int)fmt));
        return false;
    }
    if (img.getWidth() <= 0 || img.getHeight() <= 0 || img.getPixels() == nullptr) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected {}: empty image {:d}x{:d}",
                mc2::fmt::safe(nameForLog ? nameForLog : "NO NAME"), mc2::fmt::as_i32(img.getWidth()),
                mc2::fmt::as_i32(img.getHeight()));
        return false;
    }
    out.width = img.getWidth();
    out.height = img.getHeight();
    out.layout = fmt == FORMAT_RGB8 ? PixelLayout::RGB8 : PixelLayout::RGBA8;
    const size_t bytes = (size_t)out.width * (size_t)out.height * (out.layout == PixelLayout::RGB8 ? 3u : 4u);
    out.pixels.assign(img.getPixels(), img.getPixels() + bytes);
    out.wantMipmaps = wantsMipmaps(hints);
    applyFormatPolicy(out, requested, isPng && mc2::gate::get<"MC2_PNG_MAGENTA_KEY">());
    return true;
}

}  // namespace

bool wantsMipmaps(DWORD hints) {
    return (hints & gosHint_MipmapFilter0) != 0 && (hints & gosHint_DisableMipmap) == 0;
}

void applyFormatPolicy(DecodedTexture& t, gos_TextureFormat requested, bool keyMagenta) {
    const bool rgba = t.layout == PixelLayout::RGBA8;
    const size_t n = (size_t)t.width * (size_t)t.height;
    uint8_t* p = t.pixels.data();
    if (rgba && keyMagenta) {
        // Transparent so the key cuts out, black so bilinear filtering halos dark instead of pink.
        for (size_t i = 0; i < n; ++i, p += 4)
            if (p[0] == 0xFF && p[1] == 0x00 && p[2] == 0xFF) p[0] = p[1] = p[2] = p[3] = 0;
        p = t.pixels.data();
    }
    if (requested == gos_Texture_Detect) {
        bool anyTranslucent = false;
        if (rgba)
            for (size_t i = 0; i < n && !anyTranslucent; ++i) anyTranslucent = p[i * 4 + 3] != 0xFF;
        requested = anyTranslucent ? gos_Texture_Alpha : gos_Texture_Solid;
    }
    if (requested == gos_Texture_Solid && rgba)
        for (size_t i = 0; i < n; ++i) p[i * 4 + 3] = 0xFF;
    t.format = requested;
}

bool decodeFile(const char* path, gos_TextureFormat requested, DWORD hints, DecodedTexture& out) {
    if (!path) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected a file texture with no path");
        return false;
    }
    Image img;
    if (!img.loadFromFile(path)) {
        MC2_LOG(Core, Debug, "failed to load texture from file: {}", mc2::fmt::safe(path));
        return false;
    }
    return finishDecode(img, path, endsWithPng(path), requested, hints, out);
}

bool decodeMemory(const uint8_t* bytes, size_t size, const char* nameForLog, gos_TextureFormat requested, DWORD hints,
                  DecodedTexture& out) {
    if (!bytes || size == 0) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected {}: no bytes", mc2::fmt::safe(nameForLog ? nameForLog : "NO NAME"));
        return false;
    }
    // The texture cache stores raw file bytes; the signature picks the decoder (UI pages reference .png art through this path).
    const bool png = hasPngSignature(bytes, size);
    if (!png && !tgaFits(bytes, size)) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected {}: truncated or unsupported TGA ({:d} bytes)",
                mc2::fmt::safe(nameForLog ? nameForLog : "NO NAME"), mc2::fmt::as_size(size));
        return false;
    }
    Image img;
    const bool decoded = png ? img.loadPNG(bytes, size) : img.loadTGA(bytes, size);
    if (!decoded) {
        MC2_LOG(Core, Debug, "failed to load texture from data: {}", mc2::fmt::safe(nameForLog ? nameForLog : "NO NAME"));
        return false;
    }
    return finishDecode(img, nameForLog, png, requested, hints, out);
}

bool makePlaceholder(int width, int height, gos_TextureFormat requested, DWORD hints, DecodedTexture& out) {
    if (width <= 0 || height <= 0) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected a placeholder texture of {:d}x{:d}", mc2::fmt::as_i32(width),
                mc2::fmt::as_i32(height));
        return false;
    }
    out.width = width;
    out.height = height;
    out.layout = PixelLayout::RGBA8;
    out.format = requested;
    out.wantMipmaps = wantsMipmaps(hints);
    const size_t n = (size_t)out.width * (size_t)out.height;
    out.pixels.resize(n * 4u);
    for (size_t i = 0; i < n; ++i) {
        out.pixels[i * 4 + 0] = 0xFF;
        out.pixels[i * 4 + 1] = 0xFF;
        out.pixels[i * 4 + 2] = 0x00;
        out.pixels[i * 4 + 3] = 0xFF;
    }
    return true;
}

}  // namespace mc2tex
