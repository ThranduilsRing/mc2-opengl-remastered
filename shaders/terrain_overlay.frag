// terrain_overlay.frag
// Fragment shader for alpha cement perimeter / transition tiles.
// These tiles are definitively concrete so we can apply the full cloud FBM range
// (mix 0.70-1.0) that gos_terrain.frag uses — no more per-pixel range narrowing.
//
// MRT:
//   location=0  FragColor       — lit scene colour
//   location=1  GBuffer1.alpha  = 1.0  → shadow_screen.frag skips these pixels
//                                 (same flag as solid terrain, so deferred shadow pass
//                                  treats them identically to interior runway tiles)

#define PREC highp

#include <include/noise.hglsl>
#include <include/shadow.hglsl>
#include <include/render_contract.hglsl>
// TERRAIN-DECAL-LIGHTING-1a: share the terrain lighting stack so cement
// transition overlays no longer form a lighting seam against lit terrain.
// Same 6 uniforms + computeTerrainNormalFromHeight() as gos_terrain.frag.
// Default-OFF (all 3 terrain env gates unset) → uniforms force-zeroed by
// CPU helper → all branches short-circuit → byte-identical legacy output.
#include <include/terrain_height_normal.hglsl>

// [RENDER_CONTRACT]
//   Pass:           TerrainOverlay
//   Color0:         RGBA, alpha-blended (binary alpha; transparent pixels discarded)
//   GBuffer1:       rc_gbuffer1_shadowHandled_flatUp
//   ShadowContract: castsStatic=false, castsDynamic=false,
//                   skipsPostScreenShadow=true (overlay handles shadow inline)
//   StateContract:  depthTest=true, depthWrite=false, blend=AlphaBlend,
//                   requiresMRT=true

in PREC vec3  WorldPos;
in PREC vec2  Texcoord;
in PREC float FogValue;   // 1=clear, 0=fully fogged
in PREC vec4  Color;      // RGBA [0,1]

layout(location=0) out PREC vec4 FragColor;
#ifdef MRT_ENABLED
layout(location=1) out PREC vec4 GBuffer1;
#endif

uniform sampler2D tex1;
uniform PREC vec4 fog_color;
uniform PREC float time;
uniform PREC vec4 cameraPos;
uniform vec4 terrainLightDir;
uniform int surfaceDebugMode;
uniform PREC float mapHalfExtent;  // half side length of playable map (0 = disabled)
uniform int u_pathTint;  // MC2_SHADER_PATH_TINT: 1 = solid signature colour (debug); 0 = normal

// CEMENT-NIGHT-LIGHT-1: mission flat-pad light colour. This shader's fixed
// tints are light-blind (main terrain darkness is burned into its colormap
// CPU-side), so runway/road decals + cement transition rings stayed bright on
// night missions. Multiply into the composed albedo. Day ~= (1,1,1) -> near
// byte-identical. Killswitch MC2_CEMENT_MISSION_LIGHT=0 force-uploads (1,1,1).
// Uploaded by uploadOverlayUniforms_ (all binders of this frag route there).
uniform PREC vec3      u_missionFlatLight;
// RUNWAY-CLOSE-1 (MC2_OVERLAY_ART_FLATLIGHT, default 1 = stock): 0 uploads
// u_overlayArtFlatExempt = 1.0, which EXEMPTS the non-adopt (authored road /
// runway art) else-branch from the CEMENT-NIGHT-LIGHT-1 u_missionFlatLight
// multiply. A/B arm for the reopened "runway is the wrong color" item.
// NOTE (measured, do not re-derive): the chunk cement apron ALSO multiplies
// u_missionFlatLight (terrain_lod_chunk.frag:999), so on mc2_10
// (flatLight = 192/255 grey, LightDirPitch 180 -> ambient branch) full
// exemption OVERBRIGHTENS art by ~1.33x relative to the apron. It exists to
// measure that, not to be presumed correct.
uniform PREC float     u_overlayArtFlatExempt;

