// tools/asset_viewer/LegacyImageDecoder.cpp
#include "LegacyImageDecoder.h"
#include "tool_vk_gpu.h"
#include "stb_image.h"   // implementation lives in MaterialTextureLoader.cpp
#include <cstdint>
#include <vector>

std::vector<std::string> LegacyImageDecoder::extensions() const
{
    return {"png","jpg","jpeg","bmp","tga"};
}

bool LegacyImageDecoder::handles(std::string_view extLower) const
{
    return extLower == "png" || extLower == "jpg" || extLower == "jpeg"
        || extLower == "bmp" || extLower == "tga";
}

DecodedTexture LegacyImageDecoder::load(const std::string& path) const
{
    DecodedTexture d;
    int w = 0, h = 0, n = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);   // force RGBA
    if (!px) {
        d.error = std::string("Failed to load image (not found or decode error): ") + stbi_failure_reason();
        return d;
    }
    d.texId = toolvk::createTexture(w, h, toolvk::TexFormat::Rgba8Unorm, px, {}, false, false);
    stbi_image_free(px);
    if (!d.texId) { d.error = "Vulkan texture upload failed."; return d; }
    d.width         = w;
    d.height        = h;
    d.mipCount      = 1;
    d.formatLabel   = "RGBA8";
    d.isCompressed  = false;
    d.ownsTexture   = true;
    return d;
}
