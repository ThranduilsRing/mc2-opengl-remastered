#include "mc2_czstring.h"
#include "gos_hdri.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1

#include <cstdio>
#include <cstdlib>  // for free()
#include <cmath>    // for atan2, fabsf, cosf, sinf
#include <cstring>  // for strlen, memcpy
#include <cstddef>
#include <vector>
#include <algorithm>
#include <optional>

// tinyexr brings in miniz transitively. Define implementation macros
// in EXACTLY this one TU. miniz.c is compiled separately (registered
// in CMake at T7).
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

// HDRI-BC6H-1: BC6H sidecar upload via KtxLoader (GL-free loader, BC6H blocks).
#include "../../RenderCore/KtxLoader.h"
#include "../../mclib/texture_assets.h"
#include "../../mclib/file.h"
#include "../../3rdparty/bcdec/bcdec.h"
#include "Gates/gate.h"
#include "Gates/render_options.h"
#include "mc2fmt.h"

// HDRI-SKY Item 2: scan a decoded equirect RGBA-float buffer for the dominant
// above-horizon light source and return its GL-equirect azimuth (radians).
//
// Coordinate convention MUST match hdri_skybox.frag exactly:
//   the frag samples uv.x = atan(worldDir.z, worldDir.x)/(2PI) + 0.5,
//                    uv.y = 1.0 - (asin(worldDir.y)/PI + 0.5).
// EXR row 0 is the TOP of the image (v=0 = zenith, worldDir.y=+1). A texel at
// image column c (u=(c+0.5)/w) therefore maps to GL azimuth
//   azGL = (u - 0.5) * 2*PI   (so atan(z,x) of the recovered worldDir).
// "Above horizon" = v < 0.5 (upper half), matching asin(y) > 0.
//
// Method: luminance-weighted centroid over the brightest above-horizon texels.
// To find "the sun" robustly we (1) compute the max luminance over the upper
// half, (2) keep texels within a fraction of that max, (3) take the
// luminance-weighted *circular* mean of their azimuths (sum of unit vectors)
// so the wrap at +/-PI is handled correctly. Returns nullopt if no above-horizon
// energy is found.
static std::optional<float> scanHdriSunAzGL(const float* px, int w, int h)
{
    if (!px || w <= 0 || h <= 0) return std::nullopt;

    const int halfRows = h / 2;          // upper half = above horizon
    if (halfRows <= 0) return std::nullopt;

    // Pass 1: peak luminance in the upper half.
    float maxLum = 0.0f;
    for (int r = 0; r < halfRows; ++r) {
        const float* row = px + static_cast<size_t>(r) * static_cast<size_t>(w) * 4u;
        for (int c = 0; c < w; ++c) {
            const float* p = row + static_cast<size_t>(c) * 4u;
            // Rec.709 luma; HDRI is linear so this is physical luminance.
            const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            maxLum = std::max(maxLum, lum);
        }
    }
    if (!(maxLum > 0.0f)) return std::nullopt;

    // Pass 2: luminance-weighted circular mean of azimuths over the brightest
    // texels (top fraction of peak). 0.5 of peak keeps the sun disk + immediate
    // glow while rejecting general sky brightness.
    const float thresh = maxLum * 0.5f;
    const float TWO_PI = 6.28318530717958647692f;
    double sx = 0.0, sy = 0.0;           // weighted unit-vector accumulators
    double wsum = 0.0;
    for (int r = 0; r < halfRows; ++r) {
        const float* row = px + static_cast<size_t>(r) * static_cast<size_t>(w) * 4u;
        for (int c = 0; c < w; ++c) {
            const float* p = row + static_cast<size_t>(c) * 4u;
            const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            if (lum < thresh) continue;
            const float u   = (static_cast<float>(c) + 0.5f) / static_cast<float>(w);
            const float az  = (u - 0.5f) * TWO_PI;   // GL atan(z,x) azimuth
            sx += static_cast<double>(lum) * static_cast<double>(cosf(az));
            sy += static_cast<double>(lum) * static_cast<double>(sinf(az));
            wsum += static_cast<double>(lum);
        }
    }
    if (!(wsum > 0.0)) return std::nullopt;
    return static_cast<float>(atan2(sy, sx));
}

// HDRI-BC6H-1: gate — MC2_HDRI_BC6H controls BC6H sidecar upload.
// Default ON (absent = ON); set =0 to force RGBA16F fallback path.
// Read at first use, not at static-init time: the launcher writes this variable after the exe starts.
static bool hdriBc6hOn()
{
    static const bool on = true;
    return on;
}

