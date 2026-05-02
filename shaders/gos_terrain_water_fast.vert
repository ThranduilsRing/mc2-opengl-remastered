//#version 430 (provided by makeProgram prefix)
//
// Stage 2 of the renderWater architectural slice. Pairs with gos_tex_vertex.frag
// (the existing water FS) — this VS produces the same Color/Texcoord/FogValue
// varyings the legacy gos_tex_vertex.vert produced, but reads from a static
// per-mission WaterRecipe SSBO + a per-frame WaterFrame ring SSBO instead of
// CPU-staged gos_VERTEX arrays.
//
// Spec: docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md
// Recipe: GameOS/gameos/gos_terrain_water_stream.h (64 B WaterRecipe, 32 B WaterFrame).
// Projection chain: identical to gos_terrain_thin.vert (terrainMVP → viewport
// → projection_), preserves abs(clip.w) sign trap (memory: clip_w_sign_trap.md,
// terrain_mvp_gl_false.md, terrain_tes_projection.md).

// --- Recipe SSBO (mission-static) ---
// Std430 layout matches the C++ struct WaterRecipe (64 B).
//   vec4 v01  = (v0x, v0y, v1x, v1y)
//   vec4 v23  = (v2x, v2y, v3x, v3y)
//   vec4 elev = (v0e, v1e, v2e, v3e)
//   uvec4 ctrl = (quadIdx, flags, terrainTypes, waterBits)
//     flags: bit0=uvMode (0=BOTTOMRIGHT, 1=BOTTOMLEFT), bit1=hasDetail
//     terrainTypes: 4 bytes packed v0..v3 low->high
//     waterBits:    4 bytes packed v0..v3 low->high; bit6/bit7 modulate wave
struct WaterRecipe {
    vec4  v01;
    vec4  v23;
    vec4  elev;
    uvec4 ctrl;
};
layout(std430, binding = 5) readonly buffer WaterRecipeBuf {
    WaterRecipe recipes[];
};

// --- Per-frame thin record SSBO ---
// 48 bytes/record. One entry per in-window water-bearing quad this frame.
// CPU walks the camera-windowed quadList, looks up each quad's stable
// recipe by top-left vertexNum, packs lightRGB/fogRGB/pzValid here, and
// passes the count as the draw instance count.
//
// flags bit 0 = pzValid (CPU's `waterHandle != 0xffffffff` gate from
// setupTextures' water block — mirrors the legacy water emit gate).
// VS cull-emits degenerate when pzValid=0.
struct WaterThinRecord {
    uvec4 ctrl;        // (recipeIdx, flags, _pad, _pad)
    uvec4 lightRGB;    // corner0..3 ARGB
    uvec4 fogRGB;      // corner0..3 fogRGB; .w byte is FogValue
};
layout(std430, binding = 6) readonly buffer WaterThinBuf {
    WaterThinRecord thinRecs[];
};
const uint kPzTri1ValidBit = 0x1u;
const uint kPzTri2ValidBit = 0x2u;

// Output varyings — must match gos_tex_vertex.frag `in` exactly.
out vec4  Color;
out vec2  Texcoord;
out float FogValue;

// Uniforms — set by Terrain::renderWaterFastPath C++ code.
uniform mat4  terrainMVP;        // axisSwap * worldToClip
uniform mat4  mvp;               // projection_: screen pixels -> NDC
uniform vec4  terrainViewport;   // (vmx, vmy, vax, vay)
uniform float waterElevation;    // Terrain::waterElevation
uniform float alphaDepth;        // MapData::alphaDepth
uniform vec2  mapTopLeft;        // Terrain::mapTopLeft3d.xy (note: y is positive-up)
uniform float frameCos;          // Terrain::frameCos (per-frame oscillator)
uniform float frameCosAlpha;     // Terrain::frameCosAlpha
// Per-pass UV: differs base (oneOverTF, cloudOffset) vs detail (oneOverWaterTF, sprayOffset)
uniform float uvScale;
uniform vec2  uvOffset;
// Per-pass alpha-band byte values (alphaEdge / alphaMiddle / alphaDeep are
// DWORDs whose alpha-byte is OR'd into argb's alpha). Pass the alpha BYTE
// directly (0..255). Stored as `int` because `uniform uint` crashes the
// project's shader_builder (memory: uniform_uint_crash.md). Cast to uint
// inside the shader before bitwise ops.
uniform int  alphaEdgeByte;
uniform int  alphaMiddleByte;
uniform int  alphaDeepByte;
// Per-pass color override: 0 = use lightRGB (base), 1 = white (detail).
uniform int   detailMode;
// Debug: 0 = normal, 1 = solid magenta opaque (verifies geometry),
//        2 = green from worldPos (verifies recipe data), 3 = red from UV.
uniform int   debugMode;
// MaxMinUV wrap floor — replicates legacy quad.cpp:2863-2884 wrap correction.
uniform float maxMinUV;

