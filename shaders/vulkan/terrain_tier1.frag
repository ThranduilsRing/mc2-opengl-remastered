// VK-P10 TERRAIN TIER-1 — 2-output GBuffer FS (in-place shared-GBuffer co-write).
// =============================================================================
// Co-writes the LOD0 terrain patch IN-PLACE into the SHARED scene GBuffer (no
// side-image, no composite), exactly like the prop Tier-1 FS but with the TERRAIN
// shadow-class alpha:
//
//   location=0 color  -> sceneColorTex_  (MRT0, RGBA16F). ★COLORMAP-BASE sub-slice:
//                        samples the REAL colormap atlas (VK-owned copy-once import
//                        of the GL g_atlasGLTex; see vulkan_terrain_island.cpp) at
//                        the SAME world-XY -> atlas-UV as the GL LOD-chunk frag
//                        (terrain_lod_chunk.frag:462-482), including the 9-tap disc
//                        blur, so the VK terrain BASE color matches GL's colormap.
//                        cement/road/overlay-sidecar/detail/POM/lighting are STILL
//                        DEFERRED (later sub-slices). Fail-soft: if the colormap is
//                        not importable/ready (uT.colormapReady==0) fall back to the
//                        olive placeholder so the pass never crashes or holes.
//                        a=1 (opaque, in-place — NOT premult; Tier-1 does not blend).
//
//   location=1 GBuffer1 -> sceneNormalTex_ (MRT1, RGBA16F). ★TERRAIN encoding
//                        vec4(0.5, 0.5, 1.0, 1.0): flat-up normal (0.5,0.5,1.0) +
//                        ★a=1.0 = terrain shadow-class "I handle my own shadow"
//                        (rc_gbuffer1_shadowHandled). BIT-EXACT with
//                        terrain_lod_chunk.frag:430 `vec4(0.5,0.5,1.0,1.0)`. This is
//                        the ONE difference vs props (a=0.25); the coverage oracle
//                        histograms a==1.0 inside the terrain silhouette.
#version 450

layout(location = 0) in vec3 v_worldPos;

layout(location = 0) out vec4 fragColor;   // MRT0 color
layout(location = 1) out vec4 GBuffer1;    // MRT1 normal + shadow class

// LOCKSTEP with terrain_tier1.vert TerrainFrameUBO (std140, one dynamic slot).
layout(set = 0, binding = 0, std140) uniform TerrainFrameUBO {
    mat4  worldToClipGL;
    int   mapSide;
    float halfMap;
    int   blockOriginX;
    int   blockOriginY;
    // COLORMAP-BASE sub-slice: atlas-UV reconstruction params (mirror the GL
    // u_atlasTopLeftX/Y + u_atlasOneOverWorldUnits uniforms) + a ready flag.
    float atlasTopLeftX;
    float atlasTopLeftY;
    float atlasOneOverWorldUnits;
    int   colormapReady;   // 1 = colormap atlas imported+bound; 0 = fall back olive
    // CEMENT/ROAD sub-slice: cement uniforms (mirror the GL LOD-chunk frag's
    // u_useCement/u_cementGridSide/u_cementWUPT/u_useTransitionMask/u_cementDiagConnect).
    int   useCement;        // 1 = cement atlas + cement-word snapshot both live
    int   cementGridSide;   // atlas tiles across
    float cementWUPT;       // world units per cement tile (== 128)
    int   useTransitionMask;// CEMENT-HARD-EDGE-1 gate (GL default-on)
    int   cementDiagConnect;// CEMENT-DIAG-CONNECT-1 gate (GL default-off)
} uT;

// COLORMAP-BASE: the VK-owned copy-once import of the GL colormap atlas.
layout(set = 0, binding = 2) uniform sampler2D u_colormap;
// CEMENT/ROAD: the VK-owned copy-once import of the GL cement atlas (REPEAT sampler) +
// the map-load snapshot of the GL per-vertex cement-word SSBO (GL binding 25).
layout(set = 0, binding = 3) uniform sampler2D u_cementAtlas;
layout(set = 0, binding = 4, std430) readonly buffer TerrainCementBuf {
    uint cementWordsF[];
} uC;

