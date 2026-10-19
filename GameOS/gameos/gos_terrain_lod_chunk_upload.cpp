// gos_terrain_lod_chunk: heightfield, visual-damp, control-map, overlay/shoreline sidecar and cement-word uploads, and the terrain light/ambient accessors.
// Declared in the class headers; shared names in gos_terrain_lod_chunk_internal.h.
#include "gos_terrain_lod_chunk_internal.h"
#include "mc2fmt.h"
#include "Gates/render_options.h"
#include "terrain_field_store.h"


// ---------------------------------------------------------------------------
// Full heightfield upload — called once at map load.
// elevations: float[mapSide*mapSide] row-major.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_UploadHeightFull(const float* elevations, int mapSide)
{
    if (!s_heightSsbo.created())
    {
        mc2::fmt::print_err("[TerrainLodChunk] UploadHeightFull called before Init\n");
        fflush(stderr);
        return;
    }
    // The store keeps the CPU copy the VK terrain island draws from; GL uploads the same bytes.
    mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    if (!store || !store->setHeight(elevations, mapSide))
        return;

    GLsizeiptr bytes = (GLsizeiptr)mapSide * mapSide * sizeof(float);

    s_heightSsbo.bind();
    s_heightSsbo.bufferData((intptr_t)bytes, store->height().data(),
                            RenderCore::GpuBufferUsage::DynamicDraw);
    s_heightSsbo.bindBase(TERRAIN_HEIGHT_SSBO_BINDING);
    s_heightSsbo.unbind();

    s_mapSide = mapSide;
    s_halfMap = (float)mapSide * 128.0f * 0.5f;

    // REGISTRY-TERRAIN-SSBO-1: register the live height SSBO (observe-only metadata;
    // never read by the draw path). Registered here (not at Init) because the byte
    // size is only known once the full heightfield is uploaded.
    s_heightSsbo.registerNow(static_cast<uint64_t>(bytes));

#ifdef _DEBUG
    // First-frame readback verify: confirm that the GPU round-trips the first
    // float correctly. glGetBufferSubData is available on all desktop GL >=3.1.
    float firstSample = 0.0f;
    s_heightSsbo.bind();
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(float), &firstSample);
    s_heightSsbo.unbind();
    if (firstSample != elevations[0])
    {
        mc2::fmt::print_err("[TerrainLodChunk] readback mismatch: wrote {:.6f}, got {:.6f}\n", elevations[0], firstSample);
        fflush(stderr);
    }
#endif
}

// TERRAIN-VISUAL-HEIGHT-SAMPLE-1 Stage 1: upload the 4x VISUAL heightfield bake to
// a dedicated SSBO (binding 26). Lazily allocated. NO geometry samples it yet —
// Stage 2 (corner-pinned interior subdivision) consumes it. Load+log only here.
void gos_TerrainLodChunk_UploadVisualHeightFull(const float* visualHeights, int V,
                                                const float* mipMaxes, int mipFloats)
{
    if (!mipMaxes) mipFloats = 0;
    mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    if (!store || !store->setVisualHeight(visualHeights, V, mipMaxes, mipFloats))
        return;
    if (!s_visualHeightSsbo.created())
    {
        if (!s_visualHeightSsbo.create())
        {
            mc2::fmt::print_err("[VISUAL_HEIGHT v1] glGenBuffers failed\n");
            fflush(stderr);
            return;
        }
    }
    // TERRAIN-LOD-GEOMORPH-1: mips (when shipped) are appended to the SAME
    // buffer after the fine bake — shader offsets are computable from
    // u_visualSide/u_mapSide alone, no extra binding slot consumed.
    const std::vector<float>& field = store->visualHeight();   // fine bake then mip maxes
    GLsizeiptr bytes = (GLsizeiptr)field.size() * (GLsizeiptr)sizeof(float);
    s_visualHeightSsbo.bind();
    s_visualHeightSsbo.bufferData((intptr_t)bytes, field.data(),
                                  RenderCore::GpuBufferUsage::StaticDraw);
    s_visualHeightSsbo.unbind();
    s_visualSide      = V;   // remembered for the displaced draw (u_visualSide)
    s_visualMipFloats = (int)mipFloats;

    // TERRAIN-VISUAL-HEIGHT-SSBO-OWNER-1: register the live visual-height SSBO
    // (observe-only metadata; never read by the draw path). Reached here only when
    // the upstream gate (MC2_TERRAIN_VISUAL_HEIGHT/_DISPLACE + bake) called us.
    s_visualHeightSsbo.registerNow(static_cast<uint64_t>(bytes));
    // Bound to base 26 in the draw only when displacement is active.
    mc2::fmt::print_err("[VISUAL_HEIGHT v1] SSBO uploaded binding={:d} V={:d} bytes={:d} first={:.3f} geomorphMips={}({:d} floats)\n",
            mc2::fmt::as_u32(TERRAIN_VISUAL_HEIGHT_SSBO_BINDING), mc2::fmt::as_i32(V), mc2::fmt::as_i64((long long)bytes), visualHeights[0],
            mc2::fmt::safe(mipFloats > 0 ? "YES" : "no"), mc2::fmt::as_i32((int)mipFloats));
    fflush(stderr);
}

