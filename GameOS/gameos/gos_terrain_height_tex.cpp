// TERRAIN-NORMALS-FROM-HEIGHT-1: per-mission terrain height texture.
//
// Self-contained TU. Holds a single R32F texture + the world-space mapping
// metadata needed to convert a fragment's WorldPos.xy back into texel coords
// inside the shader. Visual-only — gameplay height stays in MapData.
//
// Trace logging is deliberately quiet (one stderr line per upload) so it can
// stay compiled-in even when the gate is OFF.

#include "gos_terrain_height_tex.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <GL/glew.h>

#include "../../RenderCore/RenderResourceRegistry.h"
#include "Gates/gate.h"   // typed gate registry
#include "mc2fmt.h"

namespace {

GLuint g_handle              = 0;
int    g_side                = 0;     // render-side (post-resample)
int    g_sourceSide          = 0;     // pre-resample source grid side
int    g_resampleFactor      = 1;     // 1, 2, or 4 (TERRAIN-RESAMPLE-1)
float  g_worldUnitsPerVertex = 128.0f; // per RENDER sample (= source / factor)
float  g_mapTopLeftX         = 0.0f;
float  g_mapTopLeftY         = 0.0f;

// TERRAIN-RESAMPLE-1 live-rebuild cache. First upload from real MapData
// blocks stashes the source elevation grid here (one float per source
// vertex) so subsequent gos_setTerrainHeightResampleFactor() calls can
// re-resample + re-upload without touching mclib/MapData. Cleared by
// gos_resetTerrainHeightTex().
std::vector<float> g_cachedSourceElev;
int    g_cachedSourceSide        = 0;
float  g_cachedSourceWuPerVertex = 128.0f;
float  g_cachedMapTopLeftX       = 0.0f;
float  g_cachedMapTopLeftY       = 0.0f;
// -1 sentinel = no override (env var still authoritative).
// >0 = ImGui setter has overridden; env is ignored on subsequent uploads.
int    g_overrideFactor      = -1;

// TERRAIN-RESAMPLE-1: read the resample-factor env var. Accepted values:
// 1, 2, 4. Anything else (unset / non-numeric / out-of-set) clamps to 1
// so the default path is byte-equivalent to pre-slice TERRAIN-NORMALS-
// FROM-HEIGHT-1. Re-evaluated per upload — toggling the env var across
// missions takes effect on next mission load.
int read_resample_factor_env() {
    // Row: Enum 1|2|4, default 1; any other text reads as 1 (and is flagged badvalue in [CONFIG v1]).
    if (mc2::gate::is<"MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR", "2">()) return 2;
    if (mc2::gate::is<"MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR", "4">()) return 4;
    return 1;
}

// Corner-aligned CPU bilinear resample. Source samples at (i, j) land at
// (i*factor, j*factor) in the render grid with zero error — bilinear
// weights collapse to a single source tap at those positions. Interior
// render samples interpolate from the 4 surrounding source taps. Both
// axes use the same factor. Render side = (sourceSide-1)*factor + 1 so
// the very last source column/row keeps a representative sample.
void resample_bilinear(const float* src, int sourceSide,
                       int factor,
                       std::vector<float>& dst, int& renderSide)
{
    renderSide = (sourceSide - 1) * factor + 1;
    dst.assign((size_t)renderSide * (size_t)renderSide, 0.0f);
    const float invFactor = 1.0f / (float)factor;
    const int   srcMax    = sourceSide - 1;
    for (int rrow = 0; rrow < renderSide; ++rrow) {
        const float srcRowF = (float)rrow * invFactor;
        int srcRow0 = (int)srcRowF;
        if (srcRow0 > srcMax) srcRow0 = srcMax;
        int srcRow1 = srcRow0 + 1;
        if (srcRow1 > srcMax) srcRow1 = srcMax;
        const float fy = srcRowF - (float)srcRow0;
        for (int rcol = 0; rcol < renderSide; ++rcol) {
            const float srcColF = (float)rcol * invFactor;
            int srcCol0 = (int)srcColF;
            if (srcCol0 > srcMax) srcCol0 = srcMax;
            int srcCol1 = srcCol0 + 1;
            if (srcCol1 > srcMax) srcCol1 = srcMax;
            const float fx = srcColF - (float)srcCol0;

            const float h00 = src[(size_t)srcRow0 * (size_t)sourceSide + (size_t)srcCol0];
            const float h10 = src[(size_t)srcRow0 * (size_t)sourceSide + (size_t)srcCol1];
            const float h01 = src[(size_t)srcRow1 * (size_t)sourceSide + (size_t)srcCol0];
            const float h11 = src[(size_t)srcRow1 * (size_t)sourceSide + (size_t)srcCol1];

            const float h0 = h00 * (1.0f - fx) + h10 * fx;
            const float h1 = h01 * (1.0f - fx) + h11 * fx;
            dst[(size_t)rrow * (size_t)renderSide + (size_t)rcol] = h0 * (1.0f - fy) + h1 * fy;
        }
    }
}

}  // namespace