void main() {
    vec3 base;
    if (uT.colormapReady != 0) {
        // Mirror terrain_lod_chunk.frag:466-482 EXACTLY (world-XY -> atlas-UV, then
        // the 9-tap disc blur) so the VK base color matches GL's colormap sample.
        vec2 uv;
        uv.x = (v_worldPos.x - uT.atlasTopLeftX) * uT.atlasOneOverWorldUnits;
        uv.y = (uT.atlasTopLeftY - v_worldPos.y) * uT.atlasOneOverWorldUnits;
        const float CMAP_BLUR = 0.0020;
        const float CMAP_R2   = CMAP_BLUR * 0.707;
        base = texture(u_colormap, uv).rgb
             + texture(u_colormap, uv + vec2( CMAP_BLUR, 0.0)).rgb
             + texture(u_colormap, uv + vec2(-CMAP_BLUR, 0.0)).rgb
             + texture(u_colormap, uv + vec2(0.0,  CMAP_BLUR)).rgb
             + texture(u_colormap, uv + vec2(0.0, -CMAP_BLUR)).rgb
             + texture(u_colormap, uv + vec2( CMAP_R2,  CMAP_R2)).rgb
             + texture(u_colormap, uv + vec2(-CMAP_R2,  CMAP_R2)).rgb
             + texture(u_colormap, uv + vec2( CMAP_R2, -CMAP_R2)).rgb
             + texture(u_colormap, uv + vec2(-CMAP_R2, -CMAP_R2)).rgb;
        base /= 9.0;
    } else {
        // Fail-soft olive placeholder (colormap not importable/ready this frame).
        float hMod = clamp(v_worldPos.z / 512.0 + 0.5, 0.35, 1.0);
        base = vec3(0.42, 0.40, 0.28) * hMod;
    }

    // ★CEMENT/ROAD sub-slice: concrete apron / roads. Ports the GL LOD-chunk cement
    // branch (terrain_lod_chunk.frag:484-624) VERBATIM: the solid-tile override, the
    // CEMENT-HARD-EDGE-1 marching-squares transition cut, and CEMENT-DIAG-CONNECT-1.
    // Fed by the per-vertex cement-word SSBO (uC) + the REPEAT-sampled cement atlas.
    // The overlay-sidecar composite (GL :626-642) is STILL DEFERRED (not fed here).
    // uT.useCement==0 (gate off / atlas or words not ready) -> whole block skipped ->
    // colormap base only, byte-identical to the colormap-only frame.
    if (uT.useCement != 0) {
        bool cementHit = false;
        int  ctX = clamp(int(floor((v_worldPos.x + uT.halfMap) / 128.0)), 0, uT.mapSide - 1);
        int  ctY = clamp(int(floor((uT.halfMap - v_worldPos.y) / 128.0)), 0, uT.mapSide - 1);
        uint cw  = uC.cementWordsF[ctX + ctY * uT.mapSide];
        if ((cw & 0x80000000u) != 0u) {
            uint cLayerIdx    = cw & 0xFFFFu;
            bool isTransition = (cw & 0x40000000u) != 0u;
            int  cGridSide = uT.cementGridSide;
            if (cGridSide < 1) cGridSide = 1;
            int  cCol = int(cLayerIdx) % cGridSide;
            int  cRow = int(cLayerIdx) / cGridSide;
            vec2 cTileUV  = fract(vec2(v_worldPos.x, -v_worldPos.y) / uT.cementWUPT);
            vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cTileUV) / float(cGridSide);
            vec3 cementColor = texture(u_cementAtlas, cAtlasUV).rgb;
            if (isTransition) {
                if (uT.useTransitionMask != 0) {
                    bool nXm = false, nXp = false, nYm = false, nYp = false;
                    bool nXmYm = false, nXpYm = false, nXpYp = false, nXmYp = false;
                    int xm = ctX - 1, xp = ctX + 1, ym = ctY - 1, yp = ctY + 1;
                    if (xm >= 0)                           nXm   = (uC.cementWordsF[xm + ctY * uT.mapSide] & 0x80000000u) != 0u;
                    if (xp <  uT.mapSide)                  nXp   = (uC.cementWordsF[xp + ctY * uT.mapSide] & 0x80000000u) != 0u;
                    if (ym >= 0)                           nYm   = (uC.cementWordsF[ctX + ym * uT.mapSide] & 0x80000000u) != 0u;
                    if (yp <  uT.mapSide)                  nYp   = (uC.cementWordsF[ctX + yp * uT.mapSide] & 0x80000000u) != 0u;
                    if (xm >= 0 && ym >= 0)                nXmYm = (uC.cementWordsF[xm + ym * uT.mapSide] & 0x80000000u) != 0u;
                    if (xp <  uT.mapSide && ym >= 0)       nXpYm = (uC.cementWordsF[xp + ym * uT.mapSide] & 0x80000000u) != 0u;
                    if (xp <  uT.mapSide && yp < uT.mapSide) nXpYp = (uC.cementWordsF[xp + yp * uT.mapSide] & 0x80000000u) != 0u;
                    if (xm >= 0 && yp < uT.mapSide)        nXmYp = (uC.cementWordsF[xm + yp * uT.mapSide] & 0x80000000u) != 0u;
                    float cTL = (nXm   && nYm   && nXmYm) ? 1.0 : 0.0;  // UV (0,0)
                    float cTR = (nXp   && nYm   && nXpYm) ? 1.0 : 0.0;  // UV (1,0)
                    float cBR = (nXp   && nYp   && nXpYp) ? 1.0 : 0.0;  // UV (1,1)
                    float cBL = (nXm   && nYp   && nXmYp) ? 1.0 : 0.0;  // UV (0,1)
                    float u = cTileUV.x, v = cTileUV.y;
                    int cfg = (cTL > 0.5 ? 8 : 0)
                            | (cTR > 0.5 ? 4 : 0)
                            | (cBR > 0.5 ? 2 : 0)
                            | (cBL > 0.5 ? 1 : 0);
                    float alpha = 0.0;
                    if (cfg == 0) {
                        alpha = 0.0;
                    } else if (cfg == 15) {
                        alpha = 1.0;
                    } else if (cfg == 8) {        // cTL only
                        alpha = (u + v < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 4) {        // cTR only
                        alpha = ((1.0 - u) + v < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 2) {        // cBR only
                        alpha = ((1.0 - u) + (1.0 - v) < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 1) {        // cBL only
                        alpha = (u + (1.0 - v) < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 7) {        // missing cTL
                        alpha = (u + v >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 11) {       // missing cTR
                        alpha = ((1.0 - u) + v >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 13) {       // missing cBR
                        alpha = ((1.0 - u) + (1.0 - v) >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 14) {       // missing cBL
                        alpha = (u + (1.0 - v) >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 12) {       // cTL+cTR, top
                        alpha = (v < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 6) {        // cTR+cBR, right
                        alpha = (u >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 3) {        // cBR+cBL, bottom
                        alpha = (v >= 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 9) {        // cBL+cTL, left
                        alpha = (u < 0.5) ? 1.0 : 0.0;
                    } else if (cfg == 10) {       // cTL+cBR
                        alpha = ((u + v < 0.5) || ((1.0 - u) + (1.0 - v) < 0.5)) ? 1.0 : 0.0;
                    } else if (cfg == 5) {        // cTR+cBL
                        alpha = (((1.0 - u) + v < 0.5) || (u + (1.0 - v) < 0.5)) ? 1.0 : 0.0;
                    }
                    base = mix(base, cementColor, alpha);
                    if (alpha > 0.5) cementHit = true;
                }
            } else {
                base = cementColor;
                cementHit = true;
            }
        }

        // CEMENT-DIAG-CONNECT-1 (gate uT.cementDiagConnect): fill this tile's corner
        // quadrant toward a diagonally adjacent SOLID cement tile. Off by default.
        if (uT.cementDiagConnect != 0 && !cementHit) {
            vec2 dTileUV = fract(vec2(v_worldPos.x, -v_worldPos.y) / uT.cementWUPT);
            int  dGridSide = uT.cementGridSide;
            if (dGridSide < 1) dGridSide = 1;
            int dnX = (dTileUV.x > 0.5) ? (ctX + 1) : (ctX - 1);
            int dnY = (dTileUV.y > 0.5) ? (ctY + 1) : (ctY - 1);
            if (dnX >= 0 && dnX < uT.mapSide && dnY >= 0 && dnY < uT.mapSide) {
                uint dnw = uC.cementWordsF[dnX + dnY * uT.mapSide];
                bool dnValid      = (dnw & 0x80000000u) != 0u;
                bool dnTransition = (dnw & 0x40000000u) != 0u;
                if (dnValid && !dnTransition) {
                    uint dLayerIdx = dnw & 0xFFFFu;
                    int  dCol = int(dLayerIdx) % dGridSide;
                    int  dRow = int(dLayerIdx) / dGridSide;
                    vec2 dAtlasUV = (vec2(float(dCol), float(dRow)) + dTileUV) / float(dGridSide);
                    base = texture(u_cementAtlas, dAtlasUV).rgb;
                }
            }
        }
    }

    fragColor = vec4(base, 1.0);   // opaque in-place (a=1, NOT premult)

    // ★TERRAIN shadow-class GBuffer1: flat-up normal + a=1.0 (handles own shadow).
    GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
}