// TERRAIN-REAUTH-UNPIN-1 Half B: static (buildings) object-proximity damp map.
// Reached only when the upstream gates passed (mclib/terrain.cpp). Keeps a CPU
// copy so per-frame mover stamps can min-combine without re-reading the GPU.
void gos_TerrainLodChunk_UploadVisualDampStatic(const float* damp01, int side)
{
    if (!damp01 || side <= 0)
        return;
    const size_t count = (size_t)side * (size_t)side;
    s_visualDampStatic.assign(damp01, damp01 + count);
    s_visualDampCombined = s_visualDampStatic;
    s_visualDampSide = side;
    s_visualDampHadMovers = false;
    if (!s_visualDampSsbo.created())
    {
        if (!s_visualDampSsbo.create())
        {
            mc2::fmt::print_err("[VISUAL_DAMP v1] glGenBuffers failed\n");
            fflush(stderr);
            return;
        }
    }
    const GLsizeiptr bytes = (GLsizeiptr)(count * sizeof(float));
    s_visualDampSsbo.bind();
    s_visualDampSsbo.bufferData((intptr_t)bytes, s_visualDampCombined.data(),
                                RenderCore::GpuBufferUsage::DynamicDraw);
    s_visualDampSsbo.unbind();
    s_visualDampSsbo.registerNow(static_cast<uint64_t>(bytes));
    mc2::fmt::print_err("[VISUAL_DAMP v1] SSBO uploaded binding={:d} side={:d} bytes={:d}\n", mc2::fmt::as_u32(TERRAIN_VISUAL_DAMP_SSBO_BINDING), mc2::fmt::as_i32(side), mc2::fmt::as_i64((long long)bytes));
    fflush(stderr);
}

bool gos_TerrainLodChunk_VisualDampWanted()
{
    static const bool s_displaceGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE">();
    static const bool s_dampGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE">();
    return s_displaceGate && s_dampGate && s_visualDampSsbo.created()
           && s_visualDampSide > 0 && !s_visualDampStatic.empty();
}

// Per-frame mover stamps. combined = min(static, smoothstep(inner, inner+radius,
// dist)) per mover; one full-buffer BufferSubData (side^2 floats, ~57KB@120 —
// negligible). Zero-mover frames upload only on the first one after movers
// disappear (restores the pure-static map, then no-ops).
void gos_TerrainLodChunk_UpdateVisualDampMovers(const float* cellXY, int count,
                                                float radiusCells, float innerCells)
{
    if (!s_visualDampSsbo.created() || s_visualDampSide <= 0 || s_visualDampStatic.empty())
        return;
    if (count <= 0 && !s_visualDampHadMovers)
        return;   // steady state: static map already on the GPU
    const int N = s_visualDampSide;
    s_visualDampCombined = s_visualDampStatic;
    if (radiusCells < 0.25f) radiusCells = 0.25f;
    if (innerCells < 0.0f) innerCells = 0.0f;
    const float reach = innerCells + radiusCells;
    for (int m = 0; m < count; ++m)
    {
        const float cx = cellXY[m * 2 + 0];
        const float cy = cellXY[m * 2 + 1];
        int x0 = (int)floorf(cx - reach), x1 = (int)ceilf(cx + reach);
        int y0 = (int)floorf(cy - reach), y1 = (int)ceilf(cy + reach);
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
        if (x1 > N - 1) x1 = N - 1; if (y1 > N - 1) y1 = N - 1;
        for (int iy = y0; iy <= y1; ++iy)
            for (int ix = x0; ix <= x1; ++ix)
            {
                const float dx = (float)ix - cx;
                const float dy = (float)iy - cy;
                float t = (sqrtf(dx * dx + dy * dy) - innerCells) / radiusCells;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                const float s = t * t * (3.0f - 2.0f * t);
                float& ref = s_visualDampCombined[(size_t)ix + (size_t)iy * N];
                if (s < ref) ref = s;
            }
    }
    s_visualDampHadMovers = (count > 0);
    s_visualDampSsbo.bind();
    s_visualDampSsbo.bufferSubData(0,
                    (intptr_t)(s_visualDampCombined.size() * sizeof(float)),
                    s_visualDampCombined.data());
    s_visualDampSsbo.unbind();
}