// Use the same overlay/FST provider as the cooked-sidecar decision.
static int loadSourceExr(float** pixels, int* w, int* h, const char* path, const char** error)
{
    File source;
    if (source.open(path) != NO_ERR) return TINYEXR_ERROR_CANT_OPEN_FILE;
    const long size = source.fileSize();
    if (size <= 0 || size > 512 * 1024 * 1024) return TINYEXR_ERROR_INVALID_DATA;
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    if (source.read(bytes.data(), size) != size) return TINYEXR_ERROR_INVALID_DATA;
    return LoadEXRFromMemory(pixels, w, h, bytes.data(), bytes.size(), error);
}

// Derive sidecar path: replace trailing ".exr" with ".ktx2".
// Returns false if path does not end with ".exr" or output buffer is too small.
static bool deriveSidecarKtx2(mc2::czstring exrPath, char* outBuf, size_t outBufLen)
{
    if (!exrPath || !outBuf || outBufLen < 2) return false;
    const size_t n = strlen(exrPath);
    if (n < 4) return false;
    if (exrPath[n-4] != '.' ||
        (exrPath[n-3] != 'e' && exrPath[n-3] != 'E') ||
        (exrPath[n-2] != 'x' && exrPath[n-2] != 'X') ||
        (exrPath[n-1] != 'r' && exrPath[n-1] != 'R')) {
        return false;
    }
    if (n - 4 + 5 + 1 > outBufLen) return false;  // ".ktx2\0"
    memcpy(outBuf, exrPath, n - 4);
    memcpy(outBuf + n - 4, ".ktx2", 6);  // includes '\0'
    return true;
}

namespace {

// IEEE half from float, round to nearest even (the conversion a GL_FLOAT upload into GL_RGBA16F performs on the drivers we run).
uint16_t floatToHalf(float f)
{
    uint32_t x = 0;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t biased = (x >> 23) & 0xFFu;
    uint32_t mant = x & 0x7FFFFFu;
    if (biased == 0xFFu) return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));   // inf / NaN
    const int32_t exp = static_cast<int32_t>(biased) - 127 + 15;
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);                                 // overflow -> inf
    if (exp <= 0) {                                                                              // subnormal or zero
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t h = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u), halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (h & 1u))) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;   // a carry into the exponent is the correct rounding
    return static_cast<uint16_t>(sign | h);
}

void appendHalfLevel(HdriCpuImage& out, const std::vector<float>& rgba, int w, int h)
{
    std::vector<uint8_t> bytes(static_cast<size_t>(w) * h * 8);
    for (size_t i = 0; i < rgba.size(); ++i) {
        const uint16_t v = floatToHalf(rgba[i]);
        std::memcpy(bytes.data() + i * 2, &v, 2);
    }
    out.levels.push_back(std::move(bytes));
    out.dims.emplace_back(w, h);
}

// The RGBA16F image and its full mip chain, each level the 2x2 box average of the one above (edge texels repeat when a
// dimension is odd or already 1).
void buildRgba16fChain(HdriCpuImage& out, const float* pixels, int w, int h)
{
    out.format = HdriCpuImage::Format::Rgba16f;
    out.levels.clear();
    out.dims.clear();
    std::vector<float> level(pixels, pixels + static_cast<size_t>(w) * h * 4);
    appendHalfLevel(out, level, w, h);
    while (w > 1 || h > 1) {
        const int nw = std::max(1, w >> 1);
        const int nh = std::max(1, h >> 1);
        std::vector<float> next(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y) {
            const int y0 = std::min(2 * y, h - 1);
            const int y1 = std::min(2 * y + 1, h - 1);
            for (int x = 0; x < nw; ++x) {
                const int x0 = std::min(2 * x, w - 1);
                const int x1 = std::min(2 * x + 1, w - 1);
                for (int c = 0; c < 4; ++c) {
                    const float sum = level[(static_cast<size_t>(y0) * w + x0) * 4 + c] + level[(static_cast<size_t>(y0) * w + x1) * 4 + c]
                                    + level[(static_cast<size_t>(y1) * w + x0) * 4 + c] + level[(static_cast<size_t>(y1) * w + x1) * 4 + c];
                    next[(static_cast<size_t>(y) * nw + x) * 4 + c] = 0.25f * sum;
                }
            }
        }
        level.swap(next);
        w = nw;
        h = nh;
        appendHalfLevel(out, level, w, h);
    }
}

}  // namespace

