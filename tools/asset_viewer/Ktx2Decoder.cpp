// tools/asset_viewer/Ktx2Decoder.cpp
#include "Ktx2Decoder.h"
#include "tool_vk_gpu.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

static const uint8_t kKtx2Magic[12] = {
    0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,0x0d,0x0a,0x1a,0x0a
};

Ktx2DecodedImage loadKtx2Image(const std::string& path)
{
    Ktx2DecodedImage r;

    // Peek header ourselves to classify failures (ktxLoadRgba8 returns a bare bool).
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { r.error = "KTX2 file not found: " + path; return r; }
    uint8_t magic[12];
    uint32_t fields[9];   // vkFormat, typeSize, w, h, depth, layers, faces, levels, super
    bool readOk = std::fread(magic, 1, 12, f) == 12
               && std::fread(fields, 4, 9, f) == 9;
    std::fclose(f);
    if (!readOk || std::memcmp(magic, kKtx2Magic, 12) != 0) {
        r.error = "Not a KTX2 file: " + path;
        return r;
    }
    const uint32_t vkFormat = fields[0];
    const uint32_t superScheme = fields[8];
    if (superScheme != 0) {
        r.error = "Supercompressed KTX2 (Basis) not supported yet.";
        return r;
    }
    const bool known = (vkFormat == 37 || vkFormat == 43 || ((vkFormat >= 131 && vkFormat <= 134) || vkFormat == 137 || vkFormat == 138) || vkFormat == 141 || vkFormat == 145 || vkFormat == 146);
    if (!known) {
        r.error = "Unsupported KTX2 format (vkFormat=" + std::to_string(vkFormat)
                + "). Viewer supports RGBA8, BC1, BC3, BC5 and BC7.";
        return r;
    }

    // Reject KTX2 variants the viewer/loader does not support, with an accurate
    // message (otherwise these would fall through and be mislabeled "corrupt").
    const uint32_t typeSize    = fields[1];
    const uint32_t pixelDepth  = fields[4];
    const uint32_t faceCount   = fields[6];
    const uint32_t levelCount  = fields[7];
    if (faceCount != 1 || pixelDepth != 0 || typeSize != 1 || levelCount < 1) {
        r.error = "Unsupported KTX2 variant (cubemap, 3D, array, or non-byte channel type) not supported.";
        return r;
    }

    // Real load via the engine parser (also handles BC7 despite the name).
    if (!RenderCore::ktxLoadRgba8(path.c_str(), r.img)) {
        r.error = "Corrupt KTX2: mip level size/offset out of range.";
        return r;
    }
    r.ok = true;
    return r;
}

bool Ktx2Decoder::handles(const std::string& extLower) const
{
    return extLower == "ktx2";
}

std::vector<std::string> Ktx2Decoder::extensions() const
{
    return {"ktx2"};
}

DecodedTexture Ktx2Decoder::load(const std::string& path) const
{
    DecodedTexture d;
    Ktx2DecodedImage k = loadKtx2Image(path);
    if (!k.ok) { d.error = k.error; return d; }
    const RenderCore::KtxImage& img = k.img;

    d.width         = img.width;
    d.height        = img.height;
    d.mipCount      = img.mipCount;
    d.isCompressed  = img.isCompressed;

    d.texId = uploadKtx2Image(img, img.isSrgb, false);
    const char* format = img.vkFormat >= 131 && img.vkFormat <= 134 ? "BC1"
                       : img.vkFormat == 137 || img.vkFormat == 138 ? "BC3"
                       : img.vkFormat == 141 ? "BC5"
                       : img.isCompressed ? "BC7" : "RGBA8";
    d.formatLabel = std::string(format) + (img.isSrgb ? " (sRGB)" : "") + ", " + std::to_string(img.mipCount) + " mips";
    if (!d.texId) { d.error = "Vulkan texture upload or shared block decode failed for " + path; return d; }
    d.ownsTexture = true;
    return d;
}

uint32_t uploadKtx2Image(const RenderCore::KtxImage& img, bool srgb, bool repeat)
{
    // Tool residency is local to its Vulkan shell. Parsing and block decoding use
    // RenderCore's device-free contract, without a game TextureService/device.
    const bool bc7 = img.vkFormat == 145 || img.vkFormat == 146;
    const bool native = bc7 && toolvk::hasBc7();
    std::vector<uint8_t> rgba;
    std::vector<toolvk::MipLevel> levels;
    for (int level=0; level<img.mipCount; ++level) {
        if (img.isCompressed && !native) {
            std::vector<uint8_t> mip;
            if (!RenderCore::ktxDecodeBlocksToRgba8(img, level, mip)) return 0;
            levels.push_back({rgba.size(), mip.size()});
            rgba.insert(rgba.end(), mip.begin(), mip.end());
        } else {
            const size_t off = img.mipByteOffsets[level];
            const size_t next = level+1<img.mipCount ? img.mipByteOffsets[level+1] : img.pixels.size();
            levels.push_back({off, next-off});
        }
    }
    const auto format = native ? (srgb ? toolvk::TexFormat::Bc7Srgb : toolvk::TexFormat::Bc7Unorm)
                               : (srgb ? toolvk::TexFormat::Rgba8Srgb : toolvk::TexFormat::Rgba8Unorm);
    return toolvk::createTexture(img.width, img.height, format,
        rgba.empty() ? img.pixels.data() : rgba.data(), levels, false, repeat);
}
