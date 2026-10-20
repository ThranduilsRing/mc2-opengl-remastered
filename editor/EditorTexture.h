#include "../RenderCore/TextureService.h"
// Editor-owned UI images share the session residency; no GL producer or borrowed descriptor.
#pragma once
#include "../GameOS/gameos/ui_texture.h"
#include "../GameOS/gameos/texture_decode.h"
#include "file.h"

inline RenderCore::ServiceTextureRef loadEditorTexture(const char* path) {
    if (!RenderCore::activeTextureService || !mc2vk::uiTextureServiceReady()) return {};
    File source;
    if (source.open(path) != NO_ERR) return {};
    const long size = source.fileSize();
    if (size <= 0 || size > 256 * 1024 * 1024) return {};
    std::vector<uint8_t> bytes(size);
    if (source.read(bytes.data(), size) != size) return {};
    const char* mod = LookupModOwner(path);
    const std::string provider = mod ? std::string("mod:") + mod : source.isLoadedFromDisk() ? "base-loose" : "fst";
    auto decoded = mc2tex::decodeMemory(bytes.data(), bytes.size(), path, gos_Texture_Alpha, 0);
    if (!decoded) return {};
    auto image = std::make_shared<RenderCore::KtxImage>();
    image->width = decoded->width; image->height = decoded->height;
    image->vkFormat = 37; // UI displays encoded colour bytes, matching the legacy atlas policy.
    image->mipCount = 1; image->mipByteOffsets = {0};
    image->pixels = std::move(decoded->pixels);
    if (decoded->layout == mc2tex::PixelLayout::RGB8) {
        const auto rgb = std::move(image->pixels);
        image->pixels.resize(size_t(image->width) * image->height * 4);
        for (size_t i=0; i<rgb.size()/3; ++i) {
            for (int c=0; c<3; ++c) image->pixels[i*4+c]=rgb[i*3+c];
            image->pixels[i*4+3]=255;
        }
    }
    RenderCore::TextureKey key;
    key.source = RenderCore::makeTextureSourceId(provider, path,
        RenderCore::textureContentHash(bytes.data(), bytes.size()));
    key.format = image->vkFormat;
    auto& service = RenderCore::textureService();
    auto handle = service.acquire(key, RenderCore::kNoTextureNode,
        {uint32_t(image->width),uint32_t(image->height),uint32_t(image->width),uint32_t(image->height)});
    if (handle.isValid()) mc2vk::attachUiImage(handle, std::move(image));
    return RenderCore::ServiceTextureRef::adopt(service, handle);
}
