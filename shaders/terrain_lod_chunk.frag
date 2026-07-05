// Phase 5: LOD-band debug visualization.
// u_lodStep ∈ {1,2,4,5,10,20} corresponding to LOD levels 0-5.
// Fine (green) -> coarse (dark red). Elevation modulates brightness.
// Phase 6: skirts are darkened (50%) for debug visibility when u_skirtDepth > 0.
// Phase 7.5: u_forceColor=1 enables neon palette — unmistakable proof chunk renderer is active.

// Phase 10 Step 1c: shadows. shadow.hglsl declares shadowMap/lightSpaceMatrix/
// enableShadows/shadowSoftness + the dynamic equivalents and provides calcShadow/
// calcDynamicShadow (Poisson PCF). The chunk DRIVER must bind those uniforms (same
// as the legacy terrain draw) or enableShadows reads 0 -> calcShadow returns 1.0.
#include <include/shadow.hglsl>
#define PREC highp                // noise.hglsl uses PREC (legacy frag defines it)
#include <include/noise.hglsl>   // fbm() for the colour break-up (matches legacy)
#include <include/edge_haze.hglsl> // TERRAIN-EDGE-FEATHER-1: edgeHazeAmount + EDGE_HAZE_SKY (legacy parity)

in vec3  v_worldPos;
in float v_terrainType;       // Step 5b: interpolated per-vertex terrainType (concrete)
uniform int   u_lodStep;
uniform int   u_pathTint;  // MC2_SHADER_PATH_TINT: 1 = solid signature colour (debug); 0 = normal

// Step 5c: cement catalog atlas (legacy tex3). Concrete tiles sample this instead
// of the colormap. Same UV math as gos_terrain.frag.
uniform sampler2D  u_cementAtlas;
uniform int        u_useCement;
uniform int        u_cementGridSide;
uniform float      u_cementWUPT;   // world units per cement tile (= 128)
// CEMENT-DIAG-CONNECT-1 (gate MC2_TERRAIN_CEMENT_DIAG_CONNECT): 0 = off (byte-identical).
// When non-zero, a non-cement tile fills its corner quadrant toward any diagonally
// adjacent SOLID cement tile (hard quadrant cut, no feather).
uniform int        u_cementDiagConnect;
// Stage B: transition mask array (14 layers R8, unit 11).
uniform sampler2DArray u_transitionMaskArray;
uniform int            u_useTransitionMask;
uniform float u_skirtDepth;  // Phase 6: >0 when drawing a skirt strip
uniform int   u_forceColor;  // Phase 7.5: 1 = neon debug palette; 0 = colormap

// Phase 10 (Step 1a): production base color from the merged colormap atlas
// (tex1 in legacy gos_terrain.frag). Same atlas-UV reconstruction as the
// legacy useAtlasColormap path: atlas-absolute UV from world position.
uniform sampler2D u_colormap;
uniform float u_atlasTopLeftX;            // = Terrain::mapTopLeft3d.x
uniform float u_atlasTopLeftY;            // = Terrain::mapTopLeft3d.y
uniform float u_atlasOneOverWorldUnits;   // = Terrain::oneOverWorldUnitsMapSide

// TERRAIN-CONTROLMAP-SAMPLE-1: authored override control map (unit 12). RGBA =
// rock/grass/dirt/concrete weights, same semantics as chunkColorWeights() output.
// v1 = OVERRIDE-ONLY PASSTHROUGH: only takes effect when a sidecar was loaded at
// mission load AND the gate is on (MC2_TERRAIN_CONTROLMAP). Gate OFF -> driver
// uploads u_useControlMap=0 -> this branch never taken -> byte-identical to the
// pre-slice classifier path (chunkColorWeights(base)).
uniform sampler2D u_controlMap;
uniform int       u_useControlMap;

// TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar
// (unit TERRAIN_OVERLAY_SIDECAR_TEXUNIT). RGB = pre-tinted cement/overlay
// diffuse, A = coverage/edge alpha. Sampled by WORLD XY via u_overlayBounds
// -- NOT the 128wu cementWordsF tile grid -- so it is off-grid-correct.
// u_overlayBounds = (topLeftX, topLeftY, sizeX, sizeY): topLeftX = MIN world X
// (west edge), topLeftY = MAX world Y (north/top edge) -- SAME convention as
// the colormap atlas uniforms (u_atlasTopLeftX/Y just above: uv.y = (topLeftY
// - worldY) * oneOverSize), so PNG row 0 (top of image) == north edge, no
// vertical flip, matching control_map_tool.py's documented row-0 convention.
// v1 = ADDITIVE: legacy cement-word composite + separate overlay pass stay
// verbatim; when a sidecar is loaded AND the gate is on (MC2_TERRAIN_OVERLAY_V2),
// this composites OVER the legacy cement result using its alpha. Gate OFF or
// no sidecar -> u_useOverlaySidecar=0 -> this branch never taken ->
// byte-identical to the legacy composite.
uniform sampler2D u_overlaySidecar;
uniform int       u_useOverlaySidecar;
uniform vec4      u_overlayBounds;  // topLeftX, topLeftY(=maxY), sizeX, sizeY (world units)

// TERRAIN-SHORELINE-V3: band PLACEMENT is now driven by ELEVATION
// (v_worldPos.z - u_waterElevation), not by the world-XY mask. Root cause of
// v1/v2 zigzag+float-uphill: the mask EDT was cooked against a coarse or
// smoothed height SOURCE that never exactly matches the RENDERED (bilinear-
// interpolated) waterline, so a horizontal-distance mask either staircases
// (faithful coarse source) or floats up-slope (smoothed source). Elevation
// hugs the drawn waterline BY CONSTRUCTION -- v_worldPos.z is the same
// interpolated height the rasterizer produced, so "am I within wetHeight of
// the water surface" is exact at every fragment regardless of LOD/slope.
// u_useShorelineMask==0 (gate OFF) -> the whole shoreline block below is
// skipped -> byte-identical to the pre-slice composite (the legacy screen
// runShoreline() pass stays active in that case; the C++ driver suppresses
// it only when this gate is genuinely active -- see gos_postprocess.cpp
// runShoreline()). NOTE: despite the name, u_useShorelineMask now gates the
// elevation bands themselves; the mask (u_shorelineMask/u_hasShorelineMask)
// is an OPTIONAL modulator applied on top (wide-beach falloff / basin
// exclusion) -- absent mask still produces full elevation bands.
uniform int       u_useShorelineMask;      // 1 = elevation bands active (MC2_TERRAIN_SHORELINE on)
uniform float     u_waterElevation;        // Terrain::waterElevation (world units, same as water fast path)
uniform sampler2D u_shorelineMask;         // OPTIONAL modulator: R=signed dist, G=wet, B=foam, A=valid
uniform int       u_hasShorelineMask;      // 1 = sidecar loaded -> apply modulator; 0 -> pure elevation bands
uniform vec4      u_shorelineBounds;       // topLeftX, topLeftY(=maxY), sizeX, sizeY (world units) -- for the modulator sample
uniform float     u_shaderTime;      // f(worldPos,time)-only clock for foam animation (NOT camera)
// TERRAIN-SHORELINE-MASK-1 (visual-quality pass): user-tunable overall
// strength knobs (MC2_TERRAIN_SHORELINE_STRENGTH / _FOAM env vars, C++ side
// clamps to [0,2], default 1.0 = the authored look below). Kept separate from
// the mask's own G/B weights so art can dial intensity without a re-cook.
uniform float     u_shorelineStrength;     // wet/damp darken multiplier
uniform float     u_shorelineFoamStrength; // foam rim multiplier
// TERRAIN-SHORELINE-V3 (horizontal-run fix): band widths, HORIZONTAL world-unit
// runs from the drawn waterline (main() converts vertical rise -> horizontal
// run via the macro slope). wetHeight is the outer wet/damp lobe; foamHeight
// is the narrower bright rim hugging the exact waterline. The c1593a1f
// horizontal conversion kept the old VERTICAL defaults (3.0/1.2wu) as runs --
// ~1m of band, invisible at RTS zoom. Horizontal-native defaults: wet 16wu,
// foam 5wu (MC2_TERRAIN_SHORELINE_WET_RUN / _FOAM_RUN; legacy _HEIGHT aliased).
uniform float     u_shorelineWetHeight;    // default 16.0 wu horizontal run
uniform float     u_shorelineFoamHeight;   // default 5.0 wu horizontal run
// TERRAIN-SHORELINE-V4-STYLE (zigzag fix): STATIC world-XY fbm jitter (wu,
// horizontal) added to the band's distance-from-waterline before the lobes
// are shaped. The drawn waterline on the coarse/displaced mesh is a polyline
// of long straight segments (the "diamond" zigzag); an un-jittered band traces
// it faithfully, so the foam rim reads as a hard zigzag edge. Signed noise
// breaks that contour into organic wisp clusters that cross the line
// irregularly. f(worldPos) ONLY -- no time (placement must not swim), no
// camera (advisor ruling). 0 = exact V3 contour behavior.
uniform float     u_shorelineEdgeJitter;   // default 4.0 wu (MC2_TERRAIN_SHORELINE_EDGE_JITTER)

uniform vec4  terrainLightDir;            // Phase 10 Step 1b: sun dir (same uniform as legacy)
uniform int   u_shadowTier;               // Slice B: per-chunk shadow tier (0=high,1=low,2=static,3=none)
uniform int   u_diag;                     // Bisection bitmask (MC2_TERRAIN_LOD_CHUNK_DIAG):
                                          //   1  = do NOT write GBuffer1
                                          //   2  = no depth fudge (raw gl_FragCoord.z)
                                          //   4  = no lighting (colormap only)
                                          //   8  = no shadows (skip calcShadow)
                                          //   16 = flat per-triangle normal (old dFdx)
                                          //   32 = no material detail normals (Step 5a)
                                          //   64 = viz raw matNormalArray rock sample
                                          //  128 = viz v_terrainType (grey + red=concrete)
                                          // 1024 = viz control-map weights (matWeights.rgb,
                                          //        after selection): TERRAIN-CONTROLMAP-SAMPLE-1
                                          // 2048 = viz shoreline mask channels (R=dist,G=wet,
                                          //        B=foam as RGB): TERRAIN-SHORELINE-MASK-1
                                          // 4096 = viz POM UV-offset heat (blue=0 .. red=scale):
                                          //        TERRAIN-CHUNK-POM-1
                                          // 8192 = viz frag->camera dir in MC2 world (R=east,
                                          //        G=north remapped *0.5+0.5, B=up): POM view-
                                          //        vector swizzle oracle (ruling R1)
                                          // Exact-value escapes (not bitmask): 40 shadow-tier
                                          // tint (Slice B); 41 LOD-band tint; 42 geomorph
                                          // morph-factor heat (TERRAIN-LOD-GEOMORPH-1)

// TERRAIN-LOD-GEOMORPH-1: per-block geomorph factor (0 = own band, 1 = parent
// band). Same program-level uniform the vert consumes; read here ONLY by the
// u_diag==42 heat view.
uniform float u_morphFactor;

// LIGHTING-DEBUG-VIEWS-1A-CHUNK: unified lighting debug channel, SAME enum as
// static_prop / gos_terrain.frag. Separate from u_diag (bitmask) to avoid
// mis-triggering bits. 0 = OFF (default; driver uploads 0 unless
// MC2_LIGHTING_DEBUG_VIEW selects a channel). 40 albedo, 41 normal, 42 sun,
// 43 ambient/hemi, 44 shadow, 45 final (falls through), 46 overbright.
uniform int   u_lightingDebugView;

// Step 1c-fix: SMOOTH per-pixel normal from the heightfield. The frag reads the
// SAME height SSBO the vert uses (binding 23) and takes a bilinear central
// difference, so even coarse LOD triangles shade smoothly instead of as flat
// faceted wedges (the "atrocious cliff" artifact). u_mapSide/u_halfMap are the
// SAME linked-program uniforms the vert already receives from the driver.
uniform int   u_mapSide;
uniform float u_halfMap;
layout(binding = 23, std430) readonly buffer TerrainHeightBufFrag {
    float heightsF[];
};
// Step 5c: per-tile cement words, read by WORLD TILE in the frag (LOD-independent
// -> continuous runways at any LOD; coarse geometry is fine since cement is flat).
layout(binding = 25, std430) readonly buffer TerrainCementBufFrag {
    uint cementWordsF[];
};

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 GBuffer1;   // shadow-handled flat-up (terrain MRT composite)

// Reverse-Z terrain depth bias (net -0.004, matching the legacy thin path) is now
// applied PRE-DIVIDE in terrain_lod_chunk.vert (clip.z += 2*FUDGE*clip.w), NOT via
// gl_FragDepth here. Writing gl_FragDepth disabled early-Z/Hi-Z on AMD and caused
// decal tearing at the cement boundary under camera motion. See the vert.

float heightAtCell(int cx, int cy) {
    cx = clamp(cx, 0, u_mapSide - 1);
    cy = clamp(cy, 0, u_mapSide - 1);
    return heightsF[cx + cy * u_mapSide];
}

// Bilinear height at a continuous (col,row) — matches the legacy LINEAR-filtered
// terrainHeightTex so the central difference is continuous (no per-cell terrace).
float heightBilinear(float fx, float fy) {
    int   x0 = int(floor(fx)), y0 = int(floor(fy));
    float tx = fx - float(x0), ty = fy - float(y0);
    float h00 = heightAtCell(x0,     y0);
    float h10 = heightAtCell(x0 + 1, y0);
    float h01 = heightAtCell(x0,     y0 + 1);
    float h11 = heightAtCell(x0 + 1, y0 + 1);
    return mix(mix(h00, h10, tx), mix(h01, h11, tx), ty);
}

// Smooth world-space normal from the heightfield (central difference). World
// convention matches the vert: worldX = col*128 - halfMap, worldY = halfMap -
// row*128 (row increases as Y decreases -> dh/dy = -dh/drow). Returns +Z up.
vec3 smoothTerrainNormal(vec2 worldXY) {
    float colF = (worldXY.x + u_halfMap) / 128.0;
    float rowF = (u_halfMap - worldXY.y) / 128.0;
    float span = 2.0 * 128.0;
    float dhdx   = (heightBilinear(colF + 1.0, rowF) - heightBilinear(colF - 1.0, rowF)) / span;
    float dhdrow = (heightBilinear(colF, rowF + 1.0) - heightBilinear(colF, rowF - 1.0)) / span;
    float dhdy   = -dhdrow;
    return normalize(vec3(-dhdx, -dhdy, 1.0));
}

// Phase 10 Step 5a: detail material normals. The legacy terrain blends 5 per-
// material tangent-space normal maps (rock/grass/dirt/concrete/snow) chosen by
// colormap-derived weights -> high-frequency surface relief the flat colormap
// lacks. Port the COLOR-weight + normal-blend core (POM / anti-tiling / per-cell
// TerrainType concrete deferred). Tunables hardcoded to the legacy CPU defaults;
// wire to the ImGui uniforms in a later pass. u_diag&32 disables (A/B).
#include <include/terrain_mat_layers.hglsl>
uniform sampler2DArray matNormalArray;

