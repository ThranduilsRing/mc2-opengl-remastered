#include "gos_hdri.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1

#include <cstdio>
#include <cstdlib>  // for free()
#include <cmath>    // for atan2, fabsf, cosf, sinf
#include <cstring>  // for strlen, memcpy
#include <cstddef>
#include <vector>
#include <algorithm>

// tinyexr brings in miniz transitively. Define implementation macros
// in EXACTLY this one TU. miniz.c is compiled separately (registered
// in CMake at T7).
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

// HDRI-BC6H-1: BC6H sidecar upload via KtxLoader (GL-free loader, BC6H blocks).
#include "../../RenderCore/KtxLoader.h"
#include "Gates/gate.h"
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
// so the wrap at +/-PI is handled correctly. Returns false if no above-horizon
// energy is found.
static bool scanHdriSunAzGL(const float* px, int w, int h, float* outAzGL)
{
    if (!px || w <= 0 || h <= 0 || !outAzGL) return false;

    const int halfRows = h / 2;          // upper half = above horizon
    if (halfRows <= 0) return false;

    // Pass 1: peak luminance in the upper half.
    float maxLum = 0.0f;
    for (int r = 0; r < halfRows; ++r) {
        const float* row = px + static_cast<size_t>(r) * static_cast<size_t>(w) * 4u;
        for (int c = 0; c < w; ++c) {
            const float* p = row + static_cast<size_t>(c) * 4u;
            // Rec.709 luma; HDRI is linear so this is physical luminance.
            const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            if (lum > maxLum) maxLum = lum;
        }
    }
    if (!(maxLum > 0.0f)) return false;

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
    if (!(wsum > 0.0)) return false;
    *outAzGL = static_cast<float>(atan2(sy, sx));
    return true;
}

// HDRI-BC6H-1: gate — MC2_HDRI_BC6H controls BC6H sidecar upload.
// Default ON (absent = ON); set =0 to force RGBA16F fallback path.
// Read at first use, not at static-init time: the launcher writes this variable after the exe starts.
static bool hdriBc6hOn()
{
    static const bool on = mc2::gate::get<"MC2_HDRI_BC6H">();
    return on;
}

