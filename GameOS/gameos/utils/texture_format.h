#pragma once
#include <cstdint>

// CPU texel encodings. These values also index the source decode paths.
enum TexFormat {
    TF_NONE, TF_R8, TF_RG8, TF_RGB8, TF_RGBA8,
    TF_R32F, TF_RG32F, TF_RGB32F, TF_RGBA32F, TF_COUNT
};

inline uint32_t getTexFormatPixelSize(TexFormat format) {
    constexpr uint32_t bytes[] = {0, 1, 2, 3, 4, 4, 8, 12, 16};
    return format >= TF_NONE && format < TF_COUNT ? bytes[format] : 0;
}

struct CpuTextureDesc {
    int w = 0, h = 0;
    TexFormat fmt_ = TF_NONE;
    bool has_mipmaps = false;
};
