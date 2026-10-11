#include "gos_hdri.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1

#include <cstdio>
#include <cstdlib>  // for free()
#include <cmath>    // for atan2, fabsf, cosf, sinf
#include <cstring>  // for strlen, memcpy
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
        const float* row = px + (size_t)r * (size_t)w * 4u;
        for (int c = 0; c < w; ++c) {
            const float* p = row + (size_t)c * 4u;
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
        const float* row = px + (size_t)r * (size_t)w * 4u;
        for (int c = 0; c < w; ++c) {
            const float* p = row + (size_t)c * 4u;
            const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            if (lum < thresh) continue;
            const float u   = ((float)c + 0.5f) / (float)w;
            const float az  = (u - 0.5f) * TWO_PI;   // GL atan(z,x) azimuth
            sx += (double)lum * (double)cosf(az);
            sy += (double)lum * (double)sinf(az);
            wsum += (double)lum;
        }
    }
    if (!(wsum > 0.0)) return false;
    *outAzGL = (float)atan2(sy, sx);
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

GLuint loadHdriTexture(const char* path,
                       float* outSunAzGL,
                       bool*  outSunValid)
{
    if (outSunValid) *outSunValid = false;
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
                        mc2::fmt::print_err("[HDRI_SKY] bc6h: ON (default) sidecar={} w={:d} h={:d} glIF=0x{:x} bytes={:d} tex={:d}\n", mc2::fmt::safe(sidecarPath), mc2::fmt::as_i32(img.width), mc2::fmt::as_i32(img.height), mc2::fmt::as_u32((unsigned)glIF), mc2::fmt::as_size(mip0Bytes), mc2::fmt::as_u32((unsigned)tex));

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
        MC2_DIAGFE(stderr,
            "[HDRI_SKY v1] enabled=0 reason=load_failed path=%s rc=%d err=%s\n",
            path, rc, err ? err : "(null)");
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
        MC2_DIAGFE(stderr,
            "[HDRI_SKY v1] enabled=0 reason=glGenTextures_failed path=%s\n", path);
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

    free(pixels);  // tinyexr uses plain malloc; free with free() not delete

    const GLenum glErr = glGetError();
    if (glErr != GL_NO_ERROR) {
        MC2_DIAGFE(stderr,
            "[HDRI_SKY v1] enabled=0 reason=gl_error path=%s err=0x%x\n",
            path, glErr);
        glDeleteTextures(1, &tex);
        return 0;
    }

    MC2_DIAGFE(stderr,
        "[HDRI_SKY v1] loaded path=%s w=%d h=%d format=RGBA16F tex=%u\n",
        path, w, h, tex);
    return tex;
}