// Step 5b: per-vertex terrainType upload (parallel to the heightfield). Used by
// the chunk frag's concrete material/colour selection (pureConcrete).
void gos_TerrainLodChunk_UploadTerrainTypeFull(const float* types, int mapSide)
{
    if (!s_typeSsbo.created() || !types || mapSide <= 0)
        return;
    GLsizeiptr bytes = (GLsizeiptr)mapSide * mapSide * sizeof(float);
    s_typeSsbo.bind();
    s_typeSsbo.bufferData((intptr_t)bytes, types,
                          RenderCore::GpuBufferUsage::DynamicDraw);
    s_typeSsbo.bindBase(TERRAIN_TYPE_SSBO_BINDING);
    s_typeSsbo.unbind();

    // TERRAIN-LODCHUNK-SSBO-OWNER-1: register the live type SSBO (observe-only
    // metadata; never read by the draw path). Registered here (not at Init)
    // because the byte size is only known once the type data is uploaded.
    s_typeSsbo.registerNow(static_cast<uint64_t>(bytes));
}

// TERRAIN-CONTROLMAP-SAMPLE-1: upload the authored RGBA control map as a plain
// GL_RGBA8 2D texture (GL_LINEAR / CLAMP_TO_EDGE, matches the colormap's
// filtering so bilinear blend across cells is consistent with today's sampling).
// Called ONLY when mclib/terrain.cpp actually loaded a sidecar (gate ON + file
// present). rgba: uint8[side*side*4] row-major, R=rock G=grass B=dirt A=concrete.
void gos_TerrainLodChunk_UploadControlMap(const unsigned char* rgba, int side)
{
    if (!rgba || side <= 0)
        return;

    if (s_controlMapTex == 0)
        // TEX-CLASS: asset-pool -- terrain control-map sidecar (mission-lifetime content)
        glGenTextures(1, &s_controlMapTex);
    if (s_controlMapTex == 0)
    {
        mc2::fmt::print_err("[TERRAIN_CONTROLMAP v1] glGenTextures failed\n");
        fflush(stderr);
        return;
    }

    GLint prevActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    GLint prev2D = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2D);

    glBindTexture(GL_TEXTURE_2D, s_controlMapTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2D);
    glActiveTexture((GLenum)prevActive);

    s_controlMapSide = side;

    mc2::fmt::print_err("[TERRAIN_CONTROLMAP v1] uploaded handle={:d} side={:d} bytes={:d}\n", mc2::fmt::as_u32((unsigned)s_controlMapTex), mc2::fmt::as_i32(side), mc2::fmt::as_size((size_t)side * (size_t)side * 4u));
    fflush(stderr);
}

// TERRAIN-OVERLAY-V2-PARITY-1: upload the authored cement/pad/runway overlay
// sidecar as a plain GL_RGBA8 2D texture (GL_LINEAR / CLAMP_TO_EDGE, arbitrary
// WxH -- NOT tied to the vertex grid or the 128wu cement tile grid; sampled by
// world XY via u_overlayBounds in the frag). Called ONLY when mclib/terrain.cpp
// actually loaded a sidecar (gate ON + file present). rgba: uint8[w*h*4]
// row-major, RGB = pre-tinted cement/overlay diffuse, A = coverage/edge alpha.
void gos_TerrainLodChunk_UploadOverlaySidecar(const unsigned char* rgba, int w, int h,
                                               float boundsTopLeftX, float boundsTopLeftY,
                                               float boundsSizeX, float boundsSizeY)
{
    if (!rgba || w <= 0 || h <= 0)
        return;

    if (s_overlaySidecarTex == 0)
        // TEX-CLASS: asset-pool -- terrain overlay sidecar (mission-lifetime content)
        glGenTextures(1, &s_overlaySidecarTex);
    if (s_overlaySidecarTex == 0)
    {
        mc2::fmt::print_err("[TERRAIN_OVERLAY_V2 v1] glGenTextures failed\n");
        fflush(stderr);
        return;
    }

    GLint prevActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    GLint prev2D = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2D);

    glBindTexture(GL_TEXTURE_2D, s_overlaySidecarTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2D);
    glActiveTexture((GLenum)prevActive);

    s_overlayBounds[0] = boundsTopLeftX;
    s_overlayBounds[1] = boundsTopLeftY;
    s_overlayBounds[2] = boundsSizeX;
    s_overlayBounds[3] = boundsSizeY;

    mc2::fmt::print_err("[TERRAIN_OVERLAY_V2 v1] uploaded handle={:d} w={:d} h={:d} bytes={:d} bounds=({:.1f},{:.1f},{:.1f},{:.1f})\n", mc2::fmt::as_u32((unsigned)s_overlaySidecarTex), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h), mc2::fmt::as_size((size_t)w * (size_t)h * 4u), boundsTopLeftX, boundsTopLeftY, boundsSizeX, boundsSizeY);
    fflush(stderr);
}