// NIGHT-LIGHT-POOLS-2b (gate MC2_TERRAIN_LIGHT_POOLS, shared with the chunk
// terrain): road/runway/cement decals take the same additive light-pool term
// as the terrain around them — without it a decal crossing a lit pool stayed
// a dark band. Same GpuTerrainLight SSBO (binding 28, bound by the host at
// upload time) + same falloff/soft/dim math as terrain_lod_chunk.frag.
// u_numPoolLights==0 (gate off / producer off / no lights) -> loop skipped ->
// byte-identical.
struct PoolLight {
    vec3  position;
    uint  lightType;
    vec3  color;
    float closeDistance;
    float farDistance;
    float oneOverDistance;
    float _plPad1;   // 1.0 = mover-attached (producer-packed)
    float _plPad2;
};
layout(binding = 28, std430) readonly buffer TerrainLightPoolOverlay {
    PoolLight poolLights[];
};
uniform int   u_numPoolLights;
uniform int   u_lightPoolSoft;
uniform float u_lightPoolMoverDim;
// NIGHT-LIGHT-POOLS-3: tone-compress the summed pool (same semantics as the
// chunk frag; keeps decal pooling in lockstep with the terrain under it).
uniform int   u_lightPoolCompress;
// NIGHT-LIGHT-POOLS-4: mover radius scale + max-blend (lockstep with the chunk
// frag — see terrain_lod_chunk.frag block comment for the model rationale).
uniform float u_lightPoolMoverScale;
uniform int   u_lightPoolMoverMax;

// PAD-AREA-GLOW-1: same cement-word SSBO the chunk terrain binds at binding 25
// (persistent context-global binding, same pattern as the pool SSBO above).
// Road/runway/cement decals crossing a glowing pad must take the same lift or
// they read as dark bands. u_padGlowMapSide force-uploads 0 when the chunk
// terrain grid is absent (legacy terrain path) -> loop never entered -> the
// SSBO is never read -> safe + byte-identical.
layout(binding = 25, std430) readonly buffer TerrainCementBufOverlay {
    uint cementWordsPG[];
};
uniform PREC vec3  u_padGlow;        // premultiplied strength*color*nightFactor
uniform PREC float u_padGlowSpill;   // feather distance (wu), < 128
uniform int        u_padGlowMapSide; // chunk cement grid side (0 = unavailable)
uniform PREC float u_padGlowHalfMap; // mapSide * 128 / 2 (wu)

// OVERLAY-CEMENT-SHADOW-MATCH-1 (gate MC2_OVERLAY_CEMENT_SHADOW_MATCH, default
// 0 = OFF = byte-identical). CEMENT-PATCHY-CAUSE-1 attributed the mc2_23 apron
// "outlines of the concrete" to a SEAM BETWEEN TWO SHADERS, not to the cement
// atlas: a paved surface is drawn partly by terrain_lod_chunk.frag (the cement
// terrain) and partly by THIS shader (the overlay/decal layer, eid 3678 in
// scratch/cpc1/rg_bm.rdc), and the two disagree about the concrete lighting
// exemption. The chunk frag drops the STATIC (baked) shadow on concrete —
// `shadow = mix(shadow, dynS, pureConcrete)` — while the overlay multiplies by
// staticShadow * dynamicShadow unconditionally. Where the two layers abut, the
// static-shadow term appears on one side of a hard polygon edge and not the
// other, which reads as a tone outline through flat concrete.
//
// When armed, an overlay fragment whose OWN 128wu tile carries a VALID cement
// word (bit31 — the same word the chunk frag keys `cementHit` off, and the same
// SSBO already bound at binding 25 for the pad glow) drops staticShadow exactly
// as the chunk frag does, keeping the dynamic shadow. No new binding, no new
// texture fetch — one SSBO word. u_padGlowMapSide == 0 (legacy terrain path, no
// chunk grid) => never armed => the SSBO is not read.
uniform int u_overlayCementShadowMatch;

