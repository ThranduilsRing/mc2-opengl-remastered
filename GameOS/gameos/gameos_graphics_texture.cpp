// gosTexture prepares retained CPU pixels; Vulkan residency owns all GPU uploads.
// Decode and alpha/mipmap policy live in texture_decode.cpp.

#include "gameos_graphics_internal.h"

#include "diagnostic_trace.h"
#include "texture_decode.h"

#include <cstdio>
#include <vector>
#include <memory>
#include <utility>
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "utils/mc2log.h"

namespace {

// OVERLAY-MAGENTA-TEXTURE-RECON-1: names the texture that resolved to no source (the placeholder fill), the highest-value probe
// for a debug-coloured overlay. Gated MC2_OVERLAY_MAGENTA_TRACE + MC2_DIAG_TAGS=OVERLAY_MAGENTA; read via
// get_diagnostic_events("OVERLAY_MAGENTA").
void tracePlaceholder(const char* filename, const char* texname, int w, int h) {
    static const bool s_magentaTrace = false;
    if (!s_magentaTrace || !mc2_diag::tagEnabled("OVERLAY_MAGENTA"))
        return;
    char fn[256];
    char tn[256];
    auto slashes = [](char* dst, size_t cap, const char* src) {
        size_t k = 0;
        for (; k < cap - 1 && src[k]; ++k) dst[k] = (src[k] == '\\') ? '/' : src[k];
        dst[k] = '\0';
    };
    slashes(fn, sizeof(fn), filename ? filename : "");
    slashes(tn, sizeof(tn), texname ? texname : "");
    char buf[600];
    mc2::fmt::to_buf(buf, "{{\"site\":\"fallback_fill\",\"filename\":\"{}\",\"texname\":\"{}\",\"w\":{:d},\"h\":{:d}}}",
                     mc2::fmt::safe(fn), mc2::fmt::safe(tn), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h));
    mc2_diag::writeEvent("OVERLAY_MAGENTA", 1, 0, buf);
}

}  // namespace

bool gosTexture::prepareSource() {
    std::optional<mc2tex::DecodedTexture> decoded;
    if (!is_from_memory_) {
        decoded = mc2tex::decodeFile(filename_, format_, hints_);
    } else if (pcompdata_ && size_ > 0) {
        decoded = mc2tex::decodeMemory(pcompdata_, size_, filename_ ? filename_ : texname_, format_, hints_);
    } else {
        // A sized texture with no source and no bytes.
        tracePlaceholder(filename_, texname_, tex_.w, tex_.h);
        decoded = mc2tex::makePlaceholder(tex_.w, tex_.h, format_, hints_);
    }
    if (!decoded)
        return false;
    mc2tex::DecodedTexture& img = *decoded;
    format_ = img.format;   // a placeholder keeps the requested format

    const TexFormat tf = img.layout == mc2tex::PixelLayout::RGB8 ? TF_RGB8 : TF_RGBA8;
    tex_.w = img.width;
    tex_.h = img.height;
    tex_.fmt_ = tf;
    tex_.has_mipmaps = img.wantMipmaps;
    if (tf == TF_RGBA8)
        adoptRgbaMirror(img.width, img.height, std::move(img.pixels));
    else
        setRgbaMirror(img.width, img.height, tf, img.pixels.data());
    const std::unique_ptr<BYTE[]> consumedSource(std::exchange(pcompdata_, nullptr));
    size_ = 0;
    markContentMutated();
    return true;
}
