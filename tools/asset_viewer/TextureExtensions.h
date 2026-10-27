#pragma once
#include <string>
#include <string_view>
// True if `path`'s extension is handled by some registered texture decoder
// (see TextureDecoderRegistry). Case-insensitive.
bool IsSupportedTextureFile(std::string_view path);