// TERRAIN-SHORELINE-MASK-1: upload the authored land-side wet/foam shoreline
// mask as a plain GL_RGBA8 2D texture (GL_LINEAR / CLAMP_TO_EDGE, arbitrary
// WxH -- NOT tied to the vertex grid; sampled by world XY via
// u_shorelineBounds in the frag, same pattern as the overlay-V2 sidecar).
// Called ONLY when mclib/terrain.cpp actually loaded a mask (gate ON + file
// present). rgba: uint8[w*h*4] row-major, R=signed dist, G=wet, B=foam,
// A=valid.
void gos_TerrainLodChunk_UploadShorelineMask(const unsigned char* rgba, int w, int h,
                                              float boundsTopLeftX, float boundsTopLeftY,
                                              float boundsSizeX, float boundsSizeY)
{
    // The store keeps the CPU copy the VK water island imports; GL uploads the same bytes.
    mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    if (!store || !store->setShorelineMask(rgba, w, h))
        return;
    rgba = store->shorelineMask().texels.data();

    if (s_shorelineMaskTex == 0)
        // TEX-CLASS: asset-pool -- shoreline mask sidecar (mission-lifetime content, SHORE-MASK deferred-sweep-1)
        glGenTextures(1, &s_shorelineMaskTex);
    if (s_shorelineMaskTex == 0)
    {
        MC2_DIAGFE(stderr, "[TERRAIN_SHORELINE v1] glGenTextures failed\n");
        fflush(stderr);
        return;
    }

    GLint prevActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    GLint prev2D = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2D);

    glBindTexture(GL_TEXTURE_2D, s_shorelineMaskTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2D);
    glActiveTexture((GLenum)prevActive);

    ++s_waterShoreDataGen;   // VK-PARITY-WATER-SHORE-1
    s_shorelineBounds[0] = boundsTopLeftX;
    s_shorelineBounds[1] = boundsTopLeftY;
    s_shorelineBounds[2] = boundsSizeX;
    s_shorelineBounds[3] = boundsSizeY;

    MC2_DIAGFE(stderr, "[TERRAIN_SHORELINE v1] uploaded handle=%u w=%d h=%d bytes=%zu "
            "bounds=(%.1f,%.1f,%.1f,%.1f)\n",
            (unsigned)s_shorelineMaskTex, w, h, (size_t)w * (size_t)h * 4u,
            boundsTopLeftX, boundsTopLeftY, boundsSizeX, boundsSizeY);
    fflush(stderr);
}

// WATER-SHORE-EDGE-1: upload the hi-res visual heightfield as R32F GL_LINEAR /
// CLAMP_TO_EDGE (see header for the bounds/half-texel contract). Mirrors
// gos_TerrainLodChunk_UploadShorelineMask's save/restore discipline.
void gos_TerrainLodChunk_UploadWaterShoreHeight(const float* heights, int w, int h,
                                                 float boundsTopLeftX, float boundsTopLeftY,
                                                 float boundsSizeX, float boundsSizeY)
{
    // The store keeps the CPU copy the VK water island imports; GL uploads the same bytes.
    mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    if (!store || !store->setWaterShoreHeight(heights, w, h))
        return;
    heights = store->waterShoreHeight().texels.data();

    if (s_waterShoreHeightTex == 0)
        // TEX-CLASS: asset-pool -- water shore hi-res height sidecar (mission-lifetime content)
        glGenTextures(1, &s_waterShoreHeightTex);
    if (s_waterShoreHeightTex == 0)
    {
        MC2_DIAGFE(stderr, "[WATER_SHORE_EDGE v1] glGenTextures failed\n");
        fflush(stderr);
        return;
    }

    GLint prevActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    GLint prev2D = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2D);

    GLint prevUnpack = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glBindTexture(GL_TEXTURE_2D, s_waterShoreHeightTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, heights);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpack);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2D);
    glActiveTexture((GLenum)prevActive);

    ++s_waterShoreDataGen;   // VK-PARITY-WATER-SHORE-1
    s_waterShoreHeightBounds[0] = boundsTopLeftX;
    s_waterShoreHeightBounds[1] = boundsTopLeftY;
    s_waterShoreHeightBounds[2] = boundsSizeX;
    s_waterShoreHeightBounds[3] = boundsSizeY;

    MC2_DIAGFE(stderr, "[WATER_SHORE_EDGE v1] uploaded handle=%u w=%d h=%d "
            "bounds=(%.1f,%.1f,%.1f,%.1f)\n",
            (unsigned)s_waterShoreHeightTex, w, h,
            boundsTopLeftX, boundsTopLeftY, boundsSizeX, boundsSizeY);
    fflush(stderr);
}