// Live tunables — SAME uniform names + values as the legacy terrain (driver
// uploads them from the gosRenderer members the ImGui terrain panel edits), so
// the sliders (per-material tiling, normal boost, class thresholds, detail
// tiling/strength) drive the chunk path too.
uniform vec4  terrainClassGrass;    // (gMinusRLo, gMinusRHi, gBrightLo, gBrightHi)
uniform vec4  terrainClassDirt;     // (rMinusGLo, rMinusGHi, rBrightLo, rBrightHi)
uniform vec4  matTiling;            // rock, grass, dirt, concrete
uniform vec4  matNormalBoost;       // rock, grass, dirt, concrete
uniform float matTilingSnow;
uniform vec4  detailNormalTiling;   // .x = base tiling multiplier
uniform vec4  detailNormalStrength; // .x = overall detail-normal strength
// Colour mapping (TERRAIN-TINT-UI-1): the colormap is mixed toward per-material
// tints. Same uniforms/values as legacy gos_terrain.frag.
uniform vec3  tintRock;             // default (0.36, 0.37, 0.40)
uniform vec3  tintGrass;            // default (0.35, 0.42, 0.25)
uniform vec3  tintDirt;             // default (0.48, 0.42, 0.33)
// TERRAIN-MATERIAL-LIB-1: these two were frag-literal consts; promoted to
// uniforms so terrain_materials.json can cover them. Driver always uploads
// them (no gate); default values are the EXACT former literals, so behavior
// is unchanged unless the JSON edits them.
uniform vec3  tintConcrete;         // default (0.55, 0.53, 0.50)
uniform vec3  tintSnow;             // default (0.75, 0.78, 0.84)
uniform float tintStrengthScale;   // 0 = colormap passthrough, 1 = full tint
// TERRAIN-CONTROLMAP-ALBEDO-1: lifts tintStrength toward 1.0 so authored
// matWeights (control map) can fully repaint the albedo instead of being
// capped at the classifier-era 0.18-0.50 (0.85 under snow) ceiling. 0.0 =
// exact current expression (byte-identical); 1.0 = baseColor==materialTint
// (colormap fully replaced by weight-composed per-layer tints).
uniform float u_controlAlbedoStrength; // default 0.0 (gate OFF -> identity)
uniform float snowBrightnessDampen; // <1 darkens detected snow (snowWeight-gated); default 0.78
// TERRAIN-MATERIAL-LIB-1: per-layer roughness/AO scalars (rock,grass,dirt,concrete).
// Neutral (1,1,1,1) defaults. Only consumed when u_useMaterialLib != 0 -- gate
// OFF -> driver uploads u_useMaterialLib=0 -> branch never taken -> byte-identical.
uniform vec4  matRoughness;
uniform vec4  matAO;
uniform int   u_useMaterialLib;
// TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo array (BC7 sRGB KTX2s from
// data/terrain_layers/<channel>_albedo.ktx2; layer order = MAT_LAYER_* 0..4 +
// MAT_LAYER_MARBLE_CLIFF at 5). Gate MC2_TERRAIN_MATERIAL_TEXTURES default OFF
// -> driver uploads u_useMatAlbedo=0 -> the legacy colormap-tint composition
// below runs VERBATIM (byte-identical). ON: real material albedo replaces the
// flat per-layer tints while the colormap stays the MACRO tint (2x multiply
// keeps burn-in shading + per-mission colour); u_matAlbedoStrength (JSON key
// matAlbedoStrength / env MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH, default 0.7)
// mixes legacy-tinted vs textured composite. sRGB decode is the sampler's job
// (array is GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM -- the BC7 sRGB audit finding).
uniform sampler2DArray u_matAlbedoArray;
uniform int   u_useMatAlbedo;       // 0 = legacy tint path verbatim
uniform float u_matAlbedoStrength;  // 0..1 mix toward textured composite

// Remaining legacy tunables (copied with legacy defaults; driver replicates the
// env gates so default == legacy default). cellBombParams is a DEAD uniform in
// legacy (no shader consumer) -> not wired.
uniform float terrainLightingV1Strength;       // hemisphere ambient; 0 = off (env-gated default off)
uniform float terrainLightingV2ShadowFillFloor;// shadow-aware fill floor; 1 = no influence
uniform float u_terrainCliffShadowFloor;// CLIFF SHADOW FLOOR: lifts steep faces off near-black; 0 = byte-identical
uniform float terrainNormalsFromHeightStrength;// macro-slope strength scalar (default 1.0)
uniform int   useRockSlopeBias;                 // TERRAIN-SLOPE-BIAS-VISUAL-1: 0=off (byte-identical)
uniform float rockSlopeBiasStrength;            // rock-weight bias on steep slopes (default 1.0)
uniform int   useTriplanarCliff;                // TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1: 0=off (byte-identical)
uniform float cliffTriplanarStrength;           // triplanar rock normal/relief strength (default 1.0)
// TERRAIN-CLIFF-HEIGHT-NORMAL-1 (gate MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH,
// EnvVarKind::Trace, default 2.0): the marble_cliff RGB normal (mat5 .rgb) is a
// smooth marble microsurface — it barely tilts N, so the RICH cooked cliff
// DISPLACEMENT (mat5 layer-5 ALPHA, high-contrast rock relief) never catches
// light and the lit cliff reads FLAT. This derives a SHADING normal from the
// HEIGHT GRADIENT of that displacement (triplanar, forward-difference), and
// mixes triTilt strongly toward it so the real rock structure lights up. Only
// consumed inside the useTriplanarCliff block; strength 0.0 -> pure rgb-normal
// behaviour == TRIPLANAR-1 (the gate-OFF byte-identical path is useTriplanarCliff==0).
uniform float cliffHeightNormalStrength;         // 0=rgb-normal only; default 2.0
// TERRAIN-CLIFF-DEBUG (gate MC2_TERRAIN_CLIFF_DEBUG, EnvVarKind::Trace): bounded
// debug-viz to confirm the triplanar/POM cliff material path executes+contributes
// on a given cliff. 0 = OFF (default, byte-identical). Non-zero writes directly to
// the final color and returns early:
//   1 = cliffBlend grayscale (steep-face test firing? black=0 .. white=1)
//   2 = branch-taken coverage (green = useTriplanarCliff block entered, else red)
//   3 = POM UV-offset magnitude as color (parallax march producing non-zero offset?)
//   4 = sampled mat5 (MAT_LAYER_MARBLE_CLIFF) normal-array ALPHA grayscale (cooked
//       displacement actually bound/sampled? black/uniform = blank/default)
uniform int   u_cliffDebug;
// TERRAIN-CLIFF-POM-1: triplanar Parallax Occlusion Mapping on cliff faces
// (gate MC2_TERRAIN_CLIFF_POM). .x=gate(0/1), .y=depth (world-unit height scale
// relative to the CLIFF tiling ts=256), .z=max march steps. Reuses `cameraPos`
// for the world-space view vector (same MC2 world swizzle as the legacy POM).
// Gated on useTriplanarCliff too (shares the triplanar sample setup); .x=0 ->
// no march -> the triplanar block behaves exactly as TRIPLANAR-1 (byte-identical).
uniform vec4  u_cliffPom;                        // .x=gate, .y=depth(wu), .z=maxSteps, .w=unused
uniform vec4  pomParams;                        // .x=scale(0=off), .y=minLayers, .z=maxLayers
// TERRAIN-CHUNK-POM-1: real view-vector POM (gate MC2_TERRAIN_POM, default OFF).
// u_pomView.x=1 -> the POM march in chunkDetailNormal uses the REAL per-fragment
// tangent-space view vector (distance-faded over .y=NEAR .. .z=FAR world units)
// instead of the legacy faux constant vec3(0.15,0.85,0.15). x=0 -> the legacy
// faux path runs VERBATIM (supervisor ruling: gate-OFF byte-identity INCLUDES
// the faux shear; pomParams is NOT zeroed when the gate is off).
// cameraPos: SAME uniform name + frame as legacy gos_terrain.frag — the
// Stuff/MLR eye (.x=left, .y=elevation, .z=forward). MC2 world = (-x, z, y);
// second live witness: the water reflection frame fix in gameos_graphics.cpp.
uniform vec4  cameraPos;
uniform vec4  u_pomView;                        // .x=gate(0/1), .y=fadeNear(wu), .z=fadeFar(wu), .w=unused
uniform int   g_terrainMaterialProfile;         // 0=legacy, 1=sand(mc2_24 dirt-gate widen)
uniform float macroVariationStrength;           // TERRAIN-MACRO-VARIATION-1: 0=off (byte-identical)
uniform int   u_edgeFeather;                    // TERRAIN-EDGE-FEATHER-1: 0=off (byte-identical)
uniform float u_edgeFeatherStrength;            // edge-haze blend amount (default 1.0)
// Legacy Texcoord is [0,1] per MC2 TILE = MAPCELL_DIM(3) * 128 world units. The
// chunk frag has world coords, so divide by this to get the per-tile UV before
// per-material tiling. (Using /128 = per CELL was ~3x too dense -> sub-pixel
// noise instead of detail.) This is geometry-derived, not a tunable.
const float MAT_WORLD_UNITS_PER_TILE = 768.0;  // 2 tiles/repeat (~half the prior density)

vec4 chunkColorWeights(vec3 color) {
    vec4 w = vec4(0.0);
    float gMinusR = color.g - color.r;
    w.y = smoothstep(terrainClassGrass.x, terrainClassGrass.y, gMinusR)
        * smoothstep(terrainClassGrass.z, terrainClassGrass.w, color.g);
    float rMinusG = color.r - color.g;
    // Material profile 1 (sand, mc2_24) widens the dirt gate (legacy lowers the
    // dirt saturation thresholds). Approximate by lowering the dirt brightness
    // floor so more of the sandy colormap classifies as dirt. Profile 0 = legacy.
    float dirtLo = (g_terrainMaterialProfile == 1) ? terrainClassDirt.z * 0.6 : terrainClassDirt.z;
    float dirtHi = (g_terrainMaterialProfile == 1) ? terrainClassDirt.w * 0.8 : terrainClassDirt.w;
    w.z = smoothstep(terrainClassDirt.x, terrainClassDirt.y, rMinusG)
        * smoothstep(dirtLo, dirtHi, color.r);
    w.x = 1.0 - max(w.y, w.z);   // everything else -> rock
    float isWater = smoothstep(0.0, 0.08, min(color.g, color.b) - color.r);
    w.x += isWater; w.y *= (1.0 - isWater); w.z *= (1.0 - isWater);
    float total = w.x + w.y + w.z + w.w;
    return (total < 0.01) ? vec4(1.0, 0.0, 0.0, 0.0) : w / total;
}

vec3 rgb2hsvChunk(vec3 c) {
    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + 1.0e-10)), d / (q.x + 1.0e-10), q.x);
}

// --- Anti-tiling (legacy sampleAntiTileArr) + POM (parallaxMapping) helpers ---
// Both are near-camera detail effects; gated on the per-layer fwidth (sub-pixel
// at distance -> skip, bounding cost on a default-on renderer). matTiling drives
// the anti-tile cell scale; pomParams.x (0=off) drives POM.
vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}
// UB2-01: explicit gradients (ddx/ddy of the base uv, computed by the caller in
// uniform control flow) so the three samples use textureGrad instead of implicit
// LOD — this fn is called from inside non-uniform weight branches where implicit
// derivatives are undefined (vendor-divergent / NVIDIA). The hash offsets are
// piecewise-constant within a tile, so the base-uv gradient is the correct LOD.
vec4 sampleAntiTileArr(int layer, vec2 uv, float scale, vec2 ddx, vec2 ddy) {
    vec2 off1 = hash22(floor(uv / scale)) * scale;
    vec2 off2 = hash22(floor(uv / scale) + vec2(7.0, 13.0)) * scale;
    vec4 s0 = textureGrad(matNormalArray, vec3(uv,        float(layer)), ddx, ddy);
    vec4 s1 = textureGrad(matNormalArray, vec3(uv + off1, float(layer)), ddx, ddy);
    vec4 s2 = textureGrad(matNormalArray, vec3(uv + off2, float(layer)), ddx, ddy);
    vec2 f = fract(uv / scale);
    float w1 = smoothstep(0.2, 0.5, f.x) * smoothstep(0.2, 0.5, f.y);
    float w2 = smoothstep(0.2, 0.5, 1.0 - f.x) * smoothstep(0.2, 0.5, 1.0 - f.y);
    return (s0 + s1 * w1 + s2 * w2) / (1.0 + w1 + w2);
}
float chunkSampleDisplacement(vec2 uv, vec4 w) {
    float d = 0.0;
    // UB2-01: POM displacement is sampled inside a data-dependent loop (chunkParallax)
    // with per-fragment weight branches → implicit LOD is undefined here. Use
    // textureLod(...,0.0): the displacement heightfield is read at base level
    // (standard for POM), valid in any control flow.
    if (w.x > 0.01) d += w.x * textureLod(matNormalArray, vec3(uv, float(MAT_LAYER_ROCK)),     0.0).a;
    if (w.y > 0.01) d += w.y * textureLod(matNormalArray, vec3(uv, float(MAT_LAYER_GRASS)),    0.0).a;
    if (w.w > 0.01) d += w.w * textureLod(matNormalArray, vec3(uv, float(MAT_LAYER_CONCRETE)), 0.0).a;
    return 1.0 - d;
}
// Parallax occlusion (fixed faux view dir, like legacy). Bounded 16 layers.
vec2 chunkParallax(vec2 uv, float scale, vec4 w) {
    const vec3 viewDirTS = vec3(0.15, 0.85, 0.15);
    float numLayers = mix(pomParams.y, pomParams.z, max(viewDirTS.y, 0.0));
    numLayers = clamp(numLayers, 4.0, 16.0);
    float layerDepth = 1.0 / numLayers;
    float curLayer = 0.0;
    vec2 P = viewDirTS.xz / max(viewDirTS.y, 0.001) * scale;
    vec2 dUV = P / numLayers;
    vec2 curUV = uv;
    float curD = chunkSampleDisplacement(curUV, w);
    for (int i = 0; i < 16; ++i) {
        if (curLayer >= curD) break;
        curUV -= dUV;
        curD = chunkSampleDisplacement(curUV, w);
        curLayer += layerDepth;
    }
    vec2 prevUV = curUV + dUV;
    float after  = curD - curLayer;
    float before = chunkSampleDisplacement(prevUV, w) - curLayer + layerDepth;
    return mix(curUV, prevUV, after / (after - before));
}