// Derive sidecar path: replace trailing ".exr" with ".ktx2".
// Returns false if path does not end with ".exr" or output buffer is too small.
static bool deriveSidecarKtx2(const char* exrPath, char* outBuf, size_t outBufLen)
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
        const int nw = std::max(1, w >> 1), nh = std::max(1, h >> 1);
        std::vector<float> next(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y) {
            const int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
            for (int x = 0; x < nw; ++x) {
                const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
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

GLuint loadHdriTexture(const char* path,
                       float* outSunAzGL,
                       bool*  outSunValid,
                       HdriCpuImage* outCpu)
{
    if (outSunValid) *outSunValid = false;
    if (outCpu) { outCpu->levels.clear(); outCpu->dims.clear(); }
    if (!path || !*path) {
        MC2_DIAGFE(stderr, "[HDRI_SKY v1] enabled=0 reason=path_empty\n");
        return 0;
    }

    // ---- HDRI-BC6H-1: BC6H sidecar probe ----
    // Attempt the .ktx2 sidecar before tinyexr if MC2_HDRI_BC6H=1 and BPTC
    // is available.  The sun-scan still runs from the EXR pixel buffer so the
    // azimuth estimate is identical to the RGBA16F path.
    if (hdriBc6hOn() && GLEW_ARB_texture_compression_bptc) {
        char sidecarPath[1024];
        if (deriveSidecarKtx2(path, sidecarPath, sizeof(sidecarPath))) {
            RenderCore::KtxImage img;
            if (RenderCore::ktxLoadRgba8(sidecarPath, img) &&
                img.isCompressed &&
                (img.vkFormat == 143 || img.vkFormat == 144)) {

                // BC6H_UFLOAT_BLOCK (143) maps to GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT.
                // BC6H_SFLOAT_BLOCK (144) maps to GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT.
                const GLenum glIF = (img.vkFormat == 144)
                    ? GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT
                    : GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT;

                // Mip 0 byte length.
                const size_t mip0Bytes = (img.mipCount > 1 &&
                                          img.mipByteOffsets.size() > 1)
                    ? static_cast<size_t>(img.mipByteOffsets[1])
                    : img.pixels.size();

                while (glGetError() != GL_NO_ERROR) { /* drain */ }

                GLuint tex = 0;
                // TEX-CLASS: asset-pool -- HDRI sky BC6H content texture
                glGenTextures(1, &tex);
                if (!tex) {
                    MC2_DIAGFE(stderr,
                        "[HDRI_SKY v1] bc6h: glGenTextures failed path=%s\n", path);
                    // Fall through to tinyexr path below.
                } else {
                    GLint prevBinding = 0;
                    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevBinding);

                    glBindTexture(GL_TEXTURE_2D, tex);
                    glCompressedTexImage2D(GL_TEXTURE_2D, 0, glIF,
                                          img.width, img.height, 0,
                                          static_cast<GLsizei>(mip0Bytes),
                                          img.pixels.data());
                    // Single-mip BC6H texture: clamp max level to 0 so the
                    // texture is never incomplete due to missing mip levels.
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glBindTexture(GL_TEXTURE_2D, prevBinding);

                    const GLenum glErr = glGetError();
                    if (glErr != GL_NO_ERROR) {
                        MC2_DIAGFE(stderr,
                            "[HDRI_SKY v1] bc6h: gl_error=0x%x path=%s\n",
                            glErr, path);
                        glDeleteTextures(1, &tex);
                        tex = 0;
                        // Fall through to tinyexr below.
                    } else {
                        mc2::fmt::print_err("[HDRI_SKY] bc6h: ON (default) sidecar={} w={:d} h={:d} glIF=0x{:x} bytes={:d} tex={:d}\n", mc2::fmt::safe(sidecarPath), mc2::fmt::as_i32(img.width), mc2::fmt::as_i32(img.height), mc2::fmt::as_u32(static_cast<unsigned>(glIF)), mc2::fmt::as_size(mip0Bytes), mc2::fmt::as_u32(static_cast<unsigned>(tex)));
                        if (outCpu) {
                            outCpu->format = (img.vkFormat == 144) ? HdriCpuImage::Format::Bc6hSfloat : HdriCpuImage::Format::Bc6hUfloat;
                            const size_t n = std::min(mip0Bytes, img.pixels.size());
                            outCpu->levels.emplace_back(img.pixels.begin(), img.pixels.begin() + static_cast<std::ptrdiff_t>(n));
                            outCpu->dims.emplace_back(img.width, img.height);
                        }

                        // Sun scan: still load the EXR briefly to find the sun.
                        if (outSunAzGL) {
                            float* pixels = nullptr;
                            int sw = 0, sh = 0;
                            const char* err = nullptr;
                            if (LoadEXR(&pixels, &sw, &sh, path, &err) == TINYEXR_SUCCESS) {
                                float azGL = 0.0f;
                                if (scanHdriSunAzGL(pixels, sw, sh, &azGL)) {
                                    *outSunAzGL = azGL;
                                    if (outSunValid) *outSunValid = true;
                                    MC2_DIAGFE(stderr,
                                        "[HDRI_SKY v1] bc6h: sun_scan ok az_gl_rad=%.4f az_gl_deg=%.1f\n",
                                        azGL, azGL * 57.2957795f);
                                } else {
                                    MC2_DIAGFE(stderr,
                                        "[HDRI_SKY v1] bc6h: sun_scan failed reason=no_above_horizon_energy\n");
                                }
                                free(pixels);
                            } else {
                                MC2_DIAGFE(stderr,
                                    "[HDRI_SKY v1] bc6h: sun_scan skipped reason=exr_load_failed err=%s\n",
                                    err ? err : "(null)");
                                if (err) FreeEXRErrorMessage(err);
                                if (pixels) free(pixels);
                            }
                        }

                        return tex;
                    }
                }
            } else {
                // Sidecar missing or wrong format — log at debug level and fall through.
                MC2_DIAGFE(stderr,
                    "[HDRI_SKY v1] bc6h: sidecar not found or wrong format "
                    "(%s), falling back to RGBA16F\n", sidecarPath);
            }
        }
    }

    // ---- Original tinyexr / GL_RGBA16F path ----
    // tinyexr LoadEXR signature:
    //   int LoadEXR(float** out_rgba, int* width, int* height,
    //               const char* filename, const char** err);
    // On success returns TINYEXR_SUCCESS (0), out_rgba is a newly-allocated
    // float[w*h*4] in RGBA order (always 4 channels, regardless of source).
    // Caller must free(out_rgba) and FreeEXRErrorMessage(err) if err != null.
    float* pixels = nullptr;
    int w = 0, h = 0;
    const char* err = nullptr;
    int rc = LoadEXR(&pixels, &w, &h, path, &err);
    if (rc != TINYEXR_SUCCESS) {
        mc2::fmt::print_err("[HDRI_SKY v1] enabled=0 reason=load_failed path={} rc={:d} err={}\n",
            mc2::fmt::safe(path), mc2::fmt::as_i32(rc), mc2::fmt::safe(err ? err : "(null)"));
        if (err) FreeEXRErrorMessage(err);
        if (pixels) free(pixels);  // defensive — should be null on failure
        return 0;
    }

    // Drain any stale GL errors from a previous caller so the post-upload
    // check below blames only this function.
    while (glGetError() != GL_NO_ERROR) { /* drain */ }

    GLuint tex = 0;
    // TEX-CLASS: asset-pool -- HDRI sky LDR content texture
    glGenTextures(1, &tex);
    if (!tex) {
        mc2::fmt::print_err("[HDRI_SKY v1] enabled=0 reason=glGenTextures_failed path={}\n", mc2::fmt::safe(path));
        free(pixels);
        return 0;
    }

    GLint prevBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevBinding);

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0,
                 GL_RGBA, GL_FLOAT, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);   // WATER-HDRI-REFL-1: mipmaps for textureLod in water FS
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, prevBinding);

    // HDRI-SKY Item 2: scan for the baked sun BEFORE freeing the pixel buffer.
    if (outSunAzGL) {
        float azGL = 0.0f;
        const bool ok = scanHdriSunAzGL(pixels, w, h, &azGL);
        if (ok) {
            *outSunAzGL = azGL;
            if (outSunValid) *outSunValid = true;
            MC2_DIAGFE(stderr,
                "[HDRI_SKY v1] sun_scan ok az_gl_rad=%.4f az_gl_deg=%.1f\n",
                azGL, azGL * 57.2957795f);
        } else {
            MC2_DIAGFE(stderr,
                "[HDRI_SKY v1] sun_scan failed reason=no_above_horizon_energy\n");
        }
    }

    if (outCpu) buildRgba16fChain(*outCpu, pixels, w, h);
    free(pixels);  // tinyexr uses plain malloc; free with free() not delete

    const GLenum glErr = glGetError();
    if (glErr != GL_NO_ERROR) {
        mc2::fmt::print_err("[HDRI_SKY v1] enabled=0 reason=gl_error path={} err=0x{:x}\n",
            mc2::fmt::safe(path), mc2::fmt::as_u32(static_cast<unsigned>(glErr)));
        glDeleteTextures(1, &tex);
        if (outCpu) { outCpu->levels.clear(); outCpu->dims.clear(); }
        return 0;
    }

    MC2_DIAGFE(stderr,
        "[HDRI_SKY v1] loaded path=%s w=%d h=%d format=RGBA16F tex=%u\n",
        path, w, h, tex);
    return tex;
}