void gos_TerrainLodChunk_UploadWaterShoreSdf(const float* heights, int w, int h,
                                              float boundsTopLeftX, float boundsTopLeftY,
                                              float boundsSizeX, float boundsSizeY)
{
    // Upload the caller's texels (never the store's own buffer: setWaterShoreHeight may
    // reallocate it), then re-tag the stored copy as a real shore SDF.
    gos_TerrainLodChunk_UploadWaterShoreHeight(heights, w, h, boundsTopLeftX, boundsTopLeftY,
                                               boundsSizeX, boundsSizeY);
    if (mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore())
        store->setWaterShoreHeight(heights, w, h, mc2terrain::WaterShoreHeightKind::ShoreSdf);
}

// WATER-SHORE-EDGE-1: single engine-wide resolution of the water shore-edge
// mode (see header). Same lazy-once idiom as gos_TerrainShorelineGateOn so the
// loader (mclib/terrain.cpp) and the draw path (gameos_graphics.cpp) can never
// disagree about what mode a stock launch runs.
int gos_WaterShoreMode()
{
    static const int s_mode = []() {
        // Enum row 0|1|2|3: anything else (unset, empty, unparseable) falls through to default resolution
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "0">()) return 0;
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "1">()) return 1;
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "2">()) return 2;
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "3">()) return 3;
        // Back-compat killswitch: MC2_WATER_SHORE_MASK=0 predates the mode
        // gate and meant "full legacy ramp"; keep that meaning.
        const bool shoreMaskOn = mc2::gate::get<"MC2_WATER_SHORE_MASK">();  // default ON
        // SHORE-FIELD-1: default is mode 3 -- edge AND depth tint from the smooth field
        // (cooked shore_field.r16f, else visual_height_4x; no sidecar -> the FS falls back to 2/1/0).
        return shoreMaskOn ? 3 : 0;
    }();
    return s_mode;
}

// TERRAIN-SHORELINE-V3: true whenever the elevation-based shoreline band is
// active (MC2_TERRAIN_SHORELINE gate on) -- NOT dependent on a mask sidecar
// being loaded, since V3 places the band by elevation unconditionally (the
// mask, if present, is only an optional modulator). Consumed by
// gos_postprocess.cpp to suppress the legacy screen runShoreline() pass
// (recon landmine #6 -- avoid double-brightening the seam). Name kept for
// caller-site stability; semantics widened from "mask uploaded" to "gate on".
bool gos_TerrainLodChunk_IsShorelineMaskActive()
{
    // SHORELINE-GLOW-1: was an INDEPENDENT presence test here, left behind when
    // SHORELINE-SMOOTHING-REVISIT-1 flipped the default. Now the one shared
    // accessor, so this answers the same thing the draw path does.
    return gos_TerrainShorelineGateOn();
}

// SHORELINE-GLOW-1 -- ONE resolution of the V3 shoreline gate, engine-wide.
//
// This gate used to be resolved by THREE independent getenv() reads: the
// draw-path read above, the accessor above that, and mclib/terrain.cpp's
// sidecar-load read. SHORELINE-SMOOTHING-REVISIT-1 flipped the default ON and
// updated two of them; gos_TerrainLodChunk_IsShorelineMaskActive() kept the
// pre-flip presence test `(v && v[0] && v[0] != '0')`. One unset environment
// therefore answered ON where the band is drawn and OFF where the legacy
// screen pass is suppressed, so the suppression could never fire on a stock
// launch. Resolving the gate exactly once makes that drift structurally
// impossible rather than merely fixed. Default ON; `MC2_TERRAIN_SHORELINE=0`
// turns the terrain-side band off.
bool gos_TerrainShorelineGateOn()
{
    static const bool s_on = mc2::gate::get<"MC2_TERRAIN_SHORELINE">();
    return s_on;
}

