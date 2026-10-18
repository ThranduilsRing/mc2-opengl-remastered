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
    const bool known = (vkFormat == 37 || vkFormat == 43 || vkFormat == 145 || vkFormat == 146);
    if (!known) {
        r.error = "Unsupported KTX2 format (vkFormat=" + std::to_string(vkFormat)
                + "). Viewer supports RGBA8 and BC7.";
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

    if (!img.isCompressed) {
        d.texId = toolvk::createTexture(img.width, img.height,
                                        img.isSrgb ? toolvk::TexFormat::Rgba8Srgb : toolvk::TexFormat::Rgba8Unorm,
                                        img.pixels.data() + img.mipByteOffsets[0], {}, false, false);
        d.formatLabel = img.isSrgb ? "RGBA8 (sRGB)" : "RGBA8";
    } else {
        if (!toolvk::hasBc7()) {
            d.error = "BC7 preview requires textureCompressionBC, unavailable on this GPU.";
            return d;
        }
        std::vector<toolvk::MipLevel> levels;
        for (int lvl = 0; lvl < img.mipCount; ++lvl) {
            const uint64_t off  = img.mipByteOffsets[lvl];
            const uint64_t next = (lvl + 1 < img.mipCount) ? img.mipByteOffsets[lvl + 1] : (uint64_t)img.pixels.size();
            levels.push_back({(size_t)off, (size_t)(next - off)});
        }
        d.texId = toolvk::createTexture(img.width, img.height,
                                        img.isSrgb ? toolvk::TexFormat::Bc7Srgb : toolvk::TexFormat::Bc7Unorm,
                                        img.pixels.data(), levels, false, false);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "BC7%s, %d mip%s",
                      img.isSrgb ? " (sRGB)" : "", img.mipCount, img.mipCount > 1 ? "s" : "");
        d.formatLabel = buf;
    }
    if (!d.texId) { d.error = "Vulkan texture upload failed for " + path; return d; }
    d.ownsTexture = true;
    return d;
}