vec2 cornerXY(WaterRecipe r, uint cornerIdx) {
    if (cornerIdx == 0u) return r.v01.xy;
    if (cornerIdx == 1u) return r.v01.zw;
    if (cornerIdx == 2u) return r.v23.xy;
    return r.v23.zw;
}

float cornerElev(WaterRecipe r, uint cornerIdx) {
    if (cornerIdx == 0u) return r.elev.x;
    if (cornerIdx == 1u) return r.elev.y;
    if (cornerIdx == 2u) return r.elev.z;
    return r.elev.w;
}

uint unpackByte(uint packed, uint cornerIdx) {
    return (packed >> (cornerIdx * 8u)) & 0xFFu;
}

uint cornerLightRGB(WaterThinRecord t, uint cornerIdx) {
    if (cornerIdx == 0u) return t.lightRGB.x;
    if (cornerIdx == 1u) return t.lightRGB.y;
    if (cornerIdx == 2u) return t.lightRGB.z;
    return t.lightRGB.w;
}

uint cornerFogRGB(WaterThinRecord t, uint cornerIdx) {
    if (cornerIdx == 0u) return t.fogRGB.x;
    if (cornerIdx == 1u) return t.fogRGB.y;
    if (cornerIdx == 2u) return t.fogRGB.z;
    return t.fogRGB.w;
}

vec4 unpackARGB(uint packed) {
    return vec4(
        float((packed >> 16u) & 0xFFu) / 255.0,  // R
        float((packed >>  8u) & 0xFFu) / 255.0,  // G
        float((packed       ) & 0xFFu) / 255.0,  // B
        float((packed >> 24u) & 0xFFu) / 255.0   // A
    );
}

// Wave displacement modulator from per-vertex water bits, replicating the
// setupTextures water-projection block at quad.cpp:689-700:
//   bit 7 set → ourCos = -frameCos
//   bit 6 set → ourCos = +frameCos (default)
//   neither   → ourCos = +frameCos (default)
float waveOurCos(uint waterBits) {
    if ((waterBits & 0x80u) != 0u) return -frameCos;
    return frameCos;
}

// Alpha-band classifier per vertex (replicates quad.cpp:2829-2856 elevation
// bands: deep / middle / edge based on elevation vs waterElevation±alphaDepth).
// Returns the alpha BYTE (0..255). The byte uniforms are `int` because
// `uniform uint` crashes shader_builder (memory: uniform_uint_crash.md).
uint elevAlphaBandByte(float elev) {
    int a = alphaMiddleByte;
    if (elev >= (waterElevation - alphaDepth))
        a = alphaEdgeByte;
    if (elev <= (waterElevation - (alphaDepth * 3.0)))
        a = alphaDeepByte;
    return uint(a);
}