// SHORELINE-GLOW-1 -- does the V3 band actually SUPPRESS the legacy screen pass?
// LEGACY-SHORELINE-MISAIM-1 (2026-07-31) -- DEFAULT FLIPPED OFF -> ON.
//
// SHORELINE-GLOW-1 left this opt-in because it could not GRADE the visible
// change. That was the right call for a pass whose effect was unknown. It is
// no longer unknown, and the thing being graded is not shoreline foam:
//
//   shaders/shoreline.frag:22 selects its pixels with
//       isWater = (GBuffer1.a > 0.15 && GBuffer1.a < 0.35)
//   and on the default LOD-chunk renderer stack NO WATER PIXEL CAN SATISFY IT.
//   Water writes alpha 0.0 (rc_gbuffer1_screenShadowEligible, three sites in
//   gos_terrain_water_mdi.frag), terrain writes 1.0
//   (rc_gbuffer1_shadowHandled_flatUp), and the MRT1 clear is 0.0. A census of
//   every fragment shader that writes attachment 1 leaves exactly two things
//   inside (0.15, 0.35): static props at 0.25 and alpha-test lattice props at
//   0.1875. So the pass has NOT been painting foam on water. It has been
//   painting an ANIMATED fbm foam brighten (up to 1.4x multiplicative, scrolled
//   by `time` and pulsed by a sin()) onto BUILDING AND FENCE SILHOUETTES within
//   ~6 screen texels of terrain, on every default frame, on every mission.
//
// The only shader that ever wrote a continuous ~0.25 there was the LEGACY
// terrain renderer (gos_terrain.frag rc_gbuffer1_legacyTerrainMaterialAlpha),
// superseded by the LOD-chunk cutover on 2026-06-09.
//
// ---------------------------------------------------------------------------
// CORRECTION -- SHORELINE-DISPOSITION-1 (2026-07-31). READ THIS BEFORE QUOTING
// THE PARAGRAPH ABOVE.
//
// This block used to continue: "That is why the suppression is still driven
// from the LOD-chunk draw path and nowhere else: under MC2_TERRAIN_LOD_CHUNK=0
// the setter is never called, the pass keeps running, and on THAT path it still
// does the job it was written for."
//
// THAT IS FALSE, and it was load-bearing -- it was the stated reason the pass
// was kept rather than deleted. Verified at source:
//
//   * gos_terrain.frag's two GBuffer1 writes (:1103, :1156) are both inside
//     `#ifdef MRT_ENABLED`.
//   * `#define MRT_ENABLED` is injected from exactly ONE C++ site --
//     gameos_graphics.cpp:495, inside getMaterialVariation(), which feeds only
//     gosRenderMaterial::load(). The three makeProgram() variants built from
//     the same .frag (thin / surface / mask_solid) never append it, so those
//     two lines are PREPROCESSED OUT of every one of them.
//   * The single program that does get MRT_ENABLED is terrain_material_
//     (gameos_graphics.cpp:5193). It is created, pushed to materialList_, and
//     destroyed -- and never apply()'d. Its draw, terrainDrawIndexedPatches,
//     was deleted by TERRAIN-DEADCODE-DELETE-1 (75b8ebefe); the survivors at
//     :7619/:7632 rewind() and fire a LegacyMLR tripwire without drawing.
//
// So MC2_TERRAIN_LOD_CHUNK=0 is NOT the configuration this pass was written
// for -- it is not a working configuration for this pass at all. There is NO
// configuration in which the (0.15, 0.35) classifier matches a water pixel.
// ShorelineLegacySuppress = FALSE does not restore shoreline foam anywhere;
// it restores the animated rim on prop silhouettes, in every configuration.
//
// Census 4 of scripts/check-render-contract-gbuffer1.sh now pins the
// single-injection-site fact so this cannot silently rot back.
//
// DISPOSITION: the pass should be DELETED, not kept and not repointed. The V3
// terrain band (MC2_TERRAIN_SHORELINE, elevation-placed in the frag) owns the
// shoreline look now, and a screen-space classifier keyed on a G-buffer alpha
// tag is the exact design that failed here. The retirement is mechanical but
// wide -- ~121 non-comment sites across gos_postprocess.{cpp,h},
// vulkan_post_screenfx_native.cpp, frame_executor.h and PipelineRegistry, plus
// two shaders, the feature registry, the launcher, doctests and the pipeline
// schema -- so it is a separate slice, not a drive-by. See the report for the
// site inventory and the pipeline-slot tombstone rationale.
//
// Cost of the flip, stated plainly because it is a real cost: prop silhouettes
// adjacent to terrain lose up to 1.4x of edge brightness while a dark-props
// report is open. What they lose with it is a shimmer that moves on a static
// building, which no grading pass would have kept.
//
// Default ON (options.cfg ShorelineLegacySuppress); FALSE restores
// the legacy pass exactly as it shipped.
bool gos_TerrainShorelineLegacySuppressOn()
{
    static const bool s_on = mc2::options::current().shorelineLegacySuppress;
    return s_on;
}

