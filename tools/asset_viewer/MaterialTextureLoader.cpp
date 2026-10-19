// tools/asset_viewer/MaterialTextureLoader.cpp
#include "MaterialTextureLoader.h"
#include "tool_vk_gpu.h"
#include "Ktx2Decoder.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"     // 3rdparty/stb on the include path

uint32_t MaterialTextureLoader_Load(const std::string& path,
                                    MaterialSlotKind kind,
                                    std::string* errorOut) {
    const bool srgb = (kind == MaterialSlotKind::BaseColor || kind == MaterialSlotKind::Emissive);
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (extension == ".ktx2") {
        auto decoded = loadKtx2Image(path); // no game renderer or device is created by the decoder
        if (!decoded.ok) { if (errorOut) *errorOut = decoded.error; return 0; }
        const auto texture = uploadKtx2Image(decoded.img, srgb, true);
        if (!texture && errorOut) *errorOut = "Vulkan texture upload or shared block decode failed";
        return texture;
    }
    int w = 0, h = 0, n = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);  // force RGBA
    if (!px) {
        if (errorOut)
            *errorOut = std::string("decode failed: ") + stbi_failure_reason();
        return 0;
    }

    const uint32_t tex = toolvk::createTexture(w, h, srgb ? toolvk::TexFormat::Rgba8Srgb : toolvk::TexFormat::Rgba8Unorm,
                                               px, {}, true, true);
    if (!tex && errorOut) *errorOut = "Vulkan texture upload failed";
    stbi_image_free(px);
    return tex;
}
