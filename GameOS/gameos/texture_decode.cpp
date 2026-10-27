// texture_decode.cpp - backend-neutral decode and upload policy for gos textures (see texture_decode.h).
// No GL and no state of its own: every call works on the caller's DecodedTexture.

#include "texture_decode.h"

#include <cstdio>
#include <cstring>
#include <span>

#include "Gates/gate.h"
#include "KtxLoader.h"
#include "mc2fmt.h"
#include "utils/Image.h"
#include "utils/mc2log.h"

namespace mc2tex {
namespace {

bool endsWithPng(const char* path) {
    const size_t n = path ? std::strlen(path) : 0;
    return n >= 4 && _stricmp(path + n - 4, ".png") == 0;
}

bool hasPngSignature(std::span<const uint8_t> bytes) {
    return bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
}

// A texture edge beyond this is malformed content, not art; it also keeps the loader's int byte counts (w * h * 4) from wrapping.
constexpr size_t kMaxTgaEdge = 16384;

// The TGA loader trusts its header; a cached file can be truncated or a mod file malformed. This checks the header before the
// loader runs: the size is bounded, an uncompressed payload must be present in full, and RLE is accepted only at 24 / 32 bpp
// (the RLE loader sizes its buffer by the stored pixel size while expanding 16-bit pixels to 4 bytes). The loader itself stops
// an RLE stream at the end of the bytes.
bool tgaFits(std::span<const uint8_t> bytes) {
    constexpr size_t kHeader = 18;
    if (bytes.size() <= kHeader) return false;
    const size_t descLen = bytes[0];
    const uint8_t imageType = bytes[2];
    const size_t w = static_cast<size_t>(bytes[12]) | (static_cast<size_t>(bytes[13]) << 8);
    const size_t h = static_cast<size_t>(bytes[14]) | (static_cast<size_t>(bytes[15]) << 8);
    const size_t bpp = bytes[16];
    if (w == 0 || h == 0 || w > kMaxTgaEdge || h > kMaxTgaEdge) return false;
    if (imageType == 2)
        return (bpp == 16 || bpp == 24 || bpp == 32) && bytes.size() >= kHeader + descLen + w * h * (bpp / 8);
    return imageType == 10 && (bpp == 24 || bpp == 32) && bytes.size() > kHeader + descLen;
}

bool endsWithTga(const char* path) {
    const size_t n = std::strlen(path);
    return n >= 4 && _stricmp(path + n - 4, ".tga") == 0;
}

// Copies the decoded image into `out` and applies the format policy. Only RGB8 / RGBA8 sources are accepted: the lock / unlock
// and mirror paths assume 8-bit channels.
std::optional<DecodedTexture> finishDecode(const Image& img, const char* nameForLog, bool isPng, gos_TextureFormat requested,
                                           DWORD hints) {
    const FORMAT fmt = img.getFormat();
    if (fmt != FORMAT_RGB8 && fmt != FORMAT_RGBA8) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected {}: unsupported pixel format {:d} (need RGB8 or RGBA8)",
                mc2::fmt::safe(nameForLog ? nameForLog : "NO NAME"), mc2::fmt::as_i32(static_cast<int>(fmt)));
        return std::nullopt;
    }
    if (img.getWidth() <= 0 || img.getHeight() <= 0 || img.getPixels() == nullptr) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected {}: empty image {:d}x{:d}",
                mc2::fmt::safe(nameForLog ? nameForLog : "NO NAME"), mc2::fmt::as_i32(img.getWidth()),
                mc2::fmt::as_i32(img.getHeight()));
        return std::nullopt;
    }
    DecodedTexture out;
    out.width = img.getWidth();
    out.height = img.getHeight();
    out.layout = fmt == FORMAT_RGB8 ? PixelLayout::RGB8 : PixelLayout::RGBA8;
    const size_t bytes = static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * (out.layout == PixelLayout::RGB8 ? 3u : 4u);
    out.pixels.assign(img.getPixels(), img.getPixels() + bytes);
    out.wantMipmaps = wantsMipmaps(hints);
    applyFormatPolicy(out, requested, isPng);
    return out;
}

}  // namespace