// OVERLAY-CEMENT-ADOPT-1 (gate MC2_OVERLAY_CEMENT_ADOPT, default 1 = ON).
// This is the delivered cure for the long-running "the concrete is a patchwork /
// the transition tiles are the wrong colour" report, and it SUPERSEDES
// MC2_OVERLAY_CEMENT_SHADOW_MATCH above (which is retained only as the narrow
// legacy killswitch; the adopt path applies that same static-shadow exemption
// and more).
//
// THE SEAM, from source. One physical concrete surface is drawn partly by
// terrain_lod_chunk.frag and partly by THIS shader, and the two compute:
//
//   chunk :  base = cementAtlas * u_missionFlatLight        (:985)
//            normalLight -> 1.0 on concrete                 (:1868-1889)
//            shadow      -> dynamicShadow only              (:1937)
//            NO distance fog anywhere in the shader
//   overlay: c = decalTex * vec3(0.82,0.80,0.76) * flatLight  (a DIFFERENT
//            texture, plus a hard ~18-20% darkening)
//            shadow = staticShadow * dynamicShadow           (static NOT dropped)
//            c = mix(c, vec3(0.58,0.65,0.75), fogAmount)     (IN-SHADER FOG,
//                                                             up to 0.70)
//
// FOUR disagreements, not one. CEMENT-PATCHY-CAUSE-1 found and gated only the
// static-shadow term, measured it at 0.0000%, and correctly reported it was not
// the cure. The term it missed is the DISTANCE FOG, and that term is what its
// own data was already showing:
//   * its measured seam step GROWS with camera distance (runway_ground -4.95 L,
//     runway_mid -16.93 L, runway_overview -14.81 L). Only a distance-dependent
//     term can do that, and fog is the only one in the difference set.
//   * its RenderDoc pixel values are BLUE-SHIFTED on the overlay side only:
//     overlay eid 3678 wrote [0.301,0.3083,0.3623] (B > R); terrain eid 408
//     wrote [0.1515,0.1836,0.1495] (B lowest). The overlay's own albedo tint is
//     WARM (0.82/0.80/0.76, B lowest) and cannot produce a blue-dominant pixel.
//     The fog colour (0.58,0.65,0.75) is blue-dominant.
// The overlay layer is DOUBLE-FOGGED: its own in-shader fog, then the shared
// screen-space edge_fog/fog_oob that both layers receive. That lifts and blues
// it away from the terrain concrete progressively with distance -- the
// "patchwork" as seen at normal gameplay altitude.
//
// THE DESIGN CALL (user-ratified): the TERRAIN path wins on a cement tile. It
// owns ~75% of the pixels, it is flat-lit by construction, and that flat-lit
// look is what the user has ratified as correct (mc2_01 concrete, the mc2_03
// pads). So on cement this shader adopts the chunk frag's expression exactly.
//
// SCOPING (user requirement 3 -- "runways, roads, other overlays are their own
// layer"):
//   bit 31 set, bit 24 CLEAR -> genuine concrete transition tile, i.e. cement
//        with NO authored overlay art on it at all:
//        adopt the cement ATLAS albedo (user requirement 1, "the transition
//        tiles should just use the same concrete texture"), drop the warm tint,
//        drop static shadow, skip in-shader fog.
//   bit 31 set, bit 24 SET   -> ANY authored &Overlays art ON a cement base
//        (paved road, runway, dirt road, ROUGH, bridge, crossings):
//        KEEP its own albedo untouched; reconcile lighting only (drop static
//        shadow, skip in-shader fog). Repainting here would be the albedo swap
//        the user rejected twice as "WAY worse".
//   bit 31 clear             -> not cement, or class unknown: LEAVE ENTIRELY
//        ALONE. Ambiguity always resolves to "do nothing".
//
// Cost: zero new bindings and zero new plumbing on the class channel -- the
// cement-word SSBO is already bound at binding 25 for PAD-AREA-GLOW-1, and the
// cement atlas is the same texture the chunk pass samples. u_padGlowMapSide == 0
// (legacy terrain path, no chunk grid) => never armed => the SSBO is not read.
// Gate OFF => u_overlayCementAdopt uploads 0 => the first compare in
// overlayCementWord() short-circuits => byte-identical.
uniform int         u_overlayCementAdopt;
uniform sampler2D   u_cementAtlasOv;
uniform int         u_cementGridSideOv;   // 0 = atlas not resident
uniform PREC float  u_cementWUPTOv;       // world units per cement tile
// OVERLAY-CEMENT-SEAM-PARITY-1 (CONCRETE-DELIVER-2): mirror of the chunk frag's
// u_cementAtlasSeamFix (gate MC2_CEMENT_ATLAS_SEAM_FIX, default ON). Uploaded
// from the SAME env read so the two shaders can never disagree on it.
uniform int         u_cementSeamFixOv;