void __stdcall gos_uploadTerrainHeightTex(
    int          side,
    const void*  blocks,
    int          postcompVertexStride,
    int          elevationOffset,
    float        mapTopLeftX,
    float        mapTopLeftY,
    float        worldUnitsPerVertex)
{
    if (!blocks || side <= 0 || postcompVertexStride <= 0
        || elevationOffset < 0
        || elevationOffset + (int)sizeof(float) > postcompVertexStride) {
        mc2::fmt::print_err("[TERRAIN_HEIGHT_TEX] skip upload side={:d} stride={:d} elevOff={:d} blocks={:016X}\n", mc2::fmt::as_i32(side), mc2::fmt::as_i32(postcompVertexStride), mc2::fmt::as_i32(elevationOffset), mc2::fmt::as_ptr(blocks));
        return;
    }

    // Extract the source elevation column into a packed float buffer.
    const size_t srcCount = (size_t)side * (size_t)side;
    std::vector<float> srcElev(srcCount);
    const uint8_t* base = static_cast<const uint8_t*>(blocks);
    for (size_t i = 0; i < srcCount; ++i) {
        std::memcpy(&srcElev[i],
                    base + i * (size_t)postcompVertexStride
                         + (size_t)elevationOffset,
                    sizeof(float));
    }

    // TERRAIN-RESAMPLE-1: cache source elevations for live re-resample by
    // gos_setTerrainHeightResampleFactor() (ImGui Terrain Pass combo). Re-
    // upload on factor change reuses this cache instead of asking mclib
    // for the blocks pointer again. Refreshed every mission load.
    g_cachedSourceElev        = srcElev;
    g_cachedSourceSide        = side;
    g_cachedSourceWuPerVertex = (worldUnitsPerVertex > 0.0f) ? worldUnitsPerVertex : 128.0f;
    g_cachedMapTopLeftX       = mapTopLeftX;
    g_cachedMapTopLeftY       = mapTopLeftY;

    // TERRAIN-RESAMPLE-1: optional CPU bilinear resample to a finer render
    // grid. factor=1 short-circuits (no copy beyond what already happened
    // above) so the default path is byte-equivalent to pre-slice. ImGui
    // setter override (sticky) takes precedence over the env var.
    const int factor = (g_overrideFactor > 0) ? g_overrideFactor
                                              : read_resample_factor_env();
    std::vector<float> resampled;  // only used when factor > 1
    int  uploadSide      = side;
    const float* uploadPtr = srcElev.data();
    if (factor > 1) {
        resample_bilinear(srcElev.data(), side, factor, resampled, uploadSide);
        uploadPtr = resampled.data();
    }
    const size_t uploadCount = (size_t)uploadSide * (size_t)uploadSide;

    if (g_handle == 0) {
        // TEX-CLASS: asset-pool -- terrain height authority R32F (content)
        glGenTextures(1, &g_handle);
    }

    // Preserve current binding on unit 0 so we don't disturb the caller —
    // mclib/terrain.cpp is mid-mission-init and may have whatever was active
    // beforehand. glActiveTexture+restore costs one int.
    GLint prevActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    GLint prev2D = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2D);

    glBindTexture(GL_TEXTURE_2D, g_handle);
    // R32F single-channel float; no mipmaps (per-vertex authority value, no
    // need to filter across LODs). CLAMP_TO_EDGE so off-map fetches mirror
    // the nearest edge sample (matches gameplay clamp semantics on lookup).
    // LINEAR filter is REQUIRED for normal-from-height: texelFetch + NEAREST
    // produces a per-tile-constant normal (flat-shaded polygon look across
    // the cell interior). With LINEAR + texture()-with-UVs in the shader,
    // the sampled height varies bilinearly across each cell so the central-
    // difference normal also varies smoothly. R32F linear-filtering requires
    // GL 3.0+ which the renderer already guarantees.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F,
                 uploadSide, uploadSide, 0,
                 GL_RED, GL_FLOAT, uploadPtr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2D);
    glActiveTexture((GLenum)prevActive);

    g_sourceSide          = side;
    g_resampleFactor      = factor;
    g_side                = uploadSide;
    const float wuPerSource = (worldUnitsPerVertex > 0.0f) ? worldUnitsPerVertex
                                                            : 128.0f;
    // Per-render-sample world spacing — sourceUnits / factor. Shader uses
    // (1 / g_worldUnitsPerVertex) for terrainHeightParams.y, so finer
    // resample → smaller span between samples → more sensitive normal.
    g_worldUnitsPerVertex = wuPerSource / (float)factor;
    g_mapTopLeftX         = mapTopLeftX;
    g_mapTopLeftY         = mapTopLeftY;

    mc2::fmt::print_err("[TERRAIN_HEIGHT_TEX] uploaded handle={:d} sourceSide={:d} factor={:d}x renderSide={:d} wuPerSample={:.2f} topLeft=({:.1f},{:.1f}) bytes={:d}\n", mc2::fmt::as_u32((unsigned)g_handle), mc2::fmt::as_i32(g_sourceSide), mc2::fmt::as_i32(g_resampleFactor), mc2::fmt::as_i32(g_side), g_worldUnitsPerVertex, g_mapTopLeftX, g_mapTopLeftY, mc2::fmt::as_size(uploadCount * sizeof(float)));

    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::TerrainHeightTexture;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::Mission;  // static height upload, rebuilt per mission load
        d.format    = RenderCore::RenderResourceFormat::R32F;
        d.debugName = "TerrainHeightTexture";
        d.width     = static_cast<uint32_t>(g_side);
        d.height    = static_cast<uint32_t>(g_side);
        d.glName    = static_cast<uint32_t>(g_handle);
        d.sizeBytes = static_cast<uint64_t>(g_side) * static_cast<uint64_t>(g_side) * sizeof(float);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
}