// Step 5c: per-vertex cement word upload (valid bit | atlas layer index). Built by
// gos_terrain_indirect after the cement catalog atlas is ready (PopulateRecipeCementWords).
void gos_TerrainLodChunk_UploadCementWordsFull(const unsigned int* words, int count, int mapSide)
{
    if (!s_cementSsbo.created() || mapSide <= 0)
        return;
    mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    if (!store || !store->setCementWords(words, count))
        return;
    GLsizeiptr bytes = (GLsizeiptr)count * sizeof(unsigned int);
    s_cementSsbo.bind();
    s_cementSsbo.bufferData((intptr_t)bytes, store->cementWords().data(),
                            RenderCore::GpuBufferUsage::DynamicDraw);
    s_cementSsbo.bindBase(TERRAIN_CEMENT_SSBO_BINDING);
    s_cementSsbo.unbind();

    // TERRAIN-LODCHUNK-SSBO-OWNER-1: register the live cement SSBO. Conditional by
    // construction -- this upload only runs when cement words actually exist, so
    // the cement slot is registered only when the buffer carries real data.
    s_cementSsbo.registerNow(static_cast<uint64_t>(bytes));
}

// ---------------------------------------------------------------------------
// Dirty-patch upload — called after setVertexHeight() modifies a block.
// rowData: compact float[(quadCountY+1)*(quadCountX+1)] row-major.
// The full SSBO is row-major with stride mapSide, so this MUST be row-by-row.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_UploadHeightPatch(
    const float* rowData,
    int originX, int originY,
    int quadCountX, int quadCountY,
    int mapSide)
{
    if (!s_heightSsbo.created())
        return;
    // The store bounds-checks the block against the map and changes the version the VK island re-copies on.
    mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    if (!store || !store->patchHeight(rowData, originX, originY, quadCountX, quadCountY, mapSide))
        return;

    s_heightSsbo.bind();

    for (int row = 0; row <= quadCountY; ++row)
    {
        int        dstIdx    = (originY + row) * mapSide + originX;
        GLintptr   dstOffset = (GLintptr)dstIdx * sizeof(float);
        GLsizeiptr bytes     = (GLsizeiptr)(quadCountX + 1) * sizeof(float);
        const float* src     = rowData + row * (quadCountX + 1);
        s_heightSsbo.bufferSubData((intptr_t)dstOffset, (intptr_t)bytes, src);
    }

    s_heightSsbo.unbind();
}

// PAD-AREA-GLOW-1: chunk cement-grid geometry for the overlay/decal uploader
// (gameos_graphics.cpp uploadOverlayUniforms_). mapSide==0 means the chunk
// terrain grid is not resident (legacy terrain path / pre-upload) — the
// overlay shader then force-uploads u_padGlowMapSide=0 and never reads the
// binding-25 cement SSBO.
void gos_GetTerrainChunkGridInfo(int* mapSide, float* halfMap)
{
    if (mapSide) *mapSide = s_mapSide;
    if (halfMap) *halfMap = s_halfMap;
}

// PAD-AREA-GLOW-1: uniform night lift on cement pads + a feathered spill
// margin (MC2_PAD_AREA_GLOW_SPILL wu) onto surrounding terrain, so buildings
// with bright pool lights no longer read as pasted-bright cutouts on black
// ground. Value = additive lift strength (pre nightFactor/color). Default ON
// (0.12); =0 kill. GLOBAL scope (not the anon namespace) — the overlay/decal
// uploader in gameos_graphics.cpp reuses both values for lockstep.
float gos_TerrainPadGlowStrength()
{
    static const float s_v = mc2::gate::get<"MC2_PAD_AREA_GLOW">();
    return s_v;
}

