#pragma once
#include "PreviewSurface.h"
#include "TextureMetadata.h"
#include "TextureDecoder.h"
#include <string>

class TexturePreview2D : public PreviewSurface {
public:
    ~TexturePreview2D() override;
    void setSource(const std::string& path) override;
    void draw(const ImVec2& availableSize) override;
    const char* label() const override { return "Texture"; }
    bool hasError() const { return hasError_; }
    const std::string& errorText() const { return errorText_; }
    const TextureMetadata& metadata() const { return meta_; }
    const std::string& sourcePath() const { return path_; }
private:
    void releaseOwned();   // delete the held GL texture iff owned

    std::string     path_;
    TextureMetadata meta_;
    DecodedTexture  current_;     // holds texId + ownsTexture
    bool            hasError_ = false;
    std::string     errorText_;
    float           zoom_ = 1.0f;
};