// TERRAIN-CHUNK-POM-1: POM march with a REAL view vector (gate-ON path only).
// Deliberately a SEPARATE function from chunkParallax() above: the gate-OFF
// path must stay byte-identical INCLUDING the compiler's const-folding of the
// faux vector, so the legacy body is left untouched. Two gate-ON improvements,
// both matching the dead-frag oracle (gos_terrain.frag parallaxMapping):
//   - numLayers orientation: MORE layers at grazing view — mix(max,min,up) —
//     the legacy chunk mix is inverted (moot under the faux up=0.85 constant).
//   - epsilon denominator guard (AMD yields inf/nan silently, NVIDIA may trap).
// UB2-01: chunkSampleDisplacement uses textureLod(...,0) — valid in the
// data-dependent loop below and in the non-uniform caller branch.
vec2 chunkParallaxView(vec2 uv, float scale, vec4 w, vec3 viewDirTS) {
    float numLayers = mix(pomParams.z, pomParams.y, max(viewDirTS.y, 0.0));
    numLayers = clamp(numLayers, 4.0, 16.0);
    float layerDepth = 1.0 / numLayers;
    float curLayer = 0.0;
    vec2 P = viewDirTS.xz / max(viewDirTS.y, 0.001) * scale;
    vec2 dUV = P / numLayers;
    vec2 curUV = uv;
    float curD = chunkSampleDisplacement(curUV, w);
    for (int i = 0; i < 16; ++i) {   // hard compile-constant cap (recon §3)
        if (curLayer >= curD) break;
        curUV -= dUV;
        curD = chunkSampleDisplacement(curUV, w);
        curLayer += layerDepth;
    }
    vec2 prevUV = curUV + dUV;
    float after  = curD - curLayer;
    float before = chunkSampleDisplacement(prevUV, w) - curLayer + layerDepth;
    return mix(curUV, prevUV, after / max(abs(after - before), 1.0e-6));
}

// TERRAIN-CHUNK-POM-1 debug: |pomOff| of this fragment, written by
// chunkDetailNormal, consumed by the u_diag&4096 heat viz in main().
vec2 g_pomOffDbg = vec2(0.0);

// TERRAIN-CLIFF-DEBUG: set inside the useTriplanarCliff cliff block when it is
// actually entered/executed (branch-taken), and captures the cliff POM UV-offset
// magnitude + the sampled mat5 (MARBLE_CLIFF) displacement alpha for the debug
// early-return in main(). All default-inert (u_cliffDebug!=0 gated).
float g_cliffBranchTaken = 0.0;   // 1.0 iff the cliff triplanar block ran
float g_cliffPomOffMag   = 0.0;   // max per-plane |POM UV offset| this fragment
float g_cliffMat5Alpha   = 0.0;   // triplanar-blended MARBLE_CLIFF disp alpha

// Material weights + snow from the colormap colour (computed once, shared by the
// detail normal AND the colour tint). w = rock/grass/dirt/concrete (sums to 1).
void chunkWeights(vec3 colAvg, out vec4 w, out float snowWeight) {
    w = chunkColorWeights(colAvg);
    vec3 hsv = rgb2hsvChunk(colAvg);
    float snowRaw = smoothstep(0.15, 0.03, hsv.y) * smoothstep(0.42, 0.62, hsv.z);
    snowWeight = smoothstep(0.25, 0.55, snowRaw);
    w *= (1.0 - snowWeight);
    float tot = w.x + w.y + w.z + w.w;
    if (tot > 0.01) w /= tot; else w = vec4(1.0, 0.0, 0.0, 0.0);
}

// Accumulated tangent-space (Z-up) detail normal from precomputed weights.
// TERRAIN-CHUNK-POM-1: macroNz = un-perturbed macro slope Z from the caller
// (ruling R2: POM fades out over the same band the cliff/triplanar blend fades
// in, so near-vertical walls stay triplanar-owned). Only the gate-ON path
// reads it; gate-OFF is byte-identical to the pre-slice 3-arg behavior.
vec3 chunkDetailNormal(vec4 w, float snowWeight, vec2 worldXY, float macroNz) {
    vec2 uv = worldXY * (detailNormalTiling.x / MAT_WORLD_UNITS_PER_TILE);  // per-tile, GL_REPEAT
    // Screen-space derivative AA (legacy fwRock/fwGrass/...): fade a layer to 0
    // as its tiling goes sub-pixel. WITHOUT this, far/zoomed-out detail collapses
    // to a dark-biased mean normal -> uniform darkening with no visible relief.
    // Rock tiling /3 (and normal strength /3 below) — rock detail was too dense/strong.
    vec2 uvRock     = uv * (matTiling.x / 3.0);
    vec2 uvGrass    = uv * matTiling.y;
    vec2 uvDirt     = uv * matTiling.z;
    vec2 uvConcrete = uv * matTiling.w;
    vec2 uvSnow     = uv * matTilingSnow;
    float fwRock     = clamp(1.0 - (length(fwidth(uvRock))     - 0.5) * 2.0, 0.0, 1.0);
    float fwGrass    = clamp(1.0 - (length(fwidth(uvGrass))    - 0.5) * 2.0, 0.0, 1.0);
    float fwDirt     = clamp(1.0 - (length(fwidth(uvDirt))     - 0.5) * 2.0, 0.0, 1.0);
    float fwConcrete = clamp(1.0 - (length(fwidth(uvConcrete)) - 0.5) * 2.0, 0.0, 1.0);
    float fwSnow     = clamp(1.0 - (length(fwidth(uvSnow))     - 0.5) * 2.0, 0.0, 1.0);

    // POM: parallax-offset the material UVs (legacy pomParams.x>0). fwidth-gated
    // (near only) -> bounded cost on a default-on renderer. Off when scale<=0.
    // TERRAIN-CHUNK-POM-1: u_pomView.x>0.5 (MC2_TERRAIN_POM) switches the march
    // to the REAL per-fragment tangent-space view vector with a world-distance
    // fade + slope/cement exclusions. Gate OFF keeps the legacy chunkParallax()
    // call VERBATIM (supervisor ruling: byte-identity includes the faux shear).
    if (pomParams.x > 0.0 && fwRock > 0.4) {
        const vec4 pomScaleMat = vec4(1.0, 1.0, 2.5, 1.0);
        float pomScale = pomParams.x * dot(pomScaleMat, w) * fwRock;
        vec2  pomUV    = uv * dot(matTiling, w);
        vec2  pomOff   = vec2(0.0);
        if (u_pomView.x > 0.5) {
            // cameraPos is the Stuff/MLR eye; MC2 world = (-x, z, y). Same
            // conversion as the live water reflection fix (gameos_graphics.cpp)
            // and the dead-frag oracle comment (gos_terrain.frag:319-325).
            vec3  camW     = vec3(-cameraPos.x, cameraPos.z, cameraPos.y);
            // World-distance fade (the real cost governor, recon §3): camera
            // ground distance + altitude boost, matching the dead frag's LOD
            // distance semantics. Beyond FAR the march is skipped entirely.
            float altBoost = max(camW.z - v_worldPos.z, 0.0) * 0.7;
            float camDist  = distance(v_worldPos.xy, camW.xy) + altBoost;
            float distFade = 1.0 - smoothstep(u_pomView.y, u_pomView.z, camDist);
            // R2: cliffs stay triplanar-owned — fade POM out over the SAME
            // slope band the cliff/triplanar blend fades in (|Nz| 0.85->0.55).
            float slopeFade = 1.0 - smoothstep(0.85, 0.55, abs(macroNz));
            // Cement/concrete excluded (runway slabs + decals must not tear):
            // w is post-cement-mix, so pure cement has w.w=1 -> strength 0.
            float pomStrength = distFade * slopeFade * (1.0 - w.w);
            // LOD belt-and-suspenders: near bands only (LOD0/1), matching the
            // detail-normal LOD fade tiers.
            if (pomStrength > 0.001 && u_lodStep <= 2) {
                vec3 Vw = normalize(camW - v_worldPos);   // fragment -> eye, MC2 world
                // Detail UVs are top-down planar with NO y-flip (uv = worldXY*k,
                // unlike the atlas/cement UVs), so tangent(u)=+east,
                // bitangent(v)=+north, normal=+up. chunkParallaxView expects
                // .xz = UV plane (x->u, z->v) and .y = up:
                vec3 viewDirTS = vec3(Vw.x, Vw.z, Vw.y);
                pomOff = (chunkParallaxView(pomUV, pomScale, w, viewDirTS) - pomUV)
                       * fwRock * pomStrength;
            }
        } else {
            pomOff = (chunkParallax(pomUV, pomScale, w) - pomUV) * fwRock;
        }
        uvRock += pomOff; uvGrass += pomOff; uvDirt += pomOff; uvConcrete += pomOff;
        g_pomOffDbg = pomOff;
    }

    // UB2-01: screen-space gradients of the FINAL (post-POM) per-layer UVs,
    // computed here in uniform control flow. The per-layer sampling below lives
    // inside non-uniform `if (w.* > 0.01)` weight branches; passing these explicit
    // gradients to textureGrad makes the LOD well-defined there (implicit LOD in
    // non-uniform flow is UB — vendor-divergent on NVIDIA). On uniform quads these
    // gradients equal what implicit texture() would derive, so output is unchanged.
    vec2 ddxRock  = dFdx(uvRock),     ddyRock  = dFdy(uvRock);
    vec2 ddxGrass = dFdx(uvGrass),    ddyGrass = dFdy(uvGrass);
    vec2 ddxDirt  = dFdx(uvDirt),     ddyDirt  = dFdy(uvDirt);
    vec2 ddxConc  = dFdx(uvConcrete), ddyConc  = dFdy(uvConcrete);
    vec2 ddxSnow  = dFdx(uvSnow),     ddySnow  = dFdy(uvSnow);

    // Anti-tile scale per material (legacy: tiling>=4 full, <=1 off). Near only.
    float atsRock  = mix(0.0, 3.0, clamp((matTiling.x - 1.0) / 3.0, 0.0, 1.0));
    float atsGrass = mix(0.0, 3.0, clamp((matTiling.y - 1.0) / 3.0, 0.0, 1.0));
    float atsDirt  = mix(0.0, 3.0, clamp((matTiling.z - 1.0) / 3.0, 0.0, 1.0));
    float atsConc  = mix(0.0, 3.0, clamp((matTiling.w - 1.0) / 3.0, 0.0, 1.0));
    bool  antiTile = true;  // gated per-layer by fw* (near) + ats below

    // UB2-01: implicit texture() → textureGrad with the uniform-scope gradients
    // above. The `if (w.* > 0.01)` weight branches stay (perf), but sampling is
    // now LOD-defined inside them.
    vec3 dN = vec3(0.0);
    if (w.x > 0.01) {
        vec4 s = (antiTile && atsRock > 0.01 && fwRock > 0.5)
               ? sampleAntiTileArr(MAT_LAYER_ROCK, uvRock, atsRock, ddxRock, ddyRock)
               : textureGrad(matNormalArray, vec3(uvRock, float(MAT_LAYER_ROCK)), ddxRock, ddyRock);
        dN += w.x * (matNormalBoost.x / 3.0) * fwRock * (s.rgb * 2.0 - 1.0);
    }
    if (w.y > 0.01) {
        vec4 s = (antiTile && atsGrass > 0.01 && fwGrass > 0.5)
               ? sampleAntiTileArr(MAT_LAYER_GRASS, uvGrass, atsGrass, ddxGrass, ddyGrass)
               : textureGrad(matNormalArray, vec3(uvGrass, float(MAT_LAYER_GRASS)), ddxGrass, ddyGrass);
        dN += w.y * matNormalBoost.y * fwGrass * (s.rgb * 2.0 - 1.0);
    }
    if (w.z > 0.01) {
        vec4 s = (antiTile && atsDirt > 0.01 && fwDirt > 0.5)
               ? sampleAntiTileArr(MAT_LAYER_DIRT, uvDirt, atsDirt, ddxDirt, ddyDirt)
               : textureGrad(matNormalArray, vec3(uvDirt, float(MAT_LAYER_DIRT)), ddxDirt, ddyDirt);
        dN += w.z * matNormalBoost.z * fwDirt * (s.rgb * 2.0 - 1.0);
    }
    if (w.w > 0.01) {
        vec4 s = (antiTile && atsConc > 0.01 && fwConcrete > 0.5)
               ? sampleAntiTileArr(MAT_LAYER_CONCRETE, uvConcrete, atsConc, ddxConc, ddyConc)
               : textureGrad(matNormalArray, vec3(uvConcrete, float(MAT_LAYER_CONCRETE)), ddxConc, ddyConc);
        dN += w.w * matNormalBoost.w * fwConcrete * (s.rgb * 2.0 - 1.0);
    }
    if (snowWeight > 0.01) dN += snowWeight * 0.9 * fwSnow *
        (textureGrad(matNormalArray, vec3(uvSnow,  float(MAT_LAYER_SNOW)), ddxSnow, ddySnow).rgb * 2.0 - 1.0);
    return dN;
}