bool wantsMipmaps(DWORD hints) {
    return (hints & gosHint_MipmapFilter0) != 0 && (hints & gosHint_DisableMipmap) == 0;
}

void applyFormatPolicy(DecodedTexture& t, gos_TextureFormat requested, bool keyMagenta) {
    const bool rgba = t.layout == PixelLayout::RGBA8;
    const size_t n = static_cast<size_t>(t.width) * static_cast<size_t>(t.height);
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

std::optional<DecodedTexture> decodeFile(const char* path, gos_TextureFormat requested, DWORD hints) {
    if (!path) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected a file texture with no path");
        return std::nullopt;
    }
    // A TGA file goes through the same header check as cached bytes: read it whole, decode from memory.
    if (endsWithTga(path) || (std::strlen(path) >= 5 && _stricmp(path + std::strlen(path) - 5, ".ktx2") == 0)) {
        std::vector<uint8_t> bytes;
        if (FILE* f = std::fopen(path, "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0) {
                bytes.resize(static_cast<size_t>(n));
                if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
            }
            std::fclose(f);
        }
        if (bytes.empty()) {
            MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected {}: file missing, empty or unreadable", mc2::fmt::safe(path));
            return std::nullopt;
        }
        return decodeMemory(bytes.data(), bytes.size(), path, requested, hints);
    }
    Image img;
    if (!img.loadFromFile(path)) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected {}: image loader failed", mc2::fmt::safe(path));
        return std::nullopt;
    }
    return finishDecode(img, path, endsWithPng(path), requested, hints);
}

std::optional<DecodedTexture> decodeMemory(const uint8_t* bytes, size_t size, const char* nameForLog, gos_TextureFormat requested,
                                           DWORD hints) {
    if (!bytes || size == 0) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected {}: no bytes", mc2::fmt::safe(nameForLog ? nameForLog : "NO NAME"));
        return std::nullopt;
    }
    if (size >= 12 && bytes[0] == 0xab && bytes[1] == 'K' && bytes[2] == 'T' && bytes[3] == 'X') {
        auto parsed = RenderCore::ktxParse(std::span<const uint8_t>(bytes, size));
        if (!parsed.image) return std::nullopt;
        const auto& img = *parsed.image;
        DecodedTexture out;
        out.isSrgb = img.isSrgb; out.sourceVkFormat = img.vkFormat; out.sourceAlpha = img.alpha;
        out.wantMipmaps = wantsMipmaps(hints);
        for (int level = 0; level < (out.wantMipmaps ? img.mipCount : 1); ++level) {
            DecodedTexture mip;
            mip.width = mipExtent(img.width, level); mip.height = mipExtent(img.height, level);
            if (img.isCompressed) {
                if (!RenderCore::ktxDecodeBlocksToRgba8(img, level, mip.pixels)) return std::nullopt;
            } else {
                const size_t at = img.mipByteOffsets[level], n = size_t(mip.width) * mip.height * 4;
                mip.pixels.assign(img.pixels.begin() + at, img.pixels.begin() + at + n);
            }
            applyFormatPolicy(mip, requested, false);
            if (level == 0) { out.width = mip.width; out.height = mip.height; out.format = mip.format; out.pixels = mip.pixels; }
            out.mipLevels.push_back(std::move(mip.pixels));
        }
        return out;
    }
    // The texture cache stores raw file bytes; the signature picks the decoder (UI pages reference .png art through this path).
    const bool png = hasPngSignature(std::span<const uint8_t>(bytes, size));
    if (!png && !tgaFits(std::span<const uint8_t>(bytes, size))) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected {}: truncated or unsupported TGA ({:d} bytes)",
                mc2::fmt::safe(nameForLog ? nameForLog : "NO NAME"), mc2::fmt::as_size(size));
        return std::nullopt;
    }
    Image img;
    const bool decoded = png ? img.loadPNG(bytes, size) : img.loadTGA(bytes, size);
    if (!decoded) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected {}: image loader failed",
                mc2::fmt::safe(nameForLog ? nameForLog : "NO NAME"));
        return std::nullopt;
    }
    return finishDecode(img, nameForLog, png, requested, hints);
}