// Returns this fragment's own 128wu cement word, or 0 when the adopt path is
// disarmed / the chunk grid is absent (in which case the SSBO is NOT read).
uint overlayCementWord(PREC vec2 wp) {
    if (u_overlayCementAdopt == 0 || u_padGlowMapSide <= 0) return 0u;
    int cx = clamp(int(floor((wp.x + u_padGlowHalfMap) / 128.0)), 0, u_padGlowMapSide - 1);
    int cy = clamp(int(floor((u_padGlowHalfMap - wp.y) / 128.0)), 0, u_padGlowMapSide - 1);
    return cementWordsPG[cx + cy * u_padGlowMapSide];
}

// Cement atlas sample, the same UV math as terrain_lod_chunk.frag:965-985 (cell
// from the low 16 bits, tile UV from world position, atlas UV from cell + tile)
// times the same u_missionFlatLight.
//
// OVERLAY-CEMENT-SEAM-PARITY-1 (CONCRETE-DELIVER-2): the original
// OVERLAY-CEMENT-ADOPT-1 landing claimed "byte-for-byte the same UV math" but
// OMITTED the chunk frag's CEMENT-ATLAS-SEAM-1 half-texel inset (:976-980),
// which is DEFAULT ON. Without it the adopted sample runs to the raw cell edge,
// so within a half-texel of every 128wu tile border the bilinear tap reaches
// into the NEIGHBOURING atlas cell -- a different cement layer -- and lays a thin
// wrong-coloured fringe along exactly the tile boundaries the adopt path was
// added to make seamless. That is the same artifact class the user reports as a
// "weird coloured edge", so shipping the adoption without the inset would have
// traded one edge for another. Inset here, from the SAME gate, same formula.
PREC vec3 overlayCementAlbedo(uint cw, PREC vec2 wp) {
    uint cLayerIdx = cw & 0xFFFFu;
    int  cGridSide = max(u_cementGridSideOv, 1);
    int  cCol = int(cLayerIdx) % cGridSide;
    int  cRow = int(cLayerIdx) / cGridSide;
    PREC vec2 cTileUV   = fract(vec2(wp.x, -wp.y) / u_cementWUPTOv);
    PREC vec2 cSampleUV = cTileUV;
    if (u_cementSeamFixOv != 0) {
        PREC float cCellPx = float(textureSize(u_cementAtlasOv, 0).x) / float(cGridSide);
        PREC float cInset  = 0.5 / max(cCellPx, 1.0);
        cSampleUV = clamp(cTileUV, vec2(cInset), vec2(1.0 - cInset));
    }
    PREC vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cSampleUV) / float(cGridSide);
    return texture(u_cementAtlasOv, cAtlasUV).rgb * u_missionFlatLight;
}

bool overlayOwnTileIsCement(PREC vec2 wp) {
    if (u_overlayCementShadowMatch == 0 || u_padGlowMapSide <= 0) return false;
    int cx = clamp(int(floor((wp.x + u_padGlowHalfMap) / 128.0)), 0, u_padGlowMapSide - 1);
    int cy = clamp(int(floor((u_padGlowHalfMap - wp.y) / 128.0)), 0, u_padGlowMapSide - 1);
    return (cementWordsPG[cx + cy * u_padGlowMapSide] & 0x80000000u) != 0u;
}

