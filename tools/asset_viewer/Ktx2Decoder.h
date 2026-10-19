// tools/asset_viewer/Ktx2Decoder.h
#pragma once
#include "TextureDecoder.h"
#include "KtxLoader.h"      // RenderCore::KtxImage / ktxLoadRgba8 (RenderCore on include path)
#include <string>
#include <vector>

// Viewer-side adapter. Loads a KTX2 file into a RenderCore::KtxImage and, on
// failure, classifies WHY into a friendly message. BC7 is loaded here too --
// we deliberately do NOT expose the engine's Rgba8-named symbol to callers.
struct Ktx2DecodedImage {
    RenderCore::KtxImage img;
    bool        ok = false;
    std::string error;     // empty on success
};

Ktx2DecodedImage loadKtx2Image(const std::string& path);
// Upload the full supplied chain; material slots choose colour space, UI honours source metadata.
uint32_t uploadKtx2Image(const RenderCore::KtxImage& img, bool srgb, bool repeat);

// The .ktx2 decoder. Uploads through toolvk::createTexture.
class Ktx2Decoder : public ITextureDecoder {
public:
    bool          handles(const std::string& extLower) const override;
    DecodedTexture load(const std::string& path) const override;
    std::vector<std::string> extensions() const override;
};