void __stdcall gos_resetTerrainHeightTex(void)
{
    if (g_handle != 0) {
        glDeleteTextures(1, &g_handle);
        g_handle = 0;
    }
    g_side                = 0;
    g_sourceSide          = 0;
    g_resampleFactor      = 1;
    g_worldUnitsPerVertex = 128.0f;
    g_mapTopLeftX         = 0.0f;
    g_mapTopLeftY         = 0.0f;
    g_cachedSourceElev.clear();
    g_cachedSourceElev.shrink_to_fit();
    g_cachedSourceSide        = 0;
    g_cachedSourceWuPerVertex = 128.0f;
    g_cachedMapTopLeftX       = 0.0f;
    g_cachedMapTopLeftY       = 0.0f;
    g_overrideFactor          = -1;

    RenderCore::RenderResourceDesc invalid;
    invalid.id = RenderCore::RenderResourceId::TerrainHeightTexture;
    RenderCore::registerOrUpdateRenderResource(invalid);
}

void __stdcall gos_setTerrainHeightResampleFactor(int factor)
{
    if (factor != 1 && factor != 2 && factor != 4) factor = 1;
    // Sticky: even if asked for the current factor, mark override so the
    // env var stops being authoritative on subsequent uploads.
    g_overrideFactor = factor;
    if (factor == g_resampleFactor) {
        // No re-upload work needed — same factor as already-uploaded grid.
        return;
    }
    if (g_cachedSourceElev.empty() || g_cachedSourceSide <= 0) {
        // No mission loaded yet — the next gos_uploadTerrainHeightTex()
        // call (mission load) will pick up g_overrideFactor naturally.
        return;
    }
    // Re-upload from cache. gos_uploadTerrainHeightTex extracts elevations
    // using the supplied stride/offset, so we feed the cached float array
    // directly with stride=sizeof(float), offset=0. The re-entrant call
    // refreshes the cache trivially (cache copies cache; no growth) and
    // reads g_overrideFactor on the way to the factor decision above.
    gos_uploadTerrainHeightTex(
        g_cachedSourceSide,
        g_cachedSourceElev.data(),
        (int)sizeof(float),
        /*elevationOffset=*/0,
        g_cachedMapTopLeftX,
        g_cachedMapTopLeftY,
        g_cachedSourceWuPerVertex);
}

uint32_t __stdcall gos_terrainHeightTexHandle(void)         { return (uint32_t)g_handle; }
int      __stdcall gos_terrainHeightTexSide(void)            { return g_side; }
float    __stdcall gos_terrainHeightWorldUnitsPerVertex(void){ return g_worldUnitsPerVertex; }
float    __stdcall gos_terrainHeightMapTopLeftX(void)        { return g_mapTopLeftX; }
float    __stdcall gos_terrainHeightMapTopLeftY(void)        { return g_mapTopLeftY; }
int      __stdcall gos_terrainHeightSourceSide(void)         { return g_sourceSide; }
int      __stdcall gos_terrainHeightResampleFactor(void)     { return g_resampleFactor; }
