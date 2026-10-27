// tools/asset_viewer/TextureDecoderRegistry.cpp
#include "TextureDecoderRegistry.h"
#include "LegacyImageDecoder.h"
#include "Ktx2Decoder.h"
#include <algorithm>
#include <cctype>

std::string TextureExtLower(std::string_view path)
{
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext(path.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return ext;
}

void TextureDecoderRegistry::add(std::unique_ptr<ITextureDecoder> d)
{
    decoders_.push_back(std::move(d));
}

const ITextureDecoder* TextureDecoderRegistry::find(std::string_view path) const
{
    std::string ext = TextureExtLower(path);
    if (ext.empty()) return nullptr;
    for (const auto& d : decoders_)
        if (d->handles(ext)) return d.get();
    return nullptr;
}

DecodedTexture TextureDecoderRegistry::load(const std::string& path) const
{
    const ITextureDecoder* d = find(path);
    if (!d) { DecodedTexture r; r.error = "No decoder for this file type."; return r; }
    return d->load(path);
}

bool TextureDecoderRegistry::isSupported(std::string_view path) const
{
    return find(path) != nullptr;
}

std::vector<std::string> TextureDecoderRegistry::supportedExtensions() const
{
    // Accumulate from each registered decoder's self-reported extensions().
    // Dedup while preserving registration order; no hardcoded probe list.
    std::vector<std::string> out;
    for (const auto& d : decoders_) {
        for (const std::string& ext : d->extensions()) {
            bool already = false;
            for (const std::string& seen : out)
                if (seen == ext) { already = true; break; }
            if (!already) out.push_back(ext);
        }
    }
    return out;
}

// Default decoder set.
static void buildDefaultRegistry(TextureDecoderRegistry& reg)
{
    reg.add(std::make_unique<LegacyImageDecoder>());
    reg.add(std::make_unique<Ktx2Decoder>());
}

TextureDecoderRegistry& textureDecoderRegistry()
{
    static TextureDecoderRegistry reg = []{
        TextureDecoderRegistry r;
        buildDefaultRegistry(r);
        return r;
    }();
    return reg;
}