// Lockstep with terrain_lod_chunk.frag padGlowMask() — see the writeup there.
PREC float padGlowMaskOv(PREC vec2 wp) {
    PREC vec2 g = vec2((wp.x + u_padGlowHalfMap) / 128.0,
                       (u_padGlowHalfMap - wp.y) / 128.0);
    int cx = clamp(int(floor(g.x)), 0, u_padGlowMapSide - 1);
    int cy = clamp(int(floor(g.y)), 0, u_padGlowMapSide - 1);
    PREC float m = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = cx + dx, ny = cy + dy;
            if (nx < 0 || ny < 0 || nx >= u_padGlowMapSide || ny >= u_padGlowMapSide) continue;
            if ((cementWordsPG[nx + ny * u_padGlowMapSide] & 0x80000000u) == 0u) continue;
            PREC vec2 rMin = vec2(float(nx), float(ny));
            PREC vec2 dv = max(max(rMin - g, g - (rMin + 1.0)), vec2(0.0));
            PREC float distWu = length(dv) * 128.0;
            m = max(m, 1.0 - smoothstep(0.0, max(u_padGlowSpill, 1.0), distWu));
            if (m >= 1.0) return 1.0;
        }
    }
    return m;
}

PREC vec3 overlayPoolLight(PREC vec3 wp) {
    PREC vec3 pool      = vec3(0.0);
    PREC vec3 poolMover = vec3(0.0);   // NIGHT-LIGHT-POOLS-4: max-combined movers
    for (int i = 0; i < u_numPoolLights; ++i) {
        if (poolLights[i].lightType == 0u) continue;
        PREC vec3  vtl = wp - poolLights[i].position;
        PREC float len = sqrt(dot(vtl, vtl));
        bool  mover = poolLights[i]._plPad1 > 0.5;
        PREC float farD  = poolLights[i].farDistance;
        PREC float nearD = poolLights[i].closeDistance;
        PREC float ooD   = poolLights[i].oneOverDistance;
        if (mover && u_lightPoolMoverScale < 0.999) {
            PREC float s = max(u_lightPoolMoverScale, 0.05);
            farD *= s; nearD *= s; ooD /= s;
        }
        if (len >= farD) continue;
        PREC float falloff = (len <= nearD)
                      ? 1.0
                      : (farD - len) * ooD;
        if (u_lightPoolSoft != 0) {
            PREC float t = clamp(falloff, 0.0, 1.0);
            falloff = t * t * (3.0 - 2.0 * t);
        }
        PREC vec3 plCol = poolLights[i].color;
        if (mover) {
            plCol *= u_lightPoolMoverDim;
            if (u_lightPoolMoverMax != 0) {
                poolMover = max(poolMover, plCol * falloff);
                continue;
            }
        }
        pool += plCol * falloff;
    }
    pool += poolMover;
    if (u_lightPoolCompress != 0)
        return vec3(1.0) - exp(-1.6 * pool);
    return min(pool, vec3(1.0));
}

