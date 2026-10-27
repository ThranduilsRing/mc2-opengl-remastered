#include "TextureExtensions.h"
#include "TextureDecoderRegistry.h"

bool IsSupportedTextureFile(std::string_view path)
{
    return textureDecoderRegistry().isSupported(path);
}