std::optional<DecodedTexture> makePlaceholder(int width, int height, gos_TextureFormat requested, DWORD hints) {
    if (width <= 0 || height <= 0) {
        MC2_LOG(Core, Warn, "[TEXTURE_DECODE v1] rejected a placeholder texture of {:d}x{:d}", mc2::fmt::as_i32(width),
                mc2::fmt::as_i32(height));
        return std::nullopt;
    }
    DecodedTexture out;
    out.width = width;
    out.height = height;
    out.layout = PixelLayout::RGBA8;
    out.format = requested;
    out.wantMipmaps = wantsMipmaps(hints);
    const size_t n = static_cast<size_t>(out.width) * static_cast<size_t>(out.height);
    out.pixels.resize(n * 4u);
    for (size_t i = 0; i < n; ++i) {
        out.pixels[i * 4 + 0] = 0xFF;
        out.pixels[i * 4 + 1] = 0xFF;
        out.pixels[i * 4 + 2] = 0x00;
        out.pixels[i * 4 + 3] = 0xFF;
    }
    return out;
}

size_t bc7LevelBytes(uint32_t width, uint32_t height) {
    return (static_cast<size_t>((width + 3u)) / 4u) * (static_cast<size_t>((height + 3u)) / 4u) * 16u;
}

std::vector<uint8_t> decodeBc7(const std::vector<uint8_t>& blocks, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > RenderCore::kKtxMaxEdge || height > RenderCore::kKtxMaxEdge ||
        blocks.size() != bc7LevelBytes(width, height))
        return {};
    RenderCore::KtxImage img;
    img.pixels = blocks;
    img.width = static_cast<int>(width);
    img.height = static_cast<int>(height);
    img.mipCount = 1;
    img.mipByteOffsets = {0};
    img.vkFormat = kVkFormatBc7Unorm;   // the decoder ignores the colour space: the bytes come out as stored
    img.isCompressed = true;
    img.blockSizeBytes = 16;
    std::vector<uint8_t> rgba;
    if (!RenderCore::ktxDecodeBc7ToRgba8(img, 0, rgba)) return {};
    return rgba;
}

std::vector<uint8_t> boxDownsampleRgba8(const std::vector<uint8_t>& src, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > RenderCore::kKtxMaxEdge || height > RenderCore::kKtxMaxEdge ||
        src.size() != size_t{width} * height * 4u)
        return {};
    const uint32_t dw = mipExtent(width, 1), dh = mipExtent(height, 1);
    auto at = [](uint32_t x, uint32_t y, uint32_t rowTexels) { return (size_t{y} * rowTexels + x) * 4u; };
    std::vector<uint8_t> dst(size_t{dw} * dh * 4u);
    for (uint32_t y = 0; y < dh; ++y) {
        const uint32_t y0 = 2u * y < height ? 2u * y : height - 1u;
        const uint32_t y1 = y0 + 1u < height ? y0 + 1u : y0;
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t x0 = 2u * x < width ? 2u * x : width - 1u;
            const uint32_t x1 = x0 + 1u < width ? x0 + 1u : x0;
            const uint8_t* a = &src[at(x0, y0, width)];
            const uint8_t* b = &src[at(x1, y0, width)];
            const uint8_t* c = &src[at(x0, y1, width)];
            const uint8_t* d = &src[at(x1, y1, width)];
            uint8_t* o = &dst[at(x, y, dw)];
            for (int ch = 0; ch < 4; ++ch) o[ch] = static_cast<uint8_t>((a[ch] + b[ch] + c[ch] + d[ch] + 2u) / 4u);
        }
    }
    return dst;
}

size_t textureArrayLevelBytes(const TextureArrayCpu& a, uint32_t level) {
    const uint32_t w = mipExtent(a.width, level), h = mipExtent(a.height, level);
    if (a.vkFormat == kVkFormatRgba8Unorm) return size_t{w} * h * 4u * a.layers;
    if (a.vkFormat == 141u || a.vkFormat == kVkFormatBc7Unorm || a.vkFormat == kVkFormatBc7Srgb) return bc7LevelBytes(w, h) * a.layers;
    return 0;
}

}  // namespace mc2tex
