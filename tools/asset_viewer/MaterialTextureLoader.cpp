// tools/asset_viewer/MaterialTextureLoader.cpp
#include "MaterialTextureLoader.h"
#include "tool_vk_gpu.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"     // 3rdparty/stb on the include path

uint32_t MaterialTextureLoader_Load(const std::string& path,
                                    MaterialSlotKind kind,
                                    std::string* errorOut) {
    int w = 0, h = 0, n = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);  // force RGBA
    if (!px) {
        if (errorOut)
            *errorOut = std::string("decode failed: ") + stbi_failure_reason();
        return 0;
    }

    const bool srgb = (kind == MaterialSlotKind::BaseColor || kind == MaterialSlotKind::Emissive);
    const uint32_t tex = toolvk::createTexture(w, h, srgb ? toolvk::TexFormat::Rgba8Srgb : toolvk::TexFormat::Rgba8Unorm,
                                               px, {}, true, true);
    if (!tex && errorOut) *errorOut = "Vulkan texture upload failed";
    stbi_image_free(px);
    return tex;
}