void main()
{
    // MC2_SHADER_PATH_TINT: solid BLUE so this shader's surfaces are unmistakable.
    if (u_pathTint != 0) {
        FragColor = vec4(0.0, 0.0, 1.0, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
    PREC vec4 tex_color = texture(tex1, Texcoord);
    // Vertex argb is forced to 0xffffffff in quad.cpp so no vertex-lighting needed.
    // Solid interior cement has a warm ochre cast; transition atlas tiles are cooler/brighter.
    // Intentional border darkening: ~18% under interior cement so the edge reads as
    // deliberate weathered concrete rather than a failed near-match.
    // Warm tilt (B pulled lower than R/G) matches the ochre cast of the solid interior.
    // OVERLAY-CEMENT-ADOPT-1: classify this fragment once; see the block comment
    // above. ovCementTile gates the LIGHTING reconciliation (shadow + fog),
    // ovAdoptAlbedo additionally gates the ALBEDO adoption and is deliberately
    // the narrower of the two.
    uint ovCw          = overlayCementWord(WorldPos.xy);
    bool ovCementTile  = (ovCw & 0x80000000u) != 0u;          // bit 31 valid
    // bit 24 = ANY authored art on this cement tile, from EITHER channel.
    // OVERLAY-CEMENT-ARTGUARD-1 widened this from "hard-paved ids only" (the
    // narrow form left DIRT_ROAD / ROUGH / TWO_LANE_DIRT_ROAD / the dirt-damaged
    // crossings / OBRIDGE / DAMAGED_BRIDGE unmarked) to any &Overlays id;
    // CORRECTION-FIRST-MERGE-1 widened it again to the host-side TileIsRoadArt()
    // oracle, which ALSO covers cement-ALPHA road art carrying no &Overlays id
    // at all -- the class mc2_10's runway belongs to, and the one whose repaint
    // measured +46.8 L over 23% of that frame.
    bool ovAuthoredArt = (ovCw & 0x01000000u) != 0u;          // bit 24
    bool ovAdoptAlbedo = ovCementTile && !ovAuthoredArt && u_cementGridSideOv > 0;

    PREC vec4 c;
    // Alpha ALWAYS comes from the decal texture: the marching-squares transition
    // SHAPE is authored there and must survive the albedo adoption untouched.
    // Only the colour inside that shape changes.
    c.a = tex_color.a;
    if (ovAdoptAlbedo) {
        // User requirement 1: "the transition tiles should just use the same
        // concrete texture." Same atlas, same cell, same UV math, same flat
        // light as the terrain beside it -> no tone step across the seam. The
        // hardcoded vec3(0.82,0.80,0.76) "intentional border darkening" is
        // dropped here: that ~18-20% tint IS the user's "weird colored edge".
        c.rgb = overlayCementAlbedo(ovCw, WorldPos.xy);
    } else {
        // CEMENT-EDGE-TINT-1: the non-adopt branch is the ARTGUARD path
        // (bit 24 = authored road/transition art, contract "keep its albedo,
        // reconcile LIGHTING only") PLUS every art tile that bakes NO cement
        // word at all — the MISSING-RUNWAY-1 cement-ALPHA class (mc2_10/mc2_23
        // runways: no cement corner, so word==0 unless the default-OFF
        // MC2_CEMENT_ALPHA_ATLAS rescue runs). Stock composited the raw decal
        // texel with NO tint; the hardcoded vec3(0.82,0.80,0.76) border
        // darkening is a modernization-era addition (overlay rewrite era) and
        // is the measured grey edge slab on the mc2_24 apron (slab/interior
        // ratio 0.827/0.810/0.772 == the constant) AND the wrong-coloured
        // runway surface. Gating on ovCementTile alone misses the word==0
        // runway tiles, so the drop is keyed on the adopt gate itself:
        // MC2_OVERLAY_CEMENT_ADOPT=0 restores the tint everywhere (full
        // revert), default ON renders every authored texel untinted — the
        // stock compositing.
        c.rgb = (u_overlayCementAdopt != 0)
                    ? tex_color.rgb
                    : tex_color.rgb * vec3(0.82, 0.80, 0.76);
        // CEMENT-NIGHT-LIGHT-1: apply the mission flat-pad light to the composed
        // albedo (cement transition tint — raw/unburned art).
        // RUNWAY-CLOSE-1: u_overlayArtFlatExempt = 0.0 (default) keeps the
        // stock multiply; 1.0 lifts the flat-pad light off authored art only.
        c.rgb *= mix(u_missionFlatLight, vec3(1.0), u_overlayArtFlatExempt);
    }

    // Discard transparent pixels — cement transitions are binary-alpha tiles.
    // This keeps depth writes and GBuffer1 writes on the cement-visible region only,
    // letting the terrain underneath show through unchanged on the transparent region.
    if (c.a < 0.5) discard;

    if (surfaceDebugMode == 6) {
        FragColor = vec4(tex_color.rgb, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // Cloud shadows moved to the fullscreen cloud pass (cloud.frag).
    if (surfaceDebugMode == 3) {
        FragColor = vec4(c.rgb, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    vec3  lightDir3 = normalize(terrainLightDir.xyz);
    // TERRAIN-DECAL-LIGHTING-1a: use the height-derived normal for shadow
    // sampling when the same gate the terrain main path uses is active.
    // Matches gos_terrain.frag's shadowN convention: pass the flat up-normal
    // (NOT the height-derived one) to calcShadow so slope-scale bias stays
    // consistent pixel-to-pixel — this prevents the sprinkle/inverted-shadow
    // pattern that bumpy normals cause on shadow PCF (see the comment block
    // at gos_terrain.frag:732). Height-derived normal is only used by the
    // hemisphere additive below.
    const PREC vec3 shadowN = vec3(0.0, 0.0, 1.0);
    float staticShadow  = calcShadow(WorldPos, shadowN, lightDir3, 8);
    float dynamicShadow = calcDynamicShadow(WorldPos, shadowN, lightDir3, 4);
    float shadow        = staticShadow * dynamicShadow;
    // OVERLAY-CEMENT-SHADOW-MATCH-1: on a cement tile, match the chunk frag's
    // concrete exemption (static shadow dropped, dynamic kept). Gate OFF =>
    // overlayOwnTileIsCement() returns false on its first compare => unchanged.
    if (overlayOwnTileIsCement(WorldPos.xy)) shadow = dynamicShadow;
    // OVERLAY-CEMENT-ADOPT-1: same exemption, applied to EVERY cement-classified
    // fragment (concrete transitions AND road/runway art sitting on cement), so
    // the two shaders agree on the shadow term. Matches terrain_lod_chunk.frag
    // :1937 `shadow = mix(shadow, dynS, pureConcrete)` on full concrete.
    if (ovCementTile) shadow = dynamicShadow;
    // DEBUG-VIZ: 30 = dynamic-cast shadow only (isolates building dynamic shadow
    // on cement vs grass), 31 = combined. Grayscale, early-return.
    if (surfaceDebugMode == 30) {
        FragColor = vec4(vec3(dynamicShadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
    if (surfaceDebugMode == 31) {
        FragColor = vec4(vec3(shadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
    if (surfaceDebugMode == 2) {
        FragColor = vec4(vec3(shadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    c.rgb *= shadow;

    // TERRAIN-DECAL-LIGHTING-1a: V1 hemisphere additive (+ V2 shadow-aware
    // floor). Same expression as gos_terrain.frag:780-846 but with
    // snowWeight=0 inline (cement doesn't snow). Default-OFF byte-equivalence:
    // terrainLightingV1Strength uploads 0.0 when the V1 env gate is unset
    // → entire branch skipped → output identical to pre-slice. When V1 is
    // ON but V2 is OFF, the CPU helper force-uploads V2 floor=1.0 so the
    // mix(floor, 1.0, shadow) collapses to 1.0 → V1 unmodulated. N is the
    // height-derived normal when the NFH gate is on, else flat up.
    if (terrainLightingV1Strength > 0.0) {
        PREC vec3 N = vec3(0.0, 0.0, 1.0);
        if (useTerrainNormalsFromHeight != 0 && terrainHeightParams.x > 0.5) {
            PREC vec3 hN = computeTerrainNormalFromHeight(WorldPos.xy);
            // Same additive-perturbation form as gos_terrain.frag. No detail
            // normal contribution on cement — start from flat up.
            N.xy += (hN.xy / max(hN.z, 0.05)) * terrainNormalsFromHeightStrength;
            N.xy = clamp(N.xy, -0.75, 0.75);
            N = normalize(N);
        }
        const PREC vec3 hemiSkyTint    = vec3(0.55, 0.62, 0.75);
        const PREC vec3 hemiGroundTint = vec3(0.32, 0.28, 0.22);
        PREC float skyFactor = N.z * 0.5 + 0.5;
        PREC vec3  hemiFill  = mix(hemiGroundTint, hemiSkyTint, skyFactor);
        PREC float hemiAmount = terrainLightingV1Strength;  // no snowWeight on cement
        PREC float hemiShadowMix = mix(terrainLightingV2ShadowFillFloor, 1.0, shadow);
        c.rgb += hemiFill * hemiAmount * 0.25 * hemiShadowMix;
    }

    PREC float camDist2D = distance(WorldPos.xy, cameraPos.xy);
    PREC float terrainHeight = WorldPos.z;
    PREC float fogDensity = 0.00006;
    PREC float heightScale = exp(-max(terrainHeight, 0.0) * 0.002);
    PREC float fogAmount = 1.0 - exp(-camDist2D * fogDensity * heightScale);
    fogAmount = clamp(fogAmount, 0.0, 0.70);
    if (surfaceDebugMode == 4) {
        FragColor = vec4(vec3(1.0 - fogAmount), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
    PREC vec3 fogCol = vec3(0.58, 0.65, 0.75);
    // OVERLAY-CEMENT-ADOPT-1 — THE DOMINANT SEAM TERM. terrain_lod_chunk.frag
    // applies NO distance fog at all (verified: its only in-shader haze is the
    // map-edge sky blend at :2172); distance fog for BOTH layers is applied
    // later, in screen space, by edge_fog/fog_oob. So an overlay fragment is
    // DOUBLE-FOGGED relative to the terrain fragment beside it, and because the
    // fog colour is a light blue-grey the overlay drifts lighter and bluer with
    // distance. That is why the measured seam step grows from -4.95 L at ground
    // level to -16.93 L at mid altitude, and why the overlay pixel in the
    // RenderDoc capture is blue-dominant while the terrain pixel is not.
    // On cement, skip it and let the shared screen-space fog do the job once.
    if (!ovCementTile)
        c.rgb = mix(c.rgb, fogCol, fogAmount);

    // NIGHT-LIGHT-POOLS-2b: additive light pool AFTER the distance-fog mix —
    // the chunk terrain applies NO distance fog to its pool term, so adding
    // pre-fog here would leave the decal darker than the glowing terrain
    // around it (the reported "dark band crossing glowing grass").
    if (u_numPoolLights > 0)
        c.rgb += overlayPoolLight(WorldPos);

    // PAD-AREA-GLOW-1: same additive pad-area night lift as the chunk terrain
    // (post-fog like the pool term, for the same dark-band reason). Guarded by
    // u_padGlowMapSide so the cement SSBO is never read when the chunk grid is
    // absent. Day / gate-off uploads u_padGlow = 0 -> byte-identical.
    if (u_padGlowMapSide > 0 && dot(u_padGlow, u_padGlow) > 0.0)
        c.rgb += u_padGlow * padGlowMaskOv(WorldPos.xy);

    // Map-edge haze: same ramp as gos_terrain.frag. Alpha cement overlay tiles
    // are emitted well past the playable boundary on some missions and sample
    // magenta "no-data" colormap pixels. Fade them to sky across the last
    // ~one-tile band so they match the main terrain's edge behaviour.
    if (mapHalfExtent > 0.0) {
        PREC vec3 edgeSkyCol = vec3(0.58, 0.65, 0.75);
        PREC float chebDist  = max(abs(WorldPos.x), abs(WorldPos.y));
        PREC float edgeStart = mapHalfExtent - 256.0;
        PREC float edgeEnd   = mapHalfExtent - 32.0;
        PREC float edgeHaze  = smoothstep(edgeStart, edgeEnd, chebDist);
        c.rgb = mix(c.rgb, edgeSkyCol, edgeHaze);
    }

    FragColor = c;

#ifdef MRT_ENABLED
    // Overlay handles its own shadow inline (cloud + static + dynamic above);
    // opt out of post-shadow to avoid double-shadowing.
    GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
}
