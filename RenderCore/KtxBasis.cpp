// KtxBasis.cpp - Basis Universal (UASTC / ETC1S, optionally Zstd) KTX2 files transcoded to BC blocks at load.
//
// WHAT: ktxParse hands a KTX2 whose vkFormat is 0 (Basis payload) or whose supercompression scheme is set to ktxParseBasis. UASTC
// becomes BC7, ETC1S becomes BC3 when it carries alpha and BC1 otherwise; the result is an ordinary KtxImage of BC blocks, so
// every consumer (residency upload, decode fallback) sees what it sees for a stored BC file.
// WHY: Basis files are small on disk and transcode to the GPU's native block format in milliseconds; the transcoder is vendored
// under 3rdparty/basisu (Apache-2.0). 2D single-layer single-face only: no current caller uses arrays or cubes.
#include "KtxLoader.h"

#include <algorithm>
#include <cstdint>
#include <mutex>

#include "../3rdparty/basisu/transcoder/basisu_transcoder.h"

namespace RenderCore {

namespace {

void initTranscoder() {
    static std::once_flag once;
    std::call_once(once, [] { basist::basisu_transcoder_init(); });
}

size_t levelBlockBytes(uint32_t w, uint32_t h, uint32_t level, uint32_t blockBytes) {
    const uint32_t lw = std::max(1u, w >> level);
    const uint32_t lh = std::max(1u, h >> level);
    return static_cast<size_t>((lw + 3u) / 4u) *((lh + 3u) / 4u) * blockBytes;
}

}  // namespace

KtxParseResult ktxParseBasis(std::span<const uint8_t> file) {
    KtxParseResult r;
    initTranscoder();
    basist::ktx2_transcoder transcoder;
    if (file.size() > UINT32_MAX || !transcoder.init(file.data(), static_cast<uint32_t>(file.size()))) {
        r.error = "basis ktx2 header rejected";
        return r;
    }
    if (transcoder.get_layers() > 1 || transcoder.get_faces() != 1 || !transcoder.is_ldr() ||
        !(transcoder.is_uastc() || transcoder.is_etc1s())) {
        r.error = "basis ktx2 is not a 2D single-layer LDR UASTC/ETC1S image";
        return r;
    }
    const uint32_t width = transcoder.get_width(), height = transcoder.get_height(), levels = transcoder.get_levels();
    if (width == 0 || height == 0 || width > kKtxMaxEdge || height > kKtxMaxEdge) { r.error = "edge out of range"; return r; }
    uint32_t fullChain = 1;
    for (uint32_t m = std::max(width, height); m > 1; m >>= 1) ++fullChain;
    if (levels < 1 || levels > fullChain) { r.error = "level count out of range"; return r; }
    if (!transcoder.start_transcoding()) { r.error = "basis ktx2 transcode setup failed"; return r; }

    const bool uastc = transcoder.is_uastc();
    const bool alpha = transcoder.get_has_alpha() != 0;
    const bool srgb = transcoder.get_dfd_transfer_func() == basist::KTX2_KHR_DF_TRANSFER_SRGB;
    const auto target = uastc ? basist::transcoder_texture_format::cTFBC7_RGBA
                              : alpha ? basist::transcoder_texture_format::cTFBC3_RGBA : basist::transcoder_texture_format::cTFBC1_RGB;
    KtxImage img;
    img.width = static_cast<int>(width);
    img.height = static_cast<int>(height);
    img.mipCount = static_cast<int>(levels);
    img.isCompressed = true;
    img.blockSizeBytes = target == basist::transcoder_texture_format::cTFBC1_RGB ? 8u : 16u;
    img.vkFormat = uastc ? (srgb ? 146u : 145u) : alpha ? (srgb ? 138u : 137u) : (srgb ? 132u : 131u);
    img.isSrgb = srgb;
    img.alpha = alpha ? KtxAlpha::Straight : KtxAlpha::Opaque;
    for (uint32_t level = 0; level < levels; ++level) {
        const size_t bytes = levelBlockBytes(width, height, level, img.blockSizeBytes);
        const size_t at = img.pixels.size();
        img.mipByteOffsets.push_back(at);
        img.pixels.resize(at + bytes);
        if (!transcoder.transcode_image_level(level, 0, 0, img.pixels.data() + at, static_cast<uint32_t>(bytes / img.blockSizeBytes),
                                              target)) {
            r.error = "basis ktx2 level transcode failed";
            return r;
        }
    }
    r.image = std::move(img);
    return r;
}

}  // namespace RenderCore