// TERRAIN-CLIFF-POM-1: Parallax Occlusion march in ONE world-axis triplanar
// plane. `uv` is the plane's 2D coordinate (already /ts). `viewPlane` is the
// view direction (fragment->eye, MC2 world) PROJECTED onto that plane's 2D
// basis. `depth` scales the max UV displacement (world-unit height / ts). The
// mat5 alpha carries the REAL marble_cliff displacement (1=peak, 0=deep). We
// march from the surface INTO the rock: at each step step down in height and
// forward along the (inverted) view offset until the ray falls below the
// heightfield, then interpolate the crossing — standard POM. textureLod only
// (loop => no implicit derivatives; UB2 discipline). No gl_FragDepth write.
vec2 cliffParallaxPlane(vec2 uv, vec2 viewPlane, float depth, float steps) {
    if (depth <= 0.0) return uv;
    // more layers at grazing angles (small out-of-plane component -> long march).
    float nSteps = clamp(steps, 8.0, 32.0);
    float layerDepth = 1.0 / nSteps;
    // total UV sweep for a full 0..1 height traversal. Negative view offset so the
    // parallax shifts toward the eye (occlusion by nearer/higher rock).
    vec2 P = -viewPlane * depth;
    vec2 deltaUV = P * layerDepth;
    float curDepth = 0.0;
    vec2  curUV    = uv;
    float curH = 1.0 - textureLod(matNormalArray, vec3(curUV, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
    // bounded loop (compile-constant cap 32); break once the ray dips below rock.
    for (int i = 0; i < 32; ++i) {
        if (float(i) >= nSteps || curDepth >= curH) break;
        curUV   += deltaUV;
        curDepth += layerDepth;
        curH = 1.0 - textureLod(matNormalArray, vec3(curUV, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
    }
    // interpolate between the last two samples where the ray crossed the surface.
    vec2  prevUV = curUV - deltaUV;
    float afterH = curH - curDepth;
    float beforeH = (1.0 - textureLod(matNormalArray, vec3(prevUV, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a)
                    - (curDepth - layerDepth);
    float t = afterH / (afterH - beforeH + 1e-5);
    return mix(curUV, prevUV, clamp(t, 0.0, 1.0));
}

void main() {
    // Depth: NO gl_FragDepth write. The -0.004 net terrain bias is applied
    // PRE-DIVIDE in the vert (clip.z += 2*FUDGE*clip.w). Writing gl_FragDepth here
    // disabled early-Z/Hi-Z on AMD -> decal tearing at the cement boundary under
    // camera motion (greybeard META-FIX; vulkan_aligned_depth_bias_ruling.md).

    // MC2_SHADER_PATH_TINT: solid GREEN so this shader's surfaces are unmistakable.
    if (u_pathTint != 0) {
        fragColor = vec4(0.0, 1.0, 0.0, 1.0);
        GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);   // shadowHandled_flatUp
        return;
    }

    // Slice B debug: per-chunk SHADOW TIER map (MC2_TERRAIN_LOD_CHUNK_DIAG=40).
    // Flat tier color; does NOT alter real shadow sampling (that is Slice C).
    if (u_diag == 40) {
        vec3 tc;
        if      (u_shadowTier == 0) tc = vec3(1.0, 0.0, 0.0);   // high-res dynamic (near) red
        else if (u_shadowTier == 1) tc = vec3(1.0, 1.0, 0.0);   // low-res dynamic (mid)  yellow
        else if (u_shadowTier == 2) tc = vec3(0.0, 0.0, 1.0);   // static-only (far)      blue
        else                        tc = vec3(0.5, 0.5, 0.5);   // none/culled            grey
        fragColor = vec4(tc, 1.0);
        GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // TERRAIN-LOD-GEOMORPH-1 debug: LOD-band tint (MC2_TERRAIN_LOD_CHUNK_DIAG=41).
    // Flat color per band from u_lodStep (recon sec 6 palette, LOD0..5 =
    // green/cyan/blue/yellow/orange/red). Mirrors the ==40 shadow-tier precedent.
    if (u_diag == 41) {
        vec3 bc;
        if      (u_lodStep == 1)  bc = vec3(0.0, 1.0, 0.0);   // LOD0 green
        else if (u_lodStep == 2)  bc = vec3(0.0, 1.0, 1.0);   // LOD1 cyan
        else if (u_lodStep == 4)  bc = vec3(0.0, 0.3, 1.0);   // LOD2 blue
        else if (u_lodStep == 5)  bc = vec3(1.0, 1.0, 0.0);   // LOD3 yellow
        else if (u_lodStep == 10) bc = vec3(1.0, 0.55, 0.0);  // LOD4 orange
        else                      bc = vec3(1.0, 0.0, 0.0);   // LOD5 red
        fragColor = vec4(bc, 1.0);
        GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // TERRAIN-LOD-GEOMORPH-1 debug: morph-factor heat (MC2_TERRAIN_LOD_CHUNK_DIAG=42).
    // Black (m=0, own band) -> red -> yellow -> white (m=1, riding the parent
    // surface). Shows exactly where and how fast transitions slide.
    if (u_diag == 42) {
        float m = clamp(u_morphFactor, 0.0, 1.0);
        vec3 heat = vec3(clamp(m * 3.0, 0.0, 1.0),
                         clamp(m * 3.0 - 1.0, 0.0, 1.0),
                         clamp(m * 3.0 - 2.0, 0.0, 1.0));
        fragColor = vec4(heat, 1.0);
        GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // Phase 7.5 debug: neon LOD-band palette when u_forceColor=1 (launch_lod_*color.bat).
    if (u_forceColor != 0) {
        vec3 fc;
        if      (u_lodStep == 1)  fc = vec3(0.0,  1.0,  0.0);   // LOD0 neon green
        else if (u_lodStep == 2)  fc = vec3(1.0,  1.0,  0.0);   // LOD1 yellow
        else if (u_lodStep == 4)  fc = vec3(1.0,  0.0,  1.0);   // LOD2 magenta
        else if (u_lodStep == 5)  fc = vec3(0.0,  1.0,  1.0);   // LOD3 cyan
        else if (u_lodStep == 10) fc = vec3(1.0,  0.0,  0.0);   // LOD4 red
        else                      fc = vec3(1.0,  1.0,  1.0);   // LOD5 white
        if (u_skirtDepth > 0.0)   fc = vec3(0.0,  0.0,  0.5);   // skirts dark blue
        fragColor = vec4(fc, 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // Production: sample the colormap atlas. Atlas UV in [0,1] across the map.
    // Skirt verts share the edge surface vertex's worldPos.xy, so they sample
    // the same color as the adjacent surface -> seamless production skirts (no
    // debug darken). (Step 1b will add normals-from-height lighting + GBuffer1.)
    vec2 uv;
    uv.x = (v_worldPos.x - u_atlasTopLeftX) * u_atlasOneOverWorldUnits;
    uv.y = (u_atlasTopLeftY - v_worldPos.y) * u_atlasOneOverWorldUnits;
    // 9-tap disc blur for a soft splatted colormap (legacy colAvg style). CMAP_BLUR
    // is an atlas-UV radius — one-line tunable.
    const float CMAP_BLUR = 0.0020;
    const float CMAP_R2   = CMAP_BLUR * 0.707;
    vec3 base = texture(u_colormap, uv).rgb
              + texture(u_colormap, uv + vec2( CMAP_BLUR, 0.0)).rgb
              + texture(u_colormap, uv + vec2(-CMAP_BLUR, 0.0)).rgb
              + texture(u_colormap, uv + vec2(0.0,  CMAP_BLUR)).rgb
              + texture(u_colormap, uv + vec2(0.0, -CMAP_BLUR)).rgb
              + texture(u_colormap, uv + vec2( CMAP_R2,  CMAP_R2)).rgb
              + texture(u_colormap, uv + vec2(-CMAP_R2,  CMAP_R2)).rgb
              + texture(u_colormap, uv + vec2( CMAP_R2, -CMAP_R2)).rgb
              + texture(u_colormap, uv + vec2(-CMAP_R2, -CMAP_R2)).rgb;
    base /= 9.0;

    // Step 5c: cement catalog override.
    // cw bit layout: bit31=VALID, bit30=IS_TRANSITION, bits29:24=maskId, bits15:0=layerIdx.
    bool cementHit = false;
    float cementTransAlpha = 0.0;  // CEMENT-HARD-EDGE-1: neighbor-derived transition coverage
    int  ctX = clamp(int(floor((v_worldPos.x + u_halfMap) / 128.0)), 0, u_mapSide - 1);
    int  ctY = clamp(int(floor((u_halfMap - v_worldPos.y) / 128.0)), 0, u_mapSide - 1);
    uint cw  = cementWordsF[ctX + ctY * u_mapSide];
    if (u_useCement != 0 && (cw & 0x80000000u) != 0u) {
        uint cLayerIdx    = cw & 0xFFFFu;
        bool isTransition = (cw & 0x40000000u) != 0u;
        int  cGridSide = u_cementGridSide;
        if (cGridSide < 1) cGridSide = 1;
        int  cCol = int(cLayerIdx) % cGridSide;
        int  cRow = int(cLayerIdx) / cGridSide;
        vec2 cTileUV  = fract(vec2(v_worldPos.x, -v_worldPos.y) / u_cementWUPT);
        vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cTileUV) / float(cGridSide);
        vec3 cementColor = texture(u_cementAtlas, cAtlasUV).rgb;
        if (isTransition) {
            // CEMENT-HARD-EDGE-1 (default-on; self-disables if u_useTransitionMask==0):
            // Render the SAME solid cement atlas tile as the interior, cut by a HARD
            // mask derived from NEIGHBOR WHOLE-TILE cement state (NOT the wonky maskId).
            // The current tile is cement. Each of the 4 tile corners is "filled" iff
            // ALL 3 neighbor tiles meeting at that corner are cement (VALID bit31 set;
            // solid OR transition both count). cTileUV.x grows with +ctX, cTileUV.y
            // grows with +ctY (same convention as the solid branch / diag-connect block),
            // so corner (u,v): (0,0)=TL[-x,-y], (1,0)=TR[+x,-y], (1,1)=BR[+x,+y],
            // (0,1)=BL[-x,+y]. Bilinear-interpolate the 4 binary corners then HARD step
            // at 0.5 -> exact full / straight half-plane / diagonal corner-cut. No curves.
            if (u_useTransitionMask != 0) {
                // isCementTile(dx,dy): neighbor (ctX+dx, ctY+dy) has VALID cement word.
                // Out-of-bounds => not cement. Macro-style inline to keep 8 samples flat.
                bool nXm = false, nXp = false, nYm = false, nYp = false;
                bool nXmYm = false, nXpYm = false, nXpYp = false, nXmYp = false;
                int xm = ctX - 1, xp = ctX + 1, ym = ctY - 1, yp = ctY + 1;
                if (xm >= 0)                          nXm   = (cementWordsF[xm + ctY * u_mapSide] & 0x80000000u) != 0u;
                if (xp <  u_mapSide)                  nXp   = (cementWordsF[xp + ctY * u_mapSide] & 0x80000000u) != 0u;
                if (ym >= 0)                          nYm   = (cementWordsF[ctX + ym * u_mapSide] & 0x80000000u) != 0u;
                if (yp <  u_mapSide)                  nYp   = (cementWordsF[ctX + yp * u_mapSide] & 0x80000000u) != 0u;
                if (xm >= 0 && ym >= 0)               nXmYm = (cementWordsF[xm + ym * u_mapSide] & 0x80000000u) != 0u;
                if (xp <  u_mapSide && ym >= 0)       nXpYm = (cementWordsF[xp + ym * u_mapSide] & 0x80000000u) != 0u;
                if (xp <  u_mapSide && yp < u_mapSide) nXpYp = (cementWordsF[xp + yp * u_mapSide] & 0x80000000u) != 0u;
                if (xm >= 0 && yp < u_mapSide)        nXmYp = (cementWordsF[xm + yp * u_mapSide] & 0x80000000u) != 0u;
                // Corner filled iff all 3 neighbors meeting at it are cement.
                float cTL = (nXm   && nYm   && nXmYm) ? 1.0 : 0.0;  // UV (0,0)
                float cTR = (nXp   && nYm   && nXpYm) ? 1.0 : 0.0;  // UV (1,0)
                float cBR = (nXp   && nYp   && nXpYp) ? 1.0 : 0.0;  // UV (1,1)
                float cBL = (nXm   && nYp   && nXmYp) ? 1.0 : 0.0;  // UV (0,1)
                float u = cTileUV.x, v = cTileUV.y;
                // CEMENT-HARD-EDGE-1: explicit marching-squares half-plane tests.
                // Bilinear+step gives a CURVED (hyperbolic) cut for 1- and 3-corner
                // configs; instead branch on the 4-bit corner config and use straight
                // half-plane comparisons so every edge is a 45-degree diagonal or an
                // axis-aligned straight split (hard 0/1, never a curve).
                // Bit packing: cTL=8, cTR=4, cBR=2, cBL=1.
                int cfg = (cTL > 0.5 ? 8 : 0)
                        | (cTR > 0.5 ? 4 : 0)
                        | (cBR > 0.5 ? 2 : 0)
                        | (cBL > 0.5 ? 1 : 0);
                float alpha = 0.0;
                // No orthogonal "interior fill" override: a boundary tile whose only
                // terrain neighbor is DIAGONAL must still take its corner cut. The
                // triple cfg below trims that 1/8 outer-corner triangle to the tile-edge
                // MIDPOINTS, so the cement edge stays on the true outline instead of
                // poking the tile's outer VERTEX past it. Truly interior tiles (all 8
                // neighbors cement) are cfg==15 -> full, so nothing drops.
                if (cfg == 0) {
                    alpha = 0.0;
                } else if (cfg == 15) {
                    alpha = 1.0;
                // --- single corner: small triangle at that corner ---
                } else if (cfg == 8) {        // cTL only
                    alpha = (u + v < 0.5) ? 1.0 : 0.0;
                } else if (cfg == 4) {        // cTR only
                    alpha = ((1.0 - u) + v < 0.5) ? 1.0 : 0.0;
                } else if (cfg == 2) {        // cBR only
                    alpha = ((1.0 - u) + (1.0 - v) < 0.5) ? 1.0 : 0.0;
                } else if (cfg == 1) {        // cBL only
                    alpha = (u + (1.0 - v) < 0.5) ? 1.0 : 0.0;
                // --- triple: fill all but the missing corner's triangle ---
                // --- triple: fill all but the missing corner's triangle ---
                } else if (cfg == 7) {        // missing cTL
                    alpha = (u + v >= 0.5) ? 1.0 : 0.0;
                } else if (cfg == 11) {       // missing cTR
                    alpha = ((1.0 - u) + v >= 0.5) ? 1.0 : 0.0;
                } else if (cfg == 13) {       // missing cBR
                    alpha = ((1.0 - u) + (1.0 - v) >= 0.5) ? 1.0 : 0.0;
                } else if (cfg == 14) {       // missing cBL
                    alpha = (u + (1.0 - v) >= 0.5) ? 1.0 : 0.0;
                // --- two adjacent: straight half ---
                } else if (cfg == 12) {       // cTL+cTR, top
                    alpha = (v < 0.5) ? 1.0 : 0.0;
                } else if (cfg == 6) {        // cTR+cBR, right
                    alpha = (u >= 0.5) ? 1.0 : 0.0;
                } else if (cfg == 3) {        // cBR+cBL, bottom
                    alpha = (v >= 0.5) ? 1.0 : 0.0;
                } else if (cfg == 9) {        // cBL+cTL, left
                    alpha = (u < 0.5) ? 1.0 : 0.0;
                // --- two diagonal: opposite triangles (saddle) ---
                } else if (cfg == 10) {       // cTL+cBR
                    alpha = ((u + v < 0.5) || ((1.0 - u) + (1.0 - v) < 0.5)) ? 1.0 : 0.0;
                } else if (cfg == 5) {        // cTR+cBL
                    alpha = (((1.0 - u) + v < 0.5) || (u + (1.0 - v) < 0.5)) ? 1.0 : 0.0;
                }
                base = mix(base, cementColor, alpha);
                cementTransAlpha = alpha;
                if (alpha > 0.5) cementHit = true;
            }
        } else {
            base = cementColor;
            cementHit = true;
        }
    }

    // CEMENT-DIAG-CONNECT-1 (gate u_cementDiagConnect): when the current tile is NOT
    // a solid cement hit, fill the corner quadrant of this tile toward any diagonally
    // adjacent SOLID cement tile so two diagonal cement squares merge across the shared
    // corner. Hard quadrant cut (no feather) preserves blocky cement edges. Transition
    // (alpha bit30) neighbors are NOT bridged. Off => this whole block is skipped.
    if (u_cementDiagConnect != 0 && u_useCement != 0 && !cementHit) {
        // Sub-tile position; cTileUV.x grows with +ctX, cTileUV.y grows with +ctY.
        vec2 dTileUV = fract(vec2(v_worldPos.x, -v_worldPos.y) / u_cementWUPT);
        int  dGridSide = u_cementGridSide;
        if (dGridSide < 1) dGridSide = 1;
        // Pick the diagonal neighbor toward the quadrant this fragment lies in.
        int dnX = (dTileUV.x > 0.5) ? (ctX + 1) : (ctX - 1);
        int dnY = (dTileUV.y > 0.5) ? (ctY + 1) : (ctY - 1);
        if (dnX >= 0 && dnX < u_mapSide && dnY >= 0 && dnY < u_mapSide) {
            uint dnw = cementWordsF[dnX + dnY * u_mapSide];
            bool dnValid      = (dnw & 0x80000000u) != 0u;
            bool dnTransition = (dnw & 0x40000000u) != 0u;
            if (dnValid && !dnTransition) {
                uint dLayerIdx = dnw & 0xFFFFu;
                int  dCol = int(dLayerIdx) % dGridSide;
                int  dRow = int(dLayerIdx) / dGridSide;
                // Use this tile's own sub-tile UV so the fill aligns with the grid.
                vec2 dAtlasUV = (vec2(float(dCol), float(dRow)) + dTileUV) / float(dGridSide);
                base = texture(u_cementAtlas, dAtlasUV).rgb;
                cementHit = true;
            }
        }
    }

    // TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar
    // composite. ADDITIVE v1 -- runs AFTER the legacy cement-word + diag-connect
    // composite above (which stays verbatim), so gate-OFF or no-sidecar is
    // byte-identical by construction (uniform else-branch: this whole block is
    // skipped). World-XY sample (not the 128wu tile grid) is the off-grid fix;
    // rides the terrain frag's displaced surface Z automatically (no z-fight
    // vs the old flat-triangle overlay pass on sloped/displaced terrain).
    // TERRAIN-OVERLAY-V2-SUPPRESS-1: coverage of the authored overlay sidecar at
    // this pixel (0 = none, 1 = fully authored). Used below to stop the generic
    // concrete material tint from being re-applied OVER the authored overlay
    // (the "double cement": overlay paints authored cement/road, then cementHit
    // forces matWeights=concrete -> materialTint=tintConcrete muddies it again).
    // Stays 0 when OVERLAY_V2 is off -> byte-identical.
    float overlayCoverage = 0.0;
    if (u_useOverlaySidecar != 0) {
        vec2 ovUV;
        ovUV.x = (v_worldPos.x - u_overlayBounds.x) / max(u_overlayBounds.z, 1e-5);
        ovUV.y = (u_overlayBounds.y - v_worldPos.y) / max(u_overlayBounds.w, 1e-5);
        if (ovUV.x >= 0.0 && ovUV.x <= 1.0 && ovUV.y >= 0.0 && ovUV.y <= 1.0) {
            vec4 ov = texture(u_overlaySidecar, ovUV);
            base = mix(base, ov.rgb, ov.a);
            overlayCoverage = ov.a;
            if (ov.a > 0.5) cementHit = true;
        }
    }

    // DIAG bit 64: visualize the raw matNormalArray ROCK-layer sample as color
    // (no lighting). Bluish normal-map texture with visible detail => sampling
    // works (then the fault is the combine/strength). Flat/black/gray uniform =>
    // the array sample itself is broken (layer count, format, or coords).
    if ((u_diag & 64) != 0) {
        vec2 uvDbg = v_worldPos.xy * (detailNormalTiling.x / MAT_WORLD_UNITS_PER_TILE) * matTiling.x;
        fragColor = vec4(texture(matNormalArray, vec3(uvDbg, float(MAT_LAYER_ROCK))).rgb, 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }
    // DIAG bit 128: visualize v_terrainType (material index 0-3) as grayscale +
    // RED where it would be treated as concrete (smoothstep(2,3)). Mostly-black
    // with red only on cement = correct. Uniform grey / all-red = SSBO garbage.
    if ((u_diag & 128) != 0) {
        float g = clamp(v_terrainType / 3.0, 0.0, 1.0);
        float pc = smoothstep(2.0, 3.0, v_terrainType);
        fragColor = vec4(g + pc, g * (1.0 - pc), g * (1.0 - pc), 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // Step 1b: geometric normal from world-pos screen derivatives (faceted per
    // triangle; gives relief lighting without sampling the height SSBO in the
    // frag). Terrain world up = +Z (elevation); flip to keep N up-facing.
    // Phase 10.2: SKIRT pixels (u_skirtDepth>0) are vertical seam-fillers — their
    // geometric normal is horizontal, which would shade them as dark walls at LOD
    // edges. Use a flat-up normal so the skirt is lit like the adjacent surface it
    // fills and blends invisibly instead of drawing a dark line.
    // DIAG bit 256: raw colormap sample only (no tint/detail/lighting). Varied
    // colours => colormap fine, muting is in the composition. Uniform/flat =>
    // the colormap sample itself is broken (wrong atlas/UV/over-blur).
    if ((u_diag & 256) != 0) {
        fragColor = vec4(base, 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // Material weights + snow (shared by detail normal AND colour tint).
    vec4  matWeights; float snowWeight;
    if (u_useControlMap != 0) {
        // TERRAIN-CONTROLMAP-SAMPLE-1: authored RGBA override replaces the
        // colormap-colour classifier (chunkColorWeights). Snow stays HSV-
        // derived from the colormap (control map has no snow channel in v1) —
        // reuse the SAME HSV snow term chunkWeights() computes, applied to the
        // overridden weights.
        matWeights = texture(u_controlMap, uv);
        vec3 hsv = rgb2hsvChunk(base);
        float snowRaw = smoothstep(0.15, 0.03, hsv.y) * smoothstep(0.42, 0.62, hsv.z);
        snowWeight = smoothstep(0.25, 0.55, snowRaw);
        matWeights *= (1.0 - snowWeight);
        float tot = matWeights.x + matWeights.y + matWeights.z + matWeights.w;
        if (tot > 0.01) {
            matWeights /= tot;
        } else {
            // TERRAIN-MATTEX-BLEND-1: an EMPTY / unbound control map must NOT
            // collapse the whole surface to all-rock (1,0,0,0). That was the
            // "100% rock, no colormap" bug when u_useControlMap is armed but no
            // valid control-map sidecar is loaded (e.g. after the leaked test
            // maps were quarantined). Fall back to the colormap-colour
            // classifier — the same path used when the control map is off — so
            // terrain reads correctly instead of turning to rock.
            chunkWeights(base, matWeights, snowWeight);
        }
    } else {
        chunkWeights(base, matWeights, snowWeight);
    }

    // DIAG bit 1024: visualize control-map/classified weights (rock=R,
    // grass=G, dirt=B) AFTER selection so authored vs classified are directly
    // comparable by toggling u_useControlMap. TERRAIN-CONTROLMAP-SAMPLE-1.
    if ((u_diag & 1024) != 0) {
        fragColor = vec4(matWeights.rgb, 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // TERRAIN-SLOPE-BIAS-VISUAL-1 (B4a): optionally push material weights toward
    // rock on steep MACRO slopes so the rock detail normal + tint (not just the
    // cliff-block colour darken below) read as a real rock face. Keyed off the
    // un-perturbed macro slope — the detail-perturbed N would fire everywhere
    // (same reason the cliff block uses macroNz). Runs BEFORE the cement mix so
    // concrete/runway still overrides. Default OFF (useRockSlopeBias==0) →
    // byte-identical to the pre-slice path.
    if (useRockSlopeBias != 0) {
        float biasNz = abs(smoothTerrainNormal(v_worldPos.xy).z);
        float steep  = smoothstep(0.85, 0.55, biasNz);   // 0 flat .. 1 cliff (cliff-block band)
        float rb     = clamp(steep * rockSlopeBiasStrength, 0.0, 1.0);
        matWeights.x += rb * (matWeights.y + matWeights.z);  // grass/dirt -> rock
        matWeights.y *= (1.0 - rb);
        matWeights.z *= (1.0 - rb);
        float biasTot = matWeights.x + matWeights.y + matWeights.z + matWeights.w;
        if (biasTot > 0.01) matWeights /= biasTot;
    }

    // Step 5b: concrete/cement. TerrainType ~3 at cement vertices (interpolated,
    // so boundary patches blend). pureConcrete pushes weights fully to concrete
    // (.w), suppresses snow, and (below) restores the authored colormap tone +
    // flattens lighting. Matches legacy gos_terrain.frag:539-548,738,772.
    float pureConcrete       = smoothstep(2.0, 3.0, v_terrainType);
    if (cementHit) pureConcrete = 1.0;  // cement atlas hit -> full concrete (base already set)
    // CEMENT-HARD-EDGE-1: light the cement side of a transition tile identically to the
    // interior (no seam); the terrain side (alpha==0) keeps terrain lighting.
    pureConcrete = max(pureConcrete, cementTransAlpha);
    float concreteColorBlend = sqrt(clamp(pureConcrete, 0.0, 1.0));
    matWeights = mix(matWeights, vec4(0.0, 0.0, 0.0, 1.0), pureConcrete);
    snowWeight *= (1.0 - pureConcrete);

    // --- Surface normal (smooth macro slope + tangent-space detail) ---
    vec3  N;
    float macroNz = 1.0;   // MACRO (un-perturbed) slope Z — used for the cliff test
    if (u_skirtDepth > 0.0) {
        N = vec3(0.0, 0.0, 1.0);
    } else {
        vec3 baseN;
        if ((u_diag & 16) != 0) {
            baseN = normalize(cross(dFdx(v_worldPos), dFdy(v_worldPos)));  // flat A/B
            if (baseN.z < 0.0) baseN = -baseN;
        } else {
            baseN = smoothTerrainNormal(v_worldPos.xy);  // smooth macro slope
        }
        macroNz = baseN.z;
        if ((u_diag & 32) != 0) {
            N = baseN;  // detail disabled (A/B)
        } else {
            vec3 dN   = chunkDetailNormal(matWeights, snowWeight, v_worldPos.xy, macroNz)
                      * (1.0 - pureConcrete);  // suppress regular detail on cement
            // Cement normal: apply painted-concrete normal (MAT_LAYER_PAINTED_CONC) in the
            // cement-masked region so runways get real surface relief instead of flat shading.
            // Uses the same concrete UV tiling as chunkDetailNormal() (legacy parity).
            // UB2-01: hoist uvC + its gradient + fwidth out of the non-uniform
            // `pureConcrete` branch (uvC depends only on v_worldPos + uniforms, so
            // this is uniform flow), then sample via textureGrad. Implicit LOD and
            // fwidth() inside the per-fragment branch were UB (vendor-divergent).
            vec2  uvC  = v_worldPos.xy * (detailNormalTiling.x / MAT_WORLD_UNITS_PER_TILE) * matTiling.w;
            vec2  ddxC = dFdx(uvC), ddyC = dFdy(uvC);
            float fwC  = clamp(1.0 - (length(fwidth(uvC)) - 0.5) * 2.0, 0.0, 1.0);
            if (pureConcrete > 0.001) {
                vec4 cN    = textureGrad(matNormalArray, vec3(uvC, float(MAT_LAYER_PAINTED_CONC)), ddxC, ddyC);
                dN        += pureConcrete * matNormalBoost.w * fwC * (cN.rgb * 2.0 - 1.0);
            }
            // terrainNormalsFromHeightStrength scales the macro-slope tilt (default 1.0).
            // TERRAIN-DETAIL-ANTI-TILE-1: tie the high-frequency detail normal to
            // the chunk LOD tier so it stops forming a repeating "carpet" with
            // distance. baseN (height-derived macro slope) is left at full strength
            // — only the tiled detail (dN) fades.
            //   u_lodStep 1 = LOD0 (full) · 2 = LOD1 (fade) · >=4 = LOD2+ (gone).
            // Gate flag rides in detailNormalStrength.z (>0.5 = ON, set by C++);
            // .y = LOD1 strength. OFF => lodDetailMul=1.0 => byte-identical.
            float lodDetailMul = 1.0;
            if (detailNormalStrength.z > 0.5) {
                lodDetailMul = (u_lodStep <= 1) ? 1.0
                             : (u_lodStep <= 2) ? detailNormalStrength.y
                             : 0.0;
            }
            vec3 pert = vec3(baseN.xy / max(baseN.z, 0.2) * terrainNormalsFromHeightStrength
                             + dN.xy * detailNormalStrength.x * lodDetailMul, 1.0);
            N = normalize(pert);
        }
    }

    // TERRAIN-CHUNK-POM-1 debug viz (early-return like the other diag blocks).
    // 4096 = POM UV-offset heat: |pomOff| relative to pomParams.x (blue=0 ..
    //        red=full scale). Skirts / non-POM fragments read solid blue.
    if ((u_diag & 4096) != 0) {
        float h = clamp(length(g_pomOffDbg) / max(pomParams.x, 1e-5), 0.0, 1.0);
        fragColor = vec4(h, h * 0.25, 1.0 - h, 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }
    // 8192 = view-vector swizzle oracle (ruling R1): RGB = MC2-world fragment->
    //        camera direction, R=east*0.5+0.5, G=north*0.5+0.5, B=up. Decoded
    //        pixels must point back toward the camera azimuth (screen gradient
    //        tracks camera orbit) — numeric check on captures, not eyeballs.
    if ((u_diag & 8192) != 0) {
        vec3 camW = vec3(-cameraPos.x, cameraPos.z, cameraPos.y);
        vec3 Vw = normalize(camW - v_worldPos);
        fragColor = vec4(Vw.x * 0.5 + 0.5, Vw.y * 0.5 + 0.5, clamp(Vw.z, 0.0, 1.0), 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // --- Colour mapping: mix the colormap toward per-material tints (legacy
    // TERRAIN-TINT-UI-1). Concrete tile colour blend needs per-vertex TerrainType
    // (deferred) so concreteColorBlend=0 here. ---
    const vec3  kLumaWeights = vec3(0.299, 0.587, 0.114);
    // TERRAIN-MATERIAL-LIB-1: tintConcrete/tintSnow promoted to uniforms above
    // (were const vec3 literals here); default upload == these exact values.
    vec3  materialTint = tintRock * matWeights.x + tintGrass * matWeights.y
                       + tintDirt * matWeights.z + tintConcrete * matWeights.w
                       + tintSnow * snowWeight;
    float colLum      = dot(base, kLumaWeights);
    float tintBase    = mix(0.18, 0.50, smoothstep(0.1, 0.6, colLum));
    // TERRAIN-CONTROLMAP-ALBEDO-1: u_controlAlbedoStrength==0.0 -> mix(x,1.0,0.0)
    // == x -> algebraically identical to the pre-slice expression. >0 lifts the
    // classifier-era cap toward full weight-composed albedo (materialTint).
    float tintStrength= mix(mix(tintBase, 0.85, snowWeight) * tintStrengthScale,
                             1.0, u_controlAlbedoStrength);
    // TERRAIN-OVERLAY-V2-SUPPRESS-1: where the authored OVERLAY_V2 sidecar covers
    // this pixel, keep its authored colour instead of muddying it toward the
    // generic material tint (tintConcrete under the forced-concrete weights) —
    // this is what caused cement/roads to look "doubled". Full coverage (ov.a=1)
    // -> tintStrength 0 -> baseColor == authored overlay; edges feather. No-op
    // when OVERLAY_V2 is off (overlayCoverage stays 0).
    tintStrength *= (1.0 - overlayCoverage);
    vec3  baseColor   = mix(base, materialTint, tintStrength);
    if ((u_diag & 512) != 0) baseColor = base;   // DIAG 512: bypass material tint (A/B)

    // TERRAIN-MATERIAL-TEXTURES-1: real per-layer albedo (gate ON only; OFF ->
    // this block is skipped and everything below is the pre-slice path verbatim).
    // Same per-layer world-space UV scheme as chunkDetailNormal (matTiling from
    // the material-lib JSON; rock /3 matches the normal path) so albedo features
    // line up with the detail normals. v1 samples the UNPERTURBED UVs (no POM
    // offset -- that lives inside chunkDetailNormal; acceptable v1 drift, POM is
    // default-OFF). Uniform control flow: u_useMatAlbedo is a uniform, so the
    // implicit-LOD texture() calls here are well-defined (UB2 discipline).
    if (u_useMatAlbedo != 0) {
        // TERRAIN-MATERIAL-TEXTURES-1-FIX (fix C, cement exclusion): where a
        // cement WORD is active (apron/runway/pad) the colormap `base` is already
        // the authored cement tone and pureConcrete forced matWeights=(0,0,0,1).
        // Compositing the CONCRETE albedo (Road004/marking source) here and then
        // only PARTIALLY restoring it via concreteColorBlend below leaves a second,
        // misaligned cement layer fighting the cement atlas on the apron. Hard-
        // exclude the textured albedo on cement: cementAlbExcl==1 on a full cement
        // hit -> the block is a no-op and `base` (the cement atlas colour) wins
        // cleanly. cementTransAlpha feathers the transition-tile margin the same
        // way concreteColorBlend already feathers it, so no seam is introduced.
        float cementAlbExcl = cementHit ? 1.0 : max(concreteColorBlend, cementTransAlpha);
        vec2 uvM = v_worldPos.xy * (detailNormalTiling.x / MAT_WORLD_UNITS_PER_TILE);
        vec3 texAlb =
              matWeights.x * texture(u_matAlbedoArray, vec3(uvM * (matTiling.x / 3.0), float(MAT_LAYER_ROCK))).rgb
            + matWeights.y * texture(u_matAlbedoArray, vec3(uvM * matTiling.y, float(MAT_LAYER_GRASS))).rgb
            + matWeights.z * texture(u_matAlbedoArray, vec3(uvM * matTiling.z, float(MAT_LAYER_DIRT))).rgb
            + matWeights.w * texture(u_matAlbedoArray, vec3(uvM * matTiling.w, float(MAT_LAYER_CONCRETE))).rgb
            + snowWeight   * texture(u_matAlbedoArray, vec3(uvM * matTilingSnow, float(MAT_LAYER_SNOW))).rgb;
        // Colormap remains the MACRO tint: 2x multiply keeps burn-in lighting and
        // per-mission colour zones on top of the tiled material albedo (weights
        // matWeights+snowWeight sum to 1 after the normalizations above).
        vec3 texComposed = texAlb * base * 2.0;
        // Scale the textured contribution down to zero over cement so the cement
        // atlas (restored into `base`, mixed in at line ~concreteColorBlend) is
        // the ONE surface shown on the apron.
        float albMix = clamp(u_matAlbedoStrength, 0.0, 1.0) * (1.0 - cementAlbExcl);
        baseColor = mix(baseColor, texComposed, albMix);
    }

    // TERRAIN-MATERIAL-TEXTURES-1-FIX (fix A, INDEX ORACLE debug view): DIAG bit
    // 16384 renders each albedo texture-array LAYER as a distinct flat colour keyed
    // by ARRAY INDEX (the value the frag passes to texture()), weighted by the same
    // matWeights/snowWeight used for the real composite. This makes channel->layer
    // wiring provable by eye/capture WITHOUT trusting the texture content:
    //   ROCK(0)=red  GRASS(1)=green  DIRT(2)=blue  CONCRETE(3)=yellow  SNOW(4)=white
    // (this returns before the cliff block, so MARBLE_CLIFF(5) is proved separately
    // by the loader's per-layer "[TERRAIN_MAT_TEX] layer 5 (cliff)" log line, not here.)
    // If open dirt ground shows BLUE here, dirt->layer 2 is correct; if it shows
    // yellow, the weight->layer mapping is wrong (the reported symptom's oracle).
    if ((u_diag & 16384) != 0) {
        vec3 idxCol =
              matWeights.x * vec3(1.0, 0.0, 0.0)   // MAT_LAYER_ROCK     = 0
            + matWeights.y * vec3(0.0, 1.0, 0.0)   // MAT_LAYER_GRASS    = 1
            + matWeights.z * vec3(0.0, 0.0, 1.0)   // MAT_LAYER_DIRT     = 2
            + matWeights.w * vec3(1.0, 1.0, 0.0)   // MAT_LAYER_CONCRETE = 3
            + snowWeight   * vec3(1.0, 1.0, 1.0);  // MAT_LAYER_SNOW     = 4
        fragColor = vec4(idxCol, 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // Cement: restore the authored colormap tone (runway/apron) instead of the
    // generic concrete tint.
    baseColor = mix(baseColor, base, concreteColorBlend);

    // Cliff mapping: desaturate + darken toward rock on steep slopes. MUST use the
    // MACRO slope (un-perturbed), NOT the detail-perturbed N — detail bumps drop
    // N.z to ~0.65 even on flat ground, which fired the cliff blend everywhere and
    // desaturated the whole map to grey (matches legacy using WorldNorm.z).
    {
        float cliffBlend = smoothstep(0.85, 0.55, abs(macroNz));
        // TERRAIN-MATERIAL-TEXTURES-1: LOD for the cliff-albedo triplanar samples
        // below, computed HERE in uniform flow (fwidth inside the non-uniform
        // cliffBlend branch would be UB -- UB2 discipline). 2048 texels per
        // CLIFF_ALB_TS world units; gate OFF leaves it unused (no output change).
        const float CLIFF_ALB_TS = 256.0;   // world units per cliff-albedo repeat
        float cliffAlbLod = log2(max(length(fwidth(v_worldPos.xy)) * (2048.0 / CLIFF_ALB_TS), 1.0));
        if (cliffBlend > 0.01) {
            float luma = dot(baseColor, vec3(0.299, 0.587, 0.114));
            vec3  cliffColor = mix(vec3(luma), tintRock, 0.6) * 0.8;
            // TERRAIN-MATERIAL-TEXTURES-1: the "cliffs look like rock" payoff --
            // gate ON swaps the flat desaturate-toward-tintRock cliff colour for
            // the real CLIFF material albedo (marble_cliff, layer 5) projected
            // from world axes (triplanar, same plane weighting as the cliff
            // normal block below) so steep faces stop showing stretched top-down
            // colormap. textureLod with the hoisted LOD is branch-safe. Gate OFF
            // -> cliffColor untouched -> byte-identical.
            if (u_useMatAlbedo != 0) {
                vec3 wpc = v_worldPos;
                vec3 mNc = smoothTerrainNormal(v_worldPos.xy);
                vec3 wnc = abs(mNc); wnc /= (wnc.x + wnc.y + wnc.z + 1e-5);
                vec3 cliffAlb =
                      textureLod(u_matAlbedoArray, vec3(wpc.yz / CLIFF_ALB_TS, float(MAT_LAYER_MARBLE_CLIFF)), cliffAlbLod).rgb * wnc.x
                    + textureLod(u_matAlbedoArray, vec3(wpc.xz / CLIFF_ALB_TS, float(MAT_LAYER_MARBLE_CLIFF)), cliffAlbLod).rgb * wnc.y
                    + textureLod(u_matAlbedoArray, vec3(wpc.xy / CLIFF_ALB_TS, float(MAT_LAYER_MARBLE_CLIFF)), cliffAlbLod).rgb * wnc.z;
                cliffColor = mix(cliffColor, cliffAlb * base * 2.0, clamp(u_matAlbedoStrength, 0.0, 1.0));
            }
            baseColor = mix(baseColor, cliffColor, cliffBlend * 0.7 * (1.0 - pureConcrete));
        }
    }

    // TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1: on steep cells, project the ROCK normal
    // map from WORLD axes (triplanar) instead of the top-down per-tile UV — kills
    // the vertical UV stretch on cliff faces and gives the wall real rock relief.
    // Uses MAT_LAYER_MARBLE_CLIFF (mat5_normal.tga, marble_cliff_01). Default OFF
    // (useTriplanarCliff==0) -> byte-identical.
    if (useTriplanarCliff != 0) {
        float cb = smoothstep(0.85, 0.55, abs(macroNz));
        if (cb > 0.01) {
            g_cliffBranchTaken = 1.0;   // TERRAIN-CLIFF-DEBUG: block entered
            const float ts = 256.0;                 // world units per rock repeat
            vec3 wp = v_worldPos;
            // recompute the macro normal here (baseN is scoped to the normal block).
            vec3 mN = smoothTerrainNormal(v_worldPos.xy);
            vec3 wn = abs(mN); wn /= (wn.x + wn.y + wn.z + 1e-5);

            // Base per-plane UVs (before any parallax).
            vec2 uvX = wp.yz / ts;   // YZ plane
            vec2 uvY = wp.xz / ts;   // XZ plane
            vec2 uvZ = wp.xy / ts;   // XY plane

            // TERRAIN-CLIFF-POM-1: real triplanar Parallax Occlusion Mapping.
            // World-space view vector (fragment -> eye). cameraPos is the Stuff/MLR
            // eye; MC2 world = (-x, z, y) -- SAME swizzle the legacy chunk POM and
            // water-reflection fix use (this file, ~L473/L991). Project Vw onto each
            // plane's 2D basis and march that plane's heightfield for a UV offset.
            if (u_cliffPom.x > 0.5) {
                vec3  camW = vec3(-cameraPos.x, cameraPos.z, cameraPos.y);
                vec3  Vw   = normalize(camW - wp);          // frag -> eye, world
                // depth is a world-unit height expressed relative to the CLIFF
                // tiling (ts); the plane view offset is divided by the out-of-plane
                // view component so grazing angles sweep farther (true parallax).
                float depth = u_cliffPom.y / ts;
                float steps = u_cliffPom.z;
                // per-plane: 2D view = the two in-plane components; divide by the
                // out-of-plane component magnitude (clamped) for angle scaling.
                vec2 vpX = Vw.yz / max(abs(Vw.x), 0.30);
                vec2 vpY = Vw.xz / max(abs(Vw.y), 0.30);
                vec2 vpZ = Vw.xy / max(abs(Vw.z), 0.30);
                // weight the march by this plane's contribution so we don't pay for
                // near-zero planes (also keeps offsets small where the plane barely
                // shows). Blend offset toward base UV by wn.
                vec2 uvX0 = uvX, uvY0 = uvY, uvZ0 = uvZ;
                uvX = mix(uvX, cliffParallaxPlane(uvX, vpX, depth, steps), clamp(wn.x * 1.5, 0.0, 1.0));
                uvY = mix(uvY, cliffParallaxPlane(uvY, vpY, depth, steps), clamp(wn.y * 1.5, 0.0, 1.0));
                uvZ = mix(uvZ, cliffParallaxPlane(uvZ, vpZ, depth, steps), clamp(wn.z * 1.5, 0.0, 1.0));
                // TERRAIN-CLIFF-DEBUG: max per-plane |POM UV offset| (post-blend).
                g_cliffPomOffMag = max(max(length(uvX - uvX0), length(uvY - uvY0)),
                                       length(uvZ - uvZ0));
            }

            // tangent-space rock normals sampled in each world plane (at the
            // parallax-offset UVs when POM is on). textureLod(...,0): explicit LOD
            // is valid inside this non-uniform branch (implicit derivatives would be
            // UB / vendor-divergent — UB2 discipline).
            vec3 nX = textureLod(matNormalArray, vec3(uvX, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).rgb * 2.0 - 1.0;
            vec3 nY = textureLod(matNormalArray, vec3(uvY, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).rgb * 2.0 - 1.0;
            vec3 nZ = textureLod(matNormalArray, vec3(uvZ, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).rgb * 2.0 - 1.0;
            // reorient each plane's tangent normal into world space + blend by axis weight.
            vec3 triTilt = vec3(0.0, nX.x, nX.y) * wn.x
                         + vec3(nY.x, 0.0, nY.y) * wn.y
                         + vec3(nZ.x, nZ.y, 0.0) * wn.z;

            // TERRAIN-CLIFF-HEIGHT-NORMAL-1: derive a SHADING normal from the HEIGHT
            // GRADIENT of the cooked cliff displacement (mat5 layer-5 ALPHA) instead
            // of relying on the smooth marble RGB normal above. The alpha channel is
            // rich, high-contrast rock relief; its gradient is what should catch light.
            // Per world-axis plane: sample the layer-5 alpha at the (POM-offset when
            // POM is on) UV plus small +du/+dv steps and forward-difference to a
            // tangent-space normal n=normalize(vec3(-dHdu*s,-dHdv*s,1)); reorient into
            // world space with the SAME per-plane axis remap used for triTilt, weight
            // by wn. textureLod(...,0): explicit LOD, valid in this non-uniform branch
            // (UB2 — no implicit derivatives). Skipped when strength<=0 (== TRIPLANAR-1).
            if (cliffHeightNormalStrength > 0.0) {
                // step ~one texel of the layer-5 alpha (mat5 disp is 4k over ts wu);
                // constant in world/UV so the gradient is scale-consistent per plane.
                const float hEps = 1.0 / 2048.0;    // UV-space finite-difference step
                // per-plane heights: center + du + dv.
                float hX0 = textureLod(matNormalArray, vec3(uvX,                    float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
                float hXu = textureLod(matNormalArray, vec3(uvX + vec2(hEps, 0.0),  float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
                float hXv = textureLod(matNormalArray, vec3(uvX + vec2(0.0, hEps),  float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
                float hY0 = textureLod(matNormalArray, vec3(uvY,                    float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
                float hYu = textureLod(matNormalArray, vec3(uvY + vec2(hEps, 0.0),  float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
                float hYv = textureLod(matNormalArray, vec3(uvY + vec2(0.0, hEps),  float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
                float hZ0 = textureLod(matNormalArray, vec3(uvZ,                    float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
                float hZu = textureLod(matNormalArray, vec3(uvZ + vec2(hEps, 0.0),  float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
                float hZv = textureLod(matNormalArray, vec3(uvZ + vec2(0.0, hEps),  float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
                // forward-difference gradients -> tangent-space normals (z=1 base).
                // scale by strength/hEps so `strength` reads as a relief multiplier
                // independent of the finite-difference step.
                float g = cliffHeightNormalStrength / hEps;
                vec3 hnX = normalize(vec3(-(hXu - hX0) * g, -(hXv - hX0) * g, 1.0));
                vec3 hnY = normalize(vec3(-(hYu - hY0) * g, -(hYv - hY0) * g, 1.0));
                vec3 hnZ = normalize(vec3(-(hZu - hZ0) * g, -(hZv - hZ0) * g, 1.0));
                // reorient each plane's height normal to world space with the SAME
                // remap as triTilt and blend by axis weight (this is the tilt, so use
                // .xy the way triTilt uses the rgb-normal .xy).
                vec3 htTilt = vec3(0.0, hnX.x, hnX.y) * wn.x
                            + vec3(hnY.x, 0.0, hnY.y) * wn.y
                            + vec3(hnZ.x, hnZ.y, 0.0) * wn.z;
                // Default to a STRONG mix toward the height-derived tilt (it carries
                // the real cliff structure); the smooth rgb tilt is retained only as
                // a fine microsurface floor. mix factor saturates with strength so
                // larger values also lean harder on the height normal.
                float hMix = clamp(cliffHeightNormalStrength * 0.5, 0.0, 1.0);
                triTilt = mix(triTilt, htTilt, hMix);
            }

            vec3 triN = normalize(mN + triTilt * cliffTriplanarStrength);
            N = normalize(mix(N, triN, cb));

            // groove relief: darken by the rock displacement, triplanar-blended over
            // all three planes at the parallax UVs so side-plane occlusion (the real
            // view-dependent depth) shows on vertical faces, not just the XY plane.
            float dispX = textureLod(matNormalArray, vec3(uvX, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
            float dispY = textureLod(matNormalArray, vec3(uvY, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
            float dispZ = textureLod(matNormalArray, vec3(uvZ, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
            float disp  = dispX * wn.x + dispY * wn.y + dispZ * wn.z;
            g_cliffMat5Alpha = disp;   // TERRAIN-CLIFF-DEBUG: cooked mat5 disp alpha
            baseColor *= mix(1.0, 0.62 + 0.38 * disp, cb);

            // With POM on, also re-project the cliff ALBEDO at the parallax UVs so
            // the rock colour occludes with depth (the albedo block above sampled
            // at un-parallaxed UVs). Only when the material-albedo path is live.
            if (u_cliffPom.x > 0.5 && u_useMatAlbedo != 0) {
                vec3 cliffAlbP =
                      textureLod(u_matAlbedoArray, vec3(uvX * (ts / 256.0), float(MAT_LAYER_MARBLE_CLIFF)), 0.0).rgb * wn.x
                    + textureLod(u_matAlbedoArray, vec3(uvY * (ts / 256.0), float(MAT_LAYER_MARBLE_CLIFF)), 0.0).rgb * wn.y
                    + textureLod(u_matAlbedoArray, vec3(uvZ * (ts / 256.0), float(MAT_LAYER_MARBLE_CLIFF)), 0.0).rgb * wn.z;
                baseColor = mix(baseColor, cliffAlbP * base * 2.0,
                                cb * clamp(u_matAlbedoStrength, 0.0, 1.0));
            }
        }
    }

    // TERRAIN-CLIFF-DEBUG (gate MC2_TERRAIN_CLIFF_DEBUG): bounded debug-viz that
    // writes DIRECTLY to the final color and returns early so nothing below can
    // overwrite it. Placed AFTER the useTriplanarCliff block so the branch-taken
    // flag + POM offset + mat5 disp are already captured. u_cliffDebug==0 (default)
    // -> this whole block is skipped -> byte-identical.
    if (u_cliffDebug != 0) {
        vec3 dbg;
        if (u_cliffDebug == 1) {
            // cliffBlend grayscale (steep-face test). Recompute from the MACRO
            // slope exactly as the cliff blocks do (smoothstep(0.85,0.55,|macroNz|)).
            float cbDbg = smoothstep(0.85, 0.55, abs(macroNz));
            dbg = vec3(cbDbg);
        } else if (u_cliffDebug == 2) {
            // Branch-taken coverage: green where the cliff triplanar block ran,
            // red otherwise.
            dbg = (g_cliffBranchTaken > 0.5) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        } else if (u_cliffDebug == 3) {
            // POM UV-offset magnitude as color (blue=0 .. red=large). Offset is a
            // UV-space distance (mat UVs are worldXY/ts); scale for visibility.
            float m = clamp(g_cliffPomOffMag * 8.0, 0.0, 1.0);
            dbg = vec3(m, m * 0.25, 1.0 - m);
        } else { // u_cliffDebug == 4
            // Sampled mat5 (MARBLE_CLIFF) displacement alpha (grayscale). Use the
            // triplanar-blended value captured in the block; if the block did not
            // run for this fragment, sample the XY plane directly so the texture
            // binding is still proven.
            float a = (g_cliffBranchTaken > 0.5)
                    ? g_cliffMat5Alpha
                    : textureLod(matNormalArray,
                                 vec3(v_worldPos.xy / 256.0, float(MAT_LAYER_MARBLE_CLIFF)), 0.0).a;
            dbg = vec3(a);
        }
        fragColor = vec4(dbg, 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // World-space two-octave break-up noise (non-snow).
    {
        float lowFreq  = fbm(v_worldPos.xy * 0.0035, 3) * 0.5 + 0.5;
        float highFreq = fbm(v_worldPos.xy * 0.018,  2) * 0.5 + 0.5;
        float breakup  = mix(0.78, 1.18, mix(lowFreq, highFreq, 0.55));
        baseColor *= mix(1.0, breakup, (1.0 - snowWeight) * (1.0 - pureConcrete));
    }
    // Snow brightness dampen — only detected-snow fragments (snowWeight) are darkened.
    baseColor *= mix(1.0, snowBrightnessDampen, snowWeight);

    // TERRAIN-MACRO-VARIATION-1: large-scale TINT + SATURATION breakup so flat,
    // same-material areas stop reading as one stretched texture. The existing
    // breakup block above is brightness-only; this adds colour/warmth/saturation
    // variation across ~1000 WU zones. Uniform-flow branch (macroVariationStrength
    // is a uniform); fbm is derivative-free math, so no UB. Default OFF
    // (macroVariationStrength==0 -> block skipped -> byte-identical).
    if (macroVariationStrength > 0.0) {
        float mzA = fbm(v_worldPos.xy * 0.0011,        4) * 0.5 + 0.5;  // brightness/saturation field
        float mzB = fbm(v_worldPos.xy * 0.0009 + 47.0, 3) * 0.5 + 0.5;  // warm<->cool tint field
        float macroBright = mix(0.88, 1.12, mzA);
        vec3  warmTint    = vec3(1.06, 1.005, 0.90);   // drier / sun-bleached zones
        vec3  coolTint    = vec3(0.93, 1.00,  1.05);   // lusher / shaded zones
        vec3  macroTint   = mix(warmTint, coolTint, mzB);
        float luma        = dot(baseColor, vec3(0.299, 0.587, 0.114));
        // desaturate worn zones (mzA low), keep vivid in others (mzA high)
        vec3  macroCol    = mix(vec3(luma), baseColor, mix(0.84, 1.05, mzA)) * macroBright * macroTint;
        // keep authored surfaces clean: no macro variation on snow or concrete/cement
        float applyMask   = (1.0 - snowWeight) * (1.0 - pureConcrete);
        baseColor = mix(baseColor, macroCol, macroVariationStrength * applyMask);
    }

    // --- Lighting: NdotL relief band + sun shadow (baked; GBuffer1 stays
    // shadowHandled_flatUp so the compositor does not re-shadow terrain). ---
    float NdotL       = dot(N, terrainLightDir.xyz);
    float diffuse     = clamp(NdotL, 0.02, 1.0);
    float normalLight = ((u_diag & 4) != 0) ? 1.0 : mix(0.35, 1.20, diffuse);
    normalLight = mix(normalLight, 1.0, pureConcrete);  // cement: fully flat-lit slab

    float shadow = 1.0;
    if ((u_diag & 8) == 0) {
        const vec3 shadowN = vec3(0.0, 0.0, 1.0);
        float staticS = calcShadow(v_worldPos, shadowN, terrainLightDir.xyz, 16);
        float dynS    = calcDynamicShadow(v_worldPos, shadowN, terrainLightDir.xyz, 8);
        shadow = min(staticS, dynS);
        // CLIFF SHADOW FLOOR: steep faces fall to ~0 under CSM (calcDynamicShadow
        // is unfloored) → near-black voids. Lift ONLY steep faces by a gated floor.
        // Default 0.0 => no-op (byte-identical). macroNz is the MACRO slope Z.
        float cliffShadowBlend = smoothstep(0.85, 0.55, abs(macroNz));
        shadow = max(shadow, u_terrainCliffShadowFloor * cliffShadowBlend);
        // CEMENT-CLEAN-LIGHTING-1: concrete/cement is an authored flat slab. The
        // terrain STATIC self-shadow (calcShadow) lands as world-fixed blotches that
        // do not tile with the cement atlas (the "spots bleeding through" report).
        // Drop static shadow on concrete but KEEP the dynamic building shadow so
        // structures still cast onto roads/pads.
        shadow = mix(shadow, dynS, pureConcrete);
    }

    // DEBUG-VIZ (exact-value escape on u_diag; bitmask modes never reach 30/31):
    //   30 = dynamic-cast shadow only (isolates building dynamic shadow), 31 = min(static,dyn).
    if (u_diag == 30) {
        const vec3 shadowN = vec3(0.0, 0.0, 1.0);
        float dynVizS = calcDynamicShadow(v_worldPos, shadowN, terrainLightDir.xyz, 8);
        fragColor = vec4(vec3(dynVizS), 1.0);
        GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }
    if (u_diag == 31) {
        const vec3 shadowN = vec3(0.0, 0.0, 1.0);
        float staticVizS = calcShadow(v_worldPos, shadowN, terrainLightDir.xyz, 16);
        float dynVizS2   = calcDynamicShadow(v_worldPos, shadowN, terrainLightDir.xyz, 8);
        fragColor = vec4(vec3(min(staticVizS, dynVizS2)), 1.0);
        GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    vec3 lit = baseColor * normalLight * shadow;
    // Hemisphere ambient fill (TERRAIN-LIGHTING-1/2) — added AFTER shadow so sky/
    // ground bounce still lights shadowed terrain. Env-gated OFF by default
    // (terrainLightingV1Strength=0 -> skipped, byte-safe). Matches gos_terrain.frag.
    vec3 hemiContrib = vec3(0.0);   // LIGHTING-DEBUG-VIEWS: captured for ambient channel
    if (terrainLightingV1Strength > 0.0) {
        const vec3 hemiSkyTint    = vec3(0.55, 0.62, 0.75);
        const vec3 hemiGroundTint = vec3(0.32, 0.28, 0.22);
        float skyFactor    = N.z * 0.5 + 0.5;
        vec3  hemiFill     = mix(hemiGroundTint, hemiSkyTint, skyFactor);
        float hemiAmount   = terrainLightingV1Strength * (1.0 - 0.5 * snowWeight);
        float hemiShadowMix = mix(terrainLightingV2ShadowFillFloor, 1.0, shadow);
        hemiContrib = hemiFill * hemiAmount * 0.25 * hemiShadowMix;
        lit += hemiContrib;
    }

    // LIGHTING-DEBUG-VIEWS-1A-CHUNK: unified lighting debug channels on the
    // DEFAULT (LodChunk) terrain renderer. u_lightingDebugView==0 -> skipped
    // (pixel-invariant). 45 final / 47 / 48 have no terrain meaning -> fall
    // through to the normal lit render below.
    if (u_lightingDebugView != 0) {
        vec3 dbg; bool handled = true;
        if      (u_lightingDebugView == 40) dbg = baseColor;                            // albedo (pre-light, post-tint)
        else if (u_lightingDebugView == 41) dbg = N * 0.5 + 0.5;                        // surface normal
        else if (u_lightingDebugView == 42) dbg = vec3(diffuse);                        // sun N·L diffuse
        else if (u_lightingDebugView == 43) dbg = min(hemiContrib * 4.0, vec3(1.0));    // ambient/hemi fill
        else if (u_lightingDebugView == 44) dbg = vec3(shadow);                         // shadow factor
        else if (u_lightingDebugView == 46) {                                           // over/under-bright heatmap
            float luma = dot(lit, vec3(0.2126, 0.7152, 0.0722));
            if      (luma > 1.0)  dbg = vec3(1.0, clamp(2.0 - luma, 0.0, 1.0) * 0.4, 0.0);
            else if (luma < 0.05) dbg = vec3(0.0, 0.0, 1.0);
            else                  dbg = vec3(luma);
        }
        else handled = false;                                                           // 45/47/48/unknown -> fall through
        if (handled) {
            fragColor = vec4(dbg, 1.0);
            if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
            return;
        }
    }

    // TERRAIN-EDGE-FEATHER-1: fade the last ~one-tile band of terrain to sky/haze
    // colour so the hard, perfectly-straight map-perimeter silhouette dissolves
    // into the fog/horizon instead of cutting a knife edge. Ports the legacy
    // gos_terrain.frag edge-haze (edge_haze.hglsl) that the LodChunk path lost.
    // Colour-only (no depth/geometry change); the screen-space edge_fog/fog_oob
    // cloud pass already covers the hard depth edge behind the faded band.
    // Uniform-flow branch; default OFF (u_edgeFeather==0 -> skipped -> byte-identical).
    // u_halfMap>0 guard: s_halfMap inits 0 and is only set in the submit path;
    // edgeHazeAmount(_, 0) would wash the whole map grey if the gate armed before
    // the first upload (greybeard footgun). Skip until halfMap is real.
    if (u_edgeFeather != 0 && u_halfMap > 0.0) {
        float haze = edgeHazeAmount(v_worldPos.xy, u_halfMap) * u_edgeFeatherStrength;
        lit = mix(lit, EDGE_HAZE_SKY, clamp(haze, 0.0, 1.0));
    }

    // TERRAIN-SHORELINE-V3: land-side wet-darken + foam band, PLACED BY
    // ELEVATION (v_worldPos.z relative to u_waterElevation) instead of a
    // world-XY mask. Gate OFF -> u_useShorelineMask==0 -> this whole block is
    // skipped -> byte-identical (recon Sec.5 "byte-identity via uniform
    // else-branch"). cement/concrete excluded from wet-darken (recon ruling
    // #6 / open ruling 6: cementHit already computed above). Foam is
    // f(v_worldPos, u_shaderTime) ONLY -- no view-dependent term anywhere in
    // this block (advisor camera-independence ruling, recon landmine #2).
    //
    // V3 ROOT-CAUSE FIX (post v1/v2 "zigzag AND floats up-slope"): the mask
    // EDT was cooked off a height SOURCE (coarse or 16x-smoothed) that never
    // exactly equals the RENDERED waterline (the coarse mesh's bilinear-
    // interpolated surface), so a horizontal-distance mask either staircases
    // (faithful source) or floats above the true shoreline (smoothed source).
    // Elevation sidesteps this entirely: wetness = smoothstep over
    // (v_worldPos.z - u_waterElevation), and v_worldPos.z IS the exact height
    // the rasterizer produced for this fragment -- the band hugs the drawn
    // waterline BY CONSTRUCTION, at any LOD, on any source.
    //
    // Two mitigations carried over from v2 (both f(worldPos,time) only):
    //   1. Slope guard -- an elevation band still runs along a contour line,
    //      which on a steep bank is a near-vertical smear. Attenuate by the
    //      terrain normal's slope angle (from N, already computed above):
    //      full strength on ~flat beaches, fading to 0 by ~20 deg.
    //   2. Noise-BROKEN foam coverage -- foam alpha is `band * fbm`
    //      thresholded into a coverage mask (wisps), not a smooth brighten.
    if (u_useShorelineMask != 0) {
        // Elevation-relative height above the water surface (can be negative
        // = submerged; terrain under the water plane still rasterizes here
        // since this pass draws land. V4-STYLE: submerged fragments get ZERO
        // band contribution -- see the aboveFade clamp below).
        float aboveWater = v_worldPos.z - u_waterElevation;
        // TERRAIN-SHORELINE-V3 FIX (measured): the band's ZERO crossing is at
        // the drawn waterline by construction (u_waterElevation == the water
        // fast path's plane Z; SHORELINE_PROBE measured the fine/coarse gap at
        // the waterline = -0.004wu, i.e. exact). The user-visible "band ~6m up
        // the bank" was NOT a placement drift but the band's fixed VERTICAL
        // thickness (wet 3wu / foam 1.2wu) spread horizontally by a shallow
        // shore slope: on a ~3wu-per-128wu beach, 1.2wu of vertical foam height
        // maps to ~50wu (~15m) of horizontal run. Fix: measure the band as a
        // HORIZONTAL distance from the waterline (perpendicular run along the
        // surface), so it hugs the drawn waterline at a consistent narrow width
        // on shallow AND steep shores. Convert vertical rise -> horizontal run
        // via the MACRO terrain slope (coarse heightfield normal -- same height
        // space the water plane draws in), tan(theta) = sin/cos = sqrt(1-Nz^2)/Nz:
        //   run = rise / tan(theta) = rise * Nz / sqrt(1 - Nz^2).
        // Clamp the slope so a near-flat basin floor (sin->0, run->inf) can't
        // blow the band out to the whole map -- below ~1.7deg we fall back to a
        // capped run so flats still show a bounded rim, not an infinite smear.
        vec3  macroN   = smoothTerrainNormal(v_worldPos.xy);
        float nzc      = clamp(abs(macroN.z), 0.0, 0.9995);
        float sinSlope = sqrt(max(1.0 - nzc * nzc, 1e-6));
        sinSlope       = max(sinSlope, 0.03);            // >= ~1.7deg -> bounded run
        float horizDist = max(aboveWater, 0.0) * (nzc / sinSlope);
        // TERRAIN-SHORELINE-V4-STYLE (zigzag fix, user verdict D): jitter the
        // band distance with STATIC signed world-XY fbm so the lobes stop
        // tracing the mesh waterline's straight diamond segments. Applied to
        // the SHAPING distance only -- the strictly-above-water clamp below
        // uses the un-jittered distance, so jitter can never push a band
        // below the drawn waterline. fbm(p,2) ~ [-0.75,0.75] -> effective
        // wander ~ +/-0.75*u_shorelineEdgeJitter wu.
        float edgeJit    = fbm(v_worldPos.xy * 0.055, 2) * u_shorelineEdgeJitter;
        float horizDistJ = max(horizDist + edgeJit, 0.0);
        // Widths are HORIZONTAL world-units from the waterline (defaults now
        // horizontal-native: wet 16wu / foam 5wu -- the c1593a1f conversion
        // kept the vertical 3.0/1.2 defaults, ~1m of band, invisible at RTS
        // zoom). On a shallow beach this holds the band to a consistent
        // readable width instead of smearing 15m inland or vanishing.
        float wetHeight  = max(u_shorelineWetHeight,  1e-4);
        float foamHeight = max(u_shorelineFoamHeight,  1e-4);
        // Wet lobe: strongest just above the waterline, fading to 0.0 by
        // wetHeight (horizontal wu) inland. Mirrors the mask's old G channel.
        float slWet  = 1.0 - smoothstep(0.0, wetHeight, horizDistJ);
        // Foam rim: narrower band hugging the waterline (jittered inland edge).
        float slFoam = 1.0 - smoothstep(0.0, foamHeight, horizDistJ);
        // TERRAIN-SHORELINE-V4-STYLE (underwater-leak fix, user verdicts A+C):
        // bands live STRICTLY ABOVE the drawn waterline. The V3 code FORCED
        // slWet/slFoam to 1.0 on submerged land (aboveWater <= 0); the water
        // pass is alpha-blended (WATER_MAX_ALPHA 0.87, transmittance-faded at
        // the shore), so that painted the whole shallow shelf with white foam
        // wisps + wet darken that ghosted through the teal water tint -- the
        // "foam under the water surface" and the fluorescent green-teal rim
        // hugging island edges (white foam under teal alpha = teal-green glow).
        // horizDist is already 0 for all submerged fragments (max(aboveWater,0)),
        // so a short fade-in over the first ~1.5 horizontal wu ABOVE the
        // waterline zeroes both lobes at/below the water plane and feathers
        // them in on land. Uses the UN-jittered distance (leak-proof bound).
        const float kShoreEdgeFeatherWu = 1.5;  // horizontal wu fade-in above the waterline
        float aboveFade = smoothstep(0.0, kShoreEdgeFeatherWu, horizDist);
        slWet  *= aboveFade;
        slFoam *= aboveFade;

        // TERRAIN-SHORELINE-V3: optional mask MODULATOR. When a sidecar was
        // loaded (u_hasShorelineMask!=0), its G/B channels scale the elevation
        // bands (e.g. wide-beach falloff on flat shores, or basin exclusion)
        // -- multiplicative, so absence of a mask (u_hasShorelineMask==0)
        // leaves the pure elevation bands untouched (modulator == 1).
        if (u_hasShorelineMask != 0) {
            vec2 slUV;
            slUV.x = (v_worldPos.x - u_shorelineBounds.x) / max(u_shorelineBounds.z, 1e-5);
            slUV.y = (u_shorelineBounds.y - v_worldPos.y) / max(u_shorelineBounds.w, 1e-5);
            if (slUV.x >= 0.0 && slUV.x <= 1.0 && slUV.y >= 0.0 && slUV.y <= 1.0) {
                vec4 sl = texture(u_shorelineMask, slUV);
                float slValid = sl.a;
                // Modulator: mask-authored weight softly gates the elevation
                // band (mix toward the mask weight instead of hard-multiply,
                // so a low-res/blocky mask can only WIDEN or narrow the lobe,
                // never introduce its own stair-step hard edge).
                slWet  *= mix(1.0, clamp(sl.g, 0.0, 1.0), slValid);
                slFoam *= mix(1.0, clamp(sl.b, 0.0, 1.0), slValid);
            } else {
                // Outside the authored mask's bounds: no modulation data ->
                // treat as "no mask" (pure elevation bands) rather than 0, so
                // shorelines outside the cooked extent still show a band.
            }
        }

        // Slope guard: N.z == cos(slope angle) for a unit normal (N.z=1 on
        // flat ground). Full band strength up to ~12 deg, linear falloff
        // to 0 by ~20 deg, so steep hillsides never carry a wet/foam smear.
        // KEPT under the horizontal-run measure (reasoned, not redundant):
        // the horizontal conversion normalizes PLAN-VIEW width, but on a
        // steep bank the band still climbs run*tan(slope) wu VERTICALLY up
        // the face (16wu run @ 45 deg = 16wu-tall wet stripe on the cliff
        // wall, prominent at oblique RTS view angles). The guard is the sole
        // cliff-face suppression; it does not double-count on beaches
        // (<12 deg = full strength, where the run measure does all the work).
        const float kSlopeFullCos = 0.978; // cos(12 deg)
        const float kSlopeZeroCos = 0.940; // cos(20 deg)
        float slopeAtten = clamp((N.z - kSlopeZeroCos) / max(kSlopeFullCos - kSlopeZeroCos, 1e-5), 0.0, 1.0);

        // TERRAIN-SHORELINE-V4-STYLE (material-aware shores, user verdict
        // "this ain't it" -- thin white foam line on near-black rock reads
        // wrong): the shore treatment must match what the shore already IS,
        // not garnish every waterline uniformly.
        //   - beachMat: material affinity from the SAME matWeights the splat
        //     composite used (x=rock, snowWeight separate) -- sand/dirt/grass
        //     shores read as beaches and take foam + wet-sand; rock (and
        //     snow) coasts take near-none.
        //   - brightGround: composited ground luminance gate -- a bright foam
        //     line on near-black ground stands out at any material weight;
        //     absence beats wrongness (coordinator ruling #3).
        // slopeAtten above remains the third, geometric guard.
        float litLuma       = dot(lit, vec3(0.299, 0.587, 0.114));
        float brightGround  = smoothstep(0.08, 0.26, litLuma);
        float beachMat      = clamp(1.0 - matWeights.x - snowWeight, 0.0, 1.0);
        float shoreAffinity = brightGround * beachMat;

        // Wet band (coordinator ruling #2): must produce a VISIBLE albedo
        // response on gentle shores. Tint toward a warm wet-sand chroma held
        // at ~90% of the ground's own luminance: on bright sand this is the
        // classic slightly-darker wet-sand strip; on mid-tone soil it reads
        // as a sandy wet-soil band (hue response, not extra darkening of
        // already-dark ground). Dark/rock shores are excluded by
        // shoreAffinity (ruling #3: nothing beats wrongness there).
        // u_shorelineStrength scales overall intensity (art/runtime knob).
        if (slWet > 0.001 && cementHit == false && slopeAtten > 0.0 && shoreAffinity > 0.001) {
            float wetAmt = clamp(slWet, 0.0, 1.0) * (1.0 - pureConcrete) * slopeAtten
                         * shoreAffinity * u_shorelineStrength;
            vec3  wetSand = vec3(1.16, 1.06, 0.78) * (litLuma * 0.90);
            lit = mix(lit, wetSand, clamp(wetAmt, 0.0, 1.0) * 0.85);
        }
        // Foam: procedural fBm, f(worldPos,time) ONLY (camera-INDEPENDENT
        // by construction -- no view matrix/camera pos anywhere in this
        // expression). Coverage is THRESHOLDED noise (wisps), not a smooth
        // brighten of the whole lobe -- band*fbm decides WHERE foam shows,
        // not just how bright it is. Scaled by shoreAffinity (ruling #1):
        // full beach-foam on gentle sand/dirt shores, near-zero on rock/dark
        // coasts.
        if (slFoam > 0.001 && slopeAtten > 0.0 && shoreAffinity > 0.001) {
            float foamScroll = fbm(v_worldPos.xy * 0.09 + u_shaderTime * 0.15, 3) * 0.5 + 0.5;
            float foamPulse  = fbm(v_worldPos.xy * 0.20 - u_shaderTime * 0.22 + 19.0, 2) * 0.5 + 0.5;
            float foamNoise  = clamp(mix(foamScroll, foamPulse, 0.5), 0.0, 1.0);
            // Coverage threshold: the noise must clear a bar that rises as
            // the band weight falls, so foam appears as broken wisps near
            // the lobe's edge and near-solid only at the exact waterline.
            float coverage  = smoothstep(1.0 - clamp(slFoam, 0.0, 1.0) * 0.85, 1.0, foamNoise + (1.0 - clamp(slFoam, 0.0, 1.0)) * 0.15);
            float foamAmt   = coverage * slopeAtten * u_shorelineFoamStrength * shoreAffinity;
            // V4-STYLE (user verdict C): hue-NEUTRAL desaturated white. The old
            // (0.90,0.93,0.92) carried a faint green cast that read teal when
            // composited near the water tint; foam must not shift hue.
            lit = mix(lit, vec3(0.92), clamp(foamAmt, 0.0, 1.0) * (1.0 - pureConcrete));
        }
    }
    // DIAG bit 2048: visualize the elevation-band weights as RGB (R=height
    // above water clamped [0,1], G=wet weight, B=foam weight) so the V3
    // placement is directly inspectable without needing a mask loaded.
    if ((u_diag & 2048) != 0 && u_useShorelineMask != 0) {
        // Mirror the horizontal-distance band measure used above.
        float aboveWaterDbg = v_worldPos.z - u_waterElevation;
        vec3  macroNDbg     = smoothTerrainNormal(v_worldPos.xy);
        float nzcDbg        = clamp(abs(macroNDbg.z), 0.0, 0.9995);
        float sinSlopeDbg   = max(sqrt(max(1.0 - nzcDbg * nzcDbg, 1e-6)), 0.03);
        float horizDistDbg  = max(aboveWaterDbg, 0.0) * (nzcDbg / sinSlopeDbg);
        // V4-STYLE: mirror the jitter + strictly-above-water fade so the debug
        // view shows the weights the lit path actually applies.
        float edgeJitDbg    = fbm(v_worldPos.xy * 0.055, 2) * u_shorelineEdgeJitter;
        float horizDistJDbg = max(horizDistDbg + edgeJitDbg, 0.0);
        float aboveFadeDbg  = smoothstep(0.0, 1.5, horizDistDbg);
        float wetHeightDbg  = max(u_shorelineWetHeight,  1e-4);
        float foamHeightDbg = max(u_shorelineFoamHeight, 1e-4);
        vec3 dbgCol = vec3(
            clamp(horizDistDbg / max(wetHeightDbg, 1e-4), 0.0, 1.0),
            (1.0 - smoothstep(0.0, wetHeightDbg,  horizDistJDbg)) * aboveFadeDbg,
            (1.0 - smoothstep(0.0, foamHeightDbg, horizDistJDbg)) * aboveFadeDbg);
        fragColor = vec4(dbgCol, 1.0);
        if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    // TERRAIN-MATERIAL-LIB-1: per-layer roughness/AO scalars, weighted by the
    // same matWeights used for tint/detail. Branch-gated (not a bare multiply)
    // because a *1.0 expression is not guaranteed bit-identical in all compiler
    // paths -- default OFF (u_useMaterialLib==0) takes the untouched `lit` path,
    // exactly reproducing pre-slice output. Neutral (1,1,1,1) JSON defaults ON
    // also reduce to weightedAO==weightedRoughness==1.0 -> lit unchanged.
    if (u_useMaterialLib != 0) {
        float weightedRoughness = dot(matWeights, matRoughness);
        float weightedAO        = dot(matWeights, matAO);
        // Rougher surfaces scatter more of the direct term into ambient-like
        // falloff (cheap non-PBR approximation); AO is a straight multiplier.
        lit *= mix(1.0, 0.85, clamp(weightedRoughness - 1.0, 0.0, 1.0)) * weightedAO;
    }

    fragColor = vec4(lit, 1.0);                                     // alpha forced 1.0
    if ((u_diag & 1) == 0) GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);     // rc_gbuffer1_shadowHandled_flatUp
}
