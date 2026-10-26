// tools/asset_viewer/TextureDecoderRegistry.h
#pragma once
#include "TextureDecoder.h"
#include <memory>
#include <vector>

class TextureDecoderRegistry {
public:
    void add(std::unique_ptr<ITextureDecoder> d);
    const ITextureDecoder* find(std::string_view path) const;     // by extension; null if none
    DecodedTexture load(const std::string& path) const;             // find+load, or DecodedTexture{error}
    bool isSupported(std::string_view path) const;                // extension handled by some decoder
    std::vector<std::string> supportedExtensions() const;           // union, lowercased w/o dot
private:
    std::vector<std::unique_ptr<ITextureDecoder>> decoders_;
};

// Process-wide registry, lazily populated with the default decoders.
TextureDecoderRegistry& textureDecoderRegistry();