void main() {
    uint vid          = uint(gl_VertexID);
    uint vertInRecord = vid % 6u;
    uint triIdx       = vertInRecord / 3u;
    uint id           = vertInRecord % 3u;
    uint thinIdx      = vid / 6u;

    WaterThinRecord trec = thinRecs[thinIdx];
    uint recipeIdx       = trec.ctrl.x;
    uint thinFlags       = trec.ctrl.y;
    WaterRecipe     rec  = recipes[recipeIdx];

    // CPU-computed per-triangle pz validity. wz from setupTextures' water
    // projection; bits set when all three corners' wz ∈ [0,1). This is THE
    // LOAD-BEARING cull (memory:terrain_tes_projection.md "no homogeneous
    // clip-space test the GPU clipper can do to distinguish valid visible
    // vert from behind-camera vert"). Without it the abs(clip.w) chain
    // below produces visible garbage at far map / behind-camera quads.
    uint pzValid = (triIdx == 0u)
                   ? (thinFlags & kPzTri1ValidBit)
                   : (thinFlags & kPzTri2ValidBit);
    if (pzValid == 0u) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
        Color       = vec4(0.0);
        Texcoord    = vec2(0.0);
        FogValue    = 0.0;
        return;
    }

    uint flags  = rec.ctrl.y;
    uint uvMode = flags & 1u;

    // Corner index table — same convention as gos_terrain_thin.vert.
    //   BOTTOMRIGHT (uvMode=0): tri0=corners[0,1,2], tri1=corners[0,2,3]
    //   BOTTOMLEFT  (uvMode=1): tri0=corners[0,1,3], tri1=corners[1,2,3]
    uint cornerIdx;
    if (uvMode == 0u) {
        if (triIdx == 0u)
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 2u;
        else
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 2u : 3u;
    } else {
        if (triIdx == 0u)
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 3u;
        else
            cornerIdx = (id == 0u) ? 1u : (id == 1u) ? 2u : 3u;
    }

    // Raw world XY, elevation, and per-vertex water bits.
    vec2 vxy   = cornerXY(rec, cornerIdx);
    float velev = cornerElev(rec, cornerIdx);
    uint waterBits = unpackByte(rec.ctrl.w, cornerIdx);

    // Wave-displaced water-plane Z (replicates legacy setupTextures water
    // projection). The water surface bobs at +/- frameCos depending on the
    // mission-stable per-vertex water bits.
    float wz = waveOurCos(waterBits) + waterElevation;
    vec3 worldPos = vec3(vxy, wz);

    // World-derived UV: legacy formula at quad.cpp:2803-2810.
    //   u = (vx - mapTopLeft.x) * uvScale + uvOffset.x
    //   v = (mapTopLeft.y - vy) * uvScale + uvOffset.y
    float u = (vxy.x - mapTopLeft.x) * uvScale + uvOffset.x;
    float v = (mapTopLeft.y - vxy.y) * uvScale + uvOffset.y;

    // Per-triangle MaxMinUV wrap correction (legacy quad.cpp:2863-2884).
    // The legacy code computes max U/V across 3 verts and floor-shifts all
    // three by `floor(max - (maxMinUV - 1))`. In a per-vertex shader we can't
    // share state across the triangle, but the shift is the same for every
    // vertex of a triangle and depends only on the max — we can compute it
    // independently per vertex by examining all 3 corners of *this* triangle
    // (cheap: each corner reads its 3 sibling positions from the recipe).
    {
        // Look up all 3 corner positions for this triangle and compute their
        // UV before the shift, then shift all by the floor-of-max.
        uint c0, c1, c2;
        if (uvMode == 0u) {
            if (triIdx == 0u) { c0=0u; c1=1u; c2=2u; }
            else              { c0=0u; c1=2u; c2=3u; }
        } else {
            if (triIdx == 0u) { c0=0u; c1=1u; c2=3u; }
            else              { c0=1u; c1=2u; c2=3u; }
        }
        vec2 p0 = cornerXY(rec, c0);
        vec2 p1 = cornerXY(rec, c1);
        vec2 p2 = cornerXY(rec, c2);
        float u0 = (p0.x - mapTopLeft.x) * uvScale + uvOffset.x;
        float u1 = (p1.x - mapTopLeft.x) * uvScale + uvOffset.x;
        float u2 = (p2.x - mapTopLeft.x) * uvScale + uvOffset.x;
        float v0 = (mapTopLeft.y - p0.y) * uvScale + uvOffset.y;
        float v1 = (mapTopLeft.y - p1.y) * uvScale + uvOffset.y;
        float v2 = (mapTopLeft.y - p2.y) * uvScale + uvOffset.y;
        float maxU = max(u0, max(u1, u2));
        float maxV = max(v0, max(v1, v2));
        if (maxU > maxMinUV || maxV > maxMinUV) {
            float shiftU = floor(maxU - (maxMinUV - 1.0));
            float shiftV = floor(maxV - (maxMinUV - 1.0));
            u -= shiftU;
            v -= shiftV;
        }
    }

    // ARGB color. Base layer: low 24 bits from lightRGB, alpha byte from
    // elevation band. Detail layer: white RGB, same alpha byte.
    uint elevAlphaByte = elevAlphaBandByte(velev);
    uint argb;
    if (detailMode == 0) {
        uint lrgb = cornerLightRGB(trec, cornerIdx);
        argb = (lrgb & 0x00FFFFFFu) | (elevAlphaByte << 24);
    } else {
        argb = (elevAlphaByte << 24) | 0x00FFFFFFu;
    }

    // FogValue: high byte of fogRGB.
    uint frgb = cornerFogRGB(trec, cornerIdx);
    FogValue = float((frgb >> 24u) & 0xFFu) / 255.0;

    // gos_tex_vertex.frag does `c = Color.bgra` — that swizzle was written
    // to undo the legacy GL-attribute BGRA-in-memory byte layout. SSBO bit
    // decode here produces logical RGBA (per mc2_argb_packing.md "SSBO uint
    // → bit decode" path), so we pre-swizzle to BGRA in the VS — the FS's
    // `.bgra` then double-swizzles back to correct RGBA.
    Color    = unpackARGB(argb).bgra;
    Texcoord = vec2(u, v);

    // Debug overrides — gated by uniform debugMode. The FS still does
    //   c = Color.bgra; c *= tex_color
    // so to land magenta-opaque post-FS we encode (1,1,0,1) here (which
    // becomes (0,1,1,1) after .bgra, then multiplied by the texture sample).
    // Easier: bypass the texture multiplication by setting Texcoord to a
    // known transparent-or-edge spot is harder; just live with the texture
    // tint and observe whether ANY pixels get raster-output where water
    // should be.
    if (debugMode == 1) {
        // Magenta after .bgra swizzle (we provide it pre-swizzled).
        Color = vec4(1.0, 0.0, 1.0, 1.0).bgra;
    } else if (debugMode == 2) {
        // Green encoded so that .bgra in FS outputs green.
        Color = vec4(0.0, 1.0, 0.0, 1.0).bgra;
    } else if (debugMode == 3) {
        // Yellow.
        Color = vec4(1.0, 1.0, 0.0, 1.0).bgra;
    } else if (debugMode == 4) {
        // Per-vertex alpha-band visualizer (alpha → R/G/B hue).
        float a = float(elevAlphaByte) / 255.0;
        vec3 col = vec3(a, 1.0 - abs(a - 0.5) * 2.0, 1.0 - a);
        Color = vec4(col, 1.0).bgra;
        Texcoord = vec2(0.5);
    } else if (debugMode == 5) {
        // cornerIdx visualizer. Each corner gets a distinct color.
        // 0→red, 1→green, 2→blue, 3→yellow. If interpolation works AND
        // per-vertex cornerIdx differs, tiles show smooth gradients between
        // these colors. If a tile is uniformly one color, cornerIdx is
        // resolving to the same value for all 4 corners.
        vec3 col;
        if      (cornerIdx == 0u) col = vec3(1.0, 0.0, 0.0);
        else if (cornerIdx == 1u) col = vec3(0.0, 1.0, 0.0);
        else if (cornerIdx == 2u) col = vec3(0.0, 0.0, 1.0);
        else                       col = vec3(1.0, 1.0, 0.0);
        Color = vec4(col, 1.0).bgra;
        Texcoord = vec2(0.5);
    } else if (debugMode == 6) {
        // Per-vertex elevation visualizer. Maps elevation 0..400 to
        // 0..1 R intensity. If tiles show a gradient, elevations differ
        // per corner. If uniform, elevations are the same per corner.
        float normElev = clamp(velev / 400.0, 0.0, 1.0);
        Color = vec4(normElev, 0.0, 1.0 - normElev, 1.0).bgra;
        Texcoord = vec2(0.5);
    }

    // Double projection chain — identical to gos_terrain_thin.vert.
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    // Layered TERRAIN_DEPTH_FUDGE: water MUST be biased farther than terrain
    // so it loses LEQUAL ties to land at the coast (smooth shore — no
    // z-fighting sparkle, no false-positive water on pixels where the TES'd
    // terrain crests exactly to waterElevation).
    //
    // History: 2026-04-30 (commit bc8c4f1) shipped this at +0.001 to match
    // legacy CPU emit's TERRAIN_DEPTH_FUDGE (quad.cpp:2775). Worked because
    // terrain's TES + thin VS paths emitted screen.z without any fudge — so
    // water's +0.001 sat strictly above terrain's 0.0, water lost ties, shore
    // was smooth. 2026-05-01 (commit ee0a7bc) added +0.001 to terrain.tese:132
    // and gos_terrain_thin.vert:175 to fix decal/overlay z-ties (issue #12,
    // power generator glow). That collapsed water and terrain to the SAME
    // bias; ties at coast no longer favored terrain, render order let water
    // win, shoreline staircase regressed (v0.3 build). 2026-05-02 fix: bump
    // water to 2× the terrain fudge to re-establish the layering.
    //
    // Three-tier z-ordering invariant (load-bearing):
    //   decals/overlays:  +0.000   (drawn first; smaller z, win LEQUAL pre-terrain)
    //   terrain:          +0.001   (terrain.tese:132, gos_terrain_thin.vert:175)
    //   water:            +0.002   (this line; water draws last via post-renderLists hook)
    //
    // Future drift check: if terrain's fudge changes, water's must move with it
    // by the same delta. Watch for symmetric edits.
    screen.z = clip.z * rhw + 0.002;
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    gl_Position = vec4(ndc.xyz * absW, absW);

}