// Decode source bytes only; the native sky and water owners upload their own images.
bool decodeHdriSource(mc2::czstring path, float* outSunAzGL, bool* outSunValid, HdriCpuImage* outCpu)
{
    if (outSunValid) *outSunValid = false;
    if (!outCpu) return false;
    *outCpu = {};
    if (!path || !*path) return false;
    auto scanSun = [&](const float* pixels, int w, int h) {
        if (!outSunAzGL) return;
        if (const auto az = scanHdriSunAzGL(pixels, w, h)) {
            *outSunAzGL = *az;
            if (outSunValid) *outSunValid = true;
        }
    };
    if (hdriBc6hOn()) {
        char sidecarPath[1024];
        auto cooked = mc2::textures::loadCookedSidecar(path);
        if (deriveSidecarKtx2(path, sidecarPath, sizeof(sidecarPath)) && cooked && cooked->isCompressed &&
            (cooked->vkFormat == 143 || cooked->vkFormat == 144)) {
            auto& img = *cooked;
            const size_t count = img.mipCount > 1 && img.mipByteOffsets.size() > 1
                ? static_cast<size_t>(img.mipByteOffsets[1]) : img.pixels.size();
            const int cap = mc2::options::current().baseQuality == 0 ? 512 : 1024;
            const int nw = cap, nh = std::max(1, img.height * cap / img.width);
            size_t cappedBytes = 8;
            for (int mw = nw, mh = nh; mw > 1 || mh > 1;
                 mw = std::max(1, mw / 2),
                 mh = std::max(1, mh / 2))
                cappedBytes += static_cast<size_t>(mw) * mh * 8;
            // Keep native BC6H when the half-float mip chain would cost more memory.
            if (mc2::options::lowCost() && img.width > cap && cappedBytes < count) {
                // Single-level 16K BC6H skies otherwise consume 128 MiB EACH in sky and water.
                // Average one representative 4x4 block per output pixel: bounded load-time work,
                // no full-size float decode, no extra per-frame sampling or shared-DDR4 residency.
                const int blocksX = (img.width + 3) / 4;
                std::vector<float> rgba(static_cast<size_t>(nw) * nh * 4, 1.0f);
                for (int y = 0; y < nh; ++y) for (int x = 0; x < nw; ++x) {
                    const int bx = (x * img.width / nw + img.width / nw / 2) / 4;
                    const int by = (y * img.height / nh + img.height / nh / 2) / 4;
                    float block[48];
                    bcdec_bc6h_float(img.pixels.data() + (static_cast<size_t>(by) * blocksX + bx) * 16,
                                     block, 12, img.vkFormat == 144);
                    for (int c = 0; c < 3; ++c) {
                        float sum = 0;
                        for (int k = 0; k < 16; ++k) sum += block[k * 3 + c];
                        rgba[(static_cast<size_t>(y) * nw + x) * 4 + c] = sum / 16.0f;
                    }
                }
                buildRgba16fChain(*outCpu, rgba.data(), nw, nh);
                MC2_DIAGFE(stderr, "[HDRI_SKY v1] preset_cap=%d source=%dx%d uploaded=%dx%d\n", cap, img.width, img.height, nw, nh);
            } else {
                outCpu->format = img.vkFormat == 144 ? HdriCpuImage::Format::Bc6hSfloat : HdriCpuImage::Format::Bc6hUfloat;
                outCpu->levels.emplace_back(img.pixels.begin(), img.pixels.begin() + static_cast<std::ptrdiff_t>(count));
                outCpu->dims.emplace_back(img.width, img.height);
            }
            if (outSunAzGL && img.hasSunMetadata) {
                if (img.sunAzimuth) {
                    *outSunAzGL = *img.sunAzimuth;
                    if (outSunValid) *outSunValid = true;
                }
            } else if (outSunAzGL) {
                float* pixels = nullptr; int w = 0, h = 0; const char* error = nullptr;
                if (loadSourceExr(&pixels, &w, &h, path, &error) == TINYEXR_SUCCESS) scanSun(pixels, w, h);
                if (pixels) free(pixels);
                if (error) FreeEXRErrorMessage(error);
            }
            MC2_DIAGFE(stderr, "[HDRI_SKY v1] source=bc6h path=%s w=%d h=%d bytes=%zu\n", sidecarPath, img.width, img.height, count);
            return true;
        }
    }
    float* pixels = nullptr; int w = 0, h = 0; const char* error = nullptr;
    const int result = loadSourceExr(&pixels, &w, &h, path, &error);
    if (result != TINYEXR_SUCCESS || !pixels || w <= 0 || h <= 0 ||
        w > static_cast<int>(RenderCore::kKtxMaxEdge) || h > static_cast<int>(RenderCore::kKtxMaxEdge)) {
        MC2_DIAGFE(stderr, "[HDRI_SKY v1] enabled=0 reason=decode_failed path=%s error=%s\n", path, error ? error : "invalid_extent");
        if (pixels) free(pixels);
        if (error) FreeEXRErrorMessage(error);
        return false;
    }
    scanSun(pixels, w, h);
    buildRgba16fChain(*outCpu, pixels, w, h);
    free(pixels);
    if (error) FreeEXRErrorMessage(error);
    MC2_DIAGFE(stderr, "[HDRI_SKY v1] source=rgba16f path=%s w=%d h=%d\n", path, w, h);
    return !outCpu->levels.empty();
}
