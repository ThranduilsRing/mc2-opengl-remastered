// tools/asset_viewer/LegacyImageDecoder.h
#pragma once
#include "TextureDecoder.h"

// Wraps the existing UiEditorImageCache (PNG/JPG/JPEG/BMP/TGA). The GL texture
// is cache-owned, so DecodedTexture::ownsTexture is false.
class LegacyImageDecoder : public ITextureDecoder {
public:
    bool          handles(std::string_view extLower) const override;
    DecodedTexture load(const std::string& path) const override;
    std::vector<std::string> extensions() const override;
};