// VK-PARITY-TERRAIN-1: shared producers for TERRAIN-AMBIENT-FLOOR-2 /
// -NIGHT-1. Moved verbatim out of the per-frame uniform upload so the GL frag
// uniforms and the VK terrain island read the SAME resolved values (one getenv
// each). Semantics unchanged: floor default 0.35 (unset), clamped 0..2, NaN->0;
// night floor default 0.0, clamped 0..1.
// VK-PARITY-F3: single reader for the snow brightness dampen (default 0.78;
// MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN overrides), shared by the GL uniform upload and the VK feed.
float gos_TerrainSnowBrightnessDampen()
{
    static const float s_v = mc2::gate::get<"MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN">();
    return s_v;
}
float gos_TerrainAmbientFloor()
{
    // Row: default 0.35 (DEFAULT-ON), clampRange(0, 2).
    static const float s_v = mc2::gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">();
    return s_v;
}
bool gos_TerrainAmbientFloorEnvSet() { return mc2::gate::isNonEmpty<"MC2_TERRAIN_AMBIENT_FLOOR">(); }
float gos_TerrainAmbientNightFloor()
{
    // Row: default 0 (DEFAULT-OFF), clampRange(0, 1).
    static const float s_v = mc2::gate::get<"MC2_TERRAIN_AMBIENT_FLOOR_NIGHT">();
    return s_v;
}
bool gos_TerrainAmbientNightFloorEnvSet() { return mc2::gate::isNonEmpty<"MC2_TERRAIN_AMBIENT_FLOOR_NIGHT">(); }
// Daylight gate actually uploaded: max(daylight level, night floor).
float gos_TerrainAmbientDayGate()
{
    const float raw = gos_GetTerrainDaylightLevel();
    const float nf  = gos_TerrainAmbientNightFloor();
    return (raw > nf) ? raw : nf;
}

// VK-PARITY-TERRAIN-2: NIGHT-LIGHT-POOLS-1 pool count actually consumed by the
// terrain frag this frame (GL u_numPoolLights / VK uT.numPoolLights). Moved verbatim
// out of the GL upload: the pool SSBO must exist, capped at 256.
int gos_TerrainLightPoolCount()
{
    const GLuint   poolSsbo = gos_terrain_lighting::GetLightPoolSsbo();
    const uint32_t n        = gos_terrain_lighting::GetLightPoolLightCount();
    if (poolSsbo == 0 || n == 0) return 0;
    return static_cast<int>(n > 256u ? 256u : n);
}

// VK-PARITY-TERRAIN-2: PAD-AREA-GLOW-1 premultiplied rgb (GL u_padGlow), moved
// verbatim out of the GL upload. nightFactor = 1 - brightest mission flat-light
// channel: day missions -> exact (0,0,0).
void gos_TerrainPadGlowRGB(float out[3])
{
    float fr = 1.f, fg = 1.f, fb = 1.f;
    gos_GetTerrainMissionFlatLight(&fr, &fg, &fb);
    float flMax = fr;                     // (std::max unusable: windows.h max macro)
    if (fg > flMax) flMax = fg;
    if (fb > flMax) flMax = fb;
    float nf = 1.0f - flMax;
    if (nf < 0.0f) nf = 0.0f;
    if (nf > 1.0f) nf = 1.0f;
    const float s = gos_TerrainPadGlowStrength() * nf;
    out[0] = s * 1.00f; out[1] = s * 0.88f; out[2] = s * 0.70f;
}

// VK-PARITY-TERRAIN-2: the NIGHT-LIGHT-POOLS-2..4 / MOVER-CONTACT-SHADOW-1 knob
// constants (anonymous-namespace kTglc*) for the VK island.
void gos_TerrainLightPoolKnobs(int* soft, float* moverDim, float* moverScale,
                               int* moverMax, int* compress, float* contactShadow)
{
    if (soft)          *soft          = kTglcLightPoolSoft ? 1 : 0;
    if (moverDim)      *moverDim      = kTglcLightPoolMoverDim;
    if (moverScale)    *moverScale    = kTglcLightPoolMoverRadius;
    if (moverMax)      *moverMax      = kTglcLightPoolMoverMax ? 1 : 0;
    if (compress)      *compress      = kTglcLightPoolCompress ? 1 : 0;
    if (contactShadow) *contactShadow = kTglcMoverContactShadow;
}

float gos_TerrainPadGlowSpillWu()
{
    static const float s_v = mc2::gate::get<"MC2_PAD_AREA_GLOW_SPILL">();
    return s_v;
}
